/* Host test driver for src/xrandrfilter/xrandrfilter.c (run by tests/test_xrandrfilter.py
 * with the filter preloaded): looks up XRRGetOutputInfo in a stand-in libXrandr the way
 * winex11.so does (dlopen, dlsym on the handle) and prints what each output looks like. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long XID;
typedef struct {
    unsigned long timestamp;
    XID crtc;
    char *name;
    int nameLen;
    unsigned long mm_width, mm_height;
    unsigned short connection, subpixel_order;
} XRROutputInfo;

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    XRROutputInfo *(*get)(void *, void *, XID) = (XRROutputInfo *(*)(void *, void *, XID))dlsym(lib, "XRRGetOutputInfo");
    void (*free_info)(XRROutputInfo *) = (void (*)(XRROutputInfo *))dlsym(lib, "XRRFreeOutputInfo");
    int *crtc_frees = (int *)dlsym(lib, "crtc_frees");
    for (XID output = 1; output <= 4; output++) {
        XRROutputInfo *info = get(NULL, NULL, output);
        printf("%s %s crtc %lu\n", info->name, info->connection ? "disconnected" : "connected", info->crtc);
        free_info(info);
    }
    printf("crtc infos freed %d\n", *crtc_frees);
    /* Lookups that depend on the caller: RTLD_NEXT from here finds the next dlsym after
     * this program, the filter's own; RTLD_DEFAULT finds libc's strlen. */
    printf("RTLD_NEXT finds the preloaded dlsym %d, RTLD_DEFAULT strlen %d\n",
           dlsym(RTLD_NEXT, "dlsym") == (void *)dlsym, dlsym(RTLD_DEFAULT, "strlen") == (void *)strlen);
    return 0;
}
