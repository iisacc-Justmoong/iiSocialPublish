#include <iiSocialIntegration/Migration.h>
#include <iiSocialIntegration/UrlImport.h>
#include <iostream>

int main() {
    iiSocialIntegration::ExternalPost source;
    source.platformId = "example";
    source.id = "post-1";
    source.title = "My work";
    source.body = "An existing post.";
    auto draft = iiSocialIntegration::importPost(source);
    const auto json = iiSocialIntegration::serializeSocietyDraft(draft);
    if (!json) return 1;
    if (!iiSocialIntegration::initializeUrlImport()) return 1;
    auto page = iiSocialIntegration::parsePageDom("https://example.com/post", "<html><img src='/art.png'></html>");
    if (!page.ok() || page.post.media.size() != 1) return 1;
    std::cout << *json << '\n';
}
