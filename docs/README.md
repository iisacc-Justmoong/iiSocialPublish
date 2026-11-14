# iiSocialIntegration Documentation

## 테스트 리소스

게시와 검토 테스트는 `resources/test-fixtures/`의 재사용 가능한 미디어를 읽는다.
로컬 TLS 검증 인증서와 키는 테스트 전용이며 실제 계정이나 서비스에 사용하지 않는다.
인증서는 10년 유효 기간으로 생성되었으며 만료 전에 갱신해야 한다. 테스트는
빌드 디렉터리에 미디어·인증서 파일을 남기지 않는다.

이전 소비자 검증 소스, 조사 자료 및 출처를 확인하지 못한 파일은
`resources/.recovery/build-history-20261030/`에 보존한다.

<!-- workspace-docs:document-map:start -->
## Human and agent document map

Markdown is the maintained human source. Agent JSON is a structured, source-traceable projection.

| Document | Agent JSON |
| --- | --- |
| [`AGENT.md`](../AGENT.md) | [JSON](entrypoints/AGENT.generated.agent.json) |
| [`README.md`](../README.md) | [JSON](entrypoints/README.generated.agent.json) |
| [`docs/DynamicLinking.md`](DynamicLinking.md) | [JSON](DynamicLinking.agent.json) |
| [`docs/Execution.md`](Execution.md) | [JSON](Execution.generated.agent.json) |
| [`docs/GraphQL.md`](GraphQL.md) | [JSON](GraphQL.agent.json) |
| [`docs/Migration.md`](Migration.md) | [JSON](Migration.agent.json) |
| [`docs/Providers.md`](Providers.md) | [JSON](Providers.generated.agent.json) |
| [`docs/Review.md`](Review.md) | [JSON](Review.generated.agent.json) |
| [`docs/SourceLayout.md`](SourceLayout.md) | [JSON](SourceLayout.agent.json) |
| [`docs/UrlImport.md`](UrlImport.md) | [JSON](UrlImport.agent.json) |
| [`docs/Verification.md`](Verification.md) | [JSON](Verification.generated.agent.json) |
| [`docs/WindowsBuildAndRun.md`](WindowsBuildAndRun.md) | [JSON](WindowsBuildAndRun.generated.agent.json) |
| [`docs/YouTube.md`](YouTube.md) | [JSON](YouTube.generated.agent.json) |
| [`docs/documentation-structure.md`](documentation-structure.md) | [JSON](documentation-structure.generated.agent.json) |
<!-- workspace-docs:document-map:end -->
