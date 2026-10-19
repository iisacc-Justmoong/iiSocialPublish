#include <iiSocialPublish/Publishing.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace iiSocialPublish;
using Json = nlohmann::json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Step {
    std::string method, url;
    Response response;
    std::function<void(const Request&)> inspect;
};
struct Fake final : Transport {
    std::deque<Step> steps;
    unsigned requests = 0;
    void add(std::string method, std::string url, Json body, int status = 200, std::function<void(const Request&)> inspect = {}, Headers headers = {}) {
        steps.push_back({std::move(method), std::move(url), {status, std::move(headers), body.is_null() ? "" : body.dump()}, std::move(inspect)});
    }
    Response send(const Request& request, std::stop_token) override {
        ++requests;
        check(!steps.empty(), "Unexpected network request");
        auto step = std::move(steps.front()); steps.pop_front();
        check(request.method == step.method, "HTTP method mismatch");
        if (request.url != step.url) throw std::runtime_error("URL mismatch: " + request.url + " expected " + step.url);
        if (step.inspect) step.inspect(request);
        return step.response;
    }
};
std::string header(const Request& r, const std::string& name) {
    for (auto& [key, value] : r.headers) if (key == name) return value;
    return {};
}
struct Fixture {
    std::filesystem::path path;
    Fixture() {
        auto root = std::filesystem::current_path() / "fixtures";
        std::filesystem::create_directories(root);
        path = root / ("canvas export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
        std::ofstream(path, std::ios::binary).write("abcdEFGH", 8);
    }
    Media image() const { return {MediaKind::Image, path, {}, "image/png", "Image description"}; }
    Media video() const { return {MediaKind::Video, path, {}, "video/mp4", "Video description"}; }
};
Account account(Platform value) {
    Account a; a.accessToken = "fixture-user-token"; a.targetId = "123";
    for (auto& cap : capabilities()) if (cap.platform == value) a.scopes = cap.requiredScopes;
    a.scopes.push_back("media.write"); a.allowBillableRequests = true;
    a.instanceUrl = "https://federated.example";
    a.apiVersion = value == Platform::LinkedIn ? "202607" : "v25.0";
    if (value == Platform::LinkedIn) a.targetId = "urn:li:person:member";
    if (value == Platform::Tumblr) a.targetId = "t:blog-uuid";
    return a;
}
Outcome publish(Platform value, Fake& fake, const Post& post, const Account& a) {
    Context context{fake, {}, 3, std::chrono::milliseconds(0), [](auto, auto stop) { return !stop.stop_requested(); }};
    auto result = makePublisher(value)->publish(post, a, context);
    if (result.error == Error::InvalidResponse) std::cerr << "invalid response: " << result.message << '\n';
    return result;
}
Outcome publish(Platform value, Fake& fake, const Post& post) { return publish(value, fake, post, account(value)); }
void done(const Fake& fake, const Outcome& result) {
    if (!result.published()) throw std::runtime_error(std::string(errorName(result.error)) + ": " + result.message);
    check(fake.steps.empty(), "Not all expected API steps executed");
}
void catalog() {
    check(capabilities().size() == 10, "Exactly ten platform objects required");
    unsigned supported = 0;
    for (auto& cap : capabilities()) {
        check(platform(cap.id) == cap.platform, "Platform catalog does not round-trip");
        check(makePublisher(cap.platform)->capability().id == cap.id, "Factory returns wrong publisher");
        supported += cap.officialPublishingApi;
    }
    check(supported == 8, "Only verified official publishing APIs may be enabled");
    check(platform("tum blr") == Platform::Tumblr && !platform("unknown"), "Aliases or unknown platforms broken");
    Fake fake; Post p; p.text = "fixture";
    for (auto value : {Platform::Pixiv, Platform::ArtStation}) check(publish(value, fake, p).error == Error::Unsupported, "Unverified API must fail before networking");
    check(fake.requests == 0, "Unsupported API made a request");
}
void validation() {
    Fixture f; Fake fake; Post p; p.text = "fixture";
    auto a = account(Platform::X); a.accessToken.clear();
    check(publish(Platform::X, fake, p, a).error == Error::AuthenticationRequired, "Missing credentials accepted");
    a = account(Platform::X); a.accessToken += '\n';
    check(publish(Platform::X, fake, p, a).error == Error::AuthenticationRequired, "Header injection accepted");
    a = account(Platform::X); a.allowBillableRequests = false;
    check(publish(Platform::X, fake, p, a).error == Error::AccessNotConfigured, "Paid API silently enabled");
    a = account(Platform::X); a.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(1);
    check(publish(Platform::X, fake, p, a).error == Error::AuthenticationRequired, "Expired grant accepted");
    a = account(Platform::X); a.scopes.clear();
    check(publish(Platform::X, fake, p, a).error == Error::PermissionDenied, "Missing scope accepted");
    p.media = {f.image()};
    check(publish(Platform::Instagram, fake, p).error == Error::InvalidRequest, "Instagram pretended local path was public");
    p.media[0].localFile = "relative.png";
    check(publish(Platform::X, fake, p).error == Error::InvalidRequest, "Relative file accepted");
    p.media[0] = f.video();
    check(publish(Platform::Pinterest, fake, p).error == Error::InvalidRequest, "Pinterest video missing cover accepted");
    a = account(Platform::Mastodon); a.instanceUrl = "https://user:password@evil.example/path";
    check(publish(Platform::Mastodon, fake, p, a).error == Error::InvalidRequest, "Mastodon accepted credential/path injection");
    a = account(Platform::Facebook); a.apiVersion.clear();
    check(publish(Platform::Facebook, fake, p, a).error == Error::InvalidRequest, "Missing API version accepted");
    check(fake.requests == 0, "Validation caused a partial upload");
}
void x() {
    Fixture f; Post p; p.text = "X fixture"; p.media = {f.video()}; p.madeWithAi = true;
    Fake fake;
    fake.add("POST", "https://api.x.com/2/media/upload/initialize", {{"data", {{"id", "m1"}}}}, 200, [](auto& r) { auto body = Json::parse(r.body); check(body["total_bytes"] == 8 && body["media_category"] == "tweet_video", "Wrong X initialization body"); });
    fake.add("POST", "https://api.x.com/2/media/upload/m1/append", nullptr, 204, [](auto& r) { check(r.parts.size() == 2 && r.parts[1].file->offset == 0 && r.parts[1].file->length == 8, "X did not stream a bounded chunk"); });
    fake.add("POST", "https://api.x.com/2/media/upload/m1/finalize", {{"data", {{"processing_info", {{"state", "pending"}, {"check_after_secs", 1}}}}}});
    fake.add("GET", "https://api.x.com/2/media/upload?command=STATUS&media_id=m1", {{"data", {{"processing_info", {{"state", "succeeded"}}}}}});
    fake.add("POST", "https://api.x.com/2/media/metadata", nullptr, 204);
    fake.add("POST", "https://api.x.com/2/tweets", {{"data", {{"id", "post-x"}}}}, 201, [](auto& r) { auto body = Json::parse(r.body); check(body["media"]["media_ids"][0] == "m1" && body["made_with_ai"] == true, "X publish lost media or AI disclosure"); });
    done(fake, publish(Platform::X, fake, p));
}
void facebook() {
    Fixture f; Post p; p.text = "Photo album"; p.media = {f.image(), f.image()}; Fake fake;
    for (auto id : {"photo1", "photo2"}) fake.add("POST", "https://graph.facebook.com/v25.0/123/photos", {{"id", id}}, 200, [](auto& r) { check(r.parts[0].value == "false", "Facebook photo prematurely published"); });
    fake.add("POST", "https://graph.facebook.com/v25.0/123/feed", {{"id", "123_post"}}, 200, [](auto& r) { check(Json::parse(r.body)["attached_media"].size() == 2, "Photo album lost attachments"); });
    done(fake, publish(Platform::Facebook, fake, p));
    p.media = {f.video()};
    fake.add("POST", "https://graph-video.facebook.com/v25.0/123/videos", {{"id", "vid1"}});
    fake.add("GET", "https://graph.facebook.com/v25.0/vid1?fields=status", {{"status", {{"video_status", "processing"}}}});
    fake.add("GET", "https://graph.facebook.com/v25.0/vid1?fields=status", {{"status", {{"video_status", "ready"}}}});
    done(fake, publish(Platform::Facebook, fake, p));
}
void containers(bool ig) {
    Post p; p.text = "Container fixture";
    p.media = {{MediaKind::Image, {}, "https://media.example/a.png", "image/png", "Accessible"}, {MediaKind::Video, {}, "https://media.example/v.mp4", "video/mp4", "Video"}};
    const std::string base = ig ? "https://graph.instagram.com/v25.0/" : "https://graph.threads.net/v1.0/";
    const std::string edge = ig ? "/media" : "/threads";
    Fake fake;
    for (auto id : {"c1", "c2"}) {
        fake.add("POST", base + "123" + edge, {{"id", id}}, 200, [](auto& r) { check(r.body.find("is_carousel_item=true") != r.body.npos, "Carousel child flag missing"); check(r.body.find("access_token") == r.body.npos, "Token leaked into URL/form payload"); });
        fake.add("GET", base + id + (ig ? "?fields=status_code" : "?fields=status,error_message"), {{ig ? "status_code" : "status", "FINISHED"}});
    }
    fake.add("POST", base + "123" + edge, {{"id", "parent"}}, 200, [](auto& r) { check(r.body.find("children=c1%2Cc2") != r.body.npos, "Carousel not assembled"); });
    fake.add("GET", base + "parent" + (ig ? "?fields=status_code" : "?fields=status,error_message"), {{ig ? "status_code" : "status", "FINISHED"}});
    fake.add("POST", base + "123" + (ig ? "/media_publish" : "/threads_publish"), {{"id", "published"}});
    done(fake, publish(ig ? Platform::Instagram : Platform::Threads, fake, p));
    if (!ig) {
        p.media.clear();
        fake.add("POST", base + "123/threads", {{"id", "text-container"}});
        fake.add("GET", base + "text-container?fields=status,error_message", {{"status", "FINISHED"}});
        fake.add("POST", base + "123/threads_publish", {{"id", "text-post"}});
        done(fake, publish(Platform::Threads, fake, p));
    }
}
void pinterest() {
    Fixture f; Fake fake; Post p; p.text = "Pin"; p.title = "Title"; p.media = {f.image()};
    fake.add("POST", "https://api.pinterest.com/v5/pins", {{"id", "pin1"}}, 201, [](auto& r) { auto body = Json::parse(r.body); check(body["board_id"] == "123" && body["media_source"]["data"] == "YWJjZEVGR0g=", "Pinterest base64/board payload mismatch"); });
    done(fake, publish(Platform::Pinterest, fake, p));
    p.media = {f.video()}; p.coverImageUrl = "https://media.example/cover.png";
    fake.add("POST", "https://api.pinterest.com/v5/media", {{"media_id", "pin-media"}, {"upload_url", "https://uploads.example/upload"}, {"upload_parameters", {{"key", "provider-generated-key"}}}});
    fake.add("POST", "https://uploads.example/upload", nullptr, 204, [](auto& r) { check(r.headers.empty() && r.parts.back().file.has_value(), "OAuth token leaked to signed object storage or video not streamed"); });
    fake.add("GET", "https://api.pinterest.com/v5/media/pin-media", {{"status", "processed"}});
    fake.add("GET", "https://api.pinterest.com/v5/media/pin-media", {{"status", "succeeded"}});
    fake.add("POST", "https://api.pinterest.com/v5/pins", {{"id", "pin-video"}}, 201, [](auto& r) { auto source = Json::parse(r.body)["media_source"]; check(source["source_type"] == "video_id" && source["cover_image_url"] == "https://media.example/cover.png", "Video Pin missing registered media or cover"); });
    done(fake, publish(Platform::Pinterest, fake, p));
}
void tumblr() {
    Fixture f; Fake fake; Post p; p.title = "Title"; p.text = "NPF"; p.media = {f.image(), f.video()}; p.tags = {"art", "dreamscapes"};
    fake.add("POST", "https://api.tumblr.com/v2/blog/t%3Ablog-uuid/posts", {{"meta", {{"status", 201}}}, {"response", {{"id", "tumblr-post"}}}}, 201, [](auto& r) {
        check(r.parts.size() == 3 && r.parts[0].name == "json", "NPF multipart must start with JSON");
        auto body = Json::parse(r.parts[0].value);
        check(body["content"][2]["media"][0]["identifier"] == r.parts[1].name, "Image NPF identifier mismatch");
        check(body["content"][3]["media"]["identifier"] == r.parts[2].name, "Video NPF identifier mismatch");
        check(body["tags"] == "art,dreamscapes", "Tumblr tags lost");
    });
    done(fake, publish(Platform::Tumblr, fake, p));
}
void mastodon() {
    Fixture f; Fake fake; Post p; p.text = "Fediverse"; p.media = {f.image()}; p.visibility = "unlisted"; p.sensitive = true; p.contentWarning = "Warning"; p.idempotencyKey = "stable-key";
    fake.add("GET", "https://federated.example/api/v2/instance", {{"configuration", {{"statuses", {{"max_media_attachments", 4}}}}}});
    fake.add("POST", "https://federated.example/api/v2/media", {{"id", "media1"}, {"url", nullptr}}, 202);
    fake.add("GET", "https://federated.example/api/v1/media/media1", nullptr, 206);
    fake.add("GET", "https://federated.example/api/v1/media/media1", {{"id", "media1"}, {"url", "https://federated.example/media/1"}});
    fake.add("POST", "https://federated.example/api/v1/statuses", {{"id", "mastodon-post"}, {"url", "https://federated.example/@me/1"}}, 200, [](auto& r) { auto body = Json::parse(r.body); check(header(r, "Idempotency-Key") == "stable-key" && body["visibility"] == "unlisted" && body["sensitive"] == true, "Mastodon lost privacy/idempotency options"); });
    done(fake, publish(Platform::Mastodon, fake, p));
}
void linkedin() {
    Fixture f; Fake fake; Post p; p.text = "LinkedIn"; p.media = {f.image(), f.image()};
    auto organization = account(Platform::LinkedIn);
    organization.targetId = "urn:li:organization:123";
    organization.scopes = {"w_organization_social"};
    for (auto name : {"one", "two"}) {
        fake.add("POST", "https://api.linkedin.com/rest/images?action=initializeUpload", {{"value", {{"image", std::string("urn:li:image:") + name}, {"uploadUrl", std::string("https://www.linkedin.com/upload/") + name}}}}, 200, [](auto& r) { check(header(r, "LinkedIn-Version") == "202607" && header(r, "X-Restli-Protocol-Version") == "2.0.0", "LinkedIn version headers missing"); });
        fake.add("PUT", std::string("https://www.linkedin.com/upload/") + name, nullptr, 201, [](auto& r) { check(r.file.has_value() && !header(r, "Authorization").empty(), "LinkedIn image PUT requires the OAuth header"); });
        fake.add("GET", std::string("https://api.linkedin.com/rest/images/urn%3Ali%3Aimage%3A") + name, {{"status", "AVAILABLE"}});
    }
    fake.add("POST", "https://api.linkedin.com/rest/posts", nullptr, 201, [](auto& r) { check(Json::parse(r.body)["content"]["multiImage"]["images"].size() == 2, "LinkedIn multi-image body invalid"); }, {{"x-restli-id", "urn:li:share:123"}});
    done(fake, publish(Platform::LinkedIn, fake, p, organization));
    p.media = {f.image()};
    fake.add("POST", "https://api.linkedin.com/rest/images?action=initializeUpload", {{"value", {{"image", "urn:li:image:member"}, {"uploadUrl", "https://www.linkedin.com/upload/member"}}}});
    fake.add("PUT", "https://www.linkedin.com/upload/member", nullptr, 201);
    fake.add("POST", "https://api.linkedin.com/rest/posts", nullptr, 201, [](auto& r) { check(Json::parse(r.body)["content"]["media"]["altText"] == "Image description", "LinkedIn single-image alt text lost"); }, {{"x-restli-id", "urn:li:share:member"}});
    done(fake, publish(Platform::LinkedIn, fake, p)); // No image GET with write-only member scope.
    p.media = {f.video()};
    fake.add("POST", "https://api.linkedin.com/rest/videos?action=initializeUpload", {{"value", {{"video", "urn:li:video:v1"}, {"uploadToken", "opaque-upload-token"}, {"uploadInstructions", Json::array({{{"firstByte", 0}, {"lastByte", 3}, {"uploadUrl", "https://www.linkedin.com/upload/a"}}, {{"firstByte", 4}, {"lastByte", 7}, {"uploadUrl", "https://www.linkedin.com/upload/b"}}})}}}});
    fake.add("PUT", "https://www.linkedin.com/upload/a", nullptr, 201, [](auto& r) { check(r.file->offset == 0 && r.file->length == 4 && header(r, "Authorization").empty(), "First LinkedIn byte range or video authorization incorrect"); }, {{"ETag", "\"part-a\""}});
    fake.add("PUT", "https://www.linkedin.com/upload/b", nullptr, 201, [](auto& r) { check(r.file->offset == 4 && r.file->length == 4, "Second LinkedIn byte range incorrect"); }, {{"etag", "\"part-b\""}});
    fake.add("POST", "https://api.linkedin.com/rest/videos?action=finalizeUpload", nullptr, 200, [](auto& r) { auto body = Json::parse(r.body)["finalizeUploadRequest"]; check(body["uploadedPartIds"] == Json::array({"part-a", "part-b"}), "LinkedIn ETags not preserved in order"); });
    fake.add("GET", "https://api.linkedin.com/rest/videos/urn%3Ali%3Avideo%3Av1", {{"status", "AVAILABLE"}});
    fake.add("POST", "https://api.linkedin.com/rest/posts", nullptr, 201, {}, {{"x-restli-id", "urn:li:share:456"}});
    done(fake, publish(Platform::LinkedIn, fake, p));
}
void errors() {
    Fake fake; Post p; p.text = "Fixture";
    fake.add("POST", "https://api.x.com/2/tweets", {{"detail", "secret fixture-user-token"}}, 429, {}, {{"Retry-After", "120"}});
    auto result = publish(Platform::X, fake, p);
    check(result.error == Error::RateLimited && result.retryAfterSeconds == 120 && fake.requests == 1, "Rate limits must surface without publication retry");
    check(result.message.find("token") == result.message.npos, "Provider text leaked a secret");
    fake.steps.push_back({"POST", "https://api.x.com/2/tweets", {0, {}, {}, true, false, true}, {}});
    result = publish(Platform::X, fake, p);
    check(result.requiresReconciliation() && fake.steps.empty(), "Ambiguous publication retried or falsely failed safely");
    fake.add("POST", "https://api.x.com/2/tweets", Json::object(), 201);
    check(publish(Platform::X, fake, p).error == Error::OutcomeUnknown, "Acknowledged publication without ID must be reconciled");
    fake.add("POST", "https://api.x.com/2/tweets", {{"message", "secret"}}, 503);
    check(publish(Platform::X, fake, p).error == Error::OutcomeUnknown, "Server failure can occur after publication");
    std::stop_source stop; stop.request_stop(); Context context{fake, stop.get_token()};
    auto before = fake.requests;
    check(makePublisher(Platform::X)->publish(p, account(Platform::X), context).error == Error::Cancelled && fake.requests == before, "Cancelled job reached network");
    p.media = {{MediaKind::Image, {}, "https://media.example/a.png", "image/png", {}}};
    fake.add("POST", "https://graph.instagram.com/v25.0/123/media", {{"id", "bad-media"}});
    fake.add("GET", "https://graph.instagram.com/v25.0/bad-media?fields=status_code", {{"status_code", "ERROR"}});
    check(publish(Platform::Instagram, fake, p).error == Error::ProcessingFailed && fake.steps.empty(), "Failed processing reached publish");
    Fixture f;
    p.media = {f.image()};
    fake.add("POST", "https://api.linkedin.com/rest/images?action=initializeUpload", {{"value", {{"image", "urn:li:image:untrusted"}, {"uploadUrl", "https://www.linkedin.com.evil.example/upload"}}}});
    check(publish(Platform::LinkedIn, fake, p).error == Error::InvalidResponse && fake.steps.empty(), "Untrusted upload host received OAuth credentials");
    p.media = {f.video()};
    fake.add("POST", "https://graph-video.facebook.com/v25.0/123/videos", {{"id", "already-created"}});
    fake.add("GET", "https://graph.facebook.com/v25.0/already-created?fields=status", {{"status", {{"video_status", "error"}}}});
    result = publish(Platform::Facebook, fake, p);
    check(result.error == Error::ProcessingFailed && result.postId == "already-created" && result.requiresReconciliation(), "Facebook lost the accepted video identifier after processing failure");
}
void transport(int argc, char** argv) {
    CurlTransport transport;
    Request request; request.url = "http://127.0.0.1:1/no-request";
    check(transport.send(request, {}).transportFailed, "HTTP transport must reject cleartext before connecting");
    request.url = "https://user:password@example.com/no-request";
    check(transport.send(request, {}).transportFailed, "URL credentials must be rejected");
    if (argc < 4) return;
    CurlTransport trusted(argv[3]);
    const std::string base = argv[2];
    Fixture f;
    request = {}; request.method = "PUT"; request.url = base + "/range";
    request.file = FileSlice{f.path, 4, 4};
    auto response = trusted.send(request, {});
    check(response.status == 200 && !response.transportFailed && Json::parse(response.body)["bytes"] == "EFGH", "TLS raw upload did not stream the exact byte range");
    request = {}; request.method = "POST"; request.url = base + "/multipart";
    request.parts = {{"json", "{\"fixture\":true}", "application/json", {}}, {"file", {}, "image/png", FileSlice{f.path, 0, 4}}};
    response = trusted.send(request, {});
    check(response.status == 200 && !response.transportFailed && Json::parse(response.body)["matched"] == true, "Multipart streaming or content type incorrect");
    request = {}; request.url = base + "/redirect";
    response = trusted.send(request, {});
    check(response.status == 302 && !response.transportFailed, "Transport followed a redirect");
    request.url = base + "/large";
    check(trusted.send(request, {}).transportFailed, "Oversize response was not bounded");
    request.url = base + "/range"; request.headers = {{"Authorization", "bad\r\ninjected"}};
    check(trusted.send(request, {}).transportFailed, "Header injection reached server");
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc > 1, "Missing test case"); const std::string name = argv[1];
        if (name == "catalog") catalog(); else if (name == "validation") validation();
        else if (name == "x") x(); else if (name == "facebook") facebook();
        else if (name == "instagram") containers(true); else if (name == "threads") containers(false);
        else if (name == "pinterest") pinterest(); else if (name == "tumblr") tumblr();
        else if (name == "mastodon") mastodon(); else if (name == "linkedin") linkedin();
        else if (name == "errors") errors(); else if (name == "transport") transport(argc, argv);
        else throw std::runtime_error("Unknown test");
        std::cout << name << ": passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
