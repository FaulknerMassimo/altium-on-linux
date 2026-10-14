/*
 * Test library for tests/test_xrandrfilter_wine.py: preloaded after the XRandR filter (or
 * alone), makes the output named by XRANDR_ZERO_OUTPUT look the way XWayland lists a
 * monitor Hyprland could not bring up: connected, its CRTC with a mode of 0x0. Like
 * winex11.so's view of XRandR, it works through dlsym() on libXrandr's handle.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

typedef unsigned long XID;
typedef struct {
    unsigned long timestamp;
    XID crtc;
    char *name;
    int nameLen;
} XRROutputInfo; /* leading fields of Xrandr.h's */
typedef struct {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    XID mode;
} XRRCrtcInfo; /* leading fields of Xrandr.h's */

typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn next_dlsym;
static XRROutputInfo *(*real_output)(void *, void *, XID);
static XRRCrtcInfo *(*real_crtc)(void *, void *, XID);
static XID zero_crtc;

static XRROutputInfo *output_info(void *dpy, void *res, XID output)
{
    XRROutputInfo *info = real_output(dpy, res, output);
    const char *name = getenv("XRANDR_ZERO_OUTPUT");
    if (info && info->crtc && name && info->nameLen == (int)strlen(name) && !memcmp(info->name, name, info->nameLen))
        zero_crtc = info->crtc;
    return info;
}

static XRRCrtcInfo *crtc_info(void *dpy, void *res, XID crtc)
{
    XRRCrtcInfo *info = real_crtc(dpy, res, crtc);
    if (info && crtc == zero_crtc) info->width = info->height = 0;
    return info;
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
        real_output = (XRROutputInfo *(*)(void *, void *, XID))next_dlsym(handle, name);
        return real_output ? (void *)output_info : NULL;
    }
    if (handle != RTLD_NEXT && handle != RTLD_DEFAULT && !strcmp(name, "XRRGetCrtcInfo")) {
        real_crtc = (XRRCrtcInfo *(*)(void *, void *, XID))next_dlsym(handle, name);
        return real_crtc ? (void *)crtc_info : NULL;
    }
    __attribute__((musttail)) return next_dlsym(handle, name);
}
