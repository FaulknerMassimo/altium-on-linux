/*
 * Test library for tests/test_xrandrfilter_wine.py: preloaded after the XRandR filter (or
 * alone), makes the first XRRGetOutputInfo() of each process ask XWayland about an output
 * that does not exist, as when a dock's output goes away between Wine listing the screen's
 * outputs and asking about one: XWayland answers with a real BadRROutput error. Like
 * winex11.so's view of XRandR, it works through dlsym() on libXrandr's handle.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define NO_SUCH_OUTPUT 0x1ffffff0

typedef unsigned long XID;
typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn next_dlsym;
static void *(*real_output)(void *, void *, XID);
static int calls;

static void *output_info(void *dpy, void *res, XID output)
{
    if (!__atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED)) output = NO_SUCH_OUTPUT;
    return real_output(dpy, res, output);
}

EXPORT void *dlsym(void *handle, const char *name)
{
    if (!next_dlsym) { /* as in the filter */
        dlsym_fn libc = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!libc) libc = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        next_dlsym = libc ? (dlsym_fn)libc(RTLD_NEXT, "dlsym") : NULL;
        if (!next_dlsym) next_dlsym = libc;
    }
    if (handle != RTLD_NEXT && handle != RTLD_DEFAULT && !strcmp(name, "XRRGetOutputInfo")) {
        real_output = (void *(*)(void *, void *, XID))next_dlsym(handle, name);
        return real_output ? (void *)output_info : NULL;
    }
    __attribute__((musttail)) return next_dlsym(handle, name);
}
