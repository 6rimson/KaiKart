/* kaiHEN compat: getrandom() from the system's random source (libnx randomGet). */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
void randomGet(void *buf, size_t len); /* libnx kernel/random.h */
static inline ssize_t getrandom(void *buf, size_t len, unsigned int flags) {
    (void)flags;
    randomGet(buf, len);
    return (ssize_t)len;
}
#ifdef __cplusplus
}
#endif
