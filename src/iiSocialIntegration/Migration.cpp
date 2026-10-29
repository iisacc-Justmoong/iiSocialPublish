#include "Migration.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace iiSocialIntegration {
namespace {
using Json = nlohmann::json;

const Json& member(const Json& value, std::string_view key) {
    static const Json missing;
    if (!value.is_object()) return missing;
    const auto it = value.find(std::string(key));
    return it == value.end() ? missing : *it;
}

std::string stringValue(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer() || value.is_number_unsigned()) return value.dump();
    return {};
}

std::string field(const Json& value, std::string_view key) {
    return stringValue(member(value, key));
}

std::optional<std::string> optionalField(const Json& value, std::string_view key) {
    const auto& item = member(value, key);
    if (item.is_string()) return item.get<std::string>();
    return std::nullopt;
}

const Json& array(const Json& value) {
    static const Json empty = Json::array();
    return value.is_array() ? value : empty;
}

std::string platformId(Platform platform) {
    for (const auto& item : capabilities()) if (item.platform == platform) return item.id;
    return {};
}

void addIssue(std::vector<MigrationIssue>& issues, std::string code, std::string message, bool blocking = false) {
    if (std::none_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; }))
        issues.push_back({std::move(code), std::move(message), blocking});
}

void copyStringArray(const Json& value, std::vector<std::string>& destination) {
    for (const auto& item : array(value)) if (item.is_string()) destination.push_back(item.get<std::string>());
}

void hashtags(const Json& value, std::string_view name, std::vector<std::string>& destination) {
    for (const auto& item : array(value)) {
        auto tag = field(item, name);
        if (!tag.empty()) destination.push_back(std::move(tag));
    }
}

std::string mediaKind(std::string type) {
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (type == "photo" || type == "image") return "image";
    if (type == "video" || type == "animated_gif" || type == "gifv" || type == "reels") return "video";
    if (type == "audio") return "audio";
    return "unknown";
}

ExternalMedia simpleMedia(const Json& value, std::string_view urlKey = "url", std::string_view altKey = "alt_text") {
    return {field(value, "id"), field(value, urlKey), mediaKind(field(value, "type")),
            field(value, "mime_type"), field(value, "caption"), optionalField(value, altKey)};
}

void xPost(const Json& root, ExternalPost& post) {
    const auto& data = member(root, "data").is_object() ? member(root, "data") : root;
    post.id = field(data, "id"); post.body = field(data, "text"); post.language = field(data, "lang");
    post.createdAt = field(data, "created_at"); post.authorId = field(data, "author_id");
    hashtags(member(member(data, "entities"), "hashtags"), "tag", post.hashtags);
    const auto& expansions = array(member(member(root, "includes"), "media"));
    for (const auto& key : array(member(member(data, "attachments"), "media_keys"))) {
        const auto id = stringValue(key);
        ExternalMedia media; media.sourceId = id; media.kind = "unknown";
        for (const auto& expanded : expansions) {
            if (field(expanded, "media_key") == id) {
                media = simpleMedia(expanded); media.sourceId = id;
                break;
            }
        }
        post.media.push_back(std::move(media));
    }
}

void mastodonPost(const Json& data, ExternalPost& post) {
    post.id = field(data, "id"); post.url = field(data, "url");
    post.body = field(data, "content"); post.bodyFormat = SourceTextFormat::Html;
    post.language = field(data, "language"); post.visibility = field(data, "visibility");
    post.contentWarning = field(data, "spoiler_text"); post.createdAt = field(data, "created_at");
    post.authorId = field(member(data, "account"), "id");
    if (member(data, "sensitive").is_boolean()) post.sensitive = member(data, "sensitive").get<bool>();
    hashtags(member(data, "tags"), "name", post.hashtags);
    for (const auto& item : array(member(data, "media_attachments"))) post.media.push_back(simpleMedia(item, "url", "description"));
}

void youtubePost(const Json& data, ExternalPost& post) {
    post.id = field(data, "id");
    const auto& snippet = member(data, "snippet");
    post.title = field(snippet, "title"); post.body = field(snippet, "description");
    post.description = post.body; post.language = field(snippet, "defaultLanguage");
    post.authorId = field(snippet, "channelId"); post.createdAt = field(snippet, "publishedAt");
    post.visibility = field(member(data, "status"), "privacyStatus");
    copyStringArray(member(snippet, "tags"), post.tags);
    // videos.list provides metadata, not the original downloadable video.
    post.media.push_back({post.id, {}, "video", {}, {}, std::nullopt});
}

void metaMedia(const Json& item, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    const auto type = field(item, "media_type");
    if (type == "CAROUSEL_ALBUM" || type == "CAROUSEL") {
        const auto& children = array(member(member(item, "children"), "data"));
        if (children.empty()) {
            post.media.push_back({field(item, "id"), {}, "unknown", {}, {}, std::nullopt});
            addIssue(issues, "unexpanded_carousel", "Carousel children must be fetched or supplied by the host.");
        } else {
            for (const auto& child : children) metaMedia(child, post, issues);
        }
        return;
    }
    if (type.empty() && field(item, "media_url").empty()) {
        post.media.push_back({field(item, "id"), {}, "unknown", {}, {}, std::nullopt});
        return;
    }
    if (type == "TEXT" || type == "TEXT_POST") return;
    auto media = simpleMedia(item, "media_url");
    media.kind = mediaKind(type);
    post.media.push_back(std::move(media));
}

void metaPost(const Json& data, Platform platform, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    post.id = field(data, "id"); post.url = field(data, "permalink");
    post.body = field(data, platform == Platform::Threads ? "text" : "caption");
    post.createdAt = field(data, "timestamp"); post.authorId = field(data, "username");
    const auto type = field(data, "media_type");
    if (!type.empty()) metaMedia(data, post, issues);
}

void facebookAttachment(const Json& item, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    const auto& sub = array(member(member(item, "subattachments"), "data"));
    if (!sub.empty()) {
        for (const auto& child : sub) facebookAttachment(child, post, issues);
        return;
    }
    ExternalMedia media;
    media.sourceId = field(member(item, "target"), "id");
    media.kind = mediaKind(field(item, "type"));
    if (media.kind == "unknown")
        addIssue(issues, "unsupported_content_block", "A Facebook attachment requires a host-specific converter.", true);
    // An image in a video attachment is a preview, not the video itself.
    if (media.kind == "image") media.sourceUrl = field(member(member(item, "media"), "image"), "src");
    if (media.kind == "video") media.sourceUrl = field(member(item, "media"), "source");
    post.media.push_back(std::move(media));
}

void facebookPost(const Json& data, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    post.id = field(data, "id"); post.body = field(data, "message");
    post.url = field(data, "permalink_url"); post.createdAt = field(data, "created_time");
    post.authorId = field(member(data, "from"), "id");
    for (const auto& item : array(member(member(data, "attachments"), "data"))) facebookAttachment(item, post, issues);
}

void pinterestPost(const Json& data, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    post.id = field(data, "id"); post.title = field(data, "title");
    post.body = field(data, "description"); post.createdAt = field(data, "created_at");
    const auto& media = member(data, "media");
    if (media.is_object()) {
        ExternalMedia item; item.sourceId = post.id;
        item.kind = mediaKind(field(media, "media_type"));
        if (item.kind == "unknown")
            addIssue(issues, "unsupported_content_block", "A Pinterest media type requires a host-specific converter.", true);
        if (item.kind == "image") item.sourceUrl = field(member(member(media, "images"), "originals"), "url");
        post.media.push_back(std::move(item));
    }
}

void linkedinPost(const Json& data, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    post.id = field(data, "id"); post.body = field(data, "commentary");
    post.authorId = field(data, "author"); post.visibility = field(data, "visibility");
    const auto& content = member(data, "content");
    if (content.is_object()) {
        for (auto it = content.begin(); it != content.end(); ++it) {
            if (it.key() != "media" && it.key() != "multiImage")
                addIssue(issues, "unsupported_content_block", "A LinkedIn content type requires a host-specific converter.", true);
        }
    }
    const auto& media = member(content, "media");
    if (media.is_object()) {
        auto item = simpleMedia(media, "url", "altText");
        if (item.sourceId.find("urn:li:image:") == 0) item.kind = "image";
        if (item.sourceId.find("urn:li:video:") == 0) item.kind = "video";
        post.media.push_back(std::move(item));
    }
    const auto& multiImage = member(content, "multiImage");
    const auto& images = array(member(multiImage, "images"));
    if (multiImage.is_object() && images.empty()) {
        post.media.push_back({{}, {}, "image", {}, {}, std::nullopt});
        addIssue(issues, "unexpanded_multi_image", "LinkedIn image references must be fetched or supplied by the host.");
    }
    for (const auto& image : images) {
        auto item = simpleMedia(image, "url", "altText"); item.kind = "image";
        post.media.push_back(std::move(item));
    }
}

void tumblrPost(const Json& data, ExternalPost& post, std::vector<MigrationIssue>& issues) {
    post.id = field(data, "id"); post.url = field(data, "post_url");
    post.createdAt = field(data, "date"); post.authorId = field(data, "blog_name");
    copyStringArray(member(data, "tags"), post.tags);
    bool sawText = false, sawMedia = false;
    for (const auto& block : array(member(data, "content"))) {
        const auto type = field(block, "type");
        if (type == "text") {
            if (sawText) post.body += "\n\n";
            post.body += field(block, "text"); sawText = true;
            if (!array(member(block, "formatting")).empty() || !field(block, "subtype").empty())
                addIssue(issues, "text_formatting_not_preserved", "Tumblr text formatting remains in the original payload.");
        } else if (type == "image" || type == "video" || type == "audio") {
            ExternalMedia item; item.kind = type;
            item.altText = optionalField(block, "alt_text"); item.caption = field(block, "caption");
            const auto& sizes = array(member(block, "media"));
            if (!sizes.empty()) { item.sourceUrl = field(sizes[0], "url"); item.mimeType = field(sizes[0], "type"); }
            else if (member(block, "media").is_object()) {
                item.sourceUrl = field(member(block, "media"), "url");
                item.mimeType = field(member(block, "media"), "type");
            }
            post.media.push_back(std::move(item)); sawMedia = true;
        } else {
            addIssue(issues, "unsupported_content_block", "A Tumblr content block requires a host-specific converter.", true);
        }
    }
    if (sawText && sawMedia) addIssue(issues, "composition_order_not_preserved", "Text/media interleaving remains in the original payload; review the Society composition.");
}

bool utf8LengthWithin(std::string_view value, std::size_t minimum, std::size_t maximum) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::size_t length = 0;
        if (first <= 0x7f) length = 1;
        else if (first >= 0xc2 && first <= 0xdf) length = 2;
        else if (first >= 0xe0 && first <= 0xef) length = 3;
        else if (first >= 0xf0 && first <= 0xf4) length = 4;
        else return false;
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        if (length >= 3) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        }
        i += length;
        if (++count > maximum) return false;
    }
    return count >= minimum;
}

bool validUuid(std::string_view id) noexcept {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

bool validLabels(const std::vector<std::string>& values) noexcept {
    if (values.size() > 50) return false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!utf8LengthWithin(values[i], 1, 80)) return false;
        for (std::size_t j = 0; j < i; ++j) if (values[i] == values[j]) return false;
    }
    return true;
}

bool validPost(const SocietyPost& post, bool requireAssets = true) noexcept {
    if (!utf8LengthWithin(post.title, 0, 200) || !utf8LengthWithin(post.body, 0, 100000) ||
        !utf8LengthWithin(post.description, 0, 5000) ||
        (!post.language.empty() && !utf8LengthWithin(post.language, 2, 64)) ||
        (post.bodyFormat != "plain_text" && post.bodyFormat != "markdown") ||
        !validLabels(post.tags) || !validLabels(post.hashtags) || post.media.size() > 100) return false;
    for (const auto& media : post.media) {
        if ((requireAssets && !validUuid(media.assetId)) || !utf8LengthWithin(media.caption, 0, 2000) ||
            (media.altText && !utf8LengthWithin(*media.altText, 0, 2000))) return false;
    }
    return true;
}

std::vector<std::string> uniqueLabels(const std::vector<std::string>& source, bool& normalized) {
    std::vector<std::string> values;
    for (const auto& value : source) {
        if (value.empty() || std::find(values.begin(), values.end(), value) != values.end()) { normalized = true; continue; }
        values.push_back(value);
    }
    return values;
}
} // namespace

bool MigrationResult::ready() const noexcept {
    for (const auto& issue : issues) if (issue.blocking) return false;
    return validPost(post);
}

MigrationResult importPost(const ExternalPost& source, const MigrationOptions& options) {
    MigrationResult result; result.source = source;
    result.post.title = source.title; result.post.description = source.description; result.post.language = source.language;
    if (source.bodyFormat == SourceTextFormat::Html && !source.body.empty()) {
        auto converted = options.htmlToPlainText ? options.htmlToPlainText(source.body) : std::nullopt;
        if (converted) result.post.body = std::move(*converted);
        else addIssue(result.issues, "html_conversion_required", "Supply a trusted HTML-to-plain-text converter before creating the Society request.", true);
    } else {
        result.post.body = source.body;
        result.post.bodyFormat = source.bodyFormat == SourceTextFormat::Markdown ? "markdown" : "plain_text";
    }
    bool normalized = false;
    result.post.tags = uniqueLabels(source.tags, normalized);
    result.post.hashtags = uniqueLabels(source.hashtags, normalized);
    if (normalized) addIssue(result.issues, "classification_normalized", "Empty or duplicate labels were omitted; the original list is preserved.");
    for (const auto& media : source.media)
        result.post.media.push_back({media.sourceId, media.sourceUrl, media.kind, media.mimeType, media.caption, media.altText, {}});
    if (!source.media.empty()) addIssue(result.issues, "assets_require_registration", "Register each source asset with Society and assign its asset ID.");
    if (source.id.empty()) addIssue(result.issues, "source_id_missing", "The host cannot reliably deduplicate this post without a source ID.");
    if (!source.contentWarning.empty() || source.sensitive.value_or(false))
        addIssue(result.issues, "source_safety_requires_review", "The source warning or sensitivity is preserved for review and is not classified automatically.");
    if (source.title.empty()) addIssue(result.issues, "title_required_to_publish", "A Society draft can be created without a title; publishing requires one.");
    if (!validPost(result.post, false))
        addIssue(result.issues, "society_field_limits", "Review UTF-8 text, classification, and Society field limits before creating a request.");
    return result;
}

std::vector<MigrationResult> importPosts(const std::vector<ExternalPost>& posts, const MigrationOptions& options) {
    std::vector<MigrationResult> result; result.reserve(posts.size());
    for (const auto& post : posts) result.push_back(importPost(post, options));
    return result;
}

bool supportsNativeImport(Platform platform) noexcept {
    switch (platform) {
        case Platform::X: case Platform::Facebook: case Platform::Instagram: case Platform::Threads:
        case Platform::Pinterest: case Platform::Tumblr: case Platform::Mastodon:
        case Platform::LinkedIn: case Platform::YouTube: return true;
        default: return false;
    }
}

MigrationResult importPost(Platform platform, std::string_view nativeJson, const MigrationOptions& options) {
    ExternalPost source; source.platformId = platformId(platform);
    std::vector<MigrationIssue> issues;
    constexpr std::size_t maxJsonBytes = 8 * 1024 * 1024;
    if (nativeJson.size() > maxJsonBytes) {
        auto result = importPost(source, options);
        addIssue(result.issues, "native_payload_too_large", "Native JSON exceeds the 8 MiB input limit.", true);
        return result;
    }
    source.originalJson = std::string(nativeJson);
    const auto data = Json::parse(nativeJson.begin(), nativeJson.end(), nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        auto result = importPost(source, options);
        addIssue(result.issues, "invalid_json", "A single provider JSON post object is required.", true);
        return result;
    }
    switch (platform) {
        case Platform::X: xPost(data, source); break;
        case Platform::Mastodon: mastodonPost(data, source); break;
        case Platform::YouTube: youtubePost(data, source); break;
        case Platform::Instagram: case Platform::Threads: metaPost(data, platform, source, issues); break;
        case Platform::Facebook: facebookPost(data, source, issues); break;
        case Platform::Pinterest: pinterestPost(data, source, issues); break;
        case Platform::LinkedIn: linkedinPost(data, source, issues); break;
        case Platform::Tumblr: tumblrPost(data, source, issues); break;
        case Platform::Pixiv: case Platform::ArtStation:
            addIssue(issues, "unsupported_native_format", "Normalize this provider with ExternalPost; no native JSON contract is implemented.", true);
            break;
        default:
            addIssue(issues, "unsupported_native_format", "No native JSON contract is implemented for this platform.", true);
    }
    const auto& object = platform == Platform::X && member(data, "data").is_object() ? member(data, "data") : data;
    if ((platform == Platform::Mastodon && (!member(object, "poll").is_null() || !member(object, "reblog").is_null())) ||
        (platform == Platform::X && (!array(member(object, "referenced_tweets")).empty() ||
                                    !array(member(member(object, "attachments"), "poll_ids")).empty())))
        addIssue(issues, "unsupported_content_block", "A poll, quote, or reblog requires a host-specific converter.", true);
    if (source.id.empty() && platform != Platform::Pixiv && platform != Platform::ArtStation)
        addIssue(issues, "invalid_native_post", "The provider post must contain an ID and supported fields.", true);
    if (source.body.empty() && source.title.empty() && source.description.empty() && source.media.empty())
        addIssue(issues, "empty_native_post", "The supplied JSON has no recognized post content or media.", true);
    auto result = importPost(source, options);
    result.issues.insert(result.issues.end(), issues.begin(), issues.end());
    return result;
}

std::vector<MigrationResult> importPosts(Platform platform, const std::vector<std::string>& nativePosts,
                                         const MigrationOptions& options) {
    std::vector<MigrationResult> result; result.reserve(nativePosts.size());
    for (const auto& post : nativePosts) result.push_back(importPost(platform, post, options));
    return result;
}

std::optional<std::string> serializeSocietyDraft(const MigrationResult& result) {
    if (!result.ready()) return std::nullopt;
    const auto& post = result.post;
    Json body = {{"format", post.bodyFormat}, {"text", post.body}};
    Json content = {{"title", post.title}, {"body", std::move(body)}, {"description", post.description},
                    {"language", post.language.empty() ? Json(nullptr) : Json(post.language)}};
    Json items = Json::array();
    for (std::size_t i = 0; i < post.media.size(); ++i) {
        const auto& media = post.media[i];
        items.push_back({{"assetId", media.assetId}, {"position", i}, {"caption", media.caption},
                         {"altText", media.altText ? Json(*media.altText) : Json(nullptr)}});
    }
    Json request = {{"content", std::move(content)}, {"classification", {{"tags", post.tags}, {"hashtags", post.hashtags}}},
                    {"media", {{"items", std::move(items)}}}, {"publication", {{"status", "draft"}}},
                    {"access", {{"visibility", "private"}}}};
    try { return request.dump(); } catch (const Json::exception&) { return std::nullopt; }
}
} // namespace iiSocialIntegration
