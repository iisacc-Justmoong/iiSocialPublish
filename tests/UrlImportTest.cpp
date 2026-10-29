#include <iiSocialIntegration/UrlImport.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

using namespace iiSocialIntegration;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct FakeTransport : Transport {
    std::map<std::string, Response> responses;
    std::vector<Request> requests;
    Response send(const Request& request, std::stop_token) override {
        requests.push_back(request);
        auto found = responses.find(request.url);
        if (found == responses.end()) return {0, {}, {}, true};
        auto response = found->second;
        if (request.responseSink && response.status >= 200 && response.status < 300) {
            auto bytes = std::move(response.body); response.body.clear();
            if (bytes.size() > request.maxResponseBytes) { response.transportFailed = true; response.responseLimitExceeded = true; return response; }
            for (std::size_t i = 0; i < bytes.size(); i += 2) {
                auto part = std::string_view(bytes).substr(i, 2);
                if (!request.responseSink(part)) { response.transportFailed = true; response.responseSinkFailed = true; return response; }
                response.responseBytes += part.size();
            }
        }
        return response;
    }
};

struct ThrowingTransport : Transport {
    Response send(const Request&, std::stop_token) override { throw std::runtime_error("private transport details"); }
};

struct BufferedTransport : Transport {
    Response send(const Request&, std::stop_token) override { return {200, {{"Content-Type", "image/png"}}, "legacy"}; }
};

bool hasIssue(const std::vector<MigrationIssue>& issues, const char* code) {
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}

std::size_t findMedia(const UrlImportResult& result, const std::string& url) {
    for (std::size_t i = 0; i < result.post.media.size(); ++i)
        if (result.post.media[i].sourceUrl == url) return i;
    throw std::runtime_error("Expected media URL missing: " + url);
}

const std::string html = R"HTML(<!doctype html><html lang="ko"><head>
<meta charset="utf-8"><base href="/assets/">
<title>Fallback</title><meta property="og:title" content="작품 &amp; 이야기">
<meta property="og:description" content="A preview &amp; credit">
<meta property="og:image" content="cover.jpg"><meta property="og:image:alt" content="Cover &amp; alt">
<meta property="og:image:type" content="image/jpeg">
<meta name="twitter:player:stream" content="stream.mp4"><meta name="twitter:player:stream:content_type" content="video/mp4">
<link rel="image_src" href="legacy.jpg">
<script type="application/ld+json">{"@type":"ImageObject","contentUrl":"json.png","encodingFormat":"image/png"}</script>
<script>var ignored = '<img src="script.png">';</script></head><body>
<!-- <img src="comment.png"> -->
<img src="cover.jpg" alt="In the page"><img data-src="/full.png" alt="Full image">
<picture><source srcset="wide.jpg 2x, narrow.jpg 1x"><img src="fallback.jpg"></picture>
<video src="clip.mp4" poster="poster.jpg"><source src="clip.webm" type="video/webm"></video>
<audio src="sound.ogg"></audio><a href="original.png?download=1">Original</a>
<div style="background-image: url('background.png')"></div>
<div style="background: url('escaped\2e png')"></div>
<img src="data:image/png;base64,YWJj"><img src="javascript:alert(1)">
</body></html>)HTML";

void discovery() {
    FakeTransport transport;
    transport.responses["https://example.test/post/1"] = {302, {{"Location", "../post/final"}}, {}};
    transport.responses["https://example.test/post/final"] = {200, {{"Content-Type", "text/html; charset=utf-8"}}, html};
    Context context{transport};
    auto result = importUrl("https://example.test/post/1", context);
    check(result.ok() && result.finalUrl == "https://example.test/post/final", "Redirect resolution failed");
    check(result.post.platformId == "web" && result.post.id == result.finalUrl, "Source identity missing");
    check(result.post.title == "작품 & 이야기" && result.post.description == "A preview & credit" && result.post.language == "ko", "Page metadata or UTF-8 decoding failed");
    check(result.mediaInfo.size() == result.post.media.size(), "Candidate provenance alignment failed");
    auto cover = findMedia(result, "https://example.test/assets/cover.jpg");
    check(result.post.media[cover].altText == "Cover & alt" && result.post.media[cover].mimeType == "image/jpeg", "Open Graph structured fields lost");
    check(result.mediaInfo[cover].origins.size() == 2, "Duplicate URLs were not merged with both origins");
    const auto jsonImage = result.post.media[findMedia(result, "https://example.test/assets/json.png")];
    check(jsonImage.kind == "image" && jsonImage.mimeType == "image/png", "JSON-LD image or MIME missing");
    findMedia(result, "https://example.test/full.png");
    findMedia(result, "https://example.test/assets/wide.jpg");
    findMedia(result, "https://example.test/assets/narrow.jpg");
    check(result.post.media[findMedia(result, "https://example.test/assets/clip.mp4")].kind == "video", "Video source missing");
    check(result.post.media[findMedia(result, "https://example.test/assets/clip.webm")].mimeType == "video/webm", "Video source MIME missing");
    check(result.post.media[findMedia(result, "https://example.test/assets/sound.ogg")].kind == "audio", "Audio source missing");
    check(result.post.media[findMedia(result, "https://example.test/assets/stream.mp4")].mimeType == "video/mp4", "Twitter stream MIME missing");
    findMedia(result, "https://example.test/assets/legacy.jpg");
    check(result.mediaInfo[findMedia(result, "https://example.test/assets/poster.jpg")].preview, "Video poster must be marked as a preview");
    findMedia(result, "https://example.test/assets/original.png?download=1");
    check(result.mediaInfo[findMedia(result, "https://example.test/assets/background.png")].preview, "Inline CSS background was not identified as a review candidate");
    check(hasIssue(result.issues, "css_escape_not_parsed"), "Unsupported CSS escapes were not reported");
    findMedia(result, "data:image/png;base64,YWJj");
    for (const auto& media : result.post.media)
        check(media.sourceUrl.find("script.png") == std::string::npos && media.sourceUrl.find("comment.png") == std::string::npos, "Script/comment was treated as an element");
    check(transport.requests.size() == 2 && transport.requests[0].method == "GET" && transport.requests[1].method == "GET", "Unexpected network requests");
    check(transport.requests[0].maxResponseBytes == 8 * 1024 * 1024, "Page response size limit was not forwarded");
    for (const auto& request : transport.requests)
        for (const auto& [key, value] : request.headers) {
            (void)value;
            check(key != "Authorization" && key != "Cookie", "Credentials were attached to a public-page request");
        }
    auto migration = importPost(result);
    check(!serializeSocietyDraft(migration) && hasIssue(migration.issues, "assets_require_registration"), "Unregistered URL assets must block Society serialization");
    auto full = findMedia(result, "https://example.test/full.png");
    auto clip = findMedia(result, "https://example.test/assets/clip.mp4");
    auto chosen = selectUrlMedia(result, {clip, full});
    check(chosen.ok() && chosen.post.media.size() == 2 && chosen.post.media[0].kind == "video" && chosen.mediaInfo.size() == 2, "Media selection lost order or provenance");
    check(selectUrlMedia(result, {full, full}).error == UrlImportError::InvalidSelection &&
          selectUrlMedia(result, {result.post.media.size()}).error == UrlImportError::InvalidSelection, "Invalid selection was accepted");
    chosen = selectUrlMedia(result, {full});
    transport.responses[chosen.post.media[0].sourceUrl] = {200, {{"Content-Type", "image/png"}}, "pixels"};
    std::string saved;
    auto asset = fetchMedia(chosen.post.media[0], context, [&](std::string_view chunk) { saved.append(chunk); return true; });
    auto draft = importPost(chosen);
    check(asset.ok() && saved == "pixels" && !serializeSocietyDraft(draft), "Discovered asset did not reach the caller sink");
    draft.post.media[0].assetId = "00000000-0000-4000-8000-000000000001";
    auto request = serializeSocietyDraft(draft);
    check(request && request->find("00000000-0000-4000-8000-000000000001") != std::string::npos, "Selected asset did not reach the Society draft");
}

void errors() {
    FakeTransport transport; Context context{transport};
    check(importUrl("file:///etc/passwd", context).error == UrlImportError::InvalidUrl && transport.requests.empty(), "Non-web URL was fetched");
    check(importUrl("https://user:password@example.test/", context).error == UrlImportError::InvalidUrl, "URL credentials were accepted");
    transport.responses["https://example.test/private"] = {403};
    check(importUrl("https://example.test/private", context).error == UrlImportError::HttpError, "HTTP rejection was hidden");
    transport.responses["https://example.test/not-html"] = {200, {{"Content-Type", "application/json"}}, "{}"};
    check(importUrl("https://example.test/not-html", context).error == UrlImportError::UnsupportedContentType, "JSON was treated as HTML");
    transport.responses["https://example.test/latin"] = {200, {{"Content-Type", "text/html; charset = \"ISO-8859-1\""}}, "<html><title>Caf\xe9</title><img src='/a.png'></html>"};
    check(importUrl("https://example.test/latin", context).post.title == "Café", "HTTP charset was not honored");
    transport.responses["https://example.test/loop"] = {302, {{"location", "/loop"}}, {}};
    check(importUrl("https://example.test/loop", context).error == UrlImportError::RedirectLimit, "Redirect loop was not stopped");
    UrlImportOptions policy; policy.allowPageUrl = [](std::string_view url) { return url.find("other.test") == url.npos; };
    transport.responses["https://example.test/redirect"] = {302, {{"Location", "https://other.test/private"}}, {}};
    auto blocked = importUrl("https://example.test/redirect", context, policy);
    check(blocked.error == UrlImportError::UrlBlocked && transport.requests.back().url == "https://example.test/redirect", "Redirect bypassed host URL policy");
    UrlImportOptions limited; limited.maxMediaCandidates = 1;
    auto truncated = parsePageDom("https://example.test/post", html, limited);
    check(truncated.ok() && truncated.post.media.size() == 1 && hasIssue(truncated.issues, "media_candidate_limit_reached"), "Media limit was silently ignored");
    check(!importPost(truncated).ready(), "Truncated discovery was treated as complete");
    limited.maxHtmlBytes = 10;
    check(parsePageDom("https://example.test/post", html, limited).error == UrlImportError::InputTooLarge, "Oversized HTML accepted");
    std::stop_source stop; stop.request_stop(); Context cancelled{transport, stop.get_token()};
    check(importUrl("https://example.test/post", cancelled).error == UrlImportError::Cancelled, "Cancelled request was sent");
    check(!importPost(importUrl("https://example.test/private", context)).ready(), "Failed URL import became a ready Society draft");
    auto empty = parsePageDom("https://example.test/post", "<html><title>Empty</title></html>");
    check(empty.ok() && hasIssue(empty.issues, "no_media_discovered") && !importPost(empty).ready(), "Empty DOM became a complete media migration");
    ThrowingTransport throwing; Context throwingContext{throwing};
    auto failure = importUrl("https://example.test/post", throwingContext);
    check(failure.error == UrlImportError::TransportFailure && failure.message.find("private") == std::string::npos, "Transport exception leaked or escaped");
}

void assets() {
    FakeTransport transport; Context context{transport};
    ExternalMedia media; media.kind = "image"; media.sourceUrl = "https://example.test/art";
    transport.responses[media.sourceUrl] = {302, {{"Location", "https://cdn.test/original"}}, "redirect body"};
    transport.responses["https://cdn.test/original"] = {200, {{"Content-Type", "image/png"}}, std::string("A\0B", 3)};
    std::string output;
    auto sink = [&](std::string_view bytes) { output.append(bytes); return true; };
    auto fetched = fetchMedia(media, context, sink);
    check(fetched.ok() && fetched.bytesWritten == 3 && output == std::string("A\0B", 3), "Media bytes were not streamed intact");
    check(fetched.finalUrl == "https://cdn.test/original" && fetched.mimeType == "image/png", "Media redirect or MIME missing");
    check(transport.requests.size() == 2 && transport.requests[1].responseSink && transport.requests[1].maxResponseBytes > 8 * 1024 * 1024, "Media request did not use bounded streaming");
    UrlMediaOptions restricted; restricted.allowMediaUrl = [](std::string_view url) { return url.find("cdn.test") == url.npos; };
    check(fetchMedia(media, context, sink, restricted).error == UrlImportError::UrlBlocked, "Media redirect bypassed host URL policy");
    restricted = {}; restricted.maxBytes = 2;
    check(fetchMedia(media, context, sink, restricted).error == UrlImportError::InputTooLarge, "Oversized media was accepted");
    auto reject = [](std::string_view) -> bool { throw std::runtime_error("sink error"); };
    check(fetchMedia(media, context, reject).error == UrlImportError::SinkFailure, "Sink failure was hidden");
    media.sourceUrl = "data:image/png;base64,YWJj"; output.clear();
    fetched = fetchMedia(media, context, sink);
    check(fetched.ok() && fetched.mimeType == "image/png" && output == "abc", "Data URI bytes were not decoded");
    media.sourceUrl = "data:image/svg+xml,%3Csvg%3E%00"; output.clear();
    fetched = fetchMedia(media, context, sink);
    check(fetched.ok() && output == std::string("<svg>\0", 6), "Percent-encoded data URI bytes changed");
    media.sourceUrl = "data:image/png;base64,!!";
    check(fetchMedia(media, context, sink).error == UrlImportError::InvalidMedia, "Invalid data URI was accepted");
    media.sourceUrl = "https://example.test/legacy";
    BufferedTransport legacy; Context legacyContext{legacy}; output.clear();
    check(fetchMedia(media, legacyContext, sink).ok() && output == "legacy", "Buffered caller transport compatibility failed");
    ThrowingTransport throwing; Context throwingContext{throwing};
    check(fetchMedia(media, throwingContext, sink).error == UrlImportError::TransportFailure, "Media transport exception escaped");
}

void concurrentDom() {
    check(initializeUrlImport(), "URL import initialization failed");
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
        for (int n = 0; n < 20; ++n) {
            auto page = parsePageDom("https://example.test/dir/post", "<title>Parallel</title><img src='art.png'>");
            if (!page.ok() || page.post.title != "Parallel" || page.post.media.size() != 1 ||
                page.post.media[0].sourceUrl != "https://example.test/dir/art.png") valid = false;
        }
    });
    for (auto& worker : workers) worker.join();
    check(valid, "Concurrent DOM parsing changed the results");
}

void loopback(const char* origin, const char* certificate) {
    CurlTransport transport{certificate}; Context context{transport};
    auto result = importUrl(std::string(origin) + "/page-redirect", context);
    check(result.ok() && result.finalUrl == std::string(origin) + "/post/page", "Real HTTPS redirect failed");
    check(result.post.title == "실제 HTTPS" && result.post.media.size() == 1, "Real HTTPS page parsing failed");
    check(result.post.media[0].sourceUrl == std::string(origin) + "/images/art.png", "Real HTTPS media URL failed");
    auto large = importUrl(std::string(origin) + "/post/large", context);
    check(large.ok() && large.post.media.size() == 1, "A bounded HTML response larger than the publishing limit failed");
    ExternalMedia video; video.sourceUrl = std::string(origin) + "/asset/redirect"; video.kind = "video";
    std::size_t bytes = 0, calls = 0;
    auto streamed = fetchMedia(video, context, [&](std::string_view chunk) { bytes += chunk.size(); ++calls; return true; });
    check(streamed.ok() && bytes == 9 * 1024 * 1024 && streamed.bytesWritten == bytes && calls > 1, "Real HTTPS media was not streamed in bounded chunks");
    check(streamed.finalUrl == std::string(origin) + "/asset/video", "Real media redirect was not followed");
    UrlMediaOptions small; small.maxBytes = 1024;
    check(fetchMedia(video, context, [](std::string_view) { return true; }, small).error == UrlImportError::InputTooLarge, "Real streamed response ignored its byte limit");
    video.sourceUrl = std::string(origin) + "/asset/redirect-small"; small.maxBytes = 3;
    std::string tiny;
    auto tinyResult = fetchMedia(video, context, [&](std::string_view chunk) { tiny.append(chunk); return true; }, small);
    check(tinyResult.ok() && tiny == "abc", "Redirect body incorrectly consumed the media byte budget");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--contract-fixtures") {
            auto page = parsePageDom("https://example.test/post", "<html><title>URL draft</title><img src='/image.png' alt='Art'></html>");
            auto draft = importPost(page);
            draft.post.media[0].assetId = "00000000-0000-4000-8000-000000000001";
            auto request = serializeSocietyDraft(draft);
            if (!request) throw std::runtime_error("URL draft could not be serialized");
            std::cout << '[' << *request << ']';
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--loopback") loopback(argv[2], argv[3]);
        else { discovery(); errors(); assets(); concurrentDom(); }
        std::cout << "URL import: transport, DOM, provenance, limits and Society integration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
