# 게시 대상별 검토와 분할 게시

`Review.h`는 Society 등의 공통 게시 내용에서 플랫폼별 게시 계획을 만드는 C++23 API이다. Qt, 파일 읽기, 렌더링, 네트워크, 계정 조회 없이 동작한다. 제목, 본문, 링크, 태그, 순서가 있는 미디어와 캡션을 입력한다. 소비자 앱은 원본과 대상별 덮어쓰기를 구분하여 합성한 뒤 호출한다. 인증 토큰은 원고에 저장하지 않는다.

`planPublication()`은 미디어 원본 인덱스와 대체 텍스트를 유지한다. X의 20장 이미지 입력은 기본적으로 4장씩 5개 게시물이 되고, 각 후속 게시물은 직전 게시물에 대한 응답이다. `Separate`는 독립 게시, `Single`은 분할 없이 게시 가능한지 확인한다. 사용자가 고른 묶음 크기는 어댑터 한도를 초과할 수 없다. GIF/영상 단독 게시가 필요한 대상은 원본 순서를 보존하여 별도로 묶는다. 초과 이미지를 버리거나 자동 크롭/영상 변환하지 않는다.

| 대상 | 기본 미디어 묶음 | 초과 처리 |
| --- | --- | --- |
| X | 사진 4장, GIF/영상 1개 | 응답 스레드 또는 독립 게시 |
| Facebook Page | 사진 10장, 영상 1개 | 독립 게시, 사진은 attached_media |
| Instagram | 캐러셀 10개 | 독립 캐러셀, 긴 캡션은 검토 오류 |
| Threads | 캐러셀 20개 | 응답 스레드 또는 독립 게시 |
| Pinterest | 미디어 1개 | 순서가 있는 개별 Pin, 명시적 보드 필요 |
| Tumblr | NPF 미디어 10개 | 독립 NPF 게시 |
| Mastodon | 기본 4개 | 응답 스레드, 실제 인스턴스 한도 사전 확인 |
| LinkedIn | 이미지 20장, 영상 1개 | 독립 게시 |
| YouTube | 영상 1개 | 영상별 게시, 정적 이미지는 오류 |
| Pixiv / ArtStation | 사이트 마무리용 전체 미디어 | 검토만 가능, 자동 업로드 불가 |

위 숫자는 어댑터의 보수적인 지원 정책이다. 모든 서비스의 계정별 최대치라는 뜻이 아니다. Facebook/Tumblr의 10개와 Instagram의 10개는 현재 어댑터가 검증한 범위를 유지한다. Instagram 앱 자체의 최대 개수와 API 지원을 동일시하지 않는다. Threads 정책은 기존 어댑터의 20개를 유지한다. Meta 문서 직접 읽기가 제한된 경우 공식 Postman 컬렉션을 프로토콜 참고로 사용한다. [공식 Threads 컬렉션](https://www.postman.com/meta/threads/documentation/dht3nzz/threads-api?entity=request-34203612-ee0a2365-9d95-4cbe-8087-1cfb04d38c05), [LinkedIn MultiImage](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/multiimage-post-api), [X 게시/응답/미디어](https://docs.x.com/x-api/posts/manage-tweets/introduction), [Mastodon 상태](https://docs.joinmastodon.org/methods/statuses/).

긴 텍스트는 텍스트 단독 게시를 지원하는 대상에서 UTF-8 경계를 보존하여 나눈다. 기본 도입문은 첫 게시물에만, 선택한 각 캡션은 해당 미디어 묶음에 둔다. 번호 표시와 도입문 반복을 선택할 수 있다. X 길이 계산은 비 ASCII를 2로 세는 보수적인 계산이며 공백으로 구분한 HTTP(S) URL을 23으로 센다. twitter-text의 정규화·이모지 조합을 완전히 재현하지 않으므로 실제 한도보다 먼저 나뉠 수 있다. 원문을 자동 요약하거나 잘라내지 않는다. 예약 시각은 유지하지만 스케줄러가 없는 즉시 실행은 차단한다.

`publishPlan()`은 작업자에서 실행하며 모든 게시물의 계정·파일·파라미터 검사를 먼저 수행한다. Mastodon은 첫 업로드 전 서버의 실제 미디어·글자 한도를 확인한다. 생성 결과 ID를 다음 응답의 `reply.in_reply_to_tweet_id` / `reply_to_id` / `in_reply_to_id`로 전달한다. X는 유료 API 구성 외에 `Account.allowSelfThreadReplies`를 네이티브 계정 구성에서 검증해야 한다. 현재 X 공식 문서에는 self-serve 응답 제한이 있으므로 자기 스레드가 모든 계정에서 허용된다고 가정하지 않는다.

중간 실패나 취소 시 즉시 멈추며 `completed`, `next`, 실패 결과를 반환한다. 이미 확인된 게시물 ID 또는 결과 불명 상태가 있으면 재조정이 필요하다. 원격 롤백이나 새 게시물 자동 재시도를 하지 않는다. 프로세스 재시작 후 정확히 한 번 배달은 호스트의 영속 원장 책임이다.

검증은 `iiSocialIntegration.Review`에서 20장 분할, 모든 대상의 미디어 보존, UTF-8 무손실 분할, 단일 게시 오류, 혼합 영상 격리, API 미지원, 예약 차단, 사전 파일 검사, 실제 응답 ID 연결, 부분 실패와 취소를 가짜 전송기로 확인한다. 실제 소셜 계정 게시·동의·과금·API 심사 완료를 의미하지 않는다.
