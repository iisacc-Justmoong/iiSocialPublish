# 소스 책임 분류

`src/Publishing`, `src/Review`, `src/Migration`, `src/Import`에 공개 헤더와 구현을 함께 둔다. HTTPS 전송은 `src/Transport`, 이전 패키지의 헤더 전용 별칭은 `src/Compatibility`가 담당한다.

빌드 forwarding 헤더와 설치 패키지는 기존 `<iiSocialIntegration/...>`과 `<iiSocialPublish/...>` 경로 및 CMake 타깃을 유지한다. `tests/source_layout.cmake`와 SDK 공통 검사가 중복 디렉터리와 책임별 배치를 검사한다.
`tests/consumer`는 신규 및 호환 패키지를 설치된 상태에서 각각 소비한다.
