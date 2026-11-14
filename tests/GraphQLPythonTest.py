"""Exercise the Python binding without live social credentials."""
import sys

sys.path.insert(0, sys.argv[2])
from iiSocialIntegration import GraphQLClient

client = GraphQLClient(sys.argv[1])
result = client.execute('{ providers { id officialApi } }', accounts={})
assert len(result["response"]["data"]["providers"]) == 11, result
result = client.execute(
    '{ request(input: {platform:"pixiv", method:"GET", path:"/me"}) { status } }',
    accounts={"pixiv": {"accessToken": "must-not-leak"}},
)
assert result["error"] == "unsupported", result
assert "must-not-leak" not in str(result), result
result = client.execute_remote("http://example.com/graphql", "query { viewer { id } }")
assert result["error"] == "invalid_request", result

if len(sys.argv) > 3:
    base, certificate = sys.argv[3:5]
    client = GraphQLClient(sys.argv[1], certificate_authority=certificate)
    accounts = {"mastodon": {"accessToken": "loopback-fixture", "instanceUrl": base}}
    read = client.execute(
        'query { request(input:{platform:"mastodon",method:"GET",path:"/api/v1/accounts/verify_credentials"}) {status body} }',
        accounts=accounts,
    )
    assert read["response"]["data"]["request"]["body"]["id"] == "loopback-user", read
    update = client.execute(
        'mutation Set($input: SocialRequestInput!) { request(input:$input) { status body } }',
        {"input": {"platform": "mastodon", "method": "PATCH", "path": "/api/v1/accounts/update_credentials",
                   "body": {"display_name": "Ada"}}}, operation_name="Set", accounts=accounts,
    )
    assert update["response"]["data"]["request"]["body"]["display_name"] == "Ada", update
    binary = client.execute(
        'mutation Run($input: SocialRequestInput!) { request(input:$input) { status body } }',
        {"input": {"platform": "mastodon", "method": "POST", "path": "/api/v1/example",
                   "bodyBase64": "YWJjAA==", "contentType": "application/octet-stream"}},
        operation_name="Run", accounts=accounts,
    )
    assert binary["response"]["data"]["request"]["body"]["accepted"], binary
    removed = client.execute(
        'mutation { request(input:{platform:"mastodon",method:"DELETE",path:"/api/v1/example"}) { status } }',
        accounts=accounts,
    )
    assert removed["response"]["data"]["request"]["status"] == 204, removed
    remote = client.execute_remote(base + "/graphql", "query Viewer($id: ID!) { viewer(id:$id) {id} }",
                                   {"id": "42"}, operation_name="Viewer", access_token="loopback-fixture")
    assert remote["response"]["data"]["viewer"]["id"] == "42" and len(remote["response"]["errors"]) == 1, remote
