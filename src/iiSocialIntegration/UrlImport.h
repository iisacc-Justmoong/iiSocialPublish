#pragma once

#include <iiSocialIntegration/Migration.h>

namespace iiSocialIntegration {

enum class UrlImportError {
    None, InvalidUrl, UrlBlocked, TransportFailure, Cancelled, HttpError,
    RedirectLimit, UnsupportedContentType, InputTooLarge, InvalidHtml,
    InvalidMedia, SinkFailure, InvalidSelection
};

struct UrlImportOptions {
    std::size_t maxHtmlBytes = 8 * 1024 * 1024;
    std::size_t maxMediaCandidates = 500;
    unsigned maxRedirects = 5;
    // Called before every page request, including redirect destinations. Hosts
    // exposing this feature remotely must also enforce their network policy.
    std::function<bool(std::string_view)> allowPageUrl;
};

struct UrlMediaInfo {
    // DOM or structured-metadata locations that mentioned the same URL.
    std::vector<std::string> origins;
    bool preview = false;
};

struct UrlImportResult {
    UrlImportError error = UrlImportError::None;
    std::string message, requestedUrl, finalUrl;
    int httpStatus = 0;
    ExternalPost post;
    // Same order and length as post.media. URLs are candidates, not verified
    // ownership, original files, ready Society assets, or permission to reuse.
    std::vector<UrlMediaInfo> mediaInfo;
    std::vector<MigrationIssue> issues;
    bool ok() const noexcept { return error == UrlImportError::None; }
};

struct UrlMediaOptions {
    std::uint64_t maxBytes = 5ULL * 1024 * 1024 * 1024;
    unsigned maxRedirects = 5;
    std::function<bool(std::string_view)> allowMediaUrl;
};

struct MediaFetchResult {
    UrlImportError error = UrlImportError::None;
    std::string message, finalUrl, mimeType;
    int httpStatus = 0;
    std::uint64_t bytesWritten = 0;
    bool ok() const noexcept { return error == UrlImportError::None; }
};

// For older libcurl/libxml2 versions, call on the main thread before starting
// workers. Entry points also initialize lazily if the host has not done so.
bool initializeUrlImport() noexcept;

// Fetch one public page via the host's transport. Redirects are followed here,
// one GET at a time, with policy checks and without automatically adding
// authentication. CurlTransport itself accepts HTTPS only. Run on a worker.
UrlImportResult importUrl(std::string_view url, Context&, const UrlImportOptions& = {});

// Parse an already-serialized UTF-8 DOM, e.g. supplied by a host WebView after
// client-side rendering. This overload performs no network or script execution.
UrlImportResult parsePageDom(std::string_view documentUrl, std::string_view html,
                             const UrlImportOptions& = {});

// Select candidates in the caller's chosen order while keeping provenance and
// discovery issues. Invalid or duplicate indices produce InvalidSelection.
UrlImportResult selectUrlMedia(const UrlImportResult&, const std::vector<std::size_t>& indices);

// Convert discovered candidates to the existing Society draft contract while
// retaining discovery issues. Failed or truncated discovery blocks readiness.
MigrationResult importPost(const UrlImportResult&, const MigrationOptions& = {});

// Explicitly fetch one selected candidate to a caller-owned sink. No files are
// created. Chunks are streamed and bounded; errors can leave partial sink data,
// which the caller must discard. The caller validates bytes and authorizes reuse.
MediaFetchResult fetchMedia(const ExternalMedia&, Context&,
                            const std::function<bool(std::string_view)>& sink,
                            const UrlMediaOptions& = {});

} // namespace iiSocialIntegration
