/* kaiHEN compat: newlib has no mmap. The runtime only maps anonymous, zero-filled memory (guest
 * RAM, coroutine stacks) and unmaps whole mappings, so the heap serves. Protection changes are
 * accepted and do nothing: a game can't change its page permissions this way on kaiHEN, and the
 * stack guard page is only a safety net. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

static inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr, (void)prot, (void)offset;
    if (!(flags & MAP_ANONYMOUS) || fd != -1 || (flags & MAP_FIXED) || length == 0)
        return MAP_FAILED;
    const size_t rounded = (length + 0xfff) & ~(size_t)0xfff;
    void *p = aligned_alloc(0x1000, rounded);
    if (!p)
        return MAP_FAILED;
    memset(p, 0, rounded);
    return p;
}

static inline int munmap(void *addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}

static inline int mprotect(void *addr, size_t length, int prot) {
    (void)addr, (void)length, (void)prot;
    return 0;
}
