# Official publishing contracts

Reviewed on 2026-10-18. Publishing access, API versions, scopes, quotas and
provider review requirements can change. The deployment must select a supported
version explicitly; the library never silently pins a retired Meta or LinkedIn
version. The test versions `v25.0` and `202607` are fixtures, not deployment
configuration or claims of app approval.

| Object | Implemented route and media workflow | Required account configuration |
| --- | --- | --- |
| XPublisher | `POST /2/tweets`; v2 initialize / append / finalize / STATUS; optional media alt text. Text, 1–4 images, a single GIF, or a single video. AI disclosure is forwarded. | User OAuth `tweet.read`, `tweet.write`, `users.read`, plus `media.write` for media. X API access is billable; `allowBillableRequests` defaults to false. |
| FacebookPublisher | Page `/photos` uploads with `published=false` followed by `/feed` for text/photo albums; Graph Video `/videos` for a single video, followed by processing reads. | Page access token, numeric Page ID, `pages_manage_posts`, `pages_read_engagement`, explicit Graph version. Personal profile posting is unsupported. |
| InstagramPublisher | Instagram Login `/media`, container status, `/media_publish`; image, Reel or mixed image/video carousel. | Professional account, numeric IG user ID, `instagram_business_basic`, `instagram_business_content_publish`, explicit Graph version. Media must be available at public HTTPS URLs. |
| ThreadsPublisher | `/v1.0/{user}/threads`, container status, `/threads_publish`; text, image, video or carousel. | Threads user token/ID, `threads_basic`, `threads_content_publish`; public HTTPS media URLs. |
| PinterestPublisher | `/v5/pins` with `image_url` or `image_base64`; `/v5/media`, signed multipart video upload, media processing, then a `video_id` Pin. | `pins:write`, `boards:read`, destination board ID, approved app/access tier. A video requires a cover image URL. |
| TumblrPublisher | `/v2/blog/{blog}/posts` using Neue Post Format JSON and multipart `json` + identifier-mapped media parts. Text, images and video. | OAuth2 `write` user token and an explicit blog hostname or immutable UUID. |
| MastodonPublisher | Per-instance `/api/v2/instance`, `/api/v2/media`, async media reads, `/api/v1/statuses`. | HTTPS instance origin, instance-issued token, `write:statuses` and `write:media` for attachments (or `write`). Instance attachment count is read at runtime. Visibility, content warning, sensitive flag, alt text and Idempotency-Key are supported. |
| LinkedInPublisher | `/rest/images?action=initializeUpload`, media PUT and status; videos initialize, byte-range PUT with ordered ETags, finalize and status; `/rest/posts`. Text, image, multiple images or a single video. | User token, explicit person/organization author URN, `w_member_social` or `w_organization_social` and organization role/access, explicit supported YYYYMM version. |
| UnsupportedPublisher (Pixiv) | Returns `unsupported` before any networking. | No official public publishing contract was verified. This is a statement about verified integration support, not a claim that private partner APIs can never exist. |
| UnsupportedPublisher (ArtStation) | Returns `unsupported` before any networking. | Same restriction; no undocumented project-upload or login-session endpoint is used. |

The adapter deliberately supports a bounded common export shape: images/videos,
not arbitrary IISC documents, live streams, ads, stories, polls or document
posts. It does not resize, transcode, crop, expose local files on a public
server, or silently choose a board/Page/author. Image/video MIME types must
match their declared kind. Provider-specific size, aspect ratio, duration,
weighted text length, account eligibility and posting quotas remain
authoritative at the API. The adapter rejects invalid file paths and mixed
video batches where its implemented publishing route cannot support them.

Text includes the supplied `link` and hashtag list except where the provider
has native link/tag fields (Facebook/Pinterest link, Tumblr tags). Titles are
used by Pinterest, Tumblr, Facebook video and LinkedIn media. Visibility/CW/
sensitivity controls apply to Mastodon; other adapters accept public visibility
only. X's `madeWithAi` is a supported API disclosure, not a general provider
labeling policy. The SDK does not infer such disclosure from media pixels.

## Primary references

- [X create post](https://docs.x.com/x-api/posts/create-post),
  [v2 chunked media upload](https://docs.x.com/x-api/media/quickstart/media-upload-chunked),
  [X developer platform/access](https://developer.x.com/).
- [Facebook Page posts](https://developers.facebook.com/docs/pages-api/posts/),
  [video publishing](https://developers.facebook.com/docs/video-api/guides/publishing/).
- [Instagram Login content publishing](https://developers.facebook.com/docs/instagram-platform/instagram-api-with-instagram-login/content-publishing/),
  [Meta Instagram Login scopes](https://www.postman.com/meta/instagram/folder/6raa77c/instagram-api-with-instagram-login),
  [Meta's official Instagram API collection](https://www.postman.com/meta/instagram/documentation/23987686-9386f468-7714-490f-9bfc-9442db5c8f00).
- [Threads publishing](https://developers.facebook.com/docs/threads/posts/),
  [Meta's official Threads API collection](https://www.postman.com/meta/threads/documentation/dht3nzz/threads-api).
- [Pinterest organic Pins/media guide](https://developers.pinterest.com/docs/work-with-organic-content-and-users/create-boards-and-pins/),
  [media upload state definitions](https://developers.pinterest.com/docs/work-with-ads/manage-media-library-assets/),
  [app access](https://developers.pinterest.com/docs/getting-started/connect-app/).
- [Tumblr official OAuth2 and NPF API](https://github.com/tumblr/docs/blob/master/api.md),
  [NPF media schema](https://github.com/tumblr/docs/blob/master/npf-spec.md).
- [Mastodon media](https://docs.joinmastodon.org/methods/media/),
  [statuses](https://docs.joinmastodon.org/methods/statuses/),
  [instance limits](https://docs.joinmastodon.org/methods/instance/).
- [LinkedIn Posts](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/posts-api),
  [Images](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/images-api),
  [Videos](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/videos-api).
  [LinkedIn image/video upload authorization](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/vector-asset-api#upload-the-image)
  documents the required image PUT token and the token-free video PUT.
- [Pixiv official help](https://www.pixiv.help/hc/en-us),
  [ArtStation official help](https://help.artstation.com/en/).

Meta's direct documentation returned access/rate-limit errors in this research
session; the implementation was also checked against Meta's official Postman
collections. No third-party reverse-engineered API is treated as a contract.
App registration/review, live OAuth consent, paid access provisioning and live
publishing have not been performed by the test suite.
