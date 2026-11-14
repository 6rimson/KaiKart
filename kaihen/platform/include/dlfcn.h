/* kaiHEN compat: no dynamic libraries. Every load fails, so callers take their fallback. */
#pragma once
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_NOLOAD 0x4
static inline void *dlopen(const char *file, int mode) { (void)file, (void)mode; return 0; }
static inline void *dlsym(void *handle, const char *name) { (void)handle, (void)name; return 0; }
static inline int dlclose(void *handle) { (void)handle; return 0; }
static inline char *dlerror(void) { return (char *)"no dynamic libraries on kaiHEN"; }
