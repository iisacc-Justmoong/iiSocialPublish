# URL에서 미디어를 발견하고 가져오기

`UrlImport.h`는 플랫폼별 API 형식을 몰라도 게시물 URL을 받아 페이지의
정적 HTML DOM을 분석한다. 결과의 `post`는 기존 `ExternalPost`이므로
`importPost`로 Society 초안에 연결할 수 있다. 이 경로는 게시물 원본의
완전성이나 저작권을 판정하지 않으며 자동으로 게시하지 않는다.

```cpp
#include <iiSocialIntegration/UrlImport.h>
using namespace iiSocialIntegration;

if (!initializeUrlImport()) return; // 작업자 시작 전 메인 스레드에서 한 번 호출한다.
CurlTransport transport;
Context context{transport};  // 네트워크 호출은 앱의 작업자에서 실행한다.
auto page = importUrl("https://example.com/post/123", context);
if (!page.ok()) return;

// page.post.media와 page.mediaInfo는 같은 순서이다. 앱은 각 후보의
// 출처와 미리보기 여부를 보고 실제 이관할 인덱스를 고른다.
auto chosen = selectUrlMedia(page, {0});  // 실제 앱은 후보 수와 사용자의 선택을 확인한다.
auto draft = importPost(chosen);

// 선택한 원본을 앱 소유 저장소나 업로드 스트림으로 보낼 수 있다.
auto media = fetchMedia(chosen.post.media[0], context,
    [&](std::string_view bytes) { return applicationSink(bytes); });
// media.ok()와 바이트·권한 확인 후 Society에 등록하고 draft.post.media[0].assetId를 설정한다.
```

`applicationSink`는 호출 앱의 함수 예시이며 SDK에 포함된 함수가 아니다.
`fetchMedia`는 원본 바이트를 콜백에 순차 전달하며 파일을 만들지 않는다.
콜백의 바이트 뷰는 호출 중에만 유효하다. 실패·취소·크기 초과 또는 거절된
콘텐츠 형식은 이미 전달된 일부 바이트를 남길 수 있으므로 앱이 임시 저장을
폐기해야 한다. HTTP 콘텐츠 형식은 완료 시 확인하며 바이트의 실제 형식,
무결성, 사용 권한과 Society 수집 가능 여부는 앱·서버가 검증한다.

## 발견 범위와 결과

- `<img>`의 `src`, `srcset`, `data-src`, `data-original`, `data-lazy-src`, `data-srcset`을 읽는다.
- `<picture>`·`<source>`와 `<video>`·`<audio>`의 소스 및 영상 포스터를 찾는다. 직접 미디어 확장자를 가진 `<a href>`도 후보로 넣는다.
- 요소의 인라인 `style` 안에 있는 단순한 `url(...)`도 검토용 후보로 찾는다. 이스케이프된 CSS URL은 추측하지 않고 검토 이슈를 남긴다.
- Open Graph의 이미지·영상·오디오, Twitter 이미지·스트림, `link rel=image_src`, 이미지 관련 `itemprop`, JSON-LD의 이미지·썸네일·미디어 `contentUrl`을 읽는다. HTML 엔터티, 상대 경로, 첫 `<base href>`, 응답의 문자셋과 리다이렉트 뒤 URL을 처리한다.
- URL이 같은 후보는 합치고 발견 위치를 `mediaInfo.origins`에 보존한다. 메타데이터나 포스터만으로 발견한 항목은 `preview`로 표시한다. `data:image/*`, `data:video/*`, `data:audio/*`는 URL로 유지하고 요청 시 디코딩한다.

네트워크 자산은 바이트를 청크로 전달한다. DOM 안의 `data:` 자산은 HTML의
8 MiB 상한 안에서 메모리에 디코딩한 뒤 청크로 전달한다.

일반 페이지에는 작성자 아이콘, 사이트 로고, 광고, 미리보기와 동일 이미지의
여러 해상도가 섞일 수 있다. 후보는 게시물의 원본 파일이라는 보장이 없다.
`selectUrlMedia`로 앱이나 사용자가 선택한 순서만 초안에 넣을 수 있으며 잘못된
인덱스는 오류가 된다. 선택하지 않고 `importPost(page)`를 호출하면 발견된
모든 후보가 들어간다. Society 자산 등록 전에는 초안 직렬화가 차단된다.

페이지 제목과 설명은 Open Graph, Twitter 카드 또는 문서 메타데이터에서 얻는
미리보기이다. 본문 전체를 복원했다고 취급하지 않는다. 원본 게시물 ID 대신
리다이렉트 후 페이지 URL을 `post.id`에 둔다. 이를 원본 식별과 중복 확인에
사용할 수 있으나, URL 변경과 동일 게시물의 여러 URL은 앱이 처리해야 한다.

libxml2의 HTML DOM 파서는 HTML5 브라우저의 트리 구성과 완전히 같지 않으며
JavaScript, 외부 스타일시트, iframe, shadow DOM을 실행하거나 추가 페이지를 탐색하지 않는다. 인라인 CSS도 전체 CSS 문법을 계산하지 않는다.
클라이언트 렌더링이나 로그인 뒤에만 존재하는 자산은 기본 가져오기에 나타나지
않을 수 있다. 앱이 WebView 등에서 적법하게 얻은 렌더링 후 UTF-8 DOM을
`parsePageDom(documentUrl, html)`에 전달하면 같은 탐색·변환 경로를 쓸 수 있다.
후보가 없거나 후보 수 한도에 도달하면 검토 항목과 함께 Society 변환을 차단한다.

## 전송, 범위와 책임

`importUrl`과 `fetchMedia`는 호출자가 제공한 `Transport`를 쓰는 동기 API이며
작업자 스레드에서 호출해야 한다. 오래된 libxml2·libcurl에서도 안전하게
초기화하도록 앱은 작업자 시작 전 메인 스레드에서 `initializeUrlImport()`를
호출한다. 호출하지 않은 경우 첫 API 호출에서 지연 초기화한다. 기본 `CurlTransport`는 HTTPS만 허용하고
TLS 검증을 강제한다. 사용자 정보가 들어간 URL, 파일·스크립트 URL은 거부한다.
HTTP URL은 이를 명시적으로 지원하는 호출자 소유 전송기에서만 가능하다.
이 SDK는 쿠키나 인증 헤더를 자동으로 넣지 않는다. 인증 페이지는 호출자가
도메인별 권한을 처리하는 전송기나 이미 렌더링한 DOM을 제공해야 한다.

페이지와 미디어 리다이렉트는 각각 기본 5회, 최대 10회이다. 매 목적지에
`allowPageUrl` 또는 `allowMediaUrl`을 적용할 수 있다. 이 콜백은 URL 정책을
위한 것이며 DNS 재해석이나 사설망 접근을 완전히 차단하지 않는다. 서버에
이 기능을 노출하는 앱은 실제 연결 대상에 대한 네트워크 정책도 적용해야 한다.
서명 URL과 원본에는 비밀 값이 들어 있을 수 있으므로 호출자는 로그와 보관을
관리한다.

HTML은 기본 최대 8 MiB, 후보는 기본 500개·최대 5,000개이다. JSON-LD는 블록당
256 KiB와 깊이 64로 제한한다. 선택 자산 전송은 기본·최대 5 GiB의 스트림이며
앱이 더 작은 `maxBytes`를 지정할 수 있다. 별도의 기본 퍼블리싱 응답은 기존처럼
2 MiB로 제한된다. 버퍼 응답의 상한은 8 MiB이다. 초과는 오류 또는 차단 이슈로
반환하고 자동으로 일부만 완료 처리하지 않는다.

대표 형식은 [Open Graph](https://ogp.me/), [HTML 이미지와 srcset](https://html.spec.whatwg.org/dev/images.html),
[libxml2 HTML 파서](https://gnome.pages.gitlab.gnome.org/libxml2/html/HTMLparser_8h.html),
[libcurl URL 해석](https://curl.se/libcurl/c/curl_url_set.html)을 참고한다.
libxml2는 MIT 라이선스의 도구체인 의존성이며 이 프로젝트에 복사해 배포하지 않는다.
