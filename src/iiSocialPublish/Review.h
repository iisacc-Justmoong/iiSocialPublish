#pragma once
#include <iiSocialPublish/Publishing.h>

namespace iiSocialPublish {

enum class Sequence { Automatic, Single, Separate, Thread };
struct ReviewOptions {
    Sequence sequence = Sequence::Automatic;
    unsigned mediaPerPost = 0; // Zero uses the adapter policy, never drops overflow.
    bool includeCaptions = true, numberParts = true, repeatIntroduction = false;
    bool splitText = true;
};
struct ReviewSource {
    Post post;
    // Same indices as post.media; annotations remain attached when grouping changes.
    std::vector<std::string> captions;
    std::string scheduledAt;
};
struct ReviewIssue { std::string code, message; bool blocking = false; };
struct PlannedPost {
    Post post;
    std::vector<std::size_t> sourceMedia;
    std::optional<std::size_t> replyTo;
};
struct PublicationPlan {
    Platform platform = Platform::X;
    std::vector<PlannedPost> posts;
    std::vector<ReviewIssue> issues;
    std::string scheduledAt;
    bool ready() const noexcept;
};
struct ReviewPolicy {
    unsigned mediaPerPost, textLimit;
    bool threads, nativeTitle, nativeTags, textOnly;
};

ReviewPolicy reviewPolicy(Platform) noexcept;
// Pure in-memory preparation. No file reads, rendering, network, account lookup,
// document mutations or credentials. Text is split, never silently shortened.
PublicationPlan planPublication(Platform, const ReviewSource&, const ReviewOptions& = {});

struct SequenceOutcome {
    Outcome result;
    std::vector<Outcome> completed;
    std::size_t next = 0;
    bool published() const noexcept { return result.published(); }
    bool requiresReconciliation() const noexcept {
        return !published() && (!completed.empty() || result.requiresReconciliation());
    }
};
// Runs on a backend worker. Preflights ALL steps before the first upload; stops
// at the first failed/unknown step, retains acknowledged IDs, never retries or
// silently substitutes independent posts for a requested reply chain.
SequenceOutcome publishPlan(const PublicationPlan&, const Account&, Context&);

} // namespace iiSocialPublish
