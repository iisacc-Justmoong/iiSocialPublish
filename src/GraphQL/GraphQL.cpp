#include <iiSocialIntegration/GraphQL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace iiSocialIntegration {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxDocument = 64 * 1024;
constexpr std::size_t maxVariables = 1024 * 1024;
constexpr std::size_t maxUrl = 8192;
constexpr std::size_t maxDepth = 20;
constexpr std::size_t maxFields = 128;
constexpr std::size_t maxResponseBytes = 2 * 1024 * 1024;
constexpr std::size_t maxTotalResponseBytes = 8 * 1024 * 1024;
constexpr auto variableMarker = "$iisacc_graphql_variable";

struct JsonDepthExceeded {};
Json boundedJson(std::string_view input) {
    return Json::parse(input.begin(), input.end(), [](int depth, Json::parse_event_t, Json &) {
        if (depth > static_cast<int>(maxDepth)) throw JsonDepthExceeded{};
        return true;
    }, false);
}
struct Failure {
    Error error;
    const char *code;
    const char *message;
    bool unknown = false;
};
[[noreturn]] void fail(Error error, const char *code, const char *message, bool unknown = false) {
    throw Failure{error, code, message, unknown};
}
[[noreturn]] void invalid(const char *message = "Invalid GraphQL request") {
    fail(Error::InvalidRequest, "INVALID_REQUEST", message);
}
Json errorJson(const Failure &f) {
    return {{"message", f.message}, {"extensions", {{"code", f.code}}}};
}
GraphQLResult rejected(const Failure &f) {
    return {Json{{"errors", Json::array({errorJson(f)})}}.dump(), f.error, 0, f.unknown};
}
bool clean(std::string_view s) {
    return std::ranges::none_of(s, [](unsigned char c) { return c < 32 || c == 127; });
}
std::string decodeBase64(std::string_view input) {
    if (input.size() > maxVariables || input.size() % 4 != 0) invalid("Invalid base64 request body");
    const auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string output;
    output.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const auto a = value(input[i]), b = value(input[i + 1]);
        const auto c = input[i + 2] == '=' ? -2 : value(input[i + 2]);
        const auto d = input[i + 3] == '=' ? -2 : value(input[i + 3]);
        const bool last = i + 4 == input.size();
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && (d != -2 || (b & 15))) ||
            (d == -2 && c >= 0 && (c & 3)) ||
            (!last && (c == -2 || d == -2))) invalid("Invalid base64 request body");
        output += static_cast<char>((a << 2) | (b >> 4));
        if (c >= 0) output += static_cast<char>((b << 4) | (c >> 2));
        if (d >= 0) output += static_cast<char>((c << 6) | d);
    }
    return output;
}
bool nameStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool nameRest(unsigned char c) { return nameStart(c) || (c >= '0' && c <= '9'); }

struct Token {
    enum class Kind { End, Name, String, Number, Punct } kind = Kind::End;
    std::string text;
};
class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) { next(); }
    const Token &current() const { return token_; }
    void next() {
        if (++count_ > 16384) invalid("GraphQL token limit exceeded");
        while (position_ < input_.size()) {
            const auto c = static_cast<unsigned char>(input_[position_]);
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++position_; continue; }
            if (c == '#') { while (position_ < input_.size() && input_[position_] != '\n') ++position_; continue; }
            break;
        }
        if (position_ == input_.size()) { token_ = {}; return; }
        const auto start = position_;
        const auto c = static_cast<unsigned char>(input_[position_++]);
        if (nameStart(c)) {
            while (position_ < input_.size() && nameRest(static_cast<unsigned char>(input_[position_]))) ++position_;
            token_ = {Token::Kind::Name, std::string(input_.substr(start, position_ - start))}; return;
        }
        if (c == '"') {
            bool escaped = false, closed = false;
            while (position_ < input_.size()) {
                const auto ch = input_[position_++];
                if (!escaped && ch == '"') { closed = true; break; }
                if (!escaped && ch == '\\') escaped = true; else escaped = false;
            }
            if (!closed) invalid("Unterminated GraphQL string");
            const auto parsed = Json::parse(input_.substr(start, position_ - start), nullptr, false);
            if (!parsed.is_string()) invalid("Invalid GraphQL string");
            token_ = {Token::Kind::String, parsed.get<std::string>()}; return;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            while (position_ < input_.size()) {
                const auto n = input_[position_];
                if ((n >= '0' && n <= '9') || n == '.' || n == 'e' || n == 'E' || n == '+' || n == '-') ++position_;
                else break;
            }
            const auto text = input_.substr(start, position_ - start);
            const auto parsed = Json::parse(text, nullptr, false);
            if (!parsed.is_number()) invalid("Invalid GraphQL number");
            token_ = {Token::Kind::Number, std::string(text)}; return;
        }
        if (std::string_view("!$():=[]{}").find(static_cast<char>(c)) != std::string_view::npos) {
            token_ = {Token::Kind::Punct, std::string(1, static_cast<char>(c))}; return;
        }
        invalid("Unsupported or invalid GraphQL syntax");
    }
    bool take(std::string_view value) {
        if (token_.text != value) return false;
        next(); return true;
    }
    void expect(std::string_view value) { if (!take(value)) invalid("Invalid GraphQL syntax"); }
    std::string name() {
        if (token_.kind != Token::Kind::Name) invalid("Expected GraphQL name");
        auto value = token_.text; next(); return value;
    }
private:
    std::string_view input_;
    std::size_t position_ = 0, count_ = 0;
    Token token_;
};

struct Field {
    std::string name, alias;
    std::map<std::string, Json> arguments;
    std::vector<Field> children;
};
struct Variable {
    std::string type;
    bool required = false;
    std::optional<Json> defaultValue;
};
struct Operation {
    bool mutation = false;
    std::string name;
    std::map<std::string, Variable> variables;
    std::vector<Field> fields;
};
class Parser {
public:
    explicit Parser(std::string_view input) : lexer_(input) {}
    std::vector<Operation> parse() {
        std::vector<Operation> operations;
        while (lexer_.current().kind != Token::Kind::End) {
            if (operations.size() == 8) invalid("Too many GraphQL operations");
            Operation op;
            if (lexer_.current().text != "{") {
                const auto kind = lexer_.name();
                if (kind != "query" && kind != "mutation") invalid("Only query and mutation are supported");
                op.mutation = kind == "mutation";
                if (lexer_.current().kind == Token::Kind::Name) op.name = lexer_.name();
                if (lexer_.take("(")) {
                    while (!lexer_.take(")")) {
                        lexer_.expect("$"); const auto variableName = lexer_.name(); lexer_.expect(":");
                        Variable variable;
                        variable.type = type(0);
                        variable.required = !variable.type.empty() && variable.type.back() == '!';
                        if (lexer_.take("=")) variable.defaultValue = value(0, false);
                        if (!op.variables.emplace(variableName, std::move(variable)).second) invalid("Duplicate GraphQL variable");
                    }
                }
            }
            op.fields = selection(0);
            operations.push_back(std::move(op));
        }
        if (operations.empty()) invalid("GraphQL document is empty");
        std::set<std::string> names;
        for (const auto &op : operations) {
            if (op.name.empty() && operations.size() != 1) invalid("Anonymous operation must be alone");
            if (!op.name.empty() && !names.insert(op.name).second) invalid("Duplicate GraphQL operation");
        }
        return operations;
    }
private:
    std::string type(std::size_t depth) {
        if (depth > maxDepth) invalid("GraphQL type depth exceeded");
        std::string result;
        if (lexer_.take("[")) { result = "[" + type(depth + 1); lexer_.expect("]"); result += "]"; }
        else result = lexer_.name();
        if (lexer_.take("!")) result += "!";
        return result;
    }
    Json value(std::size_t depth, bool variables = true) {
        if (depth > maxDepth) invalid("GraphQL value depth exceeded");
        if (lexer_.take("$")) {
            if (!variables) invalid("Variable defaults must be constant");
            return Json{{variableMarker, lexer_.name()}};
        }
        if (lexer_.take("{")) {
            auto result = Json::object();
            while (!lexer_.take("}")) {
                auto key = lexer_.name(); lexer_.expect(":");
                auto item = value(depth + 1, variables);
                if (result.contains(key)) invalid("Duplicate GraphQL input field");
                result[key] = std::move(item);
            }
            return result;
        }
        if (lexer_.take("[")) {
            auto result = Json::array();
            while (!lexer_.take("]")) {
                if (result.size() == 1024) invalid("GraphQL input list is too large");
                result.push_back(value(depth + 1, variables));
            }
            return result;
        }
        const auto token = lexer_.current();
        if (token.kind == Token::Kind::String) { lexer_.next(); return token.text; }
        if (token.kind == Token::Kind::Number) { lexer_.next(); return Json::parse(token.text); }
        if (token.kind == Token::Kind::Name) {
            lexer_.next();
            if (token.text == "true") return true;
            if (token.text == "false") return false;
            if (token.text == "null") return nullptr;
            return token.text; // enum
        }
        invalid("Expected GraphQL input value");
    }
    std::vector<Field> selection(std::size_t depth) {
        if (depth > maxDepth) invalid("GraphQL selection depth exceeded");
        lexer_.expect("{"); std::vector<Field> result;
        while (!lexer_.take("}")) {
            if (++fields_ > maxFields) invalid("GraphQL field limit exceeded");
            Field field; auto first = lexer_.name();
            if (lexer_.take(":")) { field.alias = first; field.name = lexer_.name(); }
            else field.name = field.alias = first;
            if (lexer_.take("(")) {
                while (!lexer_.take(")")) {
                    const auto arg = lexer_.name(); lexer_.expect(":");
                    if (!field.arguments.emplace(arg, value(0)).second) invalid("Duplicate GraphQL argument");
                }
            }
            if (lexer_.current().text == "{") field.children = selection(depth + 1);
            result.push_back(std::move(field));
        }
        if (result.empty()) invalid("GraphQL selection must not be empty");
        return result;
    }
    Lexer lexer_;
    std::size_t fields_ = 0;
};

bool matchesType(const Json &value, std::string type) {
    if (!type.empty() && type.back() == '!') {
        type.pop_back();
        if (value.is_null()) return false;
    } else if (value.is_null()) return true;
    if (type.size() >= 2 && type.front() == '[' && type.back() == ']') {
        if (!value.is_array()) return false;
        const auto element = type.substr(1, type.size() - 2);
        return std::ranges::all_of(value, [&](const Json &item) { return matchesType(item, element); });
    }
    if (type == "SocialRequestInput" || type == "SocialPublishInput" || type == "JSON")
        return type == "JSON" || value.is_object();
    if (type == "String" || type == "ID") return value.is_string() || (type == "ID" && value.is_number_integer());
    if (type == "Int") return value.is_number_integer() && value.get<std::int64_t>() >= INT32_MIN && value.get<std::int64_t>() <= INT32_MAX;
    if (type == "Float") return value.is_number();
    if (type == "Boolean") return value.is_boolean();
    return false;
}
Json resolve(const Json &input, const Operation &op, const Json &values, std::size_t depth = 0) {
    if (depth > maxDepth) invalid("GraphQL input depth exceeded");
    if (input.is_object() && input.size() == 1 && input.contains(variableMarker)) {
        const auto name = input.at(variableMarker).get<std::string>();
        const auto definition = op.variables.find(name);
        if (definition == op.variables.end()) invalid("GraphQL variable is not declared");
        Json value;
        if (values.contains(name)) value = values.at(name);
        else if (definition->second.defaultValue) value = *definition->second.defaultValue;
        if (!matchesType(value, definition->second.type)) invalid("GraphQL variable does not match its declared type");
        return value;
    }
    if (input.is_object()) {
        Json result = Json::object();
        for (const auto &[key, value] : input.items()) result[key] = resolve(value, op, values, depth + 1);
        return result;
    }
    if (input.is_array()) {
        Json result = Json::array();
        for (const auto &item : input) result.push_back(resolve(item, op, values, depth + 1));
        return result;
    }
    return input;
}
std::string encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto c : value) {
        const auto ch = static_cast<unsigned char>(c);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}
bool httpsOrigin(std::string_view value) {
    static const std::regex pattern(R"(^https://[A-Za-z0-9][A-Za-z0-9.-]*(?::[0-9]{1,5})?$)");
    return value.size() <= 2048 && std::regex_match(value.begin(), value.end(), pattern);
}
bool httpsEndpoint(std::string_view value) {
    if (!clean(value) || value.size() > maxUrl || value.find_first_of("?#\\") != value.npos) return false;
    const auto slash = value.find('/', 8);
    const auto origin = value.substr(0, slash);
    return httpsOrigin(origin) && (slash == value.npos || value.substr(slash).find("//") == value.npos);
}
std::string platformBase(Platform platform, const Account &account, const std::string &family) {
    static const std::regex meta(R"(^v[0-9]{1,3}\.[0-9]{1,3}$)");
    static const std::regex linked(R"(^20[0-9]{2}(0[1-9]|1[0-2])$)");
    switch (platform) {
    case Platform::X:
        if (!family.empty()) invalid("Unsupported API family");
        if (!account.allowBillableRequests) fail(Error::AccessNotConfigured, "ACCESS_NOT_CONFIGURED", "X API access must be explicitly enabled");
        return "https://api.x.com";
    case Platform::Facebook: case Platform::Instagram:
        if (!std::regex_match(account.apiVersion, meta)) invalid("An explicit Meta API version is required");
        if (platform == Platform::Facebook) {
            if (family.empty()) return "https://graph.facebook.com/" + account.apiVersion;
            if (family == "video") return "https://graph-video.facebook.com/" + account.apiVersion;
        } else {
            if (family.empty()) return "https://graph.instagram.com/" + account.apiVersion;
            if (family == "facebook") return "https://graph.facebook.com/" + account.apiVersion;
        }
        invalid("Unsupported API family");
    case Platform::Threads:
        if (!family.empty()) invalid("Unsupported API family");
        return "https://graph.threads.net/v1.0";
    case Platform::Pinterest:
        if (!family.empty()) invalid("Unsupported API family");
        return "https://api.pinterest.com/v5";
    case Platform::Tumblr:
        if (!family.empty()) invalid("Unsupported API family");
        return "https://api.tumblr.com/v2";
    case Platform::Mastodon:
        if (!family.empty()) invalid("Unsupported API family");
        if (!httpsOrigin(account.instanceUrl)) invalid("An HTTPS Mastodon instance origin is required");
        return account.instanceUrl;
    case Platform::LinkedIn:
        if (family == "v2") return "https://api.linkedin.com/v2";
        if (!family.empty() && family != "rest") invalid("Unsupported API family");
        if (!std::regex_match(account.apiVersion, linked)) invalid("An explicit LinkedIn API version is required");
        return "https://api.linkedin.com/rest";
    case Platform::YouTube:
        if (family.empty() || family == "data") return "https://www.googleapis.com/youtube/v3";
        if (family == "upload") return "https://www.googleapis.com/upload/youtube/v3";
        invalid("Unsupported API family");
    case Platform::Pixiv: case Platform::ArtStation:
        fail(Error::Unsupported, "UNSUPPORTED_PLATFORM", "No verified official API is configured for this platform");
    }
    invalid("Unknown platform");
}
std::string path(std::string_view input) {
    if (input.empty() || input.front() != '/' || input.starts_with("//") || input.size() > 4096 || !clean(input) ||
        input.find_first_of("?#\\%") != input.npos) invalid("Invalid provider-relative path");
    std::string result;
    std::size_t begin = 1;
    while (begin <= input.size()) {
        const auto end = input.find('/', begin);
        const auto part = input.substr(begin, end == input.npos ? input.size() - begin : end - begin);
        if (part == "." || part == ".." || (part.empty() && end != input.npos)) invalid("Invalid provider-relative path");
        result += '/'; result += encode(part);
        if (end == input.npos) break;
        begin = end + 1;
    }
    return result;
}
std::string scalar(const Json &value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean() || value.is_number()) return value.dump();
    invalid("Query parameter must be a string, number, boolean or list");
}
std::string queryString(const Json &values) {
    if (values.is_null()) return {};
    if (!values.is_object() || values.size() > 64) invalid("Invalid provider query parameters");
    std::string out;
    for (const auto &[key, value] : values.items()) {
        auto lower = key; std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key.empty() || key.size() > 128 || !clean(key) || lower == "access_token" || lower == "refresh_token" ||
            lower == "client_secret" || lower == "authorization" || lower == "api_key") invalid("Credentials are not allowed in query parameters");
        const auto append = [&](const Json &entry) {
            const auto item = scalar(entry);
            if (item.size() > 4096 || !clean(item)) invalid("Invalid provider query parameter");
            out += out.empty() ? '?' : '&'; out += encode(key); out += '='; out += encode(item);
            if (out.size() > maxUrl) invalid("Provider query is too large");
        };
        if (value.is_array()) {
            if (value.size() > 64) invalid("Provider query list is too large");
            for (const auto &item : value) append(item);
        } else append(value);
    }
    return out;
}
struct Planned {
    std::string alias;
    std::vector<Field> projection;
    Request request;
    std::optional<Platform> publishPlatform;
    std::optional<Account> account;
    Post post;
    bool providers = false;
};
void allowedFields(const Json &object, const std::set<std::string> &allowed) {
    if (!object.is_object()) invalid("Expected an input object");
    for (const auto &[key, value] : object.items()) {
        (void)value; if (!allowed.contains(key)) invalid("Unknown input field");
    }
}
std::string optionalString(const Json &input, const char *name, std::string defaultValue = {}) {
    if (!input.contains(name)) return defaultValue;
    if (!input[name].is_string()) invalid("Expected a string input field");
    return input[name].get<std::string>();
}
bool optionalBool(const Json &input, const char *name, bool defaultValue = false) {
    if (!input.contains(name)) return defaultValue;
    if (!input[name].is_boolean()) invalid("Expected a boolean input field");
    return input[name].get<bool>();
}
Post parsePost(const Json &input) {
    allowedFields(input, {"text", "title", "link", "tags", "media", "coverImageUrl", "visibility",
        "contentWarning", "sensitive", "madeWithAi", "idempotencyKey", "youtube", "replyToId", "language", "replyControl"});
    Post post;
    post.text = optionalString(input, "text");
    post.title = optionalString(input, "title");
    post.link = optionalString(input, "link");
    post.coverImageUrl = optionalString(input, "coverImageUrl");
    post.visibility = optionalString(input, "visibility", "public");
    post.contentWarning = optionalString(input, "contentWarning");
    post.sensitive = optionalBool(input, "sensitive");
    post.madeWithAi = optionalBool(input, "madeWithAi");
    post.idempotencyKey = optionalString(input, "idempotencyKey");
    post.replyToId = optionalString(input, "replyToId");
    post.language = optionalString(input, "language");
    post.replyControl = optionalString(input, "replyControl");
    if (input.contains("tags")) {
        if (!input["tags"].is_array() || input["tags"].size() > 128) invalid("Invalid publication tags");
        for (const auto &tag : input["tags"]) {
            if (!tag.is_string()) invalid("Invalid publication tag");
            post.tags.push_back(tag.get<std::string>());
        }
    }
    if (input.contains("media")) {
        if (!input["media"].is_array() || input["media"].size() > 128) invalid("Invalid publication media");
        for (const auto &item : input["media"]) {
            allowedFields(item, {"kind", "localFile", "publicUrl", "mimeType", "altText"});
            const auto kind = optionalString(item, "kind");
            if (kind != "image" && kind != "video" && kind != "IMAGE" && kind != "VIDEO") invalid("Invalid media kind");
            Media media;
            media.kind = kind == "image" || kind == "IMAGE" ? MediaKind::Image : MediaKind::Video;
            const auto file = optionalString(item, "localFile");
            const std::u8string utf8(file.begin(), file.end());
            media.localFile = std::filesystem::path(utf8);
            media.publicUrl = optionalString(item, "publicUrl");
            media.mimeType = optionalString(item, "mimeType", "image/png");
            media.altText = optionalString(item, "altText");
            post.media.push_back(std::move(media));
        }
    }
    if (input.contains("youtube")) {
        const auto &youtube = input["youtube"];
        allowedFields(youtube, {"categoryId", "madeForKids", "containsSyntheticMedia", "notifySubscribers"});
        post.youtube.categoryId = optionalString(youtube, "categoryId", post.youtube.categoryId);
        if (youtube.contains("madeForKids")) post.youtube.madeForKids = optionalBool(youtube, "madeForKids");
        if (youtube.contains("containsSyntheticMedia")) post.youtube.containsSyntheticMedia = optionalBool(youtube, "containsSyntheticMedia");
        post.youtube.notifySubscribers = optionalBool(youtube, "notifySubscribers", true);
    }
    return post;
}
Account getAccount(Platform provider, const GraphQLAccountProvider &accounts, bool anonymous = false) {
    if (!accounts) fail(Error::AuthenticationRequired, "AUTHENTICATION_REQUIRED", "A host account provider is required");
    auto account = accounts(provider);
    if (!account || (!anonymous && (account->accessToken.empty() || account->accessToken.size() > 8192 || !clean(account->accessToken) ||
        (account->expiresAt && *account->expiresAt <= std::chrono::system_clock::now()))))
        fail(Error::AuthenticationRequired, "AUTHENTICATION_REQUIRED", "A valid provider account is required");
    return std::move(*account);
}
Planned plan(const Field &field, const Operation &op, const Json &variables, const GraphQLAccountProvider &accounts) {
    if (field.name == "providers") {
        if (op.mutation || !field.arguments.empty() || field.children.empty()) invalid("Invalid provider discovery query");
        std::set<std::string> aliases;
        for (const auto &child : field.children) {
            if ((child.name != "id" && child.name != "name" && child.name != "officialApi" &&
                 child.name != "publishingApi" && child.name != "__typename") ||
                !child.arguments.empty() || !child.children.empty() || !aliases.insert(child.alias).second)
                invalid("Invalid provider discovery field");
        }
        return {field.alias, field.children, {}, {}, {}, {}, true};
    }
    if ((field.name != "request" && field.name != "publish") ||
        field.arguments.size() != 1 || !field.arguments.contains("input") || field.children.empty())
        invalid("Unknown or invalid GraphQL root field");
    const bool publishing = field.name == "publish";
    if (publishing && !op.mutation) invalid("Publishing requires a mutation");
    std::set<std::string> resultNames;
    const std::set<std::string> requestFields{"status", "body", "bodyBase64", "headers", "outcomeUnknown", "__typename"};
    const std::set<std::string> publishFields{"published", "postId", "url", "error", "message",
        "httpStatus", "retryAfterSeconds", "requiresReconciliation", "__typename"};
    for (const auto &child : field.children) {
        if (!(publishing ? publishFields : requestFields).contains(child.name) ||
            !child.arguments.empty() || !child.children.empty() || !resultNames.insert(child.alias).second)
            invalid("Unknown or invalid result field");
    }
    const auto input = resolve(field.arguments.at("input"), op, variables);
    if (!input.is_object()) invalid("Request input must be an object");
    if (publishing) {
        allowedFields(input, {"platform", "post"});
        if (!input.contains("platform") || !input["platform"].is_string() || !input.contains("post"))
            invalid("A platform and post are required");
        const auto provider = iiSocialIntegration::platform(input["platform"].get<std::string>());
        if (!provider) invalid("Unknown social platform");
        if (*provider == Platform::Pixiv || *provider == Platform::ArtStation)
            fail(Error::Unsupported, "UNSUPPORTED_PLATFORM", "No verified official API is configured for this platform");
        auto account = getAccount(*provider, accounts);
        auto post = parsePost(input["post"]);
        const auto valid = validatePost(*provider, post, account);
        if (valid.error != Error::None) fail(valid.error, "INVALID_PUBLICATION", "Publication input failed provider validation");
        return {field.alias, field.children, {}, *provider, std::move(account), std::move(post), false};
    }
    static const std::set<std::string> allowed{"platform", "apiFamily", "method", "path", "query", "body", "bodyText", "bodyBase64", "contentType", "headers", "anonymous"};
    for (const auto &[key, value] : input.items()) { (void)value; if (!allowed.contains(key)) invalid("Unknown request input field"); }
    for (const auto *key : {"platform", "method", "path"})
        if (!input.contains(key) || !input[key].is_string()) invalid("A platform, method and path are required");
    const auto provider = iiSocialIntegration::platform(input["platform"].get<std::string>());
    if (!provider) invalid("Unknown social platform");
    if (*provider == Platform::Pixiv || *provider == Platform::ArtStation)
        fail(Error::Unsupported, "UNSUPPORTED_PLATFORM", "No verified official API is configured for this platform");
    const auto method = input["method"].get<std::string>();
    if ((!op.mutation && method != "GET") || (op.mutation && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE"))
        invalid("The HTTP method does not match the GraphQL operation");
    const bool anonymous = optionalBool(input, "anonymous");
    if (anonymous && method != "GET") invalid("Anonymous access is limited to GET");
    const auto present = [&](const char *key) { return input.contains(key) && !input[key].is_null(); };
    const int bodyCount = static_cast<int>(present("body")) + static_cast<int>(present("bodyText")) +
        static_cast<int>(present("bodyBase64"));
    if (bodyCount > 1 || (method == "GET" && bodyCount)) invalid("Invalid or conflicting request bodies");
    auto account = getAccount(*provider, accounts, anonymous);
    const auto family = optionalString(input, "apiFamily");
    Request request; request.method = method;
    request.url = platformBase(*provider, account, family) + path(input["path"].get<std::string>()) +
        queryString(input.value("query", Json::object()));
    if (request.url.size() > maxUrl) invalid("Provider URL is too large");
    request.headers = {{"Accept", "application/json"}};
    if (!anonymous) request.headers.emplace_back("Authorization", "Bearer " + account.accessToken);
    if (*provider == Platform::LinkedIn && family != "v2") {
        request.headers.emplace_back("LinkedIn-Version", account.apiVersion);
        request.headers.emplace_back("X-Restli-Protocol-Version", "2.0.0");
    }
    if (present("body")) {
        request.body = input["body"].dump();
    } else if (present("bodyText")) {
        if (!input["bodyText"].is_string()) invalid("Text request body must be a string");
        request.body = input["bodyText"].get<std::string>();
    } else if (present("bodyBase64")) {
        if (!input["bodyBase64"].is_string()) invalid("Binary request body must be base64 text");
        request.body = decodeBase64(input["bodyBase64"].get<std::string>());
    }
    if (request.body.size() > maxVariables) invalid("Provider request body is too large");
    if (present("contentType")) {
        if (!input["contentType"].is_string()) invalid("Content type must be a string");
        const auto contentType = input["contentType"].get<std::string>();
        if (contentType.empty() || contentType.size() > 255 || !clean(contentType)) invalid("Invalid content type");
        request.headers.emplace_back("Content-Type", contentType);
    } else if (present("body")) {
        request.headers.emplace_back("Content-Type", "application/json");
    }
    if (input.contains("headers")) {
        const auto &headers = input["headers"];
        if (!headers.is_object() || headers.size() > 8) invalid("Invalid provider request headers");
        static const std::map<std::string, std::string> allowedHeaders{
            {"if-match", "If-Match"}, {"if-none-match", "If-None-Match"},
            {"idempotency-key", "Idempotency-Key"}, {"accept-language", "Accept-Language"},
            {"x-restli-method", "X-RestLi-Method"}
        };
        std::set<std::string> selected;
        for (const auto &[key, value] : headers.items()) {
            auto lower = key;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return c == '_' ? '-' : static_cast<char>(std::tolower(c));
            });
            const auto found = allowedHeaders.find(lower);
            if (found == allowedHeaders.end() || !value.is_string() || !selected.insert(lower).second)
                invalid("Provider request header is not permitted");
            const auto valueText = value.get<std::string>();
            if (valueText.size() > 4096 || !clean(valueText)) invalid("Invalid provider request header");
            request.headers.emplace_back(found->second, valueText);
        }
    }
    return {field.alias, field.children, std::move(request), {}, {}, {}, false};
}
std::string base64(std::string_view input) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = static_cast<unsigned char>(input[i]);
        const auto b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const auto c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        output += alphabet[a >> 2]; output += alphabet[((a & 3) << 4) | (b >> 4)];
        output += i + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output += i + 2 < input.size() ? alphabet[c & 63] : '=';
    }
    return output;
}
Json responseHeaders(const Headers &headers) {
    Json result = Json::object();
    for (const auto &[name, value] : headers) {
        auto key = name;
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "content-type" || key == "content-range" || key == "etag" || key == "retry-after" ||
            key == "link" || key == "location" || key == "x-next-page" || key == "x-prev-page" ||
            key == "x-total-count" || key == "x-restli-id" ||
            key.starts_with("x-ratelimit-") || key.starts_with("x-rate-limit-") || key.starts_with("ratelimit-")) {
            if (clean(value) && value.size() <= 4096) {
                try { (void)Json(value).dump(); result[key] = value; } catch (const Json::exception &) {}
            }
        }
    }
    return result;
}
bool validEnvelope(const Json &value) {
    if (!value.is_object() || (!value.contains("data") && !value.contains("errors"))) return false;
    if (value.contains("data") && !value["data"].is_object() && !value["data"].is_null()) return false;
    if (value.contains("errors") && (!value["errors"].is_array() || value["errors"].empty())) return false;
    if (value.contains("errors"))
        for (const auto &error : value["errors"])
            if (!error.is_object() || !error.contains("message") || !error["message"].is_string()) return false;
    return true;
}
Json parseVariables(const GraphQLDocument &document) {
    if (document.query.empty() || document.query.size() > maxDocument || document.variablesJson.size() > maxVariables ||
        document.operationName.size() > 128) invalid("GraphQL document or variables are too large");
    Json variables;
    try { variables = boundedJson(document.variablesJson.empty() ? "{}" : document.variablesJson); }
    catch (const JsonDepthExceeded &) { invalid("GraphQL variables are too deeply nested"); }
    if (!variables.is_object()) invalid("GraphQL variables must be a JSON object");
    return variables;
}
}

GraphQLGateway::GraphQLGateway(Transport &transport, GraphQLAccountProvider accounts)
    : transport_(transport), accounts_(std::move(accounts)) {}

GraphQLResult GraphQLGateway::executeEnvelope(std::string_view json, std::stop_token stop) const {
    try {
        if (json.empty() || json.size() > maxVariables + maxDocument + 8192) invalid("GraphQL request envelope is too large");
        const auto envelope = boundedJson(json);
        if (!envelope.is_object() || !envelope.contains("query") || !envelope["query"].is_string())
            invalid("GraphQL request envelope requires a query");
        if (envelope.contains("variables") && !envelope["variables"].is_null() && !envelope["variables"].is_object())
            invalid("GraphQL variables must be a JSON object");
        if (envelope.contains("operationName") && !envelope["operationName"].is_null() && !envelope["operationName"].is_string())
            invalid("GraphQL operation name must be a string");
        const auto variables = envelope.contains("variables") && !envelope["variables"].is_null()
            ? envelope["variables"].dump() : "{}";
        const auto operation = envelope.contains("operationName") && !envelope["operationName"].is_null()
            ? envelope["operationName"].get<std::string>() : std::string{};
        return execute({envelope["query"].get<std::string>(), variables, operation}, stop);
    } catch (const Failure &failure) { return rejected(failure); }
      catch (...) { return rejected({Error::InvalidRequest, "INVALID_REQUEST", "Invalid GraphQL request envelope"}); }
}

GraphQLResult GraphQLGateway::execute(const GraphQLDocument &document, std::stop_token stop) const {
    try {
        const auto variables = parseVariables(document);
        const auto operations = Parser(document.query).parse();
        const Operation *selected = nullptr;
        for (const auto &op : operations) if (op.name == document.operationName) selected = &op;
        if (document.operationName.empty() && operations.size() == 1) selected = &operations.front();
        if (!selected) invalid("Select an existing GraphQL operation");
        for (const auto &[name, variable] : selected->variables) {
            if (variable.required && !variables.contains(name) && !variable.defaultValue) invalid("A required GraphQL variable is missing");
            if (variables.contains(name) && !matchesType(variables[name], variable.type)) invalid("GraphQL variable does not match its declared type");
        }
        std::vector<Planned> planned; std::set<std::string> aliases;
        if (selected->fields.size() > 32) invalid("Too many GraphQL root fields");
        for (const auto &field : selected->fields) {
            if (!aliases.insert(field.alias).second) invalid("Duplicate GraphQL response name");
            planned.push_back(plan(field, *selected, variables, accounts_));
        }
        Json data = Json::object(), errors = Json::array();
        Error outcome = Error::None; bool unknown = false;
        std::size_t receivedBytes = 0;
        for (const auto &item : planned) {
            try {
                if (stop.stop_requested()) fail(Error::Cancelled, "CANCELLED", "Request was cancelled");
                if (item.providers) {
                    Json rows = Json::array();
                    for (const auto &capability : capabilities()) {
                        Json object = Json::object();
                        for (const auto &child : item.projection) {
                            if (child.name == "id") object[child.alias] = capability.id;
                            else if (child.name == "name") object[child.alias] = capability.name;
                            else if (child.name == "officialApi") object[child.alias] =
                                capability.platform != Platform::Pixiv && capability.platform != Platform::ArtStation;
                            else if (child.name == "publishingApi") object[child.alias] = capability.officialPublishingApi;
                            else object[child.alias] = "SocialProvider";
                        }
                        rows.push_back(std::move(object));
                    }
                    data[item.alias] = std::move(rows);
                    continue;
                }
                if (item.publishPlatform) {
                    Context context{transport_, stop, 60, std::chrono::milliseconds(1000), {}, 3};
                    const auto result = makePublisher(*item.publishPlatform)->publish(item.post, *item.account, context);
                    Json object = Json::object();
                    for (const auto &child : item.projection) {
                        if (child.name == "published") object[child.alias] = result.published();
                        else if (child.name == "postId") object[child.alias] = result.postId;
                        else if (child.name == "url") object[child.alias] = result.url;
                        else if (child.name == "error") object[child.alias] = std::string(errorName(result.error));
                        else if (child.name == "message") object[child.alias] = result.message;
                        else if (child.name == "httpStatus") object[child.alias] = result.httpStatus;
                        else if (child.name == "retryAfterSeconds") object[child.alias] = result.retryAfterSeconds;
                        else if (child.name == "requiresReconciliation") object[child.alias] = result.requiresReconciliation();
                        else object[child.alias] = "SocialPublishResult";
                    }
                    data[item.alias] = std::move(object);
                    if (outcome == Error::None) outcome = result.error;
                    unknown = unknown || result.error == Error::OutcomeUnknown;
                    continue;
                }
                Response response;
                try { response = transport_.send(item.request, stop); }
                catch (...) { fail(selected->mutation ? Error::OutcomeUnknown : Error::TransportFailure,
                    selected->mutation ? "OUTCOME_UNKNOWN" : "TRANSPORT_FAILURE", "Provider request did not complete", selected->mutation); }
                if (response.transportFailed || response.cancelled || response.responseLimitExceeded) {
                    const bool mayHaveMutated = selected->mutation && (response.mayHaveBeenSent || response.status > 0);
                    fail(mayHaveMutated ? Error::OutcomeUnknown : response.cancelled ? Error::Cancelled : Error::TransportFailure,
                        mayHaveMutated ? "OUTCOME_UNKNOWN" : response.cancelled ? "CANCELLED" : "TRANSPORT_FAILURE",
                        "Provider request did not complete", mayHaveMutated);
                }
                if (response.body.size() > maxResponseBytes || response.body.size() > maxTotalResponseBytes - receivedBytes)
                    fail(selected->mutation ? Error::OutcomeUnknown : Error::InvalidResponse,
                        selected->mutation ? "OUTCOME_UNKNOWN" : "RESPONSE_LIMIT",
                        "Provider response exceeded the allowed size", selected->mutation);
                receivedBytes += response.body.size();
                Json body = nullptr;
                if (!response.body.empty()) {
                    try { body = boundedJson(response.body); }
                    catch (const JsonDepthExceeded &) { body = nullptr; }
                    if (body.is_discarded()) body = nullptr;
                }
                const bool mayHaveMutated = selected->mutation && response.status >= 500;
                if (response.status < 200 || response.status >= 300) {
                    auto code = response.status == 401 ? Error::AuthenticationRequired :
                        response.status == 403 ? Error::PermissionDenied : response.status == 429 ? Error::RateLimited : Error::ProviderRejected;
                    if (mayHaveMutated) code = Error::OutcomeUnknown;
                    if (outcome == Error::None) outcome = code;
                    unknown = unknown || mayHaveMutated;
                }
                Json object = Json::object();
                for (const auto &child : item.projection) {
                    if (child.name == "status") object[child.alias] = response.status;
                    else if (child.name == "body") object[child.alias] = body;
                    else if (child.name == "bodyBase64") object[child.alias] = base64(response.body);
                    else if (child.name == "headers") object[child.alias] = responseHeaders(response.headers);
                    else if (child.name == "outcomeUnknown") object[child.alias] = mayHaveMutated;
                    else object[child.alias] = "SocialResponse";
                }
                data[item.alias] = std::move(object);
            } catch (const Failure &failure) {
                auto error = errorJson(failure); error["path"] = Json::array({item.alias}); errors.push_back(std::move(error));
                data[item.alias] = nullptr;
                if (outcome == Error::None) outcome = failure.error;
                unknown = unknown || failure.unknown;
            }
        }
        Json response{{"data", data}}; if (!errors.empty()) response["errors"] = std::move(errors);
        return {response.dump(), outcome, 0, unknown};
    } catch (const Failure &failure) { return rejected(failure); }
      catch (...) { return rejected({Error::InvalidRequest, "INVALID_REQUEST", "Invalid GraphQL request"}); }
}

GraphQLResult sendGraphQL(const GraphQLEndpoint &endpoint, const GraphQLDocument &document,
                          Transport &transport, std::stop_token stop) {
    try {
        const auto variables = parseVariables(document);
        if (!httpsEndpoint(endpoint.url) || endpoint.accessToken.size() > 8192 || !clean(endpoint.accessToken))
            invalid("A trusted HTTPS GraphQL endpoint is required");
        if (stop.stop_requested()) fail(Error::Cancelled, "CANCELLED", "Request was cancelled");
        Json body{{"query", document.query}, {"variables", variables}};
        if (!document.operationName.empty()) body["operationName"] = document.operationName;
        Request request; request.method = "POST"; request.url = endpoint.url;
        request.headers = {{"Accept", "application/graphql-response+json, application/json"}, {"Content-Type", "application/json"}};
        if (!endpoint.accessToken.empty()) request.headers.emplace_back("Authorization", "Bearer " + endpoint.accessToken);
        request.body = body.dump();
        Response response;
        try { response = transport.send(request, stop); }
        catch (...) { fail(Error::OutcomeUnknown, "OUTCOME_UNKNOWN", "GraphQL request did not complete", true); }
        if (response.transportFailed || response.cancelled || response.responseLimitExceeded) {
            if (response.mayHaveBeenSent || response.status > 0) fail(Error::OutcomeUnknown, "OUTCOME_UNKNOWN", "GraphQL request outcome is unknown", true);
            fail(response.cancelled ? Error::Cancelled : Error::TransportFailure,
                 response.cancelled ? "CANCELLED" : "TRANSPORT_FAILURE", "GraphQL request did not complete");
        }
        if (response.body.size() > maxResponseBytes) {
            auto result = rejected({Error::OutcomeUnknown, "OUTCOME_UNKNOWN", "GraphQL response exceeded the allowed size", true});
            result.httpStatus = response.status; return result;
        }
        Json envelope;
        try { envelope = boundedJson(response.body); }
        catch (const JsonDepthExceeded &) { envelope = Json::value_t::discarded; }
        if (envelope.is_discarded() || !validEnvelope(envelope)) {
            const bool uncertain = response.status >= 200 && (response.status < 300 || response.status >= 500);
            auto result = rejected({uncertain ? Error::OutcomeUnknown : Error::InvalidResponse,
                uncertain ? "OUTCOME_UNKNOWN" : "INVALID_RESPONSE", "Endpoint returned an invalid GraphQL response", uncertain});
            result.httpStatus = response.status; return result;
        }
        const bool uncertain = response.status >= 500;
        const auto error = response.status >= 200 && response.status < 300 ? Error::None :
            response.status == 401 ? Error::AuthenticationRequired : response.status == 403 ? Error::PermissionDenied :
            response.status == 429 ? Error::RateLimited : uncertain ? Error::OutcomeUnknown : Error::ProviderRejected;
        return {envelope.dump(), error, response.status, uncertain};
    } catch (const Failure &failure) { return rejected(failure); }
      catch (...) { return rejected({Error::InvalidRequest, "INVALID_REQUEST", "Invalid GraphQL request"}); }
}
} // namespace iiSocialIntegration
