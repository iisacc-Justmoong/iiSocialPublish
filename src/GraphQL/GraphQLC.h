#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The returned UTF-8 JSON belongs to the caller. Free it using the function
// below. Returns NULL only when memory cannot be allocated. Synchronous.
char *iiSocialIntegrationGraphQLExecute(const char *input, size_t length);
void iiSocialIntegrationGraphQLFree(char *result);

#ifdef __cplusplus
}
#endif
