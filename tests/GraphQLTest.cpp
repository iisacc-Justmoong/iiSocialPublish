#include <iiSocialIntegration/GraphQL.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace iiSocialIntegration;
using Json = nlohmann::json;

namespace {
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
struct Fake final : Transport {
    std::vector<Request> requests;
    std::vector<Response> responses;
    Response send(const Request &request, std::stop_token) override {
        requests.push_back(request);
        if (responses.empty()) throw std::runtime_error("unexpected network request");
        auto response=responses.front(); responses.erase(responses.begin()); return response;
    }
};
Account account(Platform platform) {
    Account result; result.accessToken="test-secret"; result.allowBillableRequests=true;
    result.apiVersion=platform==Platform::LinkedIn ? "202607" : "v25.0";
    result.instanceUrl="https://social.example"; result.targetId="123";
    for (const auto &cap : capabilities()) if (cap.platform==platform) result.scopes=cap.requiredScopes;
    return result;
}
std::string header(const Request &r, const std::string &name) {
    for (const auto &[k,v]:r.headers) if (k==name) return v;
    return {};
}
Json result(const GraphQLResult &r) { return Json::parse(r.responseJson); }
void tests() {
    Fake fake;
    GraphQLGateway gateway(fake, [](Platform p) { return std::optional<Account>(account(p)); });
    fake.responses.push_back({200,{},R"({"id":"7","name":"Ada"})"});
    GraphQLDocument profile{R"(query Profile($who: SocialRequestInput!) {
      profile: request(input: $who) { status body }
    })", R"({"who":{"platform":"facebook","method":"GET","path":"/me","query":{"fields":"id,name"}}})", "Profile"};
    const auto a=result(gateway.execute(profile));
    check(a["data"]["profile"]["status"]==200 && a["data"]["profile"]["body"]["name"]=="Ada", "profile response");
    check(fake.requests.size()==1 && fake.requests[0].url=="https://graph.facebook.com/v25.0/me?fields=id%2Cname", "official provider route");
    check(header(fake.requests[0],"Authorization")=="Bearer test-secret", "credential not attached");
    check(fake.requests[0].url.find("test-secret")==std::string::npos, "credential leaked into URL");

    fake.responses.push_back({200,{},R"({"id":"42"})"});
    auto b=result(gateway.execute({R"(mutation Update($r: SocialRequestInput!) { item: request(input: $r) { status body } })",
      R"({"r":{"platform":"mastodon","method":"PATCH","path":"/api/v1/accounts/update_credentials","body":{"display_name":"Ada"}}})", "Update"}));
    check(b["data"]["item"]["body"]["id"]=="42" && fake.requests.back().method=="PATCH", "non-publishing mutation");
    check(fake.requests.back().url=="https://social.example/api/v1/accounts/update_credentials", "mastodon route");

    fake.responses.push_back({201,{},R"({"data":{"id":"post-1"}})"});
    auto pub=result(gateway.execute({R"(mutation Post($input: SocialPublishInput!) { publish(input:$input) { published postId error requiresReconciliation } })",
        R"({"input":{"platform":"x","post":{"text":"hello"}}})", "Post"}));
    check(pub["data"]["publish"]["published"]==true && pub["data"]["publish"]["postId"]=="post-1",
          "GraphQL publish did not use the existing publisher");
    check(fake.requests.back().url=="https://api.x.com/2/tweets", "wrong publish route");

    auto before=fake.requests.size();
    const auto available=result(gateway.execute({R"({ providers { id officialApi publishingApi } })"}));
    check(available["data"]["providers"].size()==11 && fake.requests.size()==before, "provider capability discovery");
    for (const auto &entry:available["data"]["providers"])
        if (entry["id"]=="pixiv" || entry["id"]=="artstation")
            check(entry["officialApi"]==false, "unverified API advertised");

    fake.responses.push_back({429,{{"Retry-After","30"},{"Link","<https://social.example/api/v1/example?max_id=2>; rel=next"},{"Set-Cookie","secret"}},R"({"error":"limited"})"});
    auto limited=gateway.execute({R"(query { request(input:{platform:"mastodon",method:"GET",path:"/api/v1/accounts/verify_credentials",headers:{If_None_Match:"v1"}}) { status headers body outcomeUnknown } })"});
    auto limitedJson=result(limited);
    check(limitedJson["data"]["request"]["status"]==429 && limitedJson["data"]["request"]["body"]["error"]=="limited"
          && limitedJson["data"]["request"]["headers"]["retry-after"]=="30"
          && limitedJson["data"]["request"]["headers"]["link"]=="<https://social.example/api/v1/example?max_id=2>; rel=next"
          && !limitedJson["data"]["request"]["headers"].contains("set-cookie")
          && limited.error==Error::RateLimited, "provider error response was not preserved safely");
    check(header(fake.requests.back(),"If-None-Match")=="v1", "safe conditional header omitted");
    before=fake.requests.size();
    auto unsupported=result(gateway.execute({R"({ request(input: {platform:"pixiv", method:"GET", path:"/me"}) { status } })"}));
    check(unsupported.contains("errors") && fake.requests.size()==before, "unsupported provider issued network request");
    auto unsafe=result(gateway.execute({R"(query { request(input:{platform:"facebook",method:"GET",path:"/../evil"}) { status } })"}));
    check(unsafe.contains("errors") && fake.requests.size()==before, "path traversal accepted");
    auto invalid=result(gateway.execute({R"(query { request(input:{platform:"facebook",method:"POST",path:"/me"}) {status} })"}));
    check(invalid.contains("errors") && fake.requests.size()==before, "query executed mutation");
    auto secret=result(gateway.execute({R"({ request(input:{platform:"facebook",method:"GET",path:"/me",query:{access_token:"secret"}}) {status} })"}));
    check(secret.contains("errors") && fake.requests.size()==before, "token in URL accepted");
    auto schema=result(gateway.execute({R"(mutation { a: request(input:{platform:"facebook",method:"POST",path:"/me",body:{x:1}}) {status} b: unknown {status} })"}));
    check(schema.contains("errors") && fake.requests.size()==before, "invalid document caused side effects");
    auto headerAttack=result(gateway.execute({R"(query { request(input:{platform:"facebook",method:"GET",path:"/me",headers:{Authorization:"stolen"}}) {status} })"}));
    check(headerAttack.contains("errors") && fake.requests.size()==before, "authorization override accepted");

    fake.responses.push_back({200,{},R"({"data":{"viewer":{"id":"1"}},"errors":[{"message":"partial"}]})"});
    auto remote=sendGraphQL({"https://gateway.example/graphql","remote-secret"},
      {"query Get($id: ID!) { viewer(id: $id) { id } }",R"({"id":"1"})","Get"},fake);
    const auto parsed=result(remote);
    check(parsed["data"]["viewer"]["id"]=="1" && parsed["errors"].size()==1, "remote partial data lost");
    auto wire=Json::parse(fake.requests.back().body);
    check(wire["operationName"]=="Get" && wire["variables"]["id"]=="1", "remote request lost variables");

    struct Route { const char *id; const char *expected; };
    const Route routes[]{
        {"x","https://api.x.com/example"},
        {"facebook","https://graph.facebook.com/v25.0/example"},
        {"instagram","https://graph.instagram.com/v25.0/example"},
        {"threads","https://graph.threads.net/v1.0/example"},
        {"pinterest","https://api.pinterest.com/v5/example"},
        {"tumblr","https://api.tumblr.com/v2/example"},
        {"mastodon","https://social.example/example"},
        {"linkedin","https://api.linkedin.com/rest/example"},
        {"youtube","https://www.googleapis.com/youtube/v3/example"}
    };
    for (const auto &route : routes) {
        fake.responses.push_back({200,{},R"({"ok":true})"});
        Json variables{{"input",{{"platform",route.id},{"method","GET"},{"path","/example"}}}};
        const auto answer=gateway.execute({R"(query Q($input: SocialRequestInput!) { request(input:$input) {status body} })",variables.dump(),"Q"});
        check(answer.error==Error::None && fake.requests.back().url==route.expected, "provider base routing");
    }
    fake.responses.push_back({200,{},std::string("\0\x01\xff",3)});
    auto binary=result(gateway.execute({R"({ request(input:{platform:"mastodon",method:"GET",path:"/binary"}) {status body bodyBase64} })"}));
    check(binary["data"]["request"]["body"]==nullptr && binary["data"]["request"]["bodyBase64"]=="AAH/", "binary response was not preserved");

    Response lost; lost.transportFailed=true; lost.mayHaveBeenSent=true;
    fake.responses.push_back(lost);
    auto uncertain=gateway.execute({R"(mutation { request(input:{platform:"mastodon",method:"DELETE",path:"/api/v1/statuses/1"}) {status} })"});
    check(uncertain.error==Error::OutcomeUnknown && uncertain.outcomeUnknown && result(uncertain).contains("errors"),
          "ambiguous mutation should require reconciliation");

    fake.responses.push_back({200,{},R"({"id":"selected"})"});
    auto selected=result(gateway.execute({R"(query One { first: request(input:{platform:"x",method:"GET",path:"/one"}) {status} }
       query Two($p: SocialRequestInput!) { second: request(input:$p) {body} })",
       R"({"p":{"platform":"mastodon","method":"GET","path":"/two"}})", "Two"}));
    check(selected["data"]["second"]["body"]["id"]=="selected" && fake.requests.back().url=="https://social.example/two",
          "named operation selection");

    std::stop_source cancelled; cancelled.request_stop(); before=fake.requests.size();
    auto cancelledResult=gateway.execute({R"({ request(input:{platform:"mastodon",method:"GET",path:"/one"}) {status} })"},cancelled.get_token());
    check(cancelledResult.error==Error::Cancelled && fake.requests.size()==before, "cancelled request used transport");
    auto badRemote=sendGraphQL({"https://user:secret@example.com/graphql","token"},{"{ viewer { id } }"},fake);
    check(badRemote.error==Error::InvalidRequest && fake.requests.size()==before, "unsafe endpoint accepted");

    fake.responses.push_back({200,{},R"({"accepted":true})"});
    auto raw=gateway.execute({R"(mutation { request(input:{platform:"mastodon",method:"POST",path:"/api/v1/example",bodyBase64:"YWJjAA==",contentType:"application/octet-stream"}) {status} })"});
    check(raw.error==Error::None && fake.requests.back().body==std::string("abc\0",4)
          && header(fake.requests.back(),"Content-Type")=="application/octet-stream", "binary request body not sent");

    fake.responses.push_back({500,{},R"({"error":"service unavailable"})"});
    auto uncertainStatus=gateway.execute({R"(mutation { request(input:{platform:"mastodon",method:"DELETE",path:"/api/v1/statuses/2"}) {status body outcomeUnknown} })"});
    check(uncertainStatus.error==Error::OutcomeUnknown && uncertainStatus.outcomeUnknown
          && result(uncertainStatus)["data"]["request"]["status"]==500, "ambiguous status or body lost");
    fake.responses.push_back({401,{},R"({"errors":[{"message":"not authorized"}]})"});
    auto remoteAuth=sendGraphQL({"https://gateway.example/graphql","token"},{"{ viewer { id } }"},fake);
    check(remoteAuth.error==Error::AuthenticationRequired && remoteAuth.httpStatus==401
          && result(remoteAuth)["errors"].size()==1, "remote GraphQL HTTP error lost");
    before=fake.requests.size();
    const std::string deep="{\"x\":"+std::string(100,'[')+"1"+std::string(100,']')+"}";
    auto tooDeep=gateway.execute({"{ providers { id } }",deep});
    check(tooDeep.error==Error::InvalidRequest && fake.requests.size()==before, "deep variables accepted");

    Response broken; broken.transportFailed=true;
    fake.responses.push_back(broken);
    fake.responses.push_back({200,{},R"({"id":"healthy"})"});
    const auto partial=result(gateway.execute({R"(query { a: request(input:{platform:"mastodon",method:"GET",path:"/a"}) {body}
        b: request(input:{platform:"mastodon",method:"GET",path:"/b"}) {body} })"}));
    check(partial["data"]["a"]==nullptr && partial["data"]["b"]["body"]["id"]=="healthy"
          && partial["errors"][0]["path"][0]=="a", "partial response did not preserve successful fields");

    before=fake.requests.size();
    auto invalid64=gateway.execute({R"(mutation { request(input:{platform:"mastodon",method:"POST",path:"/x",bodyBase64:"AB=="}) {status} })"});
    check(invalid64.error==Error::InvalidRequest && fake.requests.size()==before, "invalid base64 accepted");
    auto badVariable=gateway.execute({R"(query Q($input: SocialRequestInput!) { request(input:$input) {status} })",R"({"input":"not-an-object"})","Q"});
    check(badVariable.error==Error::InvalidRequest && fake.requests.size()==before, "incorrect GraphQL variable type accepted");

    Fake publicTransport;
    GraphQLGateway publicGateway(publicTransport, [](Platform p) {
        auto configured=account(p); configured.accessToken.clear(); return std::optional<Account>(configured);
    });
    publicTransport.responses.push_back({200,{},R"({"ok":true})"});
    const auto publicRead=publicGateway.execute({R"({ request(input:{platform:"mastodon",method:"GET",path:"/api/v2/instance",anonymous:true}) {status} })"});
    check(publicRead.error==Error::None && header(publicTransport.requests.back(),"Authorization").empty(),
          "explicit anonymous public read failed");
    const auto anonymousWrite=publicGateway.execute({R"(mutation { request(input:{platform:"mastodon",method:"POST",path:"/x",anonymous:true}) {status} })"});
    check(anonymousWrite.error==Error::InvalidRequest && publicTransport.requests.size()==1, "anonymous mutation accepted");

    Fake linkedinTransport;
    GraphQLGateway linkedIn(linkedinTransport, [](Platform p) {
        auto configured=account(p); configured.apiVersion.clear(); return std::optional<Account>(configured);
    });
    linkedinTransport.responses.push_back({200,{},R"({"sub":"user"})"});
    const auto identity=linkedIn.execute({R"({ request(input:{platform:"linkedin",apiFamily:"v2",method:"GET",path:"/userinfo"}) {status body} })"});
    check(identity.error==Error::None && linkedinTransport.requests.back().url=="https://api.linkedin.com/v2/userinfo"
          && header(linkedinTransport.requests.back(),"LinkedIn-Version").empty(),
          "LinkedIn OAuth profile route requires its official v2 family");
    const auto httpBody=gateway.executeEnvelope(R"({"query":"query Catalog { providers { id } }","operationName":"Catalog","variables":{}})");
    check(result(httpBody)["data"]["providers"].size()==11, "GraphQL HTTP JSON request envelope failed");
}
}
int main() { try { tests(); return 0; } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; } }
