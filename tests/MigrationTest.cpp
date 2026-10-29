#include <iiSocialIntegration/Migration.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>

using namespace iiSocialIntegration;
using Json = nlohmann::json;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool issue(const MigrationResult& result, const char* code) {
    for (const auto& item : result.issues) if (item.code == code) return true;
    return false;
}

const std::string asset1 = "00000000-0000-4000-8000-000000000001";
const std::string asset2 = "00000000-0000-4000-8000-000000000002";

void generic() {
    ExternalPost source;
    source.platformId = "custom-platform";
    source.id = "external-1";
    source.title = "A title";
    source.body = "한글 and 🌍";
    source.language = "ko-KR";
    source.tags = {"art", "art", "world"};
    source.media = {{"remote-1", "https://example.test/a.png", "image", "image/png", "caption", "alt"},
                    {"remote-2", "", "video", "", "", std::nullopt}};
    auto converted = importPost(source);
    check(converted.source.id == source.id && converted.post.body == source.body, "Source or body changed");
    check(converted.post.tags == std::vector<std::string>({"art", "world"}), "Tags were not deduplicated in order");
    check(!serializeSocietyDraft(converted), "Unregistered assets must block request generation");
    converted.post.media[0].assetId = asset1;
    converted.post.media[1].assetId = "not-a-uuid";
    check(!serializeSocietyDraft(converted), "Invalid asset IDs must not be emitted");
    converted.post.media[1].assetId = asset2;
    const auto request = serializeSocietyDraft(converted);
    check(request.has_value(), "Registered assets should produce a request");
    const auto json = Json::parse(*request);
    check(json.at("content").at("body").at("text") == source.body, "Request body changed");
    check(json.at("content").at("language") == "ko-KR", "Language was lost");
    check(json.at("media").at("items").size() == 2 && json.at("media").at("items")[0].at("assetId") == asset1 &&
          json.at("media").at("items")[1].at("assetId") == asset2, "Asset order or IDs changed");
    check(json.at("media").at("items")[0].at("altText") == "alt" &&
          json.at("media").at("items")[1].at("altText").is_null(), "Alt text nullability changed");
    check(json.at("publication").at("status") == "draft" && json.at("access").at("visibility") == "private", "Import must start as a private draft");
    check(!json.contains("id") && !json.contains("author") && !json.contains("source"), "Server-owned or foreign provenance was sent as Society identity");
    auto batch = importPosts({source, source});
    check(batch.size() == 2 && batch[0].source.id == "external-1", "Batch conversion lost an item");
}

void x() {
    const auto result = importPost(Platform::X, R"({"data":{"id":"42","text":"Hello #art","lang":"en","created_at":"2026-01-01T00:00:00Z","attachments":{"media_keys":["one","two"]},"entities":{"hashtags":[{"tag":"art"}]}},"includes":{"media":[{"media_key":"one","type":"photo","url":"https://example.test/a.jpg","alt_text":"A"}]}})");
    check(result.source.id == "42" && result.post.body == "Hello #art", "X text/id mapping failed");
    check(result.post.hashtags == std::vector<std::string>{"art"}, "X hashtags missing");
    check(result.post.media.size() == 2 && result.post.media[0].sourceId == "one" &&
          result.post.media[0].sourceUrl == "https://example.test/a.jpg" && result.post.media[1].sourceId == "two", "X expansion order/missing media lost");
    check(!serializeSocietyDraft(result), "Missing asset registration must block the draft");
}

void mastodon() {
    const char* payload = R"({"id":"7","url":"https://example.social/@u/7","content":"<p>Hello &amp; world</p>","language":"en","visibility":"private","spoiler_text":"CW","sensitive":true,"created_at":"2026-01-01T00:00:00Z","account":{"id":"acct"},"tags":[{"name":"art"}],"media_attachments":[{"id":"m","type":"image","url":"https://example.test/a.png","description":"A"}]})";
    auto blocked = importPost(Platform::Mastodon, payload);
    check(blocked.source.bodyFormat == SourceTextFormat::Html && issue(blocked, "html_conversion_required") && !serializeSocietyDraft(blocked), "HTML must require an explicit conversion");
    MigrationOptions options;
    options.htmlToPlainText = [](std::string_view html) -> std::optional<std::string> {
        return html == "<p>Hello &amp; world</p>" ? std::optional<std::string>("Hello & world") : std::nullopt;
    };
    auto result = importPost(Platform::Mastodon, payload, options);
    check(result.post.body == "Hello & world" && result.source.contentWarning == "CW" && result.source.sensitive == true, "Mastodon content or warning lost");
    check(result.source.visibility == "private" && result.source.originalJson == payload && result.post.media[0].altText == "A", "Mastodon provenance/alt text lost");
    result.post.media[0].assetId = asset1;
    check(serializeSocietyDraft(result).has_value(), "Converted Mastodon draft was blocked");
    auto mediaOnly = importPost(Platform::Mastodon, R"({"id":"m2","content":"","media_attachments":[{"id":"a","type":"image","url":"https://example.test/a.png"}]})");
    check(!issue(mediaOnly, "html_conversion_required"), "Empty HTML should not require a converter");
}

void youtube() {
    auto result = importPost(Platform::YouTube, R"({"id":"vid1","snippet":{"title":"Video","description":"Description","tags":["one","two"],"defaultLanguage":"en","channelId":"channel","publishedAt":"2026-01-01T00:00:00Z"},"status":{"privacyStatus":"unlisted"}})");
    check(result.post.title == "Video" && result.post.body == "Description" && result.post.tags.size() == 2, "YouTube metadata lost");
    check(result.post.media.size() == 1 && result.post.media[0].kind == "video" && result.post.media[0].sourceId == "vid1" && result.post.media[0].sourceUrl.empty(), "YouTube metadata must not masquerade as downloadable video bytes");
    check(result.source.visibility == "unlisted" && !serializeSocietyDraft(result), "Unregistered YouTube video must block request generation");
}

void instagram() {
    auto result = importPost(Platform::Instagram, R"({"id":"ig1","caption":"Caption","media_type":"CAROUSEL_ALBUM","children":{"data":[{"id":"a","media_type":"IMAGE","media_url":"https://example.test/a.jpg"},{"id":"b","media_type":"VIDEO","media_url":"https://example.test/b.mp4"}]}})");
    check(result.post.body == "Caption" && result.post.media.size() == 2, "Instagram carousel missing");
    check(result.post.media[0].sourceId == "a" && result.post.media[1].kind == "video", "Instagram carousel order/type changed");
    auto incomplete = importPost(Platform::Instagram, R"({"id":"ig2","media_type":"CAROUSEL_ALBUM"})");
    check(!incomplete.post.media.empty() && !serializeSocietyDraft(incomplete), "Unexpanded carousel must not become a text-only post");
}

void linkedin() {
    auto result = importPost(Platform::LinkedIn, R"({"id":"urn:li:share:1","commentary":"Work","visibility":"PUBLIC","content":{"multiImage":{"images":[{"id":"urn:li:image:a","altText":"A"},{"id":"urn:li:image:b","altText":"B"}]}}})");
    check(result.post.body == "Work" && result.post.media.size() == 2 && result.post.media[1].sourceId == "urn:li:image:b", "LinkedIn multi-image mapping failed");
    check(result.post.media[0].altText == "A" && result.post.media[0].sourceUrl.empty(), "LinkedIn URN was treated as a URL");
}

void otherProviders() {
    auto facebook = importPost(Platform::Facebook, R"({"id":"f1","message":"News","attachments":{"data":[{"type":"photo","target":{"id":"a"},"media":{"image":{"src":"https://example.test/a.jpg"}}},{"type":"video","target":{"id":"v"},"media":{"image":{"src":"https://example.test/preview.jpg"}}}]}})");
    check(facebook.post.body == "News" && facebook.post.media.size() == 2, "Facebook attachments missing");
    check(facebook.post.media[0].sourceUrl == "https://example.test/a.jpg" && facebook.post.media[1].sourceUrl.empty(), "Video preview was mistaken for original video");
    auto threads = importPost(Platform::Threads, R"({"id":"th1","text":"Hello","media_type":"IMAGE","media_url":"https://example.test/image.jpg"})");
    check(threads.post.body == "Hello" && threads.post.media.size() == 1 && threads.post.media[0].kind == "image", "Threads media mapping failed");
    auto pinterest = importPost(Platform::Pinterest, R"({"id":"pin1","title":"Title","description":"Description","media":{"media_type":"image","images":{"originals":{"url":"https://example.test/original.png"}}}})");
    check(pinterest.post.title == "Title" && pinterest.post.body == "Description" && pinterest.post.media[0].sourceUrl == "https://example.test/original.png", "Pinterest Pin mapping failed");
}

void tumblr() {
    auto result = importPost(Platform::Tumblr, R"({"id":"t1","post_url":"https://example.tumblr.com/post/1","content":[{"type":"text","text":"First"},{"type":"image","media":[{"url":"https://example.test/a.png"}],"alt_text":"A"},{"type":"text","text":"Second"}],"tags":["art"]})");
    check(result.post.body == "First\n\nSecond" && result.post.media.size() == 1, "Tumblr content blocks were lost");
    check(issue(result, "composition_order_not_preserved") && result.source.originalJson.find("First") != std::string::npos, "Mixed block layout needs an explicit warning and original payload");
}

void invalid() {
    check(issue(importPost(Platform::X, "{"), "invalid_json"), "Malformed JSON accepted");
    check(issue(importPost(Platform::Pixiv, "{}"), "unsupported_native_format"), "Unsupported native format silently accepted");
    check(supportsNativeImport(Platform::YouTube) && !supportsNativeImport(Platform::Pixiv), "Native import capability was misreported");
    auto batch = importPosts(Platform::X, {"{", R"({"data":{"id":"1","text":"next"}})"});
    check(batch.size() == 2 && !batch[0].ready() && batch[1].ready() && batch[1].post.body == "next", "Batch failure affected another post");
    auto missing = importPost(Platform::X, R"({"data":{"id":"123"}})");
    check(issue(missing, "empty_native_post") && !serializeSocietyDraft(missing), "Unexpanded empty post was accepted");
    auto article = importPost(Platform::LinkedIn, R"({"id":"urn:li:share:1","commentary":"Read","content":{"article":{"source":"https://example.test"}}})");
    check(issue(article, "unsupported_content_block") && !serializeSocietyDraft(article), "Unsupported LinkedIn article was silently discarded");
    auto poll = importPost(Platform::Mastodon, R"({"id":"poll","content":"<p>Vote</p>","poll":{"options":[]}})");
    check(issue(poll, "unsupported_content_block"), "Unsupported poll was silently discarded");
    auto images = importPost(Platform::LinkedIn, R"({"id":"i","commentary":"Images","content":{"multiImage":{}}})");
    check(issue(images, "unexpanded_multi_image") && !serializeSocietyDraft(images), "Unexpanded LinkedIn images were discarded");
    ExternalPost source;
    source.title = std::string(201, 'a');
    source.body = "Body";
    auto converted = importPost(source);
    check(!serializeSocietyDraft(converted) && converted.post.title == source.title, "Oversized content was truncated or serialized");
    source.title.clear();
    source.body = std::string(100001, 'x');
    converted = importPost(source);
    check(!serializeSocietyDraft(converted) && converted.post.body == source.body, "Oversized body was truncated or serialized");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--contract-fixtures") {
            ExternalPost plain; plain.title = "Hello 🌍"; plain.body = "A Society draft"; plain.tags = {"art"};
            ExternalPost media = plain; media.media = {{"remote", "", "image", "image/png", "caption", "alt"}};
            auto one = importPost(plain); auto two = importPost(media); two.post.media[0].assetId = asset1;
            std::cout << Json::array({Json::parse(*serializeSocietyDraft(one)), Json::parse(*serializeSocietyDraft(two))}).dump();
            return 0;
        }
        generic(); x(); mastodon(); youtube(); instagram(); linkedin(); otherProviders(); tumblr(); invalid();
        std::cout << "Migration: native payloads, Society draft, media ownership, privacy and limits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
