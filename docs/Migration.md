# 외부 게시물의 Society 초안 변환

`Migration.h`는 호출자가 이미 확보한 외부 게시물 하나 또는 여러 개를
`SocietyPost`와 원본 `ExternalPost`가 함께 들어 있는 `MigrationResult`로
변환한다. Society 웹의 [publishing-request v1 계약](../../../Service/Society.iisacc.com/backend/config/contracts/publishing-request.schema.json)을
기준으로 `serializeSocietyDraft`가 JSON을 만든다. 이 객체는 서버가 보유한
영속 `CanvasPost`가 아니다. 서버가 인증된 작성자, 게시물·첨부 ID, 수정
버전과 시각을 생성하고 검증한다. 외부 원본 ID를 Society 식별자나 포크
관계로 위장하지 않는다.

## 호출과 첨부 등록

```cpp
#include <iiSocialIntegration/Migration.h>
using namespace iiSocialIntegration;

ExternalPost source;
source.platformId = "my-platform";
source.id = "source-post-123";
source.title = "A new home for my work";
source.body = "An existing post";
source.media.push_back({"source-media", "https://example.com/art.png",
                        "image", "image/png", "", "Artwork description"});
auto result = importPost(source);

// 애플리케이션이 권한을 확인하고 실제 바이트를 Society에 등록한 뒤
// 서버가 반환한 UUID를 원래 순서에 맞추어 대입한다.
result.post.media[0].assetId = "00000000-0000-4000-8000-000000000001";
auto request = serializeSocietyDraft(result);
// request는 Society의 편집용 JSON이며, 전송 및 게시 여부는 앱이 결정한다.
```

`importPosts`는 정규화된 여러 `ExternalPost` 또는 같은 플랫폼의 여러 JSON을
입력 순서대로 변환한다. 한 항목의 실패가 다른 항목을 중단하지 않는다.
`supportsNativeImport`는 네이티브 JSON 변환기 유무를 확인한다.
`importPost(Platform, json)`은 하나의 플랫폼 원본 JSON 객체를 받아 다음
필드를 읽는다. 응답 목록은 호출자가 개별 객체로 나누고 필요한 확장 필드를
미리 요청해야 한다. 원본 JSON은 `result.source.originalJson`에 보존되며
Society 요청에는 포함되지 않는다. 원본에는 개인정보가 들어 있을 수 있으므로
그 보관과 접근 정책은 호출자가 정한다. 8 MiB를 넘는 원본 JSON은 복사하거나
파싱하지 않고 오류를 반환한다.

| 플랫폼 | 인식하는 원본 범위 | 첨부 변환 경계 |
| --- | --- | --- |
| X | v2 단일 `data`와 `includes.media`, 텍스트·언어·해시태그 | `attachments.media_keys` 순서 보존, 누락 확장은 미해결 항목으로 유지 |
| Facebook | 게시물 `message`, 확장된 `attachments.data`와 하위 첨부 | 영상 미리보기 이미지를 영상 원본으로 사용하지 않음 |
| Instagram / Threads | 단일 미디어, 텍스트·캡션과 확장된 `children.data` | 미확장 캐러셀은 보류 항목과 경고를 반환 |
| Pinterest | Pin 제목·설명과 `media` | 이미지 `images.originals.url`만 원본 후보로 제공 |
| Tumblr | Neue Post Format 텍스트·이미지·영상·오디오 블록과 태그 | 텍스트와 미디어의 교차 배치나 서식은 경고하고 원본 JSON을 보존 |
| Mastodon | Status 텍스트·언어·해시태그·민감 표시와 `media_attachments` | HTML은 호출자가 제공한 변환기가 필요 |
| LinkedIn | Posts API `commentary`, 단일 `media`, `multiImage.images` | 미디어 URN은 다운로드 URL로 취급하지 않음 |
| YouTube | Video `snippet`과 `status` | API 메타데이터는 영상 파일이 아니므로 영상 원본 등록이 필요 |
| Pixiv / ArtStation / 기타 | `ExternalPost`를 통한 앱의 명시적 정규화 | 미검증 네이티브 JSON은 차단 결과 반환 |

이 매핑은 모든 플랫폼 게시 유형과 모든 필드의 무손실 재현을 뜻하지 않는다.
Tumblr의 미지원 블록 등은 차단하고, 레이아웃·서식 손실이나 원본 민감
표시는 `issues`에 알린다. 원본의 가시성, 저작권, 콘텐츠 경고 및 작성자
표시는 확인 가능한 원본 메타데이터로 남으며 Society 권한이나 콘텐츠 분류로
자동 변환하지 않는다. 기본 요청은 항상 `private`·`draft`이다. 원본의 공개
범위나 민감 표시만으로 더 넓게 공개하거나 성인물로 단정하지 않는다.

Society 미디어는 서버에 등록된 자산 UUID가 필요하다. 외부 URL이나 URN은
출처일 뿐이다. `importPost`는 미디어를 자동으로 가져오거나 등록하지 않는다.
호출자는 [URL 이관 API](UrlImport.md)에서 선택한 자산을 가져올 수 있으며,
권한과 바이트를 검증해 등록하고 각 `assetId`를 설정해야 `ready()`와 직렬화가
성공한다. Society 서버의 현재 수집 가능 미디어 종류도 별도로 확인해야 한다.
반복 이관의 중복 방지 원장은 `platformId`, 원본 계정 및 게시물 ID 등을
이용해 애플리케이션이 소유한다.

## Pixiv·ArtStation 계정에서 가져오기 검토

2026-10-28 기준 이 SDK에는 두 플랫폼에 로그인하거나 로그인된 계정의
게시물 목록을 조회하고 소유자를 검증하는 구현이 없다. `ExternalPost`의
`authorId`와 `parsePageDom`의 DOM은 호출자가 제공한 데이터이며, 값이 있다는
사실만으로 로그인이나 본인 게시물 소유가 검증되지는 않는다.

[ArtStation의 개인 RSS](https://magazine.artstation.com/2016/02/artist-rss-feeds/)는
공식적으로 제공되는 최신 공개 작품 탐색 경로이지만, 로그인된 계정의 전체
게시물이나 숨겨진 프로젝트의 목록을 보장하지 않는다. [게시물에 표시되는
이미지](https://help.artstation.com/en/articles/16155257-how-do-i-protect-my-artwork-from-theft-can-i-disable-the-download-button-on-images)는
웹용으로 축소되었을 수 있어 원본 파일이라고 간주할 수 없다. Pixiv도
[공개 범위와 열람 설정](https://www.pixiv.help/hc/en-us/articles/42890163268633-Why-are-some-of-my-posts-not-appearing-with-the-others)에
따라 프로필에 표시되지 않는 게시물이 있다.

ArtStation은 [포트폴리오 관리 화면](https://help.artstation.com/en/articles/16155244-my-project-is-not-visible-help)에서
공개 프로필에 표시되지 않는 프로젝트도 보여 준다. 이 사실은 인증된 관리
화면을 통한 자동 동기화가 구현되었다거나 안정적으로 동작한다는 증거는 아니다.
Pixiv에서 별도 제공자가 만든 [브라우저 백업 도구](https://addons.mozilla.org/en-US/firefox/addon/pixiv-exporter/)도
확인되지만, 내보내기 기능의 설명만으로 이 SDK의 지속적인 계정 동기화 기능을
증명할 수 없다. 해당 도구는 Pixiv가 제공하거나 보증하는 연동이 아니다.

따라서 공개 피드 또는 단일 URL 가져오기를 로그인된 계정의 전체 백업으로
표시하지 않는다. 해당 계정의 인증된 세션, 각 게시물의 소유자 및 가져올
미디어가 확인되기 전에는 본인 게시물 자동 수집을 지원한다고 보고하지 않는다.

HTML 본문은 `MigrationOptions::htmlToPlainText`로 호출자가 신뢰하는 변환기를
제공해야 한다. 변환기가 없거나 `nullopt`를 반환하면 원본 HTML을 보존하고
직렬화를 차단한다. 텍스트는 UTF-8 코드 포인트 기준 Society 필드 한도와
미디어 개수·자산 UUID를 검사한다. 긴 본문이나 태그를 잘라내지 않으며
유효한 초안으로 편집할 때까지 `serializeSocietyDraft`는 `nullopt`를 반환한다.
제목 없는 초안 생성은 가능하지만 Society 게시에는 제목이 필요하다.

## 원본 계약과 검증

구현의 대표 원본 형식은 [Mastodon Status](https://docs.joinmastodon.org/entities/Status/),
[YouTube Video](https://developers.google.com/youtube/data/reference/rest/v3/videos),
[Tumblr NPF](https://github.com/tumblr/docs/blob/master/npf-spec.md),
[LinkedIn MultiImage](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/multiimage-post-api)의
공식 계약을 참조한다. `Migration.h`의 제공자별 변환은 응답에 실제로 포함된
필드만 변환하며 네트워크 요청을 수행하지 않는다. `iiSocialIntegration.Migration`은 정규 입력, 플랫폼
예제, 순서, 원본 보존, HTML 차단, 자산 등록, UTF-8과 한도를 검사한다.
실제 외부 계정 접근, 미디어 다운로드, Society 서버 게시나 운영 배포의 증거는 아니다.

동일 Workspace에서 Society 서비스의 설치된 `json_schemer`를 사용할 수 있다면
서버의 현재 스키마에 실제 생성 JSON을 대조할 수 있다. 이 선택 검사는 SDK의
빌드·링크 의존성에 Society를 추가하지 않는다.

```sh
cd ../../Service/Society.iisacc.com
python3 scripts/backend.py ruby ../../../SDK/iiSocialIntegration/tests/VerifySocietyContract.rb \
  ../../../SDK/iiSocialIntegration/build/iiSocialIntegrationMigrationTests config/contracts
```
