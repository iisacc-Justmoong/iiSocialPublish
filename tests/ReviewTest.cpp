#include <iiSocialIntegration/Review.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace iiSocialIntegration;
using Json = nlohmann::json;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fake : Transport {
    unsigned creates = 0, calls = 0, failAt = 0, uploads = 0;
    std::string previous;
    std::vector<Json> bodies;
    Response send(const Request& r, std::stop_token) override {
        ++calls;
        if (r.url.ends_with("/initialize")) return {200, {}, Json{{"data",{{"id",std::to_string(700+(++uploads))}}}}.dump()};
        if (r.url.ends_with("/append")) return {204};
        if (r.url.ends_with("/metadata")) {
            auto body=Json::parse(r.body);
            auto index=std::stoul(body["media_id"].get<std::string>())-701;
            check(body["alt_text"]["text"]=="Alt "+std::to_string(index),"Alt text was assigned to another source image");
            return {204};
        }
        if (r.url.ends_with("/finalize")) return {200, {}, R"({"data":{"id":"700"}})"};
        if (r.url == "https://api.x.com/2/tweets") {
            auto body = Json::parse(r.body); bodies.push_back(body);
            if (!previous.empty()) check(body["reply"]["in_reply_to_tweet_id"] == previous, "Reply did not use the previous acknowledged ID");
            if (++creates == failAt) return {0, {}, {}, true, false, true};
            previous = std::to_string(9000 + creates);
            return {201, {}, Json{{"data", {{"id", previous}}}}.dump()};
        }
        throw std::runtime_error("Unexpected review execution request");
    }
};
struct ThreadsFake : Transport {
    unsigned containers=0, published=0, replies=0;
    std::string previous;
    Response send(const Request& r, std::stop_token) override {
        if (r.method=="GET") return {200,{},R"({"status":"FINISHED"})"};
        if (r.url.ends_with("/threads_publish")) {
            previous=std::to_string(3000+(++published));
            return {200,{},Json{{"id",previous}}.dump()};
        }
        check(r.url.ends_with("/123/threads"),"Unexpected Threads endpoint");
        if (r.body.find("media_type=CAROUSEL")!=r.body.npos) {
            check(r.body.find("reply_control=accounts_you_follow")!=r.body.npos,"Threads reply control missing");
            if (!previous.empty()) { check(r.body.find("reply_to_id="+previous)!=r.body.npos,"Threads carousel reply ID missing"); ++replies; }
        }
        return {200,{},Json{{"id",std::to_string(++containers)}}.dump()};
    }
};
struct MastodonFake : Transport {
    unsigned media=0, published=0, replies=0, limit=4;
    std::string previous;
    Response send(const Request& r, std::stop_token) override {
        if (r.url.ends_with("/api/v2/instance")) return {200,{},Json{{"configuration",{{"statuses",{{"max_media_attachments",limit},{"max_characters",500}}}}}}.dump()};
        if (r.url.ends_with("/api/v2/media")) return {200,{},Json{{"id",std::to_string(++media)},{"url","https://media.example/image.png"}}.dump()};
        check(r.url.ends_with("/api/v1/statuses"),"Unexpected Mastodon endpoint");
        auto body=Json::parse(r.body);
        check(body["language"]=="ko" && body["visibility"]=="unlisted" && body["sensitive"]==true,"Mastodon inherited options missing");
        if (!previous.empty()) { check(body["in_reply_to_id"]==previous,"Mastodon reply ID missing"); ++replies; }
        previous=std::to_string(4000+(++published));
        return {200,{},Json{{"id",previous}}.dump()};
    }
};
int main() {
    const auto path = std::filesystem::current_path()/"review-fixture.png";
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); } } cleanup{path};
    try {
        { std::ofstream file(path, std::ios::binary); file << "pixels"; }
        ReviewSource source; source.post.title = "Society title"; source.post.text = "Society description";
        source.post.idempotencyKey = "original";
        for (int i = 0; i < 20; ++i) {
            source.post.media.push_back({MediaKind::Image, path, "https://media.example/export.png", "image/png", "Alt "+std::to_string(i)});
            source.captions.push_back("Caption "+std::to_string(i));
        }
        auto plan = planPublication(Platform::X, source);
        check(plan.ready() && plan.posts.size() == 5, "20 images must form five X posts");
        for (std::size_t i = 0; i < 5; ++i) {
            check(plan.posts[i].post.media.size() == 4, "X group lost attachments");
            check(plan.posts[i].sourceMedia.front() == i*4 && plan.posts[i].post.media.front().altText == "Alt "+std::to_string(i*4), "Source order/alt text changed");
            check(plan.posts[i].post.text.find("Caption "+std::to_string(i*4)) != std::string::npos, "Caption lost");
            check(i == 0 ? !plan.posts[i].replyTo : *plan.posts[i].replyTo == i-1, "Wrong reply topology");
        }
        check(source.post.media.size() == 20 && source.post.text == "Society description", "Planner mutated the common source");
        auto pins=planPublication(Platform::Pinterest,source);
        check(pins.posts[1].post.text.find("Society description")==std::string::npos,"Pinterest ignored the introduction repeat setting");
        ReviewOptions repeat; repeat.repeatIntroduction=true;
        pins=planPublication(Platform::Pinterest,source,repeat);
        check(pins.posts[19].post.text.find("Society description")!=std::string::npos,"Pinterest repeated introduction was lost");
        auto threadsSource=source; threadsSource.post.replyControl="accounts_you_follow";
        ReviewOptions threadOptions; threadOptions.mediaPerPost=4;
        Account threadsAccount; threadsAccount.accessToken="fixture"; threadsAccount.targetId="123"; threadsAccount.scopes={"threads_basic","threads_content_publish"};
        ThreadsFake threads; Context threadsContext{threads};
        auto threadsResult=publishPlan(planPublication(Platform::Threads,threadsSource,threadOptions),threadsAccount,threadsContext);
        check(threadsResult.published() && threads.published==5 && threads.replies==4,"Threads reply chain/control did not reach the wire");
        auto fediverse=source; fediverse.post.language="ko"; fediverse.post.visibility="unlisted"; fediverse.post.sensitive=true;
        Account fediverseAccount; fediverseAccount.accessToken="fixture"; fediverseAccount.instanceUrl="https://federated.example"; fediverseAccount.scopes={"write"};
        MastodonFake mastodon; Context mastodonContext{mastodon};
        auto mastodonPlan=planPublication(Platform::Mastodon,fediverse);
        check(publishPlan(mastodonPlan,fediverseAccount,mastodonContext).published() && mastodon.published==5 && mastodon.replies==4,"Mastodon chain/options did not reach the wire");
        MastodonFake restricted; restricted.limit=2; Context restrictedContext{restricted};
        check(!publishPlan(mastodonPlan,fediverseAccount,restrictedContext).published() && !restricted.media && !restricted.published,"Instance limits were checked after partial publication");
        for (const auto& [platform, expected] : std::vector<std::pair<Platform, int>>{
                {Platform::Instagram,2},{Platform::Threads,1},{Platform::Facebook,2},{Platform::Pinterest,20},
                {Platform::LinkedIn,1},{Platform::Tumblr,2},{Platform::Mastodon,5}}) {
            auto grouped = planPublication(platform, source);
            check(grouped.ready() && grouped.posts.size() == std::size_t(expected), "Incorrect destination grouping");
            std::size_t images = 0; for (auto& item : grouped.posts) images += item.sourceMedia.size();
            check(images == 20, "Destination dropped overflow media");
        }
        check(!planPublication(Platform::Pixiv, source).ready() && !planPublication(Platform::ArtStation, source).ready(), "Unverified APIs advertised automatic publishing");
        check(!planPublication(Platform::YouTube, source).ready(), "YouTube silently converted images into video");
        ReviewOptions options; options.sequence = Sequence::Single;
        check(!planPublication(Platform::X, source, options).ready(), "Single post accepted 20 images");
        options.sequence = Sequence::Separate;
        auto separate = planPublication(Platform::X, source, options);
        for (auto& item : separate.posts) check(!item.replyTo, "Separate posts linked unexpectedly");
        options.mediaPerPost = 5;
        check(!planPublication(Platform::X, source, options).ready(), "Unsupported batch size accepted");
        auto mixed = source; mixed.post.media[2].kind = MediaKind::Video; mixed.post.media[2].mimeType = "video/mp4";
        auto mixedPlan = planPublication(Platform::X, mixed);
        check(mixedPlan.posts[1].post.media.size() == 1 && mixedPlan.posts[1].sourceMedia[0] == 2, "Mixed video must be isolated without reordering");
        ReviewSource longText; longText.post.text = std::string(800, 'a')+"한글 🌍";
        ReviewSource exact; exact.post.text=std::string(280,'a');
        check(planPublication(Platform::X,exact).posts.size()==1,"Part numbering reduced a single post's text budget");
        exact.post.title=std::string(1024*1024+1,'a');
        check(!planPublication(Platform::X,exact).ready(),"Metadata safety budget was not enforced before grouping");
        options = {}; options.numberParts = false;
        auto longPlan = planPublication(Platform::X, longText, options);
        std::string joined; for (auto& item : longPlan.posts) joined += item.post.text;
        check(joined == longText.post.text && longPlan.posts.size() > 1, "Text splitting lost UTF-8 bytes");
        options.splitText = false;
        check(!planPublication(Platform::X, longText, options).ready(), "Disabled text splitting was ignored");
        auto scheduled = source; scheduled.scheduledAt = "2030-01-01T00:00:00Z";
        check(!planPublication(Platform::X, scheduled).ready(), "Scheduled content could publish immediately");
        Account a; a.accessToken = "fixture"; a.scopes = {"tweet.read","tweet.write","users.read","media.write"}; a.allowBillableRequests = true;
        Fake noEntitlement; Context first{noEntitlement};
        check(publishPlan(plan, a, first).result.error == Error::AccessNotConfigured && !noEntitlement.calls, "X reply entitlement was bypassed");
        a.allowSelfThreadReplies = true;
        Fake fake; Context context{fake}; auto result = publishPlan(plan, a, context);
        check(result.published() && result.completed.size() == 5 && result.next == 5 && fake.creates == 5, "Full X thread did not publish sequentially");
        for (std::size_t i=0;i<fake.bodies.size();++i) {
            auto ids=fake.bodies[i]["media"]["media_ids"];
            check(ids.size()==4,"Wire post did not contain four media IDs");
            for (std::size_t m=0;m<4;++m) check(ids[m]==std::to_string(701+i*4+m),"Source media order changed on the wire");
        }
        auto invalid = plan; invalid.posts.back().post.media.back().localFile = path.parent_path()/"missing-review.png";
        Fake preflight; Context preflightContext{preflight};
        check(!publishPlan(invalid, a, preflightContext).published() && !preflight.calls, "Late invalid media caused an earlier partial upload");
        Fake partial; partial.failAt = 2; Context partialContext{partial}; auto stopped = publishPlan(plan, a, partialContext);
        check(stopped.requiresReconciliation() && stopped.completed.size() == 1 && stopped.next == 1 && partial.creates == 2, "Unknown second post retried or acknowledged IDs lost");
        std::stop_source stop; stop.request_stop(); Fake cancelled; Context cancelledContext{cancelled, stop.get_token()};
        check(publishPlan(plan, a, cancelledContext).result.error == Error::Cancelled && !cancelled.calls, "Cancelled sequence still uploaded");
        std::cout << "Review: grouping, inheritance, annotation order, UTF-8, preflight, reply chain, partial outcome and cancellation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
