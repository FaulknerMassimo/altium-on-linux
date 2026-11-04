/* Host test driver for src/xrandrfilter/xrandrfilter.c (run by tests/test_xrandrfilter.py
 * with the filter preloaded): looks up XRRGetOutputInfo in a stand-in libXrandr the way
 * winex11.so does (dlopen, dlsym on the handle) and prints what each output looks like.
 * Then, linked to a stand-in libX11 as winex11.so is to libX11, it sets an error handler
 * the way winex11.so does and prints which errors reach it. */
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
typedef struct {
    int type;
    void *display;
    XID resourceid;
    unsigned long serial;
    unsigned char error_code, request_code, minor_code;
} XErrorEvent;
typedef int (*error_handler_fn)(void *dpy, XErrorEvent *event);

/* the stand-in libX11 */
error_handler_fn XSetErrorHandler(error_handler_fn handler);
int fake_default_handler(void *dpy, XErrorEvent *event);
void fake_x_error(unsigned char error_code, unsigned char request_code, unsigned char minor_code);

static int wine_handler(void *dpy, XErrorEvent *event)
{
    (void)dpy;
    printf("wine handler: error %u request %u.%u\n", event->error_code, event->request_code, event->minor_code);
    return 0;
}

static int other_handler(void *dpy, XErrorEvent *event)
{
    (void)dpy;
    printf("other handler: error %u request %u.%u\n", event->error_code, event->request_code, event->minor_code);
    return 0;
}

/* RandR's errors (147: BadRROutput, 148: BadRRCrtc, 150: BadRRProvider, 151: past RandR's),
 * then BadWindow (3) from UnmapWindow (10), MapWindow (8) and ChangeProperty (18), and
 * BadMatch (8) from UnmapWindow. */
static void raise_errors(void)
{
    fake_x_error(147, 140, 9);
    fake_x_error(148, 140, 20);
    fake_x_error(150, 140, 33);
    fake_x_error(151, 140, 9);
    fake_x_error(3, 10, 0);
    fake_x_error(3, 8, 0);
    fake_x_error(3, 18, 0);
    fake_x_error(8, 10, 0);
}

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

    int (*query)(void *, int *, int *) = (int (*)(void *, int *, int *))dlsym(lib, "XRRQueryExtension");
    int event_base, error_base;
    printf("RandR queried %d\n", query(NULL, &event_base, &error_base));
    printf("previous handler is the default %d\n", XSetErrorHandler(wine_handler) == fake_default_handler);
    raise_errors();
    printf("previous handler is wine's %d\n", XSetErrorHandler(other_handler) == wine_handler);
    fake_x_error(3, 18, 0);
    printf("previous handler is the other %d\n", XSetErrorHandler(NULL) == other_handler);
    fake_x_error(147, 140, 9);
    /* Lookups that depend on the caller: RTLD_NEXT from here finds the next dlsym after
     * this program, the filter's own; RTLD_DEFAULT finds libc's strlen. */
    printf("RTLD_NEXT finds the preloaded dlsym %d, RTLD_DEFAULT strlen %d\n",
           dlsym(RTLD_NEXT, "dlsym") == (void *)dlsym, dlsym(RTLD_DEFAULT, "strlen") == (void *)strlen);
    return 0;
}
