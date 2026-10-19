# Verification

On 2026-10-18, the macOS arm64 Release build passed all 13 CTest groups, including
the real TLS loopback transport. The installed package was consumed by
Dreamscapes. `otool -L build/iiSocialPublishTests` lists only system libcurl,
libc++ and libSystem, with no Qt library. Logs are retained under `build/ctest.txt`
and `build/link-dependencies.txt`.

The native test suite uses scripted official-contract responses. It verifies
the ten-object catalog, unsupported platforms without network traffic, scope/
token/version/file preflight, all eight supported request chains, carousel
assembly, NPF multipart identifiers, media-processing waits, ordered LinkedIn
video ranges/ETags, privacy/idempotency forwarding, sanitized errors, rate limits,
cancellation and ambiguous publication outcomes without retry.

`iiSocialPublish.TlsLoopback` starts a disposable 127.0.0.1 HTTPS server and
executes the real libcurl transport with a generated test CA. It verifies exact
byte ranges, multipart parts, redirect refusal, response-size bounds and header
injection rejection. It never sends a post to a social account.

Canonical commands:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
```

Host verification is macOS arm64. Other native toolchains can consume the C++23
CMake target with their own libcurl, but those platforms are not established as
runtime-tested by this host run. There is no Qt dependency in this module.
Live access tokens, provider app approval, remote media availability, actual
publication, quotas and billing have not been exercised.
