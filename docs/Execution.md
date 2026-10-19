# Execution and failure behavior

Provider objects are stateless. The caller owns credentials, transport, worker
and publication intent. `publish` is synchronous and must run outside a GUI
thread. `Context.stop` cancels HTTP transfers and processing waits. Polling is
bounded (60 checks, one-second minimum interval by default); X's server-provided
processing delay is respected up to a five-minute bound per check. The caller
may configure a larger polling budget or inject a wait scheduler. No polling
loop is started at construction or application startup.

There is **no automatic retry of publication**. A rate limit returns
`rate_limited`, HTTP status and numeric `Retry-After` seconds. Expired grants and
missing scopes fail without uploading. Invalid destinations, versions and media
paths are preflighted before side effects. SDK errors contain stable diagnostic
text rather than raw provider bodies, tokens, signed upload URLs or local paths.

A publication can succeed before a response is lost. Transport failure after
dispatch, a final 5xx, invalid successful publish JSON, or a successful response
without a post identifier returns `outcome_unknown`. Reconcile the provider
before retrying. Facebook creates a video before its transcoding reads; a
subsequent error retains `postId` and also requires reconciliation. A processing
timeout before the final publish can leave an unpublished media container; it
does not establish a published post. The SDK never deletes remote resources
automatically or claims a rollback of provider side effects.

Mastodon's official Idempotency-Key is forwarded. Other providers do not receive
an invented idempotency header. Cross-provider exactly-once delivery and durable
jobs across process restarts are not promised. The consumer must preserve its
publication intent/result ledger if it needs crash recovery. Credentials must
not be persisted in that ledger.

## HTTP transport

Only HTTPS URLs without embedded user information are accepted. Peer/hostname
verification and TLS 1.2+ are mandatory. Redirects are disabled so bearer headers
cannot follow a redirect to another host. The normal libcurl/system trust store
is used; an explicit `CurlTransport(certificateAuthorityFile)` can provide a
private CA (the loopback test uses this). There is no insecure TLS switch.

Binary and multipart file uploads stream from `std::ifstream` using bounded
offset/length readers, including LinkedIn multipart video and X 4 MiB chunks.
Pinterest image base64 uses a bounded 32 MiB input; other media files are capped
at 5 GiB as an adapter safety limit, not a promise that a provider accepts that
size. HTTP response bodies are capped at 2 MiB, headers at 64 KiB; connect timeout
is 10 seconds and a single transfer timeout is five minutes. Consumers exporting
larger/longer videos must transcode to the provider's supported limits first.

OAuth tokens are attached only to fixed official API hosts (or the configured
Mastodon instance). Signed Pinterest object-storage uploads receive only upload
parameters, never the OAuth token. LinkedIn media upload hosts are checked
against its linkedin.com/licdn.com domain boundary. No token is placed in a URL
query, log message or exported status. The publisher is a trusted backend API,
not an unauthenticated HTTP proxy; account configuration must remain inside the
trusted account service.

LinkedIn is an explicit exception to the signed-upload token omission: image
PUT requires the user's OAuth header on a verified LinkedIn media host, while
video PUT must omit it. A token with only `w_member_social` cannot GET Images API
status; member image publication therefore proceeds after the successful upload
without that forbidden GET. Image status polling is used only for grants that
include `rw_ads`, `w_organization_social` or `w_power_creators`. A processing
rejection from the final Posts API is surfaced without an automatic retry.
