# iiSocialPublish

[Windows 빌드 및 실행](docs/WindowsBuildAndRun.md)

Package release: **0.1.1** (2026-10-19). This patch packages the current
source snapshot; build, test and installation evidence is recorded in the SDK
patch-release report. Earlier versioned feature notes remain historical.


Qt-free C++23 publishing objects for X, Facebook Pages, Instagram Professional,
Threads, Pinterest, Tumblr, Mastodon and LinkedIn. Pixiv and ArtStation have
explicit unsupported objects because no official public publishing contract was
verified. The module does not use private/mobile APIs or browser automation.

`makePublisher(Platform)` returns a provider object implementing the same
`Publisher::publish(Post, Account, Context)` backend interface. Every supported
provider performs its real HTTP publishing protocol through an injectable
`Transport`; the included `CurlTransport` uses libcurl with mandatory TLS
verification. Objects do not contact providers when constructed.

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
