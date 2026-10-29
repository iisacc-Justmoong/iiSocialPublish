#include <iiSocialPublish/Publishing.h>
#include <iiSocialPublish/Review.h>
#include <iiSocialIntegration/Migration.h>
#include <type_traits>

static_assert(std::is_same_v<iiSocialPublish::Post, iiSocialIntegration::Post>);

int main() {
    return iiSocialPublish::capabilities().empty() ? 1 : 0;
}
