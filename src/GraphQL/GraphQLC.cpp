#include <iiSocialIntegration/GraphQL.h>
#include <iiSocialIntegration/GraphQLC.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <map>

#ifdef _WIN32
#define IISOCIAL_C_EXPORT __declspec(dllexport)
#else
#define IISOCIAL_C_EXPORT __attribute__((visibility("default")))
#endif

namespace {
using Json = nlohmann::json;
using namespace iiSocialIntegration;
std::string text(const Json &object, const char *key, std::string fallback = {}) {
    if (!object.contains(key)) return fallback;
    if (!object[key].is_string()) throw std::invalid_argument("string");
    return object[key].get<std::string>();
}
bool boolean(const Json &object, const char *key, bool fallback = false) {
    if (!object.contains(key)) return fallback;
    if (!object[key].is_boolean()) throw std::invalid_argument("boolean");
    return object[key].get<bool>();
}
std::filesystem::path utf8Path(const std::string &value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
Account parseAccount(const Json &input) {
    if (!input.is_object()) throw std::invalid_argument("account");
    Account out;
    out.accessToken = text(input, "accessToken");
    out.targetId = text(input, "targetId");
    out.instanceUrl = text(input, "instanceUrl");
    out.apiVersion = text(input, "apiVersion");
    out.allowBillableRequests = boolean(input, "allowBillableRequests");
    out.allowSelfThreadReplies = boolean(input, "allowSelfThreadReplies");
    if (input.contains("expiresAt")) {
        if (!input["expiresAt"].is_number_integer()) throw std::invalid_argument("expiry");
        const auto seconds = input["expiresAt"].get<std::int64_t>();
        const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::time_point::max().time_since_epoch()).count();
        if (seconds < 0 || seconds > maximum) throw std::invalid_argument("expiry");
        out.expiresAt = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    }
    if (input.contains("scopes")) {
        if (!input["scopes"].is_array() || input["scopes"].size() > 256) throw std::invalid_argument("scopes");
        for (const auto &scope : input["scopes"]) {
            if (!scope.is_string()) throw std::invalid_argument("scope");
            out.scopes.push_back(scope.get<std::string>());
        }
    }
    return out;
}
std::string run(const Json &input) {
    if (!input.is_object() || !input.contains("document") || !input["document"].is_object())
        throw std::invalid_argument("document");
    const auto &value = input["document"];
    if (value.contains("variables") && !value["variables"].is_object()) throw std::invalid_argument("variables");
    GraphQLDocument document{text(value, "query"), value.value("variables", Json::object()).dump(), text(value, "operationName")};
    CurlTransport transport(utf8Path(text(input, "certificateAuthority")));
    GraphQLResult result;
    const auto mode = text(input, "mode", "provider");
    if (mode == "provider") {
        std::map<Platform, Account> accounts;
        const auto values = input.value("accounts", Json::object());
        if (!values.is_object() || values.size() > 32) throw std::invalid_argument("accounts");
        for (const auto &[name, value] : values.items()) {
            const auto id = platform(name);
            if (!id || !accounts.emplace(*id, parseAccount(value)).second) throw std::invalid_argument("account");
        }
        GraphQLGateway gateway(transport, [&accounts](Platform id) -> std::optional<Account> {
            const auto found = accounts.find(id);
            if (found == accounts.end()) return {};
            return found->second;
        });
        result = gateway.execute(document);
    } else if (mode == "remote") {
        if (!input.contains("endpoint") || !input["endpoint"].is_object()) throw std::invalid_argument("endpoint");
        result = sendGraphQL({text(input["endpoint"], "url"), text(input["endpoint"], "accessToken")}, document, transport);
    } else throw std::invalid_argument("mode");
    return Json{{"response", Json::parse(result.responseJson)}, {"error", errorName(result.error)},
                {"httpStatus", result.httpStatus}, {"outcomeUnknown", result.outcomeUnknown}}.dump();
}
char *copy(const std::string &value) {
    auto *out = static_cast<char *>(std::malloc(value.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}
}

extern "C" IISOCIAL_C_EXPORT char *iiSocialIntegrationGraphQLExecute(const char *input, size_t length) {
    try {
        if (!input || length == 0 || length > 4 * 1024 * 1024) throw std::invalid_argument("length");
        const auto value = Json::parse(input, input + length, [](int depth, Json::parse_event_t, Json &) {
            if (depth > 24) throw std::invalid_argument("depth");
            return true;
        });
        return copy(run(value));
    } catch (...) {
        try {
            return copy(R"({"response":{"errors":[{"message":"Invalid GraphQL bridge input","extensions":{"code":"INVALID_REQUEST"}}]},"error":"invalid_request","httpStatus":0,"outcomeUnknown":false})");
        } catch (...) { return nullptr; }
    }
}
extern "C" IISOCIAL_C_EXPORT void iiSocialIntegrationGraphQLFree(char *result) { std::free(result); }
