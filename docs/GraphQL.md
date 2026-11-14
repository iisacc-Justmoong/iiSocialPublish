# 소셜 API의 GraphQL 경계

앱은 C++23 `GraphQLGateway`, C ABI 또는 Python `GraphQLClient`로 소셜 API를
호출할 수 있다. 소셜 계정, 권한과 전송 객체는 호스트가 공급한다. 게시뿐 아니라
권한이 허용하는 계정 정보, 목록, 통계, 설정 등도 요청할 수 있다. 모듈은 생성자에서
서버나 스레드를 시작하지 않고 요청하지 않은 조회·폴링을 수행하지 않는다. 네트워크
호출은 동기식이므로 앱의 작업 실행기에서 호출한다.

두 경로는 명확히 다르다.

* **SDK 내부 브리지:** GraphQL `request`를 선택한 제공자의 공식 HTTP API로
  변환한다. `publish`는 기존 `Publisher`를 호출해 업로드와 기존 검증·취소·복구
  계약을 유지한다. 호스트 HTTP 서버는 `executeEnvelope`로 표준 JSON 요청 봉투를
  전달할 수 있다. 이 모듈은 HTTP 리스너를 열지 않는다.
* **원격 GraphQL 클라이언트:** `sendGraphQL`은 호스트가 신뢰하는 HTTPS GraphQL
  서버에 문서·변수·작업 이름을 그대로 전송하고 서버의 응답 봉투를 반환한다.
  이 경로에는 SDK 내부 브리지의 문법 제한이 적용되지 않는다.

Meta의 Graph API는 HTTP 기반 소셜 그래프 API이며 GraphQL과 다른 계약이다.
[Meta 공식 개요](https://developers.facebook.com/docs/graph-api/overview),
[Mastodon REST API](https://docs.joinmastodon.org/api/),
[YouTube Data API](https://developers.google.com/youtube/v3/getting-started),
[LinkedIn OpenID Connect](https://learn.microsoft.com/linkedin/consumer/integrations/self-serve/sign-in-with-linkedin-v2)를
참조한다. 제공자의 HTTP 응답을 받았다는 사실은 해당 제공자가 GraphQL 서버를
운영한다는 뜻이 아니다.

## 내부 브리지의 스키마와 지원 범위

`providers`, `request`, `publish`가 루트 필드이다. 아래는 API의 형태를
설명하는 스키마이며 런타임에서 스키마 정의 문서를 입력받는다는 뜻이 아니다.

```graphql
scalar JSON
input SocialRequestInput {
  platform: String!, method: String!, path: String!, apiFamily: String
  query: JSON, headers: JSON, anonymous: Boolean, body: JSON
  bodyText: String, bodyBase64: String, contentType: String
}
input SocialPublishInput { platform: String!, post: JSON! }
type SocialProvider { id: String!, name: String!, officialApi: Boolean!, publishingApi: Boolean! }
type SocialResponse { status: Int!, body: JSON, bodyBase64: String!, headers: JSON!, outcomeUnknown: Boolean! }
type SocialPublishResult {
  published: Boolean!, postId: String!, url: String!, error: String!, message: String!
  httpStatus: Int!, retryAfterSeconds: Int!, requiresReconciliation: Boolean!
}
type Query { providers: [SocialProvider!]!, request(input: SocialRequestInput!): SocialResponse }
type Mutation {
  request(input: SocialRequestInput!): SocialResponse
  publish(input: SocialPublishInput!): SocialPublishResult
}
```

문서에는 `query`와 `mutation`, 이름 있는 다중 작업, `operationName`,
변수·기본값, 별칭, 주석, 스칼라·배열·객체 인자와 여러 루트 필드를 사용할 수
있다. 응답의 `body`·`headers`는 JSON 스칼라이며 내부에 GraphQL 선택 집합을
붙이지 않는다. API가 지원하는 필드 선택이나 페이지 커서는 제공자별 `query`
인자로 전달한다.

**내부 브리지는 전체 GraphQL 엔진이 아니다.** 프래그먼트, 디렉티브,
인스펙션, 구독, 복잡한 스키마 검증과 중복 응답 필드 병합은 지원하지 않으며
명시적 오류를 반환한다. 원격 서버에 필요한 전체 GraphQL 문법은 `sendGraphQL`로
그 서버에 전달한다. 호스트는 이 제한을 감안해 클라이언트를 구성해야 한다.

## 제공자 API 조회와 변경

```graphql
query Profile($input: SocialRequestInput!) {
  account: request(input: $input) { status body headers }
}
```

```json
{
  "input": {
    "platform": "mastodon",
    "method": "GET",
    "path": "/api/v1/accounts/verify_credentials"
  }
}
```

`SocialRequestInput`은 `platform`, `method`, `path`를 필수로 받는다.
선택 필드는 `apiFamily`, `query`, `headers`, `anonymous`, `body`,
`bodyText`, `bodyBase64`, `contentType`이다. `query`는 문자열·숫자·불리언
또는 그 배열의 JSON 객체이다. 세 본문 필드는 서로 배타적이다. `body`는 JSON,
`bodyText`는 UTF-8 텍스트, `bodyBase64`는 바이트를 나타낸다.
`contentType`을 지정하지 않은 JSON 본문에는 `application/json`을 사용한다.

`query` 작업은 GET만 허용한다. `mutation`은 POST·PUT·PATCH·DELETE를
허용한다. `anonymous: true`는 명시적 GET에서만 토큰 전송을 생략한다. 이 경우에도
호스트가 선택한 플랫폼의 인스턴스나 버전 설정을 공급해야 하며, 제공자가 공개
접근을 허용한다는 보장은 없다. 요청별 OAuth 범위와 제공자 심사·요금은 호스트가
확인하고 제공자가 최종 판정한다.

경로는 표의 원점에 상대적이다. 절대 URL, 쿼리 문자열, 역슬래시, 기입된 퍼센트
인코딩과 `.`·`..` 경로 구성요소는 받지 않는다. 경로와 질의 값은 SDK가
퍼센트 인코딩한다. 토큰, 비밀번호와 같은 자격 증명은 문서·변수·경로·질의에 넣지
않고 호스트의 계정 공급자로 전달한다. `Authorization` 같은 헤더는 직접 덮어쓸
수 없다. 요청 헤더는 `If-Match`, `If-None-Match`, `Idempotency-Key`,
`Accept-Language`, `X-RestLi-Method`만 허용한다. GraphQL 객체 리터럴에서
하이픈 대신 밑줄을 사용할 수 있다.

| 플랫폼 | 기본 공식 API 주소 | 추가 `apiFamily` |
| --- | --- | --- |
| X | `https://api.x.com` | 없음 |
| Facebook | `https://graph.facebook.com/{apiVersion}` | `video`: `graph-video.facebook.com` |
| Instagram | `https://graph.instagram.com/{apiVersion}` | `facebook`: `graph.facebook.com` |
| Threads | `https://graph.threads.net/v1.0` | 없음 |
| Pinterest | `https://api.pinterest.com/v5` | 없음 |
| Tumblr | `https://api.tumblr.com/v2` | 없음 |
| Mastodon | 호스트가 지정한 HTTPS 인스턴스 원점 | 없음 |
| LinkedIn | `https://api.linkedin.com/rest` | `v2`: `https://api.linkedin.com/v2` |
| YouTube | `https://www.googleapis.com/youtube/v3` | `upload`: `/upload/youtube/v3` |
| Pixiv·ArtStation | 미지원 | 검증된 공식 통합 계약을 확보하기 전에는 전송하지 않음 |

Meta의 `apiVersion`과 LinkedIn `rest`의 YYYYMM 버전은 호스트가 명시한다.
LinkedIn의 `v2/userinfo`에는 마케팅 API 버전 헤더를 붙이지 않는다. X API는
`allowBillableRequests`가 명시적으로 활성화된 계정에서만 호출한다.
`providers.officialApi`는 이 브리지에 공식 API 원점이 구성되었다는 의미이지,
모든 개별 엔드포인트·권한이 승인되었음을 뜻하지 않는다.

## 기존 게시 기능

```graphql
mutation Publish($input: SocialPublishInput!) {
  publish(input: $input) { published postId error requiresReconciliation }
}
```

`input`은 `platform`과 `post`를 받는다. `post`의 필드는 C++ `Post`와
같은 `text`, `title`, `link`, `tags`, `media`, `coverImageUrl`,
`visibility`, `contentWarning`, `sensitive`, `madeWithAi`, `idempotencyKey`,
`replyToId`, `language`, `replyControl`, `youtube`이다. 미디어 항목은
`kind`(`image` 또는 `video`), `localFile`, `publicUrl`, `mimeType`,
`altText`를 사용한다. 로컬 경로는 신뢰된 호스트만 공급해야 한다.
제공자별 지원 형식·권한·업로드 제한은 [게시 제공자 명세](Providers.md)와
[실행 명세](Execution.md)를 따른다. 일반 `request`의 인라인 본문과 응답에는
크기 제한이 있으므로 대용량 게시 미디어는 기존 스트리밍 퍼블리셔를 사용한다.

## C++와 Python

```cpp
#include <iiSocialIntegration/GraphQL.h>
using namespace iiSocialIntegration;

CurlTransport transport;
GraphQLGateway gateway(transport, [](Platform provider) -> std::optional<Account> {
    // 앱의 인증 저장소에서 해당 제공자 계정을 가져온다.
    return accountForProvider(provider);
});
GraphQLDocument document{queryText, variablesJson, "Profile"};
auto result = gateway.execute(document, stopToken);
// result.responseJson은 GraphQL 응답 봉투이고 result.error는 전송 상태이다.
```

설치된 Python 파일은 `lib/python/iiSocialIntegration.py`에 있다. 네이티브 공유
라이브러리를 명시하거나 시스템 로더가 찾을 수 있도록 설정한다.

```python
from iiSocialIntegration import GraphQLClient

client = GraphQLClient(library_path)
result = client.execute(query, variables, operation_name="Profile",
                        accounts={"mastodon": account_from_secure_store})
response = result["response"]
```

Python 계정 객체는 `accessToken`, `instanceUrl`, `apiVersion`, `targetId`,
`scopes`, `expiresAt`(Unix 초), `allowBillableRequests`,
`allowSelfThreadReplies`를 사용할 수 있다. 원격 서버는
`client.execute_remote(endpoint, query, variables, operation_name=..., access_token=...)`로
호출한다. C 사용자는 `GraphQLC.h`의 JSON 호출 함수와 대응하는 해제 함수를 쓴다.
Python 바인딩은 공유 라이브러리를 필요로 하며 네이티브 라이브러리 자체는 Python
인터프리터를 요구하지 않는다.

## 오류, 크기 및 수명

전체 문서와 모든 루트 입력을 먼저 검사하므로 잘못된 뒤쪽 필드 때문에 앞선 변경이
실행되지는 않는다. 실행 중 독립 필드가 실패하면 그 필드는 `null`과 경로가 있는
GraphQL 오류로 반환되고 다른 필드의 성공 데이터는 유지된다. HTTP 오류 응답은
`status`, 허용된 응답 `headers`, JSON `body`로 받을 수 있다.
`Set-Cookie`와 인증 헤더는 반환하지 않는다. JSON이 아닌 본문은 `body`가
`null`이며 `bodyBase64`로 받을 수 있다. 본문과 GraphQL 데이터는 제공자에서
받은 사적 정보를 포함할 수 있으므로 호출자가 접근과 기록을 관리해야 한다.
페이지 이동용 `Link`와 업로드 등에 사용되는 `Location` 헤더도 반환한다.
이 값에 임시 접근 권한이나 민감한 쿼리 값이 포함될 수 있으므로 기록하지 않아야
하며 SDK가 해당 주소를 자동으로 따라가거나 토큰을 전달하지는 않는다.

중단·전송 오류 뒤 변경 요청이 이미 전송되었거나 변경 요청이 5xx를 받으면
`outcome_unknown`을 반환한다. 일반 `request`와 `sendGraphQL`은 자동
재시도하지 않으며 호출자는 원격 상태를 확인해야 한다. `publish`는 기존
퍼블리셔를 사용하므로 YouTube의 동일 업로드 세션에 대한 제한된 상태 확인·재개는
기존 [실행 계약](Execution.md)을 따른다. 게시 결과의 `requiresReconciliation`도
기존 퍼블리셔 결과를 그대로 반영한다. 취소는 C++ `stop_token`으로 전달한다.

문서는 최대 64 KiB, 변수는 1 MiB, 루트 필드는 32개, 선택 필드는 128개이다.
개별 응답 본문은 최대 2 MiB, 한 내부 브리지 요청의 전체 응답 본문은 최대 8 MiB이다.
입력 변수와 응답 JSON은 최대 20단계의 중첩을 허용한다.
기본 `CurlTransport`는 HTTPS와 인증서·호스트 검증을 강제하고 리다이렉트를
따라가지 않는다. 호스트가 직접 제공하는 `Transport`는 동일한 보안 계약을
지켜야 한다. 이 API는 신뢰된 앱 백엔드용이며 무인증 범용 프록시로 노출해서는 안 된다.
