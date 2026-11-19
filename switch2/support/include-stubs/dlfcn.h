/* Switch 2 stub <dlfcn.h>: Horizon homebrew has no dynamic loader. Only for third-party code that
 * includes it on every non-Windows platform but whose dlopen path is never taken on Switch 2
 * (volk.c: plume calls volkInitializeCustom there). Every call fails. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void*)0)
static inline void* dlopen(const char* file, int mode) { (void)file; (void)mode; return 0; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return 0; }
static inline int dlclose(void* handle) { (void)handle; return -1; }
static inline char* dlerror(void) { return (char*)"no dynamic loading on Switch 2"; }
#ifdef __cplusplus
}
#endif
