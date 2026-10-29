#include <iiSocialIntegration/Publishing.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>

namespace iiSocialIntegration {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t chunkSize = 4 * 1024 * 1024;
struct Failure { Outcome outcome; };
[[noreturn]] void fail(Error error, std::string message) {
    Outcome outcome; outcome.error = error; outcome.message = std::move(message);
    throw Failure{std::move(outcome)};
}
std::string str(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return {};
    const auto& value = j.at(key);
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    return {};
}
std::string id(const Json& j, const char* key = "id") {
    auto value = str(j, key);
    if (value.empty() || value.size() > 512 || value.find_first_of("\r\n") != std::string::npos)
        fail(Error::InvalidResponse, "Provider response has no valid resource identifier");
    return value;
}
std::string header(const Headers& headers, std::string_view key) {
    for (auto& [name, value] : headers) {
        if (name.size() == key.size() && std::equal(name.begin(), name.end(), key.begin(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); })) return value;
    }
    return {};
}
std::string encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
bool clean(std::string_view value) { return value.find_first_of("\r\n") == value.npos && value.find('\0') == value.npos; }
bool https(std::string_view url) {
    static const std::regex pattern(R"(^https://[A-Za-z0-9][A-Za-z0-9.-]*(?::[0-9]{1,5})?(?:[/?#][^\s]*)?$)");
    return url.size() <= 8192 && std::regex_match(url.begin(), url.end(), pattern);
}
bool origin(std::string_view url) {
    return https(url) && url.find_first_of("/?#", 8) == url.npos;
}
bool linkedInUpload(std::string_view url) {
    if (!https(url)) return false;
    auto host = url.substr(8, url.find('/', 8) - 8);
    return host == "www.linkedin.com" || host == "api.linkedin.com" || host.ends_with(".licdn.com") || host.ends_with(".linkedin.com");
}
std::uint64_t size(const Media& media) { return std::filesystem::file_size(media.localFile); }
Part field(std::string name, std::string value) { return {std::move(name), std::move(value), {}, {}}; }
Part file(std::string name, const Media& media, std::uint64_t offset = 0, std::optional<std::uint64_t> length = {}) {
    return {std::move(name), {}, media.mimeType, FileSlice{media.localFile, offset, length}};
}
std::string text(const Post& post, bool appendLink = true, bool appendTags = true) {
    auto out = post.text;
    if (appendLink && !post.link.empty()) out += (out.empty() ? "" : "\n") + post.link;
    if (appendTags) for (auto tag : post.tags) {
        if (tag.empty()) continue;
        if (tag.front() != '#') tag.insert(tag.begin(), '#');
        out += (out.empty() ? "" : " ") + tag;
    }
    return out;
}
std::string base64File(const Media& media) {
    if (size(media) > 32 * 1024 * 1024) fail(Error::InvalidRequest, "Base64 image exceeds the adapter's 32 MiB input limit");
    std::ifstream in(media.localFile, std::ios::binary);
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned char bytes[3];
    while (in) {
        in.read(reinterpret_cast<char*>(bytes), 3);
        auto n = in.gcount();
        if (!n) break;
        out += alphabet[bytes[0] >> 2];
        out += alphabet[((bytes[0] & 3) << 4) | (n > 1 ? bytes[1] >> 4 : 0)];
        out += n > 1 ? alphabet[((bytes[1] & 15) << 2) | (n > 2 ? bytes[2] >> 6 : 0)] : '=';
        out += n > 2 ? alphabet[bytes[2] & 63] : '=';
    }
    if (in.bad()) fail(Error::InvalidRequest, "Unable to read the image");
    return out;
}
struct Runner {
    const Account& account;
    Context& context;
    Response last;
    bool publicationAcknowledged = false;
    Headers headers(bool linkedIn = false) const {
        Headers out{{"Authorization", "Bearer " + account.accessToken}, {"Accept", "application/json"}};
        if (linkedIn) { out.emplace_back("LinkedIn-Version", account.apiVersion); out.emplace_back("X-Restli-Protocol-Version", "2.0.0"); }
        return out;
    }
    Json send(Request request, bool publishing = false, bool emptyOkay = false, bool raw = false, bool resumeIncomplete = false) {
        if (context.stop.stop_requested()) fail(Error::Cancelled, "Publishing was cancelled");
        try { last = context.transport.send(request, context.stop); }
        catch (...) { fail(publishing ? Error::OutcomeUnknown : Error::TransportFailure, "Transport failed; reconcile any publication before retrying"); }
        if (last.transportFailed || last.cancelled) {
            if (publishing && last.mayHaveBeenSent) fail(Error::OutcomeUnknown, "Publication outcome is unknown; reconcile before retrying");
            fail(last.cancelled ? Error::Cancelled : Error::TransportFailure, "Request did not complete");
        }
        // YouTube's 308 is a progress acknowledgement, never an HTTP redirect.
        if (resumeIncomplete && last.status == 308) return Json::object();
        if (last.status < 200 || last.status >= 300) {
            Error error = last.status == 401 ? Error::AuthenticationRequired : last.status == 403 ? Error::PermissionDenied : last.status == 429 ? Error::RateLimited : Error::ProviderRejected;
            if (publishing && last.status >= 500) error = Error::OutcomeUnknown;
            Outcome out; out.error = error;
            out.message = error == Error::OutcomeUnknown ? "Publication outcome is unknown; reconcile before retrying" : "Provider rejected the request";
            out.httpStatus = last.status;
            auto retry = header(last.headers, "Retry-After");
            unsigned seconds = 0;
            auto parsed = std::from_chars(retry.data(), retry.data() + retry.size(), seconds);
            if (parsed.ec == std::errc{} && parsed.ptr == retry.data() + retry.size()) out.retryAfterSeconds = seconds;
            throw Failure{std::move(out)};
        }
        if (publishing) publicationAcknowledged = true;
        if (raw || (last.body.empty() && emptyOkay)) return Json::object();
        auto out = Json::parse(last.body, nullptr, false);
        if (out.is_discarded() || !out.is_object()) fail(publishing ? Error::OutcomeUnknown : Error::InvalidResponse, "Provider returned an invalid JSON response");
        if (out.contains("error") || (out.contains("errors") && !out["errors"].empty()))
            fail(publishing ? Error::OutcomeUnknown : Error::ProviderRejected, "Provider response contains errors");
        return out;
    }
    Json json(std::string method, std::string url, Json body = {}, bool publishing = false, bool linkedIn = false, bool emptyOkay = false) {
        Request request;
        request.method = std::move(method); request.url = std::move(url); request.headers = headers(linkedIn);
        if (request.method != "GET") {
            request.headers.emplace_back("Content-Type", "application/json");
            request.body = body.is_null() ? "{}" : body.dump();
        }
        return send(std::move(request), publishing, emptyOkay);
    }
    Json form(std::string url, Json values, bool publishing = false) {
        Request request; request.method = "POST"; request.url = std::move(url); request.headers = headers();
        request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
        for (auto& [key, value] : values.items()) {
            if (!request.body.empty()) request.body += '&';
            request.body += encode(key) + "=" + encode(value.is_string() ? value.get<std::string>() : value.dump());
        }
        return send(std::move(request), publishing);
    }
    Json multipart(std::string url, std::vector<Part> parts, bool publishing = false, bool authenticated = true) {
        Request request; request.method = "POST"; request.url = std::move(url); request.parts = std::move(parts);
        if (authenticated) request.headers = headers();
        return send(std::move(request), publishing, true, !authenticated);
    }
    void pause(std::chrono::milliseconds interval = {}) {
        interval = std::max(interval, context.pollInterval);
        bool completed;
        if (context.wait) completed = context.wait(interval, context.stop);
        else {
            std::mutex mutex; std::condition_variable_any cv; std::unique_lock lock(mutex);
            cv.wait_for(lock, context.stop, interval, [] { return false; });
            completed = !context.stop.stop_requested();
        }
        if (!completed || context.stop.stop_requested()) fail(Error::Cancelled, "Publishing was cancelled");
    }
    template<class F> Json poll(F action) {
        for (unsigned i = 0; i < context.maxPolls; ++i) {
            auto [done, data] = action();
            if (done) return data;
            if (i + 1 < context.maxPolls) pause();
        }
        fail(Error::ProcessingTimeout, "Media processing did not finish within the configured polling budget");
    }
    Outcome published(std::string resource, std::string url = {}) { return {Error::None, {}, std::move(resource), std::move(url), last.status}; }
};

const Capability& capabilityFor(Platform value) {
    for (auto& row : capabilities()) if (row.platform == value) return row;
    throw std::invalid_argument("Unknown publishing platform");
}
void validate(Platform value, const Post& post, const Account& account) {
    const auto& cap = capabilityFor(value);
    if (!cap.officialPublishingApi) fail(Error::Unsupported, "No verified official public publishing API is configured for this platform");
    if (!post.replyToId.empty()) {
        if (value != Platform::X && value != Platform::Threads && value != Platform::Mastodon) fail(Error::InvalidRequest, "This adapter does not support replies");
        if (post.replyToId.size() > 128 || !std::ranges::all_of(post.replyToId, [](unsigned char c) { return c >= '0' && c <= '9'; })) fail(Error::InvalidRequest, "Invalid reply resource identifier");
        if (value == Platform::X && !account.allowSelfThreadReplies) fail(Error::AccessNotConfigured, "X reply entitlement has not been verified for this account");
    }
    if (!post.language.empty() && (post.language.size() > 35 || !std::ranges::all_of(post.language, [](unsigned char c) { return std::isalnum(c) || c == '-'; }))) fail(Error::InvalidRequest, "Invalid publication language");
    if (!post.replyControl.empty()) {
        const bool allowed = value == Platform::X ? (post.replyControl == "following" || post.replyControl == "mentionedUsers" || post.replyControl == "subscribers" || post.replyControl == "verified") :
            value == Platform::Threads && (post.replyControl == "everyone" || post.replyControl == "accounts_you_follow" || post.replyControl == "mentioned_only");
        if (!allowed) fail(Error::InvalidRequest, "Unsupported destination reply control");
    }
    if (account.accessToken.empty() || account.accessToken.size() > 16384 || !clean(account.accessToken)) fail(Error::AuthenticationRequired, "A valid OAuth user access token is required");
    if (account.expiresAt && *account.expiresAt <= std::chrono::system_clock::now()) fail(Error::AuthenticationRequired, "The OAuth grant has expired");
    for (auto& scope : cap.requiredScopes) {
        if (value == Platform::Mastodon && scope == "write:media" && post.media.empty()) continue;
        bool granted = std::ranges::find(account.scopes, scope) != account.scopes.end();
        if (value == Platform::LinkedIn && account.targetId.starts_with("urn:li:organization:")) granted = std::ranges::find(account.scopes, "w_organization_social") != account.scopes.end();
        if (value == Platform::Mastodon) granted = granted || std::ranges::find(account.scopes, "write") != account.scopes.end();
        if (value == Platform::YouTube) granted = granted || std::ranges::any_of(account.scopes, [](const std::string& grant) {
            return grant == "https://www.googleapis.com/auth/youtube" || grant == "https://www.googleapis.com/auth/youtube.force-ssl" || grant == "https://www.googleapis.com/auth/youtubepartner";
        });
        if (!granted) fail(Error::PermissionDenied, "The OAuth grant is missing a required publishing scope");
    }
    if (value == Platform::X && !account.allowBillableRequests) fail(Error::AccessNotConfigured, "X paid API access has not been provisioned for this account");
    if (value == Platform::X && !post.media.empty() && std::ranges::find(account.scopes, "media.write") == account.scopes.end()) fail(Error::PermissionDenied, "X media uploads require media.write");
    if ((value == Platform::Facebook || value == Platform::Instagram) && !std::regex_match(account.apiVersion, std::regex(R"(v[0-9]{2}\.[0-9]+)"))) fail(Error::InvalidRequest, "An explicit supported Meta Graph API version is required");
    if (value == Platform::LinkedIn && !std::regex_match(account.apiVersion, std::regex(R"(20[0-9]{2}(0[1-9]|1[0-2]))"))) fail(Error::InvalidRequest, "An explicit supported LinkedIn YYYYMM API version is required");
    if (value != Platform::X && value != Platform::Mastodon && value != Platform::YouTube) {
        if (account.targetId.empty() || account.targetId.size() > 256 || !clean(account.targetId)) fail(Error::InvalidRequest, "A publishing destination is required");
        if (value == Platform::Facebook || value == Platform::Instagram || value == Platform::Threads || value == Platform::Pinterest) {
            if (!std::ranges::all_of(account.targetId, [](unsigned char c) { return c >= '0' && c <= '9'; })) fail(Error::InvalidRequest, "The destination must be a numeric provider ID");
        }
    }
    if (value == Platform::LinkedIn && !std::regex_match(account.targetId, std::regex(R"(urn:li:(person|organization):[A-Za-z0-9_-]+)"))) fail(Error::InvalidRequest, "LinkedIn requires an explicit member or organization author URN");
    if (value == Platform::Mastodon && !origin(account.instanceUrl)) fail(Error::InvalidRequest, "Mastodon requires an HTTPS instance origin without path or user information");
    if (value != Platform::Mastodon && value != Platform::YouTube && post.visibility != "public") fail(Error::InvalidRequest, "This adapter supports public posts only");
    if (value == Platform::Mastodon && post.visibility != "public" && post.visibility != "unlisted" && post.visibility != "private" && post.visibility != "direct") fail(Error::InvalidRequest, "Unsupported Mastodon visibility");
    if (text(post).empty() && post.media.empty()) fail(Error::InvalidRequest, "A post needs text or media");
    if ((!cap.text && post.media.empty()) || post.media.size() > 20 || post.text.size() > 256 * 1024 || post.tags.size() > 100) fail(Error::InvalidRequest, "Post content exceeds the adapter's supported shape");
    if (!post.idempotencyKey.empty() && (!clean(post.idempotencyKey) || post.idempotencyKey.size() > 128)) fail(Error::InvalidRequest, "Invalid idempotency key");
    if (!post.link.empty() && !https(post.link)) fail(Error::InvalidRequest, "Post links must be HTTPS URLs");
    unsigned videos = 0, gifs = 0;
    for (const auto& media : post.media) {
        videos += media.kind == MediaKind::Video;
        gifs += media.mimeType == "image/gif";
        const bool youtubeBinary = value == Platform::YouTube && media.kind == MediaKind::Video && media.mimeType == "application/octet-stream";
        if (!clean(media.mimeType) || (!youtubeBinary && !media.mimeType.starts_with(media.kind == MediaKind::Video ? "video/" : "image/"))) fail(Error::InvalidRequest, "Media MIME type does not match its kind");
        if (!media.publicUrl.empty() && !https(media.publicUrl)) fail(Error::InvalidRequest, "Media URLs must use HTTPS without user information");
        if (cap.requiresPublicMediaUrl && media.publicUrl.empty()) fail(Error::InvalidRequest, "This API fetches media from a publicly reachable HTTPS URL");
        bool localRequired = value == Platform::X || value == Platform::LinkedIn || value == Platform::Mastodon || value == Platform::YouTube || (value == Platform::Pinterest && media.kind == MediaKind::Video);
        if (localRequired && media.localFile.empty()) fail(Error::InvalidRequest, "This upload requires an exported local media file");
        if (media.localFile.empty() && media.publicUrl.empty()) fail(Error::InvalidRequest, "Media has no file or URL");
        if (!media.localFile.empty()) {
            std::error_code ec;
            if (!media.localFile.is_absolute() || !std::filesystem::is_regular_file(media.localFile, ec)) fail(Error::InvalidRequest, "Media must refer to a readable absolute file path");
            const auto bytes = std::filesystem::file_size(media.localFile, ec);
            if (ec || !bytes || bytes > 5ULL * 1024 * 1024 * 1024) fail(Error::InvalidRequest, "Media file is empty or exceeds the adapter's 5 GiB safety limit");
            std::ifstream stream(media.localFile, std::ios::binary);
            if (!stream) fail(Error::InvalidRequest, "Media file cannot be read");
        }
    }
    if (videos && (videos != 1 || post.media.size() != 1) && value != Platform::Instagram && value != Platform::Threads && value != Platform::Tumblr) fail(Error::InvalidRequest, "This adapter accepts a single video without other attachments");
    if (value == Platform::X && gifs && post.media.size() != 1) fail(Error::InvalidRequest, "An X animated GIF must be the only attachment");
    if ((value == Platform::Pinterest && post.media.size() != 1) || (value == Platform::X && post.media.size() > 4) || ((value == Platform::Facebook || value == Platform::Instagram || value == Platform::Tumblr) && post.media.size() > 10)) fail(Error::InvalidRequest, "Too many attachments for this platform adapter");
    if (value == Platform::Pinterest && videos && !https(post.coverImageUrl)) fail(Error::InvalidRequest, "A Pinterest video Pin requires a public HTTPS cover image");
    if (value == Platform::YouTube) {
        if (!account.targetId.empty()) fail(Error::InvalidRequest, "YouTube uploads to the OAuth channel; targetId must be empty");
        if (post.media.size() != 1 || videos != 1) fail(Error::InvalidRequest, "YouTube requires exactly one local video");
        if (post.visibility != "public" && post.visibility != "unlisted" && post.visibility != "private") fail(Error::InvalidRequest, "Unsupported YouTube visibility");
        const auto description = text(post, true, false);
        try { (void)Json{{"title", post.title}, {"description", description}, {"tags", post.tags}}.dump(); }
        catch (const Json::exception&) { fail(Error::InvalidRequest, "YouTube metadata must contain valid UTF-8"); }
        const auto characters = std::ranges::count_if(post.title, [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (post.title.find_first_not_of(" \t\r\n") == post.title.npos || characters > 100 || post.title.find_first_of("<>") != post.title.npos)
            fail(Error::InvalidRequest, "YouTube requires a title of at most 100 characters without angle brackets");
        if (description.size() > 5000 || description.find_first_of("<>") != description.npos) fail(Error::InvalidRequest, "YouTube description exceeds its supported shape");
        const auto& category = post.youtube.categoryId;
        if (category.empty() || category.size() > 10 || !std::ranges::all_of(category, [](unsigned char c) { return c >= '0' && c <= '9'; }))
            fail(Error::InvalidRequest, "YouTube requires a numeric video category ID");
    }
}

class BasePublisher : public Publisher {
    Platform value;
public:
    explicit BasePublisher(Platform platform) : value(platform) {}
    const Capability& capability() const noexcept override { return capabilityFor(value); }
    Outcome publish(const Post& post, const Account& account, Context& context) const final {
        Runner runner{account, context, {}};
        try { validate(value, post, account); return run(post, runner); }
        catch (const Failure& failure) {
            auto result = failure.outcome;
            if (runner.publicationAcknowledged && result.postId.empty()) {
                result.error = Error::OutcomeUnknown;
                result.message = "Publication was acknowledged but its result is incomplete; reconcile before retrying";
            }
            return result;
        }
        catch (...) {
            Outcome result; result.error = runner.publicationAcknowledged ? Error::OutcomeUnknown : Error::InvalidResponse;
            result.message = "Publishing could not complete; reconcile any acknowledged publication before retrying";
            return result;
        }
    }
    virtual Outcome run(const Post&, Runner&) const = 0;
};

class XPublisher final : public BasePublisher {
public:
    XPublisher() : BasePublisher(Platform::X) {}
    Outcome run(const Post& post, Runner& r) const override {
        Json ids = Json::array();
        for (auto& media : post.media) {
            auto bytes = size(media);
            auto init = r.json("POST", "https://api.x.com/2/media/upload/initialize", {{"media_type", media.mimeType}, {"total_bytes", bytes}, {"media_category", media.kind == MediaKind::Video ? "tweet_video" : media.mimeType == "image/gif" ? "tweet_gif" : "tweet_image"}});
            auto resource = id(init.at("data"));
            for (std::uint64_t offset = 0, segment = 0; offset < bytes; offset += chunkSize, ++segment)
                r.multipart("https://api.x.com/2/media/upload/" + encode(resource) + "/append", {field("segment_index", std::to_string(segment)), file("media", media, offset, std::min(chunkSize, bytes - offset))});
            auto data = r.json("POST", "https://api.x.com/2/media/upload/" + encode(resource) + "/finalize").at("data");
            for (unsigned attempt = 0; data.contains("processing_info"); ++attempt) {
                auto info = data.at("processing_info");
                auto state = str(info, "state");
                if (state == "succeeded") break;
                if (state == "failed") fail(Error::ProcessingFailed, "X rejected media processing");
                if (state != "pending" && state != "in_progress") fail(Error::InvalidResponse, "X returned an unknown media processing state");
                if (attempt >= r.context.maxPolls) fail(Error::ProcessingTimeout, "X media processing timed out");
                auto seconds = info.value("check_after_secs", 1u);
                r.pause(std::chrono::seconds(std::min(seconds, 300u)));
                data = r.json("GET", "https://api.x.com/2/media/upload?command=STATUS&media_id=" + encode(resource)).at("data");
            }
            if (!media.altText.empty()) r.json("POST", "https://api.x.com/2/media/metadata", {{"media_id", resource}, {"alt_text", {{"text", media.altText}}}}, false, false, true);
            ids.push_back(resource);
        }
        Json body{{"text", text(post)}};
        if (!ids.empty()) body["media"] = {{"media_ids", ids}};
        if (post.madeWithAi) body["made_with_ai"] = true;
        if (!post.replyToId.empty()) body["reply"] = {{"in_reply_to_tweet_id", post.replyToId}};
        if (!post.replyControl.empty()) body["reply_settings"] = post.replyControl;
        auto resource = id(r.json("POST", "https://api.x.com/2/tweets", body, true).at("data"));
        return r.published(resource, "https://x.com/i/status/" + encode(resource));
    }
};

class FacebookPublisher final : public BasePublisher {
public:
    FacebookPublisher() : BasePublisher(Platform::Facebook) {}
    Outcome run(const Post& post, Runner& r) const override {
        const auto base = "https://graph.facebook.com/" + r.account.apiVersion + "/";
        auto target = encode(r.account.targetId);
        if (!post.media.empty() && post.media.front().kind == MediaKind::Video) {
            const auto& media = post.media.front();
            std::vector<Part> parts{field("description", text(post)), field("title", post.title)};
            if (!media.localFile.empty()) parts.push_back(file("source", media));
            else parts.push_back(field("file_url", media.publicUrl));
            auto resource = id(r.multipart("https://graph-video.facebook.com/" + r.account.apiVersion + "/" + target + "/videos", std::move(parts), true));
            // A returned ID acknowledges creation. Transcoding is asynchronous;
            // do not repeat this request if a subsequent read cannot finish.
            try {
                r.poll([&] {
                    auto data = r.json("GET", base + encode(resource) + "?fields=status");
                    auto state = str(data.at("status"), "video_status");
                    if (state == "error") fail(Error::ProcessingFailed, "Facebook video processing failed");
                    if (state != "ready" && state != "processing") fail(Error::InvalidResponse, "Unknown Facebook video processing state");
                    return std::pair{state == "ready", data};
                });
            } catch (Failure& failure) { failure.outcome.postId = resource; throw; }
            return r.published(resource, "https://www.facebook.com/" + encode(resource));
        }
        Json attachments = Json::array();
        for (auto& media : post.media) {
            std::vector<Part> parts{field("published", "false")};
            if (!media.altText.empty()) parts.push_back(field("alt_text_custom", media.altText));
            if (!media.localFile.empty()) parts.push_back(file("source", media));
            else parts.push_back(field("url", media.publicUrl));
            attachments.push_back({{"media_fbid", id(r.multipart(base + target + "/photos", std::move(parts)))}});
        }
        Json body{{"message", text(post, false)}};
        if (!post.link.empty()) body["link"] = post.link;
        if (!attachments.empty()) body["attached_media"] = attachments;
        auto resource = id(r.json("POST", base + target + "/feed", body, true));
        return r.published(resource, "https://www.facebook.com/" + encode(resource));
    }
};

class ContainerPublisher : public BasePublisher {
    bool instagram;
public:
    explicit ContainerPublisher(bool ig) : BasePublisher(ig ? Platform::Instagram : Platform::Threads), instagram(ig) {}
    Outcome run(const Post& post, Runner& r) const override {
        auto base = instagram ? "https://graph.instagram.com/" + r.account.apiVersion + "/" : "https://graph.threads.net/v1.0/";
        auto target = encode(r.account.targetId);
        auto edge = instagram ? "/media" : "/threads";
        auto wait = [&](const std::string& resource) {
            r.poll([&] {
                auto data = r.json("GET", base + encode(resource) + (instagram ? "?fields=status_code" : "?fields=status,error_message"));
                auto status = str(data, instagram ? "status_code" : "status");
                if (status == "ERROR" || status == "EXPIRED") fail(Error::ProcessingFailed, "Media container processing failed");
                if (status != "FINISHED" && status != "IN_PROGRESS") fail(Error::InvalidResponse, "Unknown media container state");
                return std::pair{status == "FINISHED", data};
            });
        };
        Json children = Json::array();
        const bool carousel = post.media.size() > 1;
        for (auto& media : post.media) {
            Json body;
            body[media.kind == MediaKind::Video ? "video_url" : "image_url"] = media.publicUrl;
            if (media.kind == MediaKind::Video) body["media_type"] = instagram && !carousel ? "REELS" : "VIDEO";
            else if (!instagram) body["media_type"] = "IMAGE";
            if (carousel) body["is_carousel_item"] = true;
            else body[instagram ? "caption" : "text"] = text(post);
            if (!instagram && !carousel && !post.replyToId.empty()) body["reply_to_id"] = post.replyToId;
            if (!instagram && !carousel && !post.replyControl.empty()) body["reply_control"] = post.replyControl;
            if (!media.altText.empty() && (!instagram || media.kind == MediaKind::Image)) body["alt_text"] = media.altText;
            auto resource = id(r.form(base + target + edge, body));
            wait(resource); children.push_back(resource);
        }
        std::string resource;
        if (carousel) {
            Json body{{"media_type", "CAROUSEL"}, {instagram ? "caption" : "text", text(post)}};
            if (!instagram && !post.replyToId.empty()) body["reply_to_id"] = post.replyToId;
            if (!instagram && !post.replyControl.empty()) body["reply_control"] = post.replyControl;
            // Graph expects a comma-separated child ID list in form encoding.
            std::string joined;
            for (auto& child : children) { if (!joined.empty()) joined += ','; joined += child.get<std::string>(); }
            body["children"] = joined;
            resource = id(r.form(base + target + edge, body)); wait(resource);
        } else if (!children.empty()) resource = children[0].get<std::string>();
        else {
            Json body{{"media_type", "TEXT"}, {"text", text(post)}};
            if (!post.replyToId.empty()) body["reply_to_id"] = post.replyToId;
            if (!post.replyControl.empty()) body["reply_control"] = post.replyControl;
            resource = id(r.form(base + target + edge, body));
        }
        if (post.media.empty()) wait(resource);
        auto result = id(r.form(base + target + (instagram ? "/media_publish" : "/threads_publish"), {{"creation_id", resource}}, true));
        return r.published(result);
    }
};
class InstagramPublisher final : public ContainerPublisher { public: InstagramPublisher() : ContainerPublisher(true) {} };
class ThreadsPublisher final : public ContainerPublisher { public: ThreadsPublisher() : ContainerPublisher(false) {} };

class PinterestPublisher final : public BasePublisher {
public:
    PinterestPublisher() : BasePublisher(Platform::Pinterest) {}
    Outcome run(const Post& post, Runner& r) const override {
        const auto& media = post.media.front();
        Json source;
        if (media.kind == MediaKind::Image) {
            if (!media.localFile.empty()) source = {{"source_type", "image_base64"}, {"content_type", media.mimeType}, {"data", base64File(media)}};
            else source = {{"source_type", "image_url"}, {"url", media.publicUrl}};
        } else {
            auto init = r.json("POST", "https://api.pinterest.com/v5/media", {{"media_type", "video"}});
            auto resource = id(init, "media_id");
            auto upload = str(init, "upload_url");
            if (!https(upload)) fail(Error::InvalidResponse, "Pinterest returned an invalid HTTPS upload URL");
            std::vector<Part> parts;
            for (auto& [key, value] : init.at("upload_parameters").items()) {
                if (!value.is_string() || !clean(key)) fail(Error::InvalidResponse, "Invalid Pinterest upload parameters");
                parts.push_back(field(key, value.get<std::string>()));
            }
            parts.push_back(file("file", media));
            r.multipart(upload, std::move(parts), false, false);
            r.poll([&] {
                auto data = r.json("GET", "https://api.pinterest.com/v5/media/" + encode(resource));
                auto state = str(data, "status");
                if (state == "failed") fail(Error::ProcessingFailed, "Pinterest media processing failed");
                if (state != "succeeded" && state != "registered" && state != "processed" && state != "processing") fail(Error::InvalidResponse, "Unknown Pinterest media processing state");
                return std::pair{state == "succeeded", data};
            });
            source = {{"source_type", "video_id"}, {"media_id", resource}, {"cover_image_url", post.coverImageUrl}};
        }
        Json body{{"board_id", r.account.targetId}, {"title", post.title}, {"description", text(post, false)}, {"alt_text", media.altText}, {"media_source", source}};
        if (!post.link.empty()) body["link"] = post.link;
        auto resource = id(r.json("POST", "https://api.pinterest.com/v5/pins", body, true));
        return r.published(resource, "https://www.pinterest.com/pin/" + encode(resource) + "/");
    }
};

class TumblrPublisher final : public BasePublisher {
public:
    TumblrPublisher() : BasePublisher(Platform::Tumblr) {}
    Outcome run(const Post& post, Runner& r) const override {
        Json content = Json::array();
        if (!post.title.empty()) content.push_back({{"type", "text"}, {"subtype", "heading1"}, {"text", post.title}});
        if (!text(post, true, false).empty()) content.push_back({{"type", "text"}, {"text", text(post, true, false)}});
        std::vector<Part> parts;
        for (auto& media : post.media) {
            Json item{{"type", media.mimeType}};
            if (!media.localFile.empty()) {
                auto name = "media_" + std::to_string(parts.size());
                item["identifier"] = name; parts.push_back(file(name, media));
            } else item["url"] = media.publicUrl;
            if (media.kind == MediaKind::Image) content.push_back({{"type", "image"}, {"media", Json::array({item})}, {"alt_text", media.altText}});
            else content.push_back({{"type", "video"}, {"media", item}});
        }
        std::string tags;
        for (auto& tag : post.tags) { if (!tags.empty()) tags += ','; tags += tag; }
        Json body{{"content", content}, {"state", "published"}, {"tags", tags}};
        auto url = "https://api.tumblr.com/v2/blog/" + encode(r.account.targetId) + "/posts";
        Json data;
        if (parts.empty()) data = r.json("POST", url, body, true);
        else {
            parts.insert(parts.begin(), Part{"json", body.dump(), "application/json", {}});
            data = r.multipart(url, std::move(parts), true);
        }
        return r.published(id(data.at("response")));
    }
};

class MastodonPublisher final : public BasePublisher {
public:
    MastodonPublisher() : BasePublisher(Platform::Mastodon) {}
    Outcome run(const Post& post, Runner& r) const override {
        auto base = r.account.instanceUrl;
        // Instance-specific limits are authoritative, rather than mastodon.social
        // defaults being imposed on every federated server.
        auto instance = r.json("GET", base + "/api/v2/instance");
        auto limits = instance.at("configuration").at("statuses");
        if (post.media.size() > limits.at("max_media_attachments").get<unsigned>()) fail(Error::InvalidRequest, "Too many media attachments for this Mastodon instance");
        Json ids = Json::array();
        for (auto& media : post.media) {
            auto resource = id(r.multipart(base + "/api/v2/media", {file("file", media), field("description", media.altText)}));
            if (r.last.status == 202) {
                r.poll([&] {
                    Request request; request.url = base + "/api/v1/media/" + encode(resource); request.headers = r.headers();
                    auto data = r.send(std::move(request), false, true);
                    return std::pair{r.last.status != 206 && !data.value("url", Json{}).is_null(), data};
                });
            }
            ids.push_back(resource);
        }
        Json body{{"status", text(post)}, {"media_ids", ids}, {"visibility", post.visibility}, {"sensitive", post.sensitive}, {"spoiler_text", post.contentWarning}};
        if (!post.replyToId.empty()) body["in_reply_to_id"] = post.replyToId;
        if (!post.language.empty()) body["language"] = post.language;
        Request request; request.method = "POST"; request.url = base + "/api/v1/statuses"; request.headers = r.headers();
        request.headers.emplace_back("Content-Type", "application/json"); request.body = body.dump();
        if (!post.idempotencyKey.empty()) request.headers.emplace_back("Idempotency-Key", post.idempotencyKey);
        auto data = r.send(std::move(request), true);
        return r.published(id(data), str(data, "url"));
    }
};

class LinkedInPublisher final : public BasePublisher {
public:
    LinkedInPublisher() : BasePublisher(Platform::LinkedIn) {}
    Outcome run(const Post& post, Runner& r) const override {
        Json mediaIds = Json::array();
        auto put = [&](const std::string& url, const Media& media, std::uint64_t offset = 0, std::optional<std::uint64_t> length = {}) {
            if (!linkedInUpload(url)) fail(Error::InvalidResponse, "LinkedIn returned an upload URL outside its media hosts");
            Request request; request.method = "PUT"; request.url = url;
            // Images require OAuth on PUT; video upload explicitly omits it.
            // Both use only the verified LinkedIn media hosts above.
            if (media.kind == MediaKind::Image) request.headers = r.headers();
            request.headers.emplace_back("Content-Type", "application/octet-stream");
            request.file = FileSlice{media.localFile, offset, length};
            r.send(std::move(request), false, true, true);
        };
        for (auto& media : post.media) {
            std::string resource, kind = media.kind == MediaKind::Image ? "images" : "videos";
            if (media.kind == MediaKind::Image) {
                auto init = r.json("POST", "https://api.linkedin.com/rest/images?action=initializeUpload", {{"initializeUploadRequest", {{"owner", r.account.targetId}}}}, false, true).at("value");
                resource = id(init, "image"); put(str(init, "uploadUrl"), media);
            } else {
                auto init = r.json("POST", "https://api.linkedin.com/rest/videos?action=initializeUpload", {{"initializeUploadRequest", {{"owner", r.account.targetId}, {"fileSizeBytes", size(media)}, {"uploadCaptions", false}, {"uploadThumbnail", false}}}}, false, true).at("value");
                resource = id(init, "video");
                Json uploaded = Json::array();
                std::uint64_t expectedOffset = 0;
                for (auto& part : init.at("uploadInstructions")) {
                    auto first = part.at("firstByte").get<std::uint64_t>(), last = part.at("lastByte").get<std::uint64_t>();
                    if (first != expectedOffset || last < first || last >= size(media)) fail(Error::InvalidResponse, "LinkedIn returned invalid or overlapping upload ranges");
                    put(str(part, "uploadUrl"), media, first, last - first + 1);
                    auto etag = header(r.last.headers, "ETag");
                    if (etag.empty() || !clean(etag)) fail(Error::InvalidResponse, "LinkedIn upload part did not return an ETag");
                    if (etag.size() > 1 && etag.front() == '"' && etag.back() == '"') etag = etag.substr(1, etag.size() - 2);
                    uploaded.push_back(etag); expectedOffset = last + 1;
                }
                if (expectedOffset != size(media)) fail(Error::InvalidResponse, "LinkedIn upload ranges do not cover the video");
                r.json("POST", "https://api.linkedin.com/rest/videos?action=finalizeUpload", {{"finalizeUploadRequest", {{"video", resource}, {"uploadToken", str(init, "uploadToken")}, {"uploadedPartIds", uploaded}}}}, false, true, true);
            }
            const bool canReadImage = std::ranges::any_of(r.account.scopes, [](const std::string& scope) {
                return scope == "rw_ads" || scope == "w_organization_social" || scope == "w_power_creators";
            });
            // w_member_social is write-only for Images API. Requiring an image
            // GET would break ordinary member publishing permission.
            if (media.kind == MediaKind::Video || canReadImage) r.poll([&] {
                auto data = r.json("GET", "https://api.linkedin.com/rest/" + kind + "/" + encode(resource), {}, false, true);
                auto state = str(data, "status");
                if (state == "PROCESSING_FAILED" || state == "FAILED") fail(Error::ProcessingFailed, "LinkedIn media processing failed");
                if (state != "AVAILABLE" && state != "PROCESSING" && state != "WAITING_UPLOAD") fail(Error::InvalidResponse, "Unknown LinkedIn media processing state");
                return std::pair{state == "AVAILABLE", data};
            });
            mediaIds.push_back(resource);
        }
        Json body{{"author", r.account.targetId}, {"commentary", text(post)}, {"visibility", "PUBLIC"}, {"distribution", {{"feedDistribution", "MAIN_FEED"}, {"targetEntities", Json::array()}, {"thirdPartyDistributionChannels", Json::array()}}}, {"lifecycleState", "PUBLISHED"}, {"isReshareDisabledByAuthor", false}};
        if (mediaIds.size() == 1) {
            body["content"] = {{"media", {{"id", mediaIds[0]}, {"title", post.title}}}};
            if (!post.media[0].altText.empty()) body["content"]["media"]["altText"] = post.media[0].altText;
        }
        else if (mediaIds.size() > 1) {
            Json images = Json::array();
            for (unsigned i = 0; i < mediaIds.size(); ++i) images.push_back({{"id", mediaIds[i]}, {"altText", post.media[i].altText}});
            body["content"] = {{"multiImage", {{"images", images}}}};
        }
        r.json("POST", "https://api.linkedin.com/rest/posts", body, true, true, true);
        auto resource = header(r.last.headers, "x-restli-id");
        if (resource.empty() || !clean(resource)) fail(Error::OutcomeUnknown, "LinkedIn acknowledged publication without a post ID; reconcile before retrying");
        return r.published(resource, "https://www.linkedin.com/feed/update/" + encode(resource) + "/");
    }
};
class YouTubePublisher final : public BasePublisher {
    static std::uint64_t acknowledged(const Headers& headers, std::uint64_t confirmed, std::uint64_t sent, std::uint64_t total) {
        const auto range = header(headers, "Range");
        std::uint64_t next = 0;
        if (!range.empty()) {
            constexpr std::string_view prefix = "bytes=0-";
            if (!range.starts_with(prefix)) fail(Error::OutcomeUnknown, "YouTube returned an invalid upload acknowledgement; reconcile before retrying");
            const auto digits = std::string_view(range).substr(prefix.size());
            std::uint64_t last = 0;
            auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), last);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || last >= total)
                fail(Error::OutcomeUnknown, "YouTube returned an invalid upload acknowledgement; reconcile before retrying");
            next = last + 1;
        }
        if (next < confirmed || next > sent || next == total) fail(Error::OutcomeUnknown, "YouTube upload progress is inconsistent; reconcile before retrying");
        return next;
    }
public:
    YouTubePublisher() : BasePublisher(Platform::YouTube) {}
    Outcome run(const Post& post, Runner& r) const override {
        const auto& media = post.media.front();
        const auto total = size(media);
        Json metadata{{"snippet", {{"title", post.title}, {"description", text(post, true, false)}, {"tags", post.tags}, {"categoryId", post.youtube.categoryId}}},
                      {"status", {{"privacyStatus", post.visibility}}}};
        if (post.youtube.madeForKids) metadata["status"]["selfDeclaredMadeForKids"] = *post.youtube.madeForKids;
        if (post.youtube.containsSyntheticMedia) metadata["status"]["containsSyntheticMedia"] = *post.youtube.containsSyntheticMedia;
        Request start; start.method = "POST";
        start.url = std::string("https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet%2Cstatus&notifySubscribers=") + (post.youtube.notifySubscribers ? "true" : "false");
        start.headers = r.headers();
        start.headers.emplace_back("Content-Type", "application/json; charset=UTF-8");
        start.headers.emplace_back("X-Upload-Content-Type", media.mimeType);
        start.headers.emplace_back("X-Upload-Content-Length", std::to_string(total));
        start.body = metadata.dump();
        r.send(std::move(start), false, true);
        const auto session = header(r.last.headers, "Location");
        constexpr std::string_view sessionPrefix = "https://www.googleapis.com/upload/youtube/v3/videos?";
        if (!https(session) || !clean(session) || !session.starts_with(sessionPrefix) || session.size() == sessionPrefix.size() || session.find('#') != session.npos)
            fail(Error::InvalidResponse, "YouTube returned an invalid resumable session URL");

        std::uint64_t offset = 0, sent = 0;
        unsigned recoveries = 0;
        unsigned retryDelay = 0;
        bool query = false, uncertain = false;
        while (true) {
            if (query) {
                if (r.context.stop.stop_requested() || recoveries == r.context.maxUploadRecoveries)
                    fail(Error::OutcomeUnknown, "YouTube upload recovery stopped; reconcile before retrying");
                const auto delay = std::chrono::seconds(std::max(retryDelay, 1u << std::min(recoveries, 6u)));
                ++recoveries;
                try { r.pause(delay); }
                catch (const Failure&) {
                    if (uncertain) fail(Error::OutcomeUnknown, "YouTube upload was interrupted; reconcile before retrying");
                    throw;
                }
            }
            Request request; request.method = "PUT"; request.url = session; request.headers = r.headers();
            request.headers.emplace_back("Content-Type", media.mimeType);
            if (query) {
                request.headers.emplace_back("Content-Length", "0");
                request.headers.emplace_back("Content-Range", "bytes */" + std::to_string(total));
            } else {
                const auto length = std::min(chunkSize, total - offset);
                sent = offset + length;
                request.file = FileSlice{media.localFile, offset, length};
                request.headers.emplace_back("Content-Length", std::to_string(length));
                request.headers.emplace_back("Content-Range", "bytes " + std::to_string(offset) + "-" + std::to_string(sent - 1) + "/" + std::to_string(total));
            }
            Json data;
            try { data = r.send(std::move(request), true, false, false, true); }
            catch (const Failure& failure) {
                const auto error = failure.outcome.error;
                if ((error != Error::TransportFailure && error != Error::OutcomeUnknown) || r.context.stop.stop_requested() || recoveries == r.context.maxUploadRecoveries || failure.outcome.retryAfterSeconds > 300) {
                    if (query && uncertain) {
                        auto outcome = failure.outcome;
                        outcome.error = Error::OutcomeUnknown;
                        outcome.message = "YouTube upload status could not be reconciled; reconcile before retrying";
                        throw Failure{std::move(outcome)};
                    }
                    throw;
                }
                // A status query is safe; creating a second upload is not.
                uncertain = true; query = true; retryDelay = failure.outcome.retryAfterSeconds; continue;
            }
            if (r.last.status == 308) {
                const auto next = acknowledged(r.last.headers, offset, sent, total);
                // No progress requires a bounded status query before resending.
                query = !query && next == offset;
                offset = next; uncertain = false; retryDelay = 0;
                const auto retry = header(r.last.headers, "Retry-After");
                unsigned seconds = 0;
                auto parsed = std::from_chars(retry.data(), retry.data() + retry.size(), seconds);
                if (parsed.ec == std::errc{} && parsed.ptr == retry.data() + retry.size() && seconds) {
                    if (seconds > 300) {
                        Outcome result; result.error = Error::RateLimited; result.httpStatus = 308;
                        result.retryAfterSeconds = seconds; result.message = "YouTube requested an upload delay beyond the wait budget";
                        return result;
                    }
                    r.pause(std::chrono::seconds(seconds));
                }
                continue;
            }
            if (sent != total) fail(Error::OutcomeUnknown, "YouTube completed before all video bytes were sent; reconcile before retrying");
            auto result = r.published(id(data));
            result.url = "https://www.youtube.com/watch?v=" + encode(result.postId);
            const auto status = data.value("status", Json::object());
            const auto state = str(status, "uploadStatus");
            if (state == "failed" || state == "rejected" || state == "deleted") {
                result.error = Error::ProcessingFailed; result.message = "YouTube rejected the uploaded video";
            }
            return result;
        }
    }
};
class UnsupportedPublisher final : public BasePublisher {
public:
    explicit UnsupportedPublisher(Platform value) : BasePublisher(value) {}
    Outcome run(const Post&, Runner&) const override {
        Outcome result; result.error = Error::Unsupported; result.message = "No verified official public publishing API"; return result;
    }
};
} // namespace

const std::vector<Capability>& capabilities() {
    static const std::vector<Capability> rows{
        {Platform::X, "x", "X", true, true, true, true, false, {"tweet.read", "tweet.write", "users.read"}, "https://docs.x.com/x-api/posts/create-post", "OAuth user context; media.write for uploads; provisioned paid API access"},
        {Platform::Facebook, "facebook", "Facebook", true, true, true, true, false, {"pages_manage_posts", "pages_read_engagement"}, "https://developers.facebook.com/docs/pages-api/posts/", "Page access token and Page ID; personal profile publishing is unsupported"},
        {Platform::Instagram, "instagram", "Instagram", true, false, true, true, true, {"instagram_business_basic", "instagram_business_content_publish"}, "https://developers.facebook.com/docs/instagram-platform/instagram-api-with-instagram-login/content-publishing/", "Professional account via Instagram Login; public HTTPS media URL"},
        {Platform::Threads, "threads", "Threads", true, true, true, true, true, {"threads_basic", "threads_content_publish"}, "https://developers.facebook.com/docs/threads/posts/", "Threads OAuth user and publishing permissions; public HTTPS media URL"},
        {Platform::Pinterest, "pinterest", "Pinterest", true, false, true, true, false, {"pins:write", "boards:read"}, "https://developers.pinterest.com/docs/work-with-organic-content-and-users/create-boards-and-pins/", "Approved app access and destination board; video requires a cover image"},
        {Platform::Pixiv, "pixiv", "Pixiv", false, false, false, false, false, {}, "https://www.pixiv.help/hc/en-us", "No official public publishing contract verified; no private/mobile API or browser automation"},
        {Platform::ArtStation, "artstation", "ArtStation", false, false, false, false, false, {}, "https://help.artstation.com/", "No official public publishing contract verified; no scraping or private API"},
        {Platform::Tumblr, "tumblr", "Tumblr", true, true, true, true, false, {"write"}, "https://github.com/tumblr/docs/blob/master/api.md#posts---createreblog-a-post-neue-post-format", "OAuth2 write token, blog identifier, NPF posts and multipart media"},
        {Platform::Mastodon, "mastodon", "Mastodon", true, true, true, true, false, {"write:statuses", "write:media"}, "https://docs.joinmastodon.org/methods/statuses/", "Per-instance OAuth registration and user token; instance-specific attachment limits"},
        {Platform::LinkedIn, "linkedin", "LinkedIn", true, true, true, true, false, {"w_member_social"}, "https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/posts-api", "Member author or authorized organization with w_organization_social; supported YYYYMM API version"},
        {Platform::YouTube, "youtube", "YouTube", true, false, false, true, false, {"https://www.googleapis.com/auth/youtube.upload"}, "https://developers.google.com/youtube/data/reference/rest/v3/videos/insert", "OAuth channel user token; one local video and title; resumable upload; public, unlisted or private visibility"}
    };
    return rows;
}
std::optional<Platform> platform(std::string_view value) {
    if (value == "twitter") value = "x";
    if (value == "tum blr") value = "tumblr";
    for (auto& cap : capabilities()) if (cap.id == value) return cap.platform;
    return {};
}
std::string_view errorName(Error error) noexcept {
    switch (error) {
    case Error::None: return "none";
    case Error::Unsupported: return "unsupported";
    case Error::InvalidRequest: return "invalid_request";
    case Error::AuthenticationRequired: return "authentication_required";
    case Error::PermissionDenied: return "permission_denied";
    case Error::AccessNotConfigured: return "access_not_configured";
    case Error::RateLimited: return "rate_limited";
    case Error::ProviderRejected: return "provider_rejected";
    case Error::TransportFailure: return "transport_failure";
    case Error::InvalidResponse: return "invalid_response";
    case Error::ProcessingFailed: return "processing_failed";
    case Error::ProcessingTimeout: return "processing_timeout";
    case Error::Cancelled: return "cancelled";
    case Error::OutcomeUnknown: return "outcome_unknown";
    }
    return "invalid_response";
}
std::unique_ptr<Publisher> makePublisher(Platform value) {
    switch (value) {
    case Platform::X: return std::make_unique<XPublisher>();
    case Platform::Facebook: return std::make_unique<FacebookPublisher>();
    case Platform::Instagram: return std::make_unique<InstagramPublisher>();
    case Platform::Threads: return std::make_unique<ThreadsPublisher>();
    case Platform::Pinterest: return std::make_unique<PinterestPublisher>();
    case Platform::Tumblr: return std::make_unique<TumblrPublisher>();
    case Platform::Mastodon: return std::make_unique<MastodonPublisher>();
    case Platform::LinkedIn: return std::make_unique<LinkedInPublisher>();
    case Platform::YouTube: return std::make_unique<YouTubePublisher>();
    case Platform::Pixiv: case Platform::ArtStation: return std::make_unique<UnsupportedPublisher>(value);
    }
    throw std::invalid_argument("Unknown publishing platform");
}
Outcome validatePost(Platform value, const Post& post, const Account& account) {
    try { validate(value, post, account); return {}; }
    catch (const Failure& failure) { return failure.outcome; }
    catch (...) { Outcome out; out.error = Error::InvalidRequest; out.message = "Invalid publication input"; return out; }
}
} // namespace iiSocialIntegration
