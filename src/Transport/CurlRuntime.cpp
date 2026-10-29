#include "CurlRuntime.h"
#include <curl/curl.h>

namespace iiSocialIntegration::detail {
bool initializeCurl() noexcept {
    static const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
    return result == CURLE_OK;
}
} // namespace iiSocialIntegration::detail
