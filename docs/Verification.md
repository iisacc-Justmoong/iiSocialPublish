# Verification

## URL 미디어 발견, 2026-10-28

macOS arm64에서 C++23 빌드와 CTest 20개를 통과했다. URL 전용 검사는 HTML
DOM·메타데이터·상대 경로·문자셋·인라인 CSS·중복 후보·선택 순서, 데이터
URL, 병렬 파싱, 리다이렉트, 오류·취소·크기 제한과 호출자 sink를 확인한다. 실제 TLS
루프백에서는 2 MiB를 넘는 HTML과 9 MiB 미디어의 청크 전송, 리다이렉트
본문 분리와 작은 미디어 한도를 확인한다. URL에서 생성하고 자산 ID를 연결한
초안 1개는 Society 서비스의 현재 게시 요청 스키마로 별도 검증했다.

이는 공개 소셜 사이트의 실제 접근, 로그인, JavaScript 실행, 원본 미디어의
권리·바이트 검증 또는 Society 자산 등록·게시를 검증한 결과가 아니다.
탐색 범위와 호출자 책임은 [URL 이관 문서](UrlImport.md)를 따른다.

## Society 이관과 이름 변경, 2026-10-28

macOS arm64에서 `iiSocialIntegration`을 C++23으로 빌드하고 기존 퍼블리싱,
TLS 루프백, 새 이관 및 이전 이름 호환을 포함한 CTest 19개를 통과했다.
`iiSocialIntegration.Migration`은 아홉 제공자의 대표 원본 입력, 단건·일괄
결과, 첨부 순서, 누락 자산, 민감 정보 검토, HTML 변환과 Society 필드 한도를
검사한다. Pixiv와 ArtStation의 미지원 네이티브 형식은 오류 결과로 확인한다.

Society 서비스의 `json_schemer`와 현재 `publishing-request.schema.json`으로
텍스트 초안과 등록 자산이 있는 초안, 총 2개의 생성 결과를 각각 검증했다.
이는 서버의 미디어 수집·인증·영속화나 실제 계정의 원본 조회·게시를 실행했다는
뜻이 아니다. 명령과 자산 등록의 책임은 [이관 문서](Migration.md)를 따른다.

## YouTube extension, 2026-10-22

The macOS arm64 Release build passed all **16 CTest groups**, including the
three new YouTube groups and the real TLS loopback transport. The complete
existing provider suite remains enabled. The extended catalog contains eleven
targets, nine with implemented official publishing routes.

YouTube fixtures verify metadata/privacy/audience/synthetic-media declarations,
UTF-8 titles (including the 100-character boundary), local-video preflight,
least-privilege and broader upload grants, streamed byte ranges, partial
acknowledgements, lost-final-response reconciliation, cancellation, bounded
recovery, rate limits and credential-free errors. Invalid session locations and
malformed or inconsistent ranges are rejected. A failed status query after an
ambiguous dispatch remains `outcome_unknown`.

The real HTTPS loopback verifies a 308 response without redirect following,
an empty status PUT and a resumed final file range. Test configuration uses the
available `/usr/bin/openssl`; an obsolete local Homebrew cache path was replaced.
No provider credentials are used.

Installation was staged at `build/install`. A separate C++23 consumer under
`build/youtube-consumer`, configured only against that installed package,
compiled, linked and passed its CTest (**1/1**). It exercised the YouTube catalog,
public options and complete metadata/upload request chain with an offline
transport. `otool -L` confirms system libcurl, libc++ and libSystem, without Qt.
The normal installed SDK under `~/.local` was not replaced.

Evidence is retained in `build/youtube-ctest.txt`,
`build/youtube-installed-consumer-ctest.txt` and `build/youtube-verification.json`.
Actual Google login, live video upload, transcoding and public visibility were
not exercised. Windows and Linux runtime verification was not performed.

## Earlier baseline, 2026-10-18

On 2026-10-18, the macOS arm64 Release build passed all 13 CTest groups, including
the real TLS loopback transport. The installed package was consumed by
Dreamscapes. `otool -L build/iiSocialPublishTests` lists only system libcurl,
libc++ and libSystem, with no Qt library. Logs are retained under `build/ctest.txt`
and `build/link-dependencies.txt`.

The native test suite uses scripted official-contract responses. It verifies
the original ten-object catalog, unsupported platforms without network traffic, scope/
token/version/file preflight, the original eight supported request chains, carousel
assembly, NPF multipart identifiers, media-processing waits, ordered LinkedIn
video ranges/ETags, privacy/idempotency forwarding, sanitized errors, rate limits,
cancellation and ambiguous publication outcomes without retry.

`iiSocialPublish.TlsLoopback` starts a disposable 127.0.0.1 HTTPS server and
executes the real libcurl transport with a generated test CA. It verifies exact
byte ranges, multipart parts, redirect refusal, response-size bounds and header
injection rejection. It never sends a post to a social account.

The redirect fixture explicitly declares an empty response body. This keeps the
redirect assertion independent of TLS connection-close behavior on Windows
Schannel; redirect following remains disabled and is checked by the same test.

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
