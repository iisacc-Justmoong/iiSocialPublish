#include <iiSocialIntegration/GraphQL.h>
#include <iiSocialIntegration/GraphQLC.h>
#include <cstring>

class NoNetwork final : public iiSocialIntegration::Transport {
public:
    iiSocialIntegration::Response send(const iiSocialIntegration::Request &, std::stop_token) override {
        return {0, {}, {}, true};
    }
};
int main() {
    NoNetwork transport;
    iiSocialIntegration::GraphQLGateway gateway(transport, {});
    const auto result = gateway.execute({"{ providers { id officialApi } }"});
    if (result.error != iiSocialIntegration::Error::None ||
        result.responseJson.find("\"mastodon\"") == std::string::npos) return 1;
    const char input[] = R"({"document":{"query":"{ providers { id } }"}})";
    char *response = iiSocialIntegrationGraphQLExecute(input, std::strlen(input));
    if (!response) return 2;
    const bool present = std::strstr(response, "mastodon") != nullptr;
    iiSocialIntegrationGraphQLFree(response);
    return present ? 0 : 3;
}
