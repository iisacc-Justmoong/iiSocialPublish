#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iiSocialPublish {

enum class Platform { X, Facebook, Instagram, Threads, Pinterest, Pixiv, ArtStation, Tumblr, Mastodon, LinkedIn, YouTube };
enum class MediaKind { Image, Video };
enum class Error {
    None, Unsupported, InvalidRequest, AuthenticationRequired, PermissionDenied,
    AccessNotConfigured, RateLimited, ProviderRejected, TransportFailure,
    InvalidResponse, ProcessingFailed, ProcessingTimeout, Cancelled, OutcomeUnknown
};

struct Capability {
    Platform platform;
    std::string id, name;
    bool officialPublishingApi = false;
    bool text = false, images = false, video = false;
    bool requiresPublicMediaUrl = false;
    std::vector<std::string> requiredScopes;
    std::string documentation, requirement;
};

// A provider fetches publicUrl, or the transport streams localFile. No implicit
// upload to an iisacc server and no conversion of an IISC document into an image.
struct Media {
    MediaKind kind = MediaKind::Image;
    std::filesystem::path localFile;
    std::string publicUrl, mimeType = "image/png", altText;
};

struct YouTubeOptions {
    std::string categoryId = "22"; // People & Blogs; override for the video category.
    // Omitted values preserve YouTube/channel defaults; these are explicit declarations.
    std::optional<bool> madeForKids, containsSyntheticMedia;
    bool notifySubscribers = true;
};

struct Post {
    std::string text, title, link;
    std::vector<std::string> tags;
    std::vector<Media> media;
    std::string coverImageUrl; // Required for a Pinterest video Pin.
    std::string visibility = "public";
    std::string contentWarning;
    bool sensitive = false;
    bool madeWithAi = false;
    std::string idempotencyKey; // Forwarded where the official API supports it.
    YouTubeOptions youtube;
    std::string replyToId, language, replyControl;
};

// Tokens are user-context OAuth grants, never an app-only bearer token. This
// value stays on the backend; it is not part of a capability or job snapshot.
struct Account {
    std::string accessToken;
    std::string targetId; // Page, IG/Threads user, board, Tumblr blog or LinkedIn author URN. Empty for YouTube: the OAuth grant selects its channel.
    std::string instanceUrl; // Mastodon origin, e.g. https://mastodon.social.
    std::vector<std::string> scopes;
    std::string apiVersion; // Explicit Meta vNN.N / LinkedIn YYYYMM deployment version.
    bool allowBillableRequests = false; // X requests are disabled unless provisioned.
    std::optional<std::chrono::system_clock::time_point> expiresAt;
    // Set only by native account provisioning after verifying reply entitlement.
    // X self-serve reply restrictions also apply to API-created threads.
    bool allowSelfThreadReplies = false;
};

using Headers = std::vector<std::pair<std::string, std::string>>;
struct FileSlice {
    std::filesystem::path path;
    std::uint64_t offset = 0;
    std::optional<std::uint64_t> length;
};
struct Part {
    std::string name, value, mimeType;
    std::optional<FileSlice> file;
};
struct Request {
    std::string method = "GET", url;
    Headers headers;
    std::string body;
    std::vector<Part> parts;
    std::optional<FileSlice> file;
};
struct Response {
    int status = 0;
    Headers headers;
    std::string body;
    bool transportFailed = false, cancelled = false, mayHaveBeenSent = false;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual Response send(const Request&, std::stop_token) = 0;
};

// HTTPS-only libcurl transport. TLS verification is mandatory, redirects are
// disabled, file bodies are streamed and response bodies are capped at 2 MiB.
class CurlTransport final : public Transport {
public:
    explicit CurlTransport(std::filesystem::path certificateAuthority = {})
        : certificateAuthority_(std::move(certificateAuthority)) {}
    Response send(const Request&, std::stop_token) override;
private:
    std::filesystem::path certificateAuthority_;
};

struct Context {
    Transport& transport;
    std::stop_token stop;
    unsigned maxPolls = 60;
    std::chrono::milliseconds pollInterval{1000};
    // Tests/hosts may provide a scheduler. Return false on cancellation.
    std::function<bool(std::chrono::milliseconds, std::stop_token)> wait;
    // YouTube only: bounded status queries within the same resumable session.
    unsigned maxUploadRecoveries = 3;
};

struct Outcome {
    Error error = Error::None;
    std::string message; // Stable, credential-free diagnostic; never raw server text.
    std::string postId, url;
    int httpStatus = 0;
    unsigned retryAfterSeconds = 0;
    bool published() const noexcept { return error == Error::None && !postId.empty(); }
    bool requiresReconciliation() const noexcept { return error == Error::OutcomeUnknown || (!published() && !postId.empty()); }
};

class Publisher {
public:
    virtual ~Publisher() = default;
    virtual const Capability& capability() const noexcept = 0;
    // Blocking native backend operation; use a worker, never the UI thread.
    // It never retries a new publication automatically. YouTube can query and
    // resume its existing upload session within Context.maxUploadRecoveries.
    virtual Outcome publish(const Post&, const Account&, Context&) const = 0;
};

const std::vector<Capability>& capabilities();
std::optional<Platform> platform(std::string_view id);
std::string_view errorName(Error) noexcept;
std::unique_ptr<Publisher> makePublisher(Platform);
// Local validation only; no transport requests. Media file checks belong on a worker.
Outcome validatePost(Platform, const Post&, const Account&);

} // namespace iiSocialPublish
