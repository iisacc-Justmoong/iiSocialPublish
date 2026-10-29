# YouTube video upload

Implemented against the official YouTube Data API v3 and resumable upload
contract reviewed on 2026-10-22. The C++23 provider uses the existing native
libcurl transport; Google client libraries, Qt and a Python runtime are not
application dependencies.

## Account setup

Enable YouTube Data API v3 for the application's Google Cloud project and
configure a Google OAuth client appropriate to the application's login flow.
The account service obtains a user-context grant for the destination channel.
An API key alone is not an upload credential. Token acquisition, refresh,
revocation, storage and OAuth consent remain the account service's responsibility.

The catalog advertises the minimum upload scope:
`https://www.googleapis.com/auth/youtube.upload`. The upload API also accepts
`https://www.googleapis.com/auth/youtube`,
`https://www.googleapis.com/auth/youtube.force-ssl` and
`https://www.googleapis.com/auth/youtubepartner`; the adapter accepts those
grants without requiring the narrower scope in addition. Granting a scope does
not establish project eligibility, quota availability or channel permissions.
Check current Google project/access requirements before deployment.

Leave `Account.targetId` empty. Ordinary `videos.insert` uploads to the channel
selected by the OAuth grant, rather than selecting an arbitrary channel ID.
CMS content-owner delegation is outside this adapter's implemented scope.

## C++ usage

```cpp
#include <iiSocialIntegration/Publishing.h>
using namespace iiSocialIntegration;

Account account;
account.accessToken = obtainChannelUserAccessToken(); // Consumer account service.
account.scopes = {"https://www.googleapis.com/auth/youtube.upload"};

Post post;
post.title = "Created with iisacc";
post.text = "A native video export.";
post.link = "https://iisacc.com/";
post.tags = {"iisacc", "native"};
post.visibility = "private"; // Also public or unlisted.
post.youtube.categoryId = "22";
post.youtube.madeForKids = false; // Set according to the video's actual audience.
post.youtube.notifySubscribers = false;
post.media.push_back({MediaKind::Video, "/absolute/export.mp4", {}, "video/mp4", {}});

CurlTransport transport;
Context context{transport}; // Execute on an application backend worker.
context.maxUploadRecoveries = 3;
auto result = makePublisher(Platform::YouTube)->publish(post, account, context);
if (result.published()) {
    // result.postId / result.url identify the accepted video upload.
} else if (result.requiresReconciliation()) {
    // Inspect the channel before creating another upload attempt.
}
```

`platform("youtube")` resolves to `Platform::YouTube`. Construction performs no
network I/O. `Capability` advertises video support, with no standalone text,
image or public-media-URL publishing route.

## Metadata and media contract

| Input | API field or behavior |
| --- | --- |
| `title` | Required UTF-8 `snippet.title`; at most 100 Unicode code points, with no `<` or `>`. |
| `text`, `link` | `snippet.description`, joined with a newline; at most 5,000 UTF-8 bytes, with no `<` or `>`. |
| `tags` | Native `snippet.tags` array; not appended as description hashtags. |
| `youtube.categoryId` | Numeric `snippet.categoryId`; defaults to `22` (People & Blogs). The provider determines which IDs are valid. |
| `visibility` | `status.privacyStatus`: `public`, `unlisted` or `private`. Set explicitly for the intended audience; the common Post default is public. |
| `youtube.madeForKids` | Optional `status.selfDeclaredMadeForKids`. Omitted by default so the SDK does not invent an audience declaration. |
| `youtube.containsSyntheticMedia` | Optional `status.containsSyntheticMedia`, set explicitly according to YouTube's disclosure semantics. `madeWithAi` is not automatically substituted. |
| `youtube.notifySubscribers` | `notifySubscribers` request parameter; defaults to true. |
| `media` | Exactly one readable absolute local video, `video/*` or `application/octet-stream`; the adapter's existing 5 GiB safety cap applies. |

Metadata UTF-8, title/description shape, visibility, category shape, credentials,
scopes and local file requirements are checked before creating the session.
YouTube remains authoritative for category IDs, tag limits, codecs, duration,
account/project restrictions and quotas. This module does not transcode, fetch
a supplied public video URL, create a thumbnail, schedule publication, create
a live broadcast or support community posts.

## Upload sequence and recovery

1. POST metadata JSON to
   `https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet%2Cstatus&notifySubscribers=...`,
   with bearer authorization, `X-Upload-Content-Type` and `X-Upload-Content-Length`.
2. Validate the returned `Location` against the exact official upload endpoint.
3. Stream the file to that URL using PUT, `Content-Length` and `Content-Range`.
   Non-final chunks are 4 MiB, a multiple of the required 256 KiB unit.
4. On HTTP 308, parse `Range: bytes=0-N` and continue from `N + 1`. Reject
   malformed, regressing or out-of-bounds acknowledgements.
5. After a transport/server interruption, query the same session with an empty
   PUT and `Content-Range: bytes */TOTAL`. Resume only at the confirmed offset,
   or use the completed video resource returned by the status query.

Recovery queries share a bounded budget with exponential backoff. Cancellation,
rate limits and unreconciled completion are surfaced without creating another
session. A successful upload returns the video ID and watch URL; YouTube's later
transcoding, moderation and actual visibility remain separate. A private upload
also has a watch URL, which does not imply public access.

There is no persistent upload session/checkpoint API. If the process exits or
the recovery budget is exhausted, preserve the application intent and reconcile
the channel before retrying. Do not store tokens or session URLs in public job
snapshots. See [execution contracts](Execution.md) for error and TLS behavior.

## Official API references

- [videos.insert](https://developers.google.com/youtube/data/reference/rest/v3/videos/insert)
- [Resumable upload protocol](https://developers.google.com/youtube/data/guides/using_resumable_upload_protocol)
- [Video resource and metadata](https://developers.google.com/youtube/data/reference/rest/v3/videos)
- [OAuth authentication](https://developers.google.com/youtube/v3/guides/authentication)
