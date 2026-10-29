#pragma once

#include <iiSocialIntegration/Publishing.h>

namespace iiSocialIntegration {

enum class SourceTextFormat { PlainText, Markdown, Html };

// External IDs and URLs are references, not Society assets or trusted identity.
struct ExternalMedia {
    std::string sourceId, sourceUrl, kind, mimeType, caption;
    std::optional<std::string> altText;
};

struct ExternalPost {
    std::string platformId, id, url, authorId, createdAt, visibility, contentWarning;
    std::optional<bool> sensitive;
    std::string title, body, description, language;
    SourceTextFormat bodyFormat = SourceTextFormat::PlainText;
    std::vector<std::string> tags, hashtags;
    std::vector<ExternalMedia> media;
    // Preserved for caller-controlled recovery of provider-specific data. Never
    // included in a Society publishing request; it may contain private data.
    std::string originalJson;
};

struct SocietyMedia {
    std::string sourceId, sourceUrl, kind, mimeType, caption;
    std::optional<std::string> altText;
    // Assigned by the host after registering an authorized Society asset.
    std::string assetId;
};

// Editorial fields of Society's publishing-request v1. The Society server
// owns the canonical CanvasPost, identity, timestamps, and attachment IDs.
struct SocietyPost {
    std::string title, body, description, language;
    std::string bodyFormat = "plain_text";
    std::vector<std::string> tags, hashtags;
    std::vector<SocietyMedia> media;
};

struct MigrationIssue {
    std::string code, message;
    bool blocking = false;
};

struct MigrationOptions {
    // Used for HTML sources such as Mastodon. A null result blocks serialization;
    // the original HTML remains in ExternalPost for review or another converter.
    std::function<std::optional<std::string>(std::string_view)> htmlToPlainText;
};

struct MigrationResult {
    ExternalPost source;
    SocietyPost post;
    std::vector<MigrationIssue> issues;
    bool ready() const noexcept;
};

// Pure conversion: no provider requests, downloads, files, authentication or
// Society publication. A host may normalize any provider through ExternalPost.
MigrationResult importPost(const ExternalPost&, const MigrationOptions& = {});
std::vector<MigrationResult> importPosts(const std::vector<ExternalPost>&,
                                         const MigrationOptions& = {});

// Convert one supported provider's already-fetched JSON post object. Input can
// include the provider's requested expansions. Unsupported/invalid shapes yield
// a blocking issue; they are never reported as successful empty imports.
MigrationResult importPost(Platform, std::string_view nativeJson,
                           const MigrationOptions& = {});
std::vector<MigrationResult> importPosts(Platform, const std::vector<std::string>& nativePosts,
                                         const MigrationOptions& = {});
bool supportsNativeImport(Platform) noexcept;

// Serialize a private Society draft only after all media have valid asset IDs
// and Society field limits are satisfied. Returns nullopt when not ready.
std::optional<std::string> serializeSocietyDraft(const MigrationResult&);

} // namespace iiSocialIntegration
