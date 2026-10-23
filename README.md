# iiSocialPublish

[Windows 빌드 및 실행](docs/WindowsBuildAndRun.md)

Package release: **0.2.0** (2026-10-22). Native destination review, lossless
media grouping, UTF-8 text splitting and sequential reply-chain execution are
available through `Review.h`. See [destination review](docs/Review.md).


Qt-free C++23 publishing objects for X, Facebook Pages, Instagram Professional,
Threads, Pinterest, Tumblr, Mastodon, LinkedIn and YouTube. Pixiv and ArtStation have
explicit unsupported objects because no official public publishing contract was
verified. The module does not use private/mobile APIs or browser automation.

`makePublisher(Platform)` returns a provider object implementing the same
`Publisher::publish(Post, Account, Context)` backend interface. Every supported
provider performs its real HTTP publishing protocol through an injectable
`Transport`; the included `CurlTransport` uses libcurl with mandatory TLS
verification. Objects do not contact providers when constructed.

YouTube uses the official Data API v3 `videos.insert` resumable protocol:
metadata POST, streamed video PUT, 308 acknowledgements and bounded recovery
queries within the same upload session. It supports one local video, title,
description, tags, category and public/unlisted/private visibility. See the
[YouTube upload API](docs/YouTube.md) for OAuth setup, C++ usage and acceptance
versus subsequent video processing.

```cpp
#include <iiSocialPublish/Publishing.h>
using namespace iiSocialPublish;

Account account;
account.accessToken = obtainUserGrantFromTrustedAccountService();
account.instanceUrl = "https://your-instance.example";
account.scopes = {"write:statuses", "write:media"};
Post post;
post.text = "Created with Dreamscapes";
post.idempotencyKey = "a-stable-application-intent-id";
post.media.push_back({MediaKind::Image, "/absolute/export.png", {},
                      "image/png", "An accessible image description"});
CurlTransport transport;
Context context{transport}; // Run on a backend worker.
auto result = makePublisher(Platform::Mastodon)->publish(post, account, context);
if (result.requiresReconciliation()) {
    // Inspect the provider before attempting another publication.
}
```

The sample's credential function belongs to the consumer; this SDK does not
acquire, persist, refresh or expose account credentials. The caller must provide
a valid **user-context** OAuth access token, verified scopes and explicit
destination. An app-only token is insufficient for publishing. No client secret
or account password belongs in this module. Revocation/refresh remain in the
account service that owns the OAuth grant.

Build and install:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
```

Consumers use `find_package(iiSocialPublish 0.1.1 CONFIG REQUIRED)` and
`iiSocialPublish::iiSocialPublish`. The static native target depends on libcurl
7.68+ and privately vendored nlohmann/json 3.12.0; no Qt, Python interpreter or
browser is needed at runtime. Tests additionally use Python/OpenSSL for a
disposable loopback HTTPS server. The package includes third-party licensing.

See [provider API contracts](docs/Providers.md),
[execution and failure contracts](docs/Execution.md) and
[verification](docs/Verification.md). Local fixture tests establish request
construction and transport behavior, not successful live publication to any
social account.

## License

SPDX-License-Identifier: MIT

The original code and documentation in this repository are licensed under the
[MIT License](LICENSE). Third-party code, libraries, fonts, tools, and model
weights retain their respective licenses.

## Source header layout

Public headers and implementations live together in `src/iiSocialPublish/`.
There is no separate authored `include/` tree. Existing public `#include` names
and exported CMake targets are unchanged; installation still places only public
headers in the package include directory. Generated export headers belong to
`build/`, not to the authored source tree.

Validate this layout from the SDK workspace with
`python3 -B tests/test_source_header_layout.py`. Reconfigure, build, run CTest,
and verify a staged installed consumer after changing source/header paths.
