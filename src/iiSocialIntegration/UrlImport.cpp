#include "UrlImport.h"
#include "CurlRuntime.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <memory>
#include <unordered_map>

namespace iiSocialIntegration {
bool initializeUrlImport() noexcept {
    static const bool initialized = [] { xmlInitParser(); return detail::initializeCurl(); }();
    return initialized;
}
namespace {
using Json = nlohmann::json;
constexpr std::size_t hardHtmlLimit = 8 * 1024 * 1024;
constexpr std::size_t hardMediaLimit = 5000;
constexpr std::size_t maxJsonLdBytes = 256 * 1024;
constexpr std::uint64_t hardDownloadLimit = 5ULL * 1024 * 1024 * 1024;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

bool hasControl(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
}

void issue(std::vector<MigrationIssue>& issues, std::string code, std::string message, bool blocking = false) {
    if (std::none_of(issues.begin(), issues.end(), [&](const auto& item) { return item.code == code; }))
        issues.push_back({std::move(code), std::move(message), blocking});
}

using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
struct CurlStringDeleter { void operator()(char* value) const { curl_free(value); } };
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

std::optional<std::string> webUrl(std::string_view base, std::string_view reference, bool dropFragment) {
    const auto input = trim(reference);
    if (input.empty() || input.size() > 65536 || hasControl(input)) return std::nullopt;
    CurlUrl url(curl_url(), curl_url_cleanup);
    if (!url) return std::nullopt;
    if (!base.empty() && curl_url_set(url.get(), CURLUPART_URL, std::string(base).c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return std::nullopt;
    if (curl_url_set(url.get(), CURLUPART_URL, input.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return std::nullopt;
    char* raw = nullptr;
    if (curl_url_get(url.get(), CURLUPART_SCHEME, &raw, 0) != CURLUE_OK) return std::nullopt;
    CurlString scheme(raw);
    const auto protocol = lower(scheme.get());
    if (protocol != "https" && protocol != "http") return std::nullopt;
    if (dropFragment && curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK) return std::nullopt;
    raw = nullptr;
    if (curl_url_get(url.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) return std::nullopt;
    CurlString result(raw);
    return std::string(result.get());
}

std::optional<std::string> assetUrl(std::string_view base, std::string_view reference) {
    auto value = trim(reference);
    if (value.empty() || hasControl(value)) return std::nullopt;
    const auto prefix = lower(value.substr(0, std::min<std::size_t>(value.size(), 16)));
    if (prefix.starts_with("data:image/") || prefix.starts_with("data:video/") || prefix.starts_with("data:audio/")) {
        if (value.size() <= hardHtmlLimit && value.find(',') != std::string::npos) return value;
        return std::nullopt;
    }
    return webUrl(base, value, false);
}

std::optional<std::string> header(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) if (lower(key) == name) return value;
    return std::nullopt;
}

std::string charset(std::string_view contentType) {
    auto pos = contentType.find(';');
    while (pos != contentType.npos) {
        const auto next = contentType.find(';', pos + 1);
        auto parameter = contentType.substr(pos + 1, next == contentType.npos ? next : next - pos - 1);
        const auto equal = parameter.find('=');
        if (equal != parameter.npos && lower(trim(parameter.substr(0, equal))) == "charset") {
            auto value = trim(parameter.substr(equal + 1));
            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
                value = value.substr(1, value.size() - 2);
            if (value.size() > 64 || std::any_of(value.begin(), value.end(), [](unsigned char ch) {
                return !(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.');
            })) return {};
            return value;
        }
        pos = next;
    }
    return {};
}

struct XmlStringDeleter { void operator()(xmlChar* value) const { xmlFree(value); } };
using XmlString = std::unique_ptr<xmlChar, XmlStringDeleter>;
using XmlDoc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

std::optional<std::string> attribute(const xmlNode* node, const char* name) {
    XmlString value(xmlGetProp(node, reinterpret_cast<const xmlChar*>(name)));
    if (!value) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(value.get()));
}

std::string textContent(const xmlNode* node) {
    XmlString value(xmlNodeGetContent(node));
    return value ? trim(reinterpret_cast<const char*>(value.get())) : std::string{};
}

std::string tag(const xmlNode* node) {
    return node && node->name ? lower(reinterpret_cast<const char*>(node->name)) : std::string{};
}

template<class Function>
void walk(xmlNode* root, Function function) {
    if (!root) return;
    std::vector<xmlNode*> stack{root};
    while (!stack.empty()) {
        auto* node = stack.back(); stack.pop_back();
        if (node->type == XML_ELEMENT_NODE) function(node);
        for (auto* child = node->last; child; child = child->prev) stack.push_back(child);
    }
}

std::string extensionKind(std::string url) {
    auto end = url.find_first_of("?#");
    if (end != std::string::npos) url.resize(end);
    url = lower(url);
    for (const auto* suffix : {".jpg", ".jpeg", ".png", ".webp", ".gif", ".avif", ".svg", ".heic"})
        if (url.ends_with(suffix)) return "image";
    for (const auto* suffix : {".mp4", ".webm", ".mov", ".m4v", ".m3u8"}) if (url.ends_with(suffix)) return "video";
    for (const auto* suffix : {".mp3", ".wav", ".ogg", ".m4a", ".flac"}) if (url.ends_with(suffix)) return "audio";
    return {};
}

std::vector<std::string> srcset(std::string_view value) {
    std::vector<std::string> urls;
    std::size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (std::isspace(static_cast<unsigned char>(value[pos])) || value[pos] == ',')) ++pos;
        auto start = pos;
        while (pos < value.size() && !std::isspace(static_cast<unsigned char>(value[pos]))) ++pos;
        auto candidate = std::string(value.substr(start, pos - start));
        bool commaTerminated = false;
        while (!candidate.empty() && candidate.back() == ',') { candidate.pop_back(); commaTerminated = true; }
        if (!candidate.empty()) urls.push_back(std::move(candidate));
        if (commaTerminated) continue;
        unsigned parentheses = 0;
        while (pos < value.size()) {
            const char ch = value[pos++];
            if (ch == '(') ++parentheses;
            else if (ch == ')' && parentheses) --parentheses;
            else if (ch == ',' && !parentheses) break;
        }
    }
    return urls;
}

bool boundedJsonDepth(std::string_view text) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 64) return false; }
        else if ((ch == '}' || ch == ']') && depth) --depth;
    }
    return true;
}

class DomCollector {
public:
    DomCollector(UrlImportResult& result, const UrlImportOptions& options)
        : result_(result), limit_(std::min(options.maxMediaCandidates, hardMediaLimit)), base_(result.finalUrl) {}

    void collect(xmlDoc* doc) {
        auto* root = xmlDocGetRootElement(doc);
        bool sawBase = false;
        walk(root, [&](xmlNode* node) {
            if (!sawBase && tag(node) == "base") {
                auto href = attribute(node, "href");
                if (href) {
                    sawBase = true;
                    auto base = webUrl(result_.finalUrl, *href, false);
                    if (base) base_ = std::move(*base);
                    else issue(result_.issues, "invalid_base_url", "The page base URL was not a valid web URL.");
                }
            }
        });
        walk(root, [&](xmlNode* node) { visit(node); });
        if (result_.post.media.empty()) issue(result_.issues, "no_media_discovered", "No supported static DOM media references were found.", true);
        else issue(result_.issues, "media_requires_review", "Page media can include previews, responsive variants, and unrelated page elements.");
        if (!result_.post.description.empty()) issue(result_.issues, "description_is_preview", "Page metadata is a preview and may not contain the full post text.");
    }

private:
    UrlImportResult& result_;
    std::size_t limit_;
    std::string base_;
    std::unordered_map<std::string, std::size_t> byUrl_;
    int titlePriority_ = 0, descriptionPriority_ = 0;
    std::optional<std::size_t> ogImage_, ogVideo_, ogAudio_, twitterImage_, twitterVideo_;

    std::optional<std::size_t> add(std::string_view value, std::string kind, std::string mime,
                                   std::optional<std::string> alt, std::string origin, bool preview) {
        auto resolved = assetUrl(base_, value);
        if (!resolved) {
            if (!trim(value).empty()) issue(result_.issues, "unsupported_media_reference", "A media reference could not be resolved to an HTTP(S) or supported data URL.");
            return std::nullopt;
        }
        if (auto found = byUrl_.find(*resolved); found != byUrl_.end()) {
            auto index = found->second;
            auto& media = result_.post.media[index]; auto& info = result_.mediaInfo[index];
            if (media.kind == "unknown" && kind != "unknown") media.kind = std::move(kind);
            if (media.mimeType.empty()) media.mimeType = std::move(mime);
            if (!media.altText && alt) media.altText = std::move(alt);
            if (std::find(info.origins.begin(), info.origins.end(), origin) == info.origins.end()) info.origins.push_back(std::move(origin));
            info.preview = info.preview && preview;
            return index;
        }
        if (result_.post.media.size() >= limit_) {
            issue(result_.issues, "media_candidate_limit_reached", "Not all media candidates fit in the configured limit.", true);
            return std::nullopt;
        }
        auto index = result_.post.media.size();
        result_.post.media.push_back({{}, *resolved, std::move(kind), std::move(mime), {}, std::move(alt)});
        result_.mediaInfo.push_back({{std::move(origin)}, preview});
        byUrl_.emplace(std::move(*resolved), index);
        return index;
    }

    void setTitle(std::string value, int priority) {
        value = trim(value);
        if (!value.empty() && priority > titlePriority_) { result_.post.title = std::move(value); titlePriority_ = priority; }
    }
    void setDescription(std::string value, int priority) {
        value = trim(value);
        if (!value.empty() && priority > descriptionPriority_) { result_.post.description = std::move(value); descriptionPriority_ = priority; }
    }
    void structured(std::optional<std::size_t> index, std::string_view property, const std::string& value) {
        if (!index || *index >= result_.post.media.size()) return;
        auto& media = result_.post.media[*index];
        if (property == "alt" && !media.altText) media.altText = value;
        if (property == "type" && media.mimeType.empty()) media.mimeType = value;
    }

    void meta(xmlNode* node) {
        auto key = lower(attribute(node, "property").value_or(attribute(node, "name").value_or(attribute(node, "itemprop").value_or(""))));
        auto content = attribute(node, "content").value_or("");
        if (key == "og:title") setTitle(content, 3);
        else if (key == "twitter:title") setTitle(content, 2);
        else if (key == "og:description") setDescription(content, 3);
        else if (key == "twitter:description") setDescription(content, 2);
        else if (key == "description") setDescription(content, 1);
        else if (key == "og:image" || key == "og:image:url" || key == "og:image:secure_url") ogImage_ = add(content, "image", {}, {}, "meta:" + key, true);
        else if (key == "og:video" || key == "og:video:url" || key == "og:video:secure_url") ogVideo_ = add(content, "video", {}, {}, "meta:" + key, true);
        else if (key == "og:audio" || key == "og:audio:url" || key == "og:audio:secure_url") ogAudio_ = add(content, "audio", {}, {}, "meta:" + key, true);
        else if (key == "twitter:image" || key == "twitter:image:src") twitterImage_ = add(content, "image", {}, {}, "meta:" + key, true);
        else if (key == "twitter:player:stream") twitterVideo_ = add(content, "video", {}, {}, "meta:" + key, true);
        else if (key == "og:image:alt") structured(ogImage_, "alt", content);
        else if (key == "og:image:type") structured(ogImage_, "type", content);
        else if (key == "og:video:type") structured(ogVideo_, "type", content);
        else if (key == "og:audio:type") structured(ogAudio_, "type", content);
        else if (key == "twitter:image:alt") structured(twitterImage_, "alt", content);
        else if (key == "twitter:player:stream:content_type") structured(twitterVideo_, "type", content);
        else if (key == "image" || key == "thumbnailurl") add(content, "image", {}, {}, "meta:itemprop:" + key, true);
    }

    void candidates(xmlNode* node, const char* name, std::string kind, const std::string& origin, bool preview) {
        auto value = attribute(node, name);
        if (!value) return;
        auto alt = attribute(node, "alt");
        auto mime = attribute(node, "type").value_or("");
        for (const auto& item : srcset(*value)) add(item, kind, mime, alt, origin, preview);
    }

    void direct(xmlNode* node, const char* name, std::string kind, const std::string& origin, bool preview) {
        if (auto value = attribute(node, name))
            add(*value, std::move(kind), attribute(node, "type").value_or(""), attribute(node, "alt"), origin, preview);
    }

    void jsonMediaValue(const Json& value, std::string kind, bool preview, std::string_view origin) {
        std::vector<const Json*> stack{&value};
        while (!stack.empty()) {
            const auto* item = stack.back(); stack.pop_back();
            if (item->is_string()) add(item->get<std::string>(), kind, {}, {}, std::string(origin), preview);
            else if (item->is_array()) for (auto it = item->rbegin(); it != item->rend(); ++it) stack.push_back(&*it);
            else if (item->is_object()) {
                auto url = item->find("contentUrl");
                if (url == item->end()) url = item->find("url");
                if (url != item->end() && url->is_string()) {
                    auto encoding = item->find("encodingFormat");
                    auto mime = encoding != item->end() && encoding->is_string() ? encoding->get<std::string>() : std::string{};
                    add(url->get<std::string>(), kind, std::move(mime), {}, std::string(origin), preview);
                }
            }
        }
    }

    void jsonLd(xmlNode* node) {
        auto data = textContent(node);
        if (data.size() > maxJsonLdBytes || !boundedJsonDepth(data)) {
            issue(result_.issues, "jsonld_not_parsed", "JSON-LD exceeded the parsing limit or depth.");
            return;
        }
        const auto parsed = Json::parse(data, nullptr, false);
        if (parsed.is_discarded()) {
            issue(result_.issues, "jsonld_not_parsed", "A JSON-LD block could not be parsed.");
            return;
        }
        std::vector<const Json*> stack{&parsed};
        while (!stack.empty()) {
            const auto* item = stack.back(); stack.pop_back();
            if (item->is_array()) {
                for (auto it = item->rbegin(); it != item->rend(); ++it) stack.push_back(&*it);
            } else if (item->is_object()) {
                auto image = item->find("image");
                if (image != item->end()) jsonMediaValue(*image, "image", true, "jsonld:image");
                auto thumbnail = item->find("thumbnailUrl");
                if (thumbnail != item->end()) jsonMediaValue(*thumbnail, "image", true, "jsonld:thumbnailUrl");
                auto type = item->find("@type");
                std::vector<std::string> types;
                if (type != item->end() && type->is_string()) types.push_back(type->get<std::string>());
                else if (type != item->end() && type->is_array())
                    for (const auto& value : *type) if (value.is_string()) types.push_back(value.get<std::string>());
                std::string kind;
                for (const auto& name : types) {
                    if (name == "ImageObject" || name.ends_with("/ImageObject")) kind = "image";
                    else if (name == "VideoObject" || name.ends_with("/VideoObject")) kind = "video";
                    else if (name == "AudioObject" || name.ends_with("/AudioObject")) kind = "audio";
                }
                auto content = item->find("contentUrl");
                if (!kind.empty() && content != item->end() && content->is_string()) {
                    auto encoding = item->find("encodingFormat");
                    auto mime = encoding != item->end() && encoding->is_string() ? encoding->get<std::string>() : std::string{};
                    add(content->get<std::string>(), kind, std::move(mime), {}, "jsonld:contentUrl", false);
                } else if (!kind.empty() && content != item->end()) jsonMediaValue(*content, kind, false, "jsonld:contentUrl");
                else if (kind == "image") {
                    auto url = item->find("url");
                    if (url != item->end()) jsonMediaValue(*url, kind, true, "jsonld:url");
                }
                for (auto it = item->begin(); it != item->end(); ++it) if (it->is_object() || it->is_array()) stack.push_back(&*it);
            }
        }
    }

    void inlineStyle(xmlNode* node) {
        auto style = attribute(node, "style");
        if (!style) return;
        const auto low = lower(*style);
        std::size_t pos = 0;
        while ((pos = low.find("url", pos)) != std::string::npos) {
            const auto token = pos;
            pos += 3;
            if (token && (std::isalnum(static_cast<unsigned char>(low[token - 1])) || low[token - 1] == '-' || low[token - 1] == '_')) continue;
            while (pos < low.size() && std::isspace(static_cast<unsigned char>(low[pos]))) ++pos;
            if (pos == low.size() || low[pos] != '(') continue;
            ++pos;
            while (pos < low.size() && std::isspace(static_cast<unsigned char>(low[pos]))) ++pos;
            const char quote = pos < low.size() && (low[pos] == '\'' || low[pos] == '"') ? low[pos++] : 0;
            const auto start = pos;
            bool escaped = false;
            while (pos < low.size() && (quote ? low[pos] != quote : low[pos] != ')')) {
                if (low[pos] == '\\') { escaped = true; if (pos + 1 < low.size()) ++pos; }
                ++pos;
            }
            if (pos == low.size()) break;
            auto reference = trim(std::string_view(*style).substr(start, pos - start));
            if (quote) {
                ++pos;
                while (pos < low.size() && std::isspace(static_cast<unsigned char>(low[pos]))) ++pos;
                if (pos == low.size() || low[pos] != ')') continue;
            }
            ++pos;
            if (escaped) { issue(result_.issues, "css_escape_not_parsed", "An escaped inline CSS URL requires host review."); continue; }
            if (reference.empty() || reference.front() == '#') continue;
            auto kind = extensionKind(reference);
            const auto prefix = lower(reference.substr(0, std::min<std::size_t>(reference.size(), 16)));
            if (prefix.starts_with("data:image/")) kind = "image";
            else if (prefix.starts_with("data:video/")) kind = "video";
            else if (prefix.starts_with("data:audio/")) kind = "audio";
            if (kind.empty()) kind = "unknown";
            add(reference, std::move(kind), {}, {}, "style:url", true);
        }
    }

    void visit(xmlNode* node) {
        const auto name = tag(node);
        inlineStyle(node);
        if (name == "html") result_.post.language = attribute(node, "lang").value_or("");
        else if (name == "title") setTitle(textContent(node), 1);
        else if (name == "meta") meta(node);
        else if (name == "script" && lower(attribute(node, "type").value_or("")) == "application/ld+json") jsonLd(node);
        else if (name == "img") {
            direct(node, "src", "image", "img:src", false);
            direct(node, "data-src", "image", "img:data-src", false);
            direct(node, "data-original", "image", "img:data-original", false);
            direct(node, "data-lazy-src", "image", "img:data-lazy-src", false);
            candidates(node, "srcset", "image", "img:srcset", false);
            candidates(node, "data-srcset", "image", "img:data-srcset", false);
        } else if (name == "video" || name == "audio") {
            direct(node, "src", name, name + ":src", false);
            if (name == "video") direct(node, "poster", "image", "video:poster", true);
        } else if (name == "source") {
            const auto parent = tag(node->parent);
            const auto mime = lower(attribute(node, "type").value_or(""));
            std::string kind = parent == "picture" || mime.starts_with("image/") ? "image" :
                               parent == "video" || mime.starts_with("video/") ? "video" :
                               parent == "audio" || mime.starts_with("audio/") ? "audio" : "unknown";
            direct(node, "src", kind, "source:src", false);
            candidates(node, "srcset", kind, "source:srcset", false);
        } else if (name == "link") {
            auto relation = lower(attribute(node, "rel").value_or(""));
            auto itemprop = lower(attribute(node, "itemprop").value_or(""));
            if (relation == "image_src" || itemprop == "image" || itemprop == "thumbnailurl")
                direct(node, "href", "image", "link:" + (relation.empty() ? itemprop : relation), true);
        } else if (name == "a") {
            auto href = attribute(node, "href");
            if (href) {
                auto kind = extensionKind(*href);
                if (!kind.empty()) add(*href, std::move(kind), {}, {}, "a:href", false);
            }
        }
    }
};

UrlImportResult parse(std::string_view documentUrl, std::string_view html, const UrlImportOptions& options,
                      const std::string& encoding, bool requireUtf8) {
    UrlImportResult result; result.requestedUrl = std::string(documentUrl);
    if (!initializeUrlImport()) { result.error = UrlImportError::TransportFailure; result.message = "URL import could not be initialized."; return result; }
    auto normalized = webUrl({}, documentUrl, true);
    if (!normalized) { result.error = UrlImportError::InvalidUrl; result.message = "A valid HTTP(S) page URL is required."; return result; }
    result.finalUrl = *normalized;
    result.post.platformId = "web"; result.post.id = *normalized; result.post.url = *normalized;
    if (html.size() > std::min(options.maxHtmlBytes, hardHtmlLimit) || html.size() > INT_MAX) {
        result.error = UrlImportError::InputTooLarge; result.message = "HTML exceeds the configured size limit."; return result;
    }
    if (html.empty() || html.find('\0') != html.npos) {
        result.error = UrlImportError::InvalidHtml; result.message = "The page HTML is empty or contains a null byte."; return result;
    }
    const std::string bytes(html);
    const auto isUtf8 = xmlCheckUTF8(reinterpret_cast<const xmlChar*>(bytes.c_str())) != 0;
    if (requireUtf8 && !isUtf8) {
        result.error = UrlImportError::InvalidHtml; result.message = "The supplied DOM must be valid UTF-8."; return result;
    }
    const char* inputEncoding = !encoding.empty() ? encoding.c_str() : isUtf8 ? "UTF-8" : nullptr;
    const int flags = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET | HTML_PARSE_NODEFDTD;
    XmlDoc doc(htmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, inputEncoding, flags), xmlFreeDoc);
    if (!doc || !xmlDocGetRootElement(doc.get())) {
        result.error = UrlImportError::InvalidHtml; result.message = "The HTML document could not be parsed."; return result;
    }
    DomCollector(result, options).collect(doc.get());
    return result;
}

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<std::string> percentDecode(std::string_view text) {
    std::string output; output.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') { output.push_back(text[i]); continue; }
        if (i + 2 >= text.size()) return std::nullopt;
        const int high = hexDigit(text[i + 1]), low = hexDigit(text[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        output.push_back(static_cast<char>((high << 4) | low)); i += 2;
    }
    return output;
}

std::optional<std::string> decodeBase64(std::string_view text) {
    auto paddingStart = text.find('=');
    const auto dataSize = paddingStart == text.npos ? text.size() : paddingStart;
    const auto padding = text.size() - dataSize;
    if (padding > 2 || (padding && (text.size() % 4 != 0 ||
        (dataSize % 4 == 2 ? padding != 2 : dataSize % 4 == 3 ? padding != 1 : true))) || dataSize % 4 == 1)
        return std::nullopt;
    for (std::size_t i = dataSize; i < text.size(); ++i) if (text[i] != '=') return std::nullopt;
    std::string output; output.reserve(dataSize / 4 * 3 + 2);
    unsigned accumulator = 0, bits = 0;
    for (std::size_t i = 0; i < dataSize; ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        int digit = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 :
                    ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : ch == '/' ? 63 : -1;
        if (digit < 0) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<unsigned>(digit); bits += 6;
        if (bits >= 8) { bits -= 8; output.push_back(static_cast<char>((accumulator >> bits) & 0xff)); }
        accumulator &= bits ? (1U << bits) - 1 : 0;
    }
    if (accumulator != 0) return std::nullopt;
    return output;
}

bool mediaMime(std::string_view type) {
    return type.empty() || type.starts_with("image/") || type.starts_with("video/") ||
           type.starts_with("audio/") || type == "application/octet-stream" || type == "application/ogg";
}
} // namespace

UrlImportResult parsePageDom(std::string_view documentUrl, std::string_view html, const UrlImportOptions& options) {
    return parse(documentUrl, html, options, "UTF-8", true);
}

UrlImportResult selectUrlMedia(const UrlImportResult& page, const std::vector<std::size_t>& indices) {
    auto result = page;
    if (!result.ok()) return result;
    if (page.mediaInfo.size() != page.post.media.size()) {
        result.error = UrlImportError::InvalidSelection; result.message = "Media candidates and provenance do not match."; return result;
    }
    std::vector<ExternalMedia> selected;
    std::vector<UrlMediaInfo> selectedInfo;
    std::vector<std::size_t> seen;
    selected.reserve(indices.size()); selectedInfo.reserve(indices.size());
    for (const auto index : indices) {
        if (index >= page.post.media.size() || std::find(seen.begin(), seen.end(), index) != seen.end()) {
            result.error = UrlImportError::InvalidSelection; result.message = "A media selection index is invalid or duplicated."; return result;
        }
        seen.push_back(index); selected.push_back(page.post.media[index]); selectedInfo.push_back(page.mediaInfo[index]);
    }
    result.post.media = std::move(selected); result.mediaInfo = std::move(selectedInfo);
    return result;
}

UrlImportResult importUrl(std::string_view input, Context& context, const UrlImportOptions& options) {
    UrlImportResult result; result.requestedUrl = std::string(input);
    if (!initializeUrlImport()) { result.error = UrlImportError::TransportFailure; result.message = "URL import could not be initialized."; return result; }
    auto current = webUrl({}, input, true);
    if (!current) { result.error = UrlImportError::InvalidUrl; result.message = "A valid HTTP(S) page URL is required."; return result; }
    std::vector<std::string> visited;
    for (unsigned redirects = 0;; ++redirects) {
        result.finalUrl = *current;
        if (context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Page discovery was cancelled."; return result; }
        if (std::find(visited.begin(), visited.end(), *current) != visited.end()) {
            result.error = UrlImportError::RedirectLimit; result.message = "The page redirect loop was stopped."; return result;
        }
        visited.push_back(*current);
        if (options.allowPageUrl) {
            bool allowed = false;
            try { allowed = options.allowPageUrl(*current); } catch (...) { allowed = false; }
            if (!allowed) { result.error = UrlImportError::UrlBlocked; result.message = "The host declined the page URL."; return result; }
        }
        Request request; request.url = *current; request.headers = {{"Accept", "text/html, application/xhtml+xml"}};
        request.maxResponseBytes = std::min(options.maxHtmlBytes, hardHtmlLimit);
        if (!request.maxResponseBytes) { result.error = UrlImportError::InputTooLarge; result.message = "The HTML size limit is zero."; return result; }
        Response response;
        try { response = context.transport.send(request, context.stop); }
        catch (...) { result.error = UrlImportError::TransportFailure; result.message = "The page could not be fetched."; return result; }
        result.httpStatus = response.status;
        if (response.cancelled || context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Page discovery was cancelled."; return result; }
        if (response.responseLimitExceeded) { result.error = UrlImportError::InputTooLarge; result.message = "HTML exceeds the configured size limit."; return result; }
        if (response.transportFailed) { result.error = UrlImportError::TransportFailure; result.message = "The page could not be fetched."; return result; }
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            if (redirects >= std::min(options.maxRedirects, 10U)) { result.error = UrlImportError::RedirectLimit; result.message = "The page redirect limit was reached."; return result; }
            const auto location = header(response.headers, "location");
            auto next = location ? webUrl(*current, *location, true) : std::nullopt;
            if (!next) { result.error = UrlImportError::InvalidUrl; result.message = "The redirect URL is invalid or unsupported."; return result; }
            current = std::move(next);
            continue;
        }
        if (response.status < 200 || response.status >= 300) { result.error = UrlImportError::HttpError; result.message = "The page request was rejected."; return result; }
        auto contentType = header(response.headers, "content-type").value_or("");
        auto mediaType = lower(trim(contentType.substr(0, contentType.find(';'))));
        if (!mediaType.empty() && mediaType != "text/html" && mediaType != "application/xhtml+xml") {
            result.error = UrlImportError::UnsupportedContentType; result.message = "The response is not an HTML page."; return result;
        }
        auto parsed = parse(*current, response.body, options, charset(contentType), false);
        parsed.requestedUrl = result.requestedUrl; parsed.httpStatus = response.status;
        return parsed;
    }
}

MigrationResult importPost(const UrlImportResult& page, const MigrationOptions& options) {
    auto result = importPost(page.post, options);
    if (!page.ok()) issue(result.issues, "url_import_failed", "The page could not be imported.", true);
    for (const auto& item : page.issues) issue(result.issues, item.code, item.message, item.blocking);
    return result;
}

MediaFetchResult fetchMedia(const ExternalMedia& media, Context& context,
                            const std::function<bool(std::string_view)>& sink, const UrlMediaOptions& options) {
    MediaFetchResult result;
    if (!initializeUrlImport()) { result.error = UrlImportError::TransportFailure; result.message = "URL import could not be initialized."; return result; }
    if (!sink) { result.error = UrlImportError::SinkFailure; result.message = "A media sink is required."; return result; }
    if (context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Media fetch was cancelled."; return result; }
    const auto limit = std::min(options.maxBytes, hardDownloadLimit);
    if (!limit) { result.error = UrlImportError::InputTooLarge; result.message = "The media size limit is zero."; return result; }
    bool sinkFailed = false, exceeded = false;
    auto write = [&](std::string_view chunk) {
        if (context.stop.stop_requested() || sinkFailed || exceeded) return false;
        if (chunk.size() > limit - result.bytesWritten) { exceeded = true; return false; }
        try { if (!sink(chunk)) { sinkFailed = true; return false; } }
        catch (...) { sinkFailed = true; return false; }
        result.bytesWritten += chunk.size();
        return true;
    };
    const auto source = trim(media.sourceUrl);
    if (lower(source.substr(0, std::min<std::size_t>(source.size(), 5))) == "data:") {
        if (source.size() > hardHtmlLimit) { result.error = UrlImportError::InputTooLarge; result.message = "The data URL is too large."; return result; }
        const auto comma = source.find(',');
        if (comma == source.npos || hasControl(source)) { result.error = UrlImportError::InvalidMedia; result.message = "The data URL is invalid."; return result; }
        const auto metadata = lower(source.substr(5, comma - 5));
        const auto semicolon = metadata.find(';');
        result.mimeType = metadata.substr(0, semicolon);
        if (!result.mimeType.starts_with("image/") && !result.mimeType.starts_with("video/") && !result.mimeType.starts_with("audio/")) {
            result.error = UrlImportError::InvalidMedia; result.message = "The data URL does not contain supported media."; return result;
        }
        const bool base64 = metadata.ends_with(";base64");
        auto decoded = percentDecode(std::string_view(source).substr(comma + 1));
        if (decoded && base64) decoded = decodeBase64(*decoded);
        if (!decoded || decoded->empty()) { result.error = UrlImportError::InvalidMedia; result.message = "The data URL could not be decoded."; return result; }
        if (decoded->size() > limit) { result.error = UrlImportError::InputTooLarge; result.message = "Media exceeds the configured size limit."; return result; }
        constexpr std::size_t chunkSize = 64 * 1024;
        for (std::size_t pos = 0; pos < decoded->size(); pos += chunkSize) {
            if (!write(std::string_view(*decoded).substr(pos, chunkSize))) break;
        }
        if (context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Media fetch was cancelled."; }
        else if (exceeded) { result.error = UrlImportError::InputTooLarge; result.message = "Media exceeds the configured size limit."; }
        else if (sinkFailed) { result.error = UrlImportError::SinkFailure; result.message = "The media sink rejected data."; }
        return result;
    }
    auto current = webUrl({}, source, true);
    if (!current) { result.error = UrlImportError::InvalidUrl; result.message = "A valid HTTP(S) media URL is required."; return result; }
    std::vector<std::string> visited;
    for (unsigned redirects = 0;; ++redirects) {
        result.finalUrl = *current;
        if (context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Media fetch was cancelled."; return result; }
        if (std::find(visited.begin(), visited.end(), *current) != visited.end()) {
            result.error = UrlImportError::RedirectLimit; result.message = "The media redirect loop was stopped."; return result;
        }
        visited.push_back(*current);
        if (options.allowMediaUrl) {
            bool allowed = false;
            try { allowed = options.allowMediaUrl(*current); } catch (...) { allowed = false; }
            if (!allowed) { result.error = UrlImportError::UrlBlocked; result.message = "The host declined the media URL."; return result; }
        }
        Request request; request.url = *current; request.headers = {{"Accept", "image/*, video/*, audio/*, application/octet-stream"}};
        request.maxResponseBytes = limit; request.responseSink = write;
        const auto before = result.bytesWritten;
        Response response;
        try { response = context.transport.send(request, context.stop); }
        catch (...) { result.error = UrlImportError::TransportFailure; result.message = "The media could not be fetched."; return result; }
        result.httpStatus = response.status;
        if (response.cancelled || context.stop.stop_requested()) { result.error = UrlImportError::Cancelled; result.message = "Media fetch was cancelled."; return result; }
        if (response.responseLimitExceeded || exceeded) { result.error = UrlImportError::InputTooLarge; result.message = "Media exceeds the configured size limit."; return result; }
        if (response.responseSinkFailed || sinkFailed) { result.error = UrlImportError::SinkFailure; result.message = "The media sink rejected data."; return result; }
        if (response.transportFailed) { result.error = UrlImportError::TransportFailure; result.message = "The media could not be fetched."; return result; }
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            if (result.bytesWritten != before) { result.error = UrlImportError::InvalidMedia; result.message = "The transport streamed a redirect body."; return result; }
            if (redirects >= std::min(options.maxRedirects, 10U)) { result.error = UrlImportError::RedirectLimit; result.message = "The media redirect limit was reached."; return result; }
            const auto location = header(response.headers, "location");
            auto next = location ? webUrl(*current, *location, true) : std::nullopt;
            if (!next) { result.error = UrlImportError::InvalidUrl; result.message = "The media redirect URL is invalid or unsupported."; return result; }
            current = std::move(next);
            continue;
        }
        if (response.status < 200 || response.status >= 300) { result.error = UrlImportError::HttpError; result.message = "The media request was rejected."; return result; }
        // A legacy injected transport may return a buffered body rather than
        // invoke the sink. Reject transports that both stream and buffer.
        if (!response.body.empty()) {
            if (result.bytesWritten != before) { result.error = UrlImportError::InvalidMedia; result.message = "The transport returned duplicate media data."; return result; }
            if (!write(response.body)) {
                result.error = context.stop.stop_requested() ? UrlImportError::Cancelled : exceeded ? UrlImportError::InputTooLarge : UrlImportError::SinkFailure;
                result.message = "The media sink could not accept data."; return result;
            }
        }
        auto contentType = header(response.headers, "content-type").value_or("");
        result.mimeType = lower(trim(contentType.substr(0, contentType.find(';'))));
        if (!mediaMime(result.mimeType)) { result.error = UrlImportError::UnsupportedContentType; result.message = "The response is not a supported media type."; return result; }
        if (result.bytesWritten == 0) { result.error = UrlImportError::InvalidMedia; result.message = "The media response was empty."; }
        return result;
    }
}
} // namespace iiSocialIntegration
