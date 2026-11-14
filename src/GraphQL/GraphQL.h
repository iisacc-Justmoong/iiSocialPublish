#pragma once

#include <iiSocialIntegration/Publishing.h>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace iiSocialIntegration {

// variablesJson is a JSON object. Credentials belong to the host, never here.
struct GraphQLDocument {
    std::string query;
    std::string variablesJson = "{}";
    std::string operationName;
};
struct GraphQLResult {
    // GraphQL response envelope. For a locally rejected request it has errors.
    std::string responseJson;
    Error error = Error::None;
    int httpStatus = 0; // Remote endpoint only; provider status is in data.
    bool outcomeUnknown = false;
};
using GraphQLAccountProvider = std::function<std::optional<Account>(Platform)>;

// Request-scoped bridge. No retained credentials, background work or polling.
// `request(input: SocialRequestInput!)` returns {status, body}; body is a JSON
// scalar containing the official provider's response. The mutation field
// publish(input: SocialPublishInput!) uses the existing native publisher.
// See docs/GraphQL.md for supported syntax, inputs and error contracts.
class GraphQLGateway final {
public:
    GraphQLGateway(Transport &transport, GraphQLAccountProvider accounts);
    GraphQLResult execute(const GraphQLDocument &, std::stop_token = {}) const;
    // Decode the standard JSON request envelope for an application-owned HTTP
    // host. This method does not start a server or acquire credentials.
    GraphQLResult executeEnvelope(std::string_view, std::stop_token = {}) const;
private:
    Transport &transport_;
    GraphQLAccountProvider accounts_;
};

// A trusted host configures this endpoint; it is not inferred from a platform.
// A provider's REST or Graph API is never advertised as a GraphQL endpoint.
struct GraphQLEndpoint {
    std::string url, accessToken;
};
GraphQLResult sendGraphQL(const GraphQLEndpoint &, const GraphQLDocument &,
                          Transport &, std::stop_token = {});

} // namespace iiSocialIntegration
