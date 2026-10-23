#include <iiSocialPublish/Review.h>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace iiSocialPublish {
namespace {
void append(std::string& into, const std::string& value) {
    if (value.empty()) return;
    if (!into.empty()) into += '\n';
    into += value;
}
// Conservative X weighting: non-ASCII code points cost two. This may split a
// little earlier than twitter-text (emoji sequences/Latin extensions), never
// claims to reproduce its grapheme normalization. HTTPS tokens cost 23.
std::vector<std::pair<std::size_t, unsigned>> units(const std::string& value, bool x) {
    std::vector<std::pair<std::size_t, unsigned>> out;
    for (std::size_t i = 0; i < value.size();) {
        if (x && (i == 0 || std::isspace(static_cast<unsigned char>(value[i-1]))) &&
            (value.compare(i, 8, "https://") == 0 || value.compare(i, 7, "http://") == 0)) {
            auto end = value.find_first_of(" \r\n\t", i);
            i = end == value.npos ? value.size() : end;
            out.emplace_back(i, 23); continue;
        }
        auto c = static_cast<unsigned char>(value[i]);
        auto length = c < 0x80 ? 1u : c < 0xe0 ? 2u : c < 0xf0 ? 3u : 4u;
        i += std::min<std::size_t>(length, value.size()-i);
        out.emplace_back(i, x && c >= 0x80 ? 2 : 1);
    }
    return out;
}
std::vector<std::string> split(const std::string& value, unsigned limit, bool x) {
    if (value.empty()) return {""};
    std::vector<std::string> out;
    std::size_t start = 0, last = 0;
    unsigned count = 0;
    for (auto [end, cost] : units(value, x)) {
        if (count + cost > limit && last > start) {
            out.push_back(value.substr(start, last-start)); start = last; count = 0;
        }
        count += cost; last = end;
    }
    if (last > start) out.push_back(value.substr(start));
    return out;
}
unsigned measure(const std::string& value, bool x) {
    unsigned count = 0; for (auto [end, cost] : units(value,x)) { (void)end; count += cost; } return count;
}
bool nativeLink(Platform p) { return p == Platform::Facebook || p == Platform::Pinterest; }
bool isolated(Platform p, const Media& m) {
    return (m.kind == MediaKind::Video || m.mimeType == "image/gif") &&
        p != Platform::Instagram && p != Platform::Threads && p != Platform::Tumblr &&
        p != Platform::Pixiv && p != Platform::ArtStation;
}
Outcome invalid(std::string message, Error error = Error::InvalidRequest) {
    Outcome out; out.error = error; out.message = std::move(message); return out;
}
} // namespace

bool PublicationPlan::ready() const noexcept {
    return !posts.empty() && std::ranges::none_of(issues, &ReviewIssue::blocking);
}
ReviewPolicy reviewPolicy(Platform p) noexcept {
    switch (p) {
    case Platform::X: return {4, 280, true, false, false, true};
    case Platform::Facebook: return {10, 60000, false, false, false, true};
    case Platform::Instagram: return {10, 2200, false, false, false, false};
    case Platform::Threads: return {20, 500, true, false, false, true};
    case Platform::Pinterest: return {1, 800, false, true, false, false};
    case Platform::Tumblr: return {10, 60000, false, true, true, true};
    case Platform::Mastodon: return {4, 500, true, false, false, true};
    case Platform::LinkedIn: return {20, 3000, false, false, false, true};
    case Platform::YouTube: return {1, 5000, false, true, true, false};
    case Platform::Pixiv: case Platform::ArtStation: return {1000, 60000, false, true, true, false};
    }
    return {};
}
PublicationPlan planPublication(Platform platform, const ReviewSource& source, const ReviewOptions& options) {
    PublicationPlan plan; plan.platform = platform; plan.scheduledAt = source.scheduledAt;
    auto issue = [&](std::string code, std::string message, bool blocking = false) {
        plan.issues.push_back({std::move(code), std::move(message), blocking});
    };
    const auto policy = reviewPolicy(platform);
    std::size_t metadataBytes = source.post.text.size()+source.post.title.size()+source.post.link.size();
    for (const auto& caption : source.captions) metadataBytes += caption.size();
    for (const auto& media : source.post.media) metadataBytes += media.altText.size();
    for (const auto& tag : source.post.tags) metadataBytes += tag.size();
    if (!policy.mediaPerPost || source.post.media.size() > 1000 || source.post.text.size() > 256*1024 || metadataBytes > 1024*1024) {
        issue("shape", "Publication exceeds the review safety budget.", true); return plan;
    }
    try { (void)nlohmann::json{{"text", source.post.text}, {"title", source.post.title},
        {"tags", source.post.tags}, {"captions", source.captions}}.dump(); }
    catch (...) { issue("utf8", "Publication text must be valid UTF-8.", true); return plan; }
    const bool manual = platform == Platform::Pixiv || platform == Platform::ArtStation;
    if (manual) issue("manual", "Finish on the destination website; no verified official publishing API.", true);
    if (!source.scheduledAt.empty()) issue("schedule", "Scheduled time is preserved. A scheduler must dispatch this plan when due; immediate upload is disabled.", true);
    const auto cap = options.mediaPerPost ? options.mediaPerPost : policy.mediaPerPost;
    if (cap == 0 || cap > policy.mediaPerPost) {
        issue("media_limit", "Attachments per post exceed the adapter policy.", true); return plan;
    }
    if (options.sequence == Sequence::Thread && !policy.threads) issue("thread", "This destination does not support a reply chain in this adapter.", true);
    if (platform == Platform::Mastodon) issue("instance", "Review uses 4 attachments / 500 characters by default; server limits are checked again before uploading.");
    if (platform == Platform::X) issue("x_access", "X requests require provisioned billable access; reply chains also require verified native account reply entitlement.");
    if (platform == Platform::Instagram || platform == Platform::Threads) issue("public_media", "Exported media must have public HTTPS URLs before this API can fetch them.");
    if (source.post.visibility != "public" && platform != Platform::Mastodon && platform != Platform::YouTube)
        issue("visibility", "The inherited visibility is not supported by this adapter. Select a destination override explicitly.", true);
    if (platform == Platform::YouTube && (source.post.media.empty() || std::ranges::any_of(source.post.media, [](auto& m) { return m.kind != MediaKind::Video; })))
        issue("video", "YouTube requires exported video; still canvases are not silently converted into video.", true);
    if ((platform == Platform::Pinterest || platform == Platform::YouTube) && units(source.post.title, false).size() > 100)
        issue("title", "The destination title exceeds 100 characters; edit the destination override.", true);
    if (platform == Platform::YouTube && source.post.title.empty()) issue("title", "YouTube requires a title.", true);

    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < source.post.media.size(); ++i) {
        const bool alone = isolated(platform, source.post.media[i]);
        if (groups.empty() || groups.back().size() >= cap || alone || isolated(platform, source.post.media[groups.back().front()])) groups.emplace_back();
        groups.back().push_back(i);
    }
    if (groups.empty()) groups.emplace_back();
    if (!policy.textOnly && source.post.media.empty()) issue("media", "This destination requires media.", true);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        PlannedPost item; item.post = source.post; item.post.media.clear(); item.sourceMedia = groups[g];
        std::string body;
        if (g == 0 || options.repeatIntroduction) {
            if (!policy.nativeTitle) append(body, source.post.title);
            append(body, source.post.text);
            if (!nativeLink(platform)) append(body, source.post.link);
            if (!policy.nativeTags) for (auto tag : source.post.tags) {
                if (tag.empty()) continue;
                if (tag.front() != '#') tag.insert(tag.begin(), '#');
                append(body, tag);
            }
        }
        for (auto index : groups[g]) {
            item.post.media.push_back(source.post.media[index]);
            if (options.includeCaptions && index < source.captions.size()) append(body, source.captions[index]);
        }
        if (!nativeLink(platform)) item.post.link.clear();
        if (!policy.nativeTags) item.post.tags.clear();
        const auto reserve = options.numberParts && (groups.size()>1 || measure(body,platform==Platform::X)>policy.textLimit) ? 14u : 0u;
        auto chunks = split(body, policy.textLimit-reserve, platform == Platform::X);
        if (platform == Platform::YouTube && body.size()>5000) issue("text_limit", "YouTube description exceeds 5000 UTF-8 bytes.", true);
        if (chunks.size() > 1 && (!policy.textOnly || !options.splitText)) {
            issue("text_limit", "Text exceeds the destination budget. Edit its override or enable text splitting on a destination that supports text posts.", true);
            chunks = {body};
        }
        for (std::size_t c = 0; c < chunks.size(); ++c) {
            if (plan.posts.size()>=1000) { issue("steps", "Too many publication steps.", true); return plan; }
            auto part = item;
            if (c > 0) { part.post.media.clear(); part.sourceMedia.clear(); }
            part.post.text = std::move(chunks[c]);
            plan.posts.push_back(std::move(part));
        }
    }
    if (options.sequence == Sequence::Single && plan.posts.size() > 1)
        issue("single", "A single post cannot contain all content. Choose automatic grouping, separate posts or a supported thread.", true);
    const bool thread = policy.threads && (options.sequence == Sequence::Automatic || options.sequence == Sequence::Thread);
    for (std::size_t i = 0; i < plan.posts.size(); ++i) {
        auto& item = plan.posts[i];
        if (thread && i) item.replyTo = i-1;
        if (plan.posts.size() > 1 && options.numberParts) append(item.post.text, "("+std::to_string(i+1)+"/"+std::to_string(plan.posts.size())+")");
        if (!source.post.idempotencyKey.empty()) item.post.idempotencyKey = source.post.idempotencyKey+"/"+std::to_string(i+1);
    }
    if (plan.posts.size() > 1000) issue("steps", "Too many publication steps.", true);
    return plan;
}

SequenceOutcome publishPlan(const PublicationPlan& plan, const Account& account, Context& context) {
    SequenceOutcome out;
    if (!plan.ready() || !plan.scheduledAt.empty()) { out.result = invalid("Resolve blocking review issues before publishing"); return out; }
    const bool chained = std::ranges::any_of(plan.posts, [](auto& p) { return p.replyTo.has_value(); });
    if (chained && plan.platform == Platform::X && !account.allowSelfThreadReplies) {
        out.result = invalid("X reply entitlement has not been verified for this account", Error::AccessNotConfigured); return out;
    }
    for (std::size_t i = 0; i < plan.posts.size(); ++i) {
        if (plan.posts[i].replyTo && (*plan.posts[i].replyTo >= i || !reviewPolicy(plan.platform).threads)) {
            out.result = invalid("Invalid reply dependency"); return out;
        }
        auto checked = validatePost(plan.platform, plan.posts[i].post, account);
        if (checked.error != Error::None) { out.result = std::move(checked); return out; }
    }
    // Server-dependent checks happen before the first media upload, including
    // later steps. A review never performs this read on the UI thread.
    if (plan.platform == Platform::Mastodon) {
        Request request; request.url = account.instanceUrl+"/api/v2/instance";
        Response response;
        try { response = context.transport.send(request, context.stop); }
        catch (...) { out.result = invalid("Instance limit lookup failed", Error::TransportFailure); return out; }
        if (response.cancelled || context.stop.stop_requested()) { out.result = invalid("Publishing was cancelled", Error::Cancelled); return out; }
        if (response.transportFailed || response.status != 200) { out.result = invalid("Instance limit lookup failed", Error::TransportFailure); return out; }
        try {
            auto limits = nlohmann::json::parse(response.body).at("configuration").at("statuses");
            auto media = limits.at("max_media_attachments").get<unsigned>();
            auto chars = limits.at("max_characters").get<unsigned>();
            for (auto& item : plan.posts) if (item.post.media.size() > media || units(item.post.text+item.post.contentWarning, false).size() > chars) {
                out.result = invalid("Review exceeds this Mastodon instance's limits"); return out;
            }
        } catch (...) { out.result = invalid("Instance returned invalid publication limits", Error::InvalidResponse); return out; }
    }
    auto publisher = makePublisher(plan.platform);
    for (const auto& item : plan.posts) {
        if (context.stop.stop_requested()) { out.result = invalid("Publishing was cancelled", Error::Cancelled); return out; }
        auto post = item.post;
        if (item.replyTo) post.replyToId = out.completed.at(*item.replyTo).postId;
        out.result = publisher->publish(post, account, context);
        if (!out.result.published()) return out;
        out.completed.push_back(out.result); ++out.next;
    }
    return out;
}
} // namespace iiSocialPublish
