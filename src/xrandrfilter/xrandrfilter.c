/*
 * LD_PRELOAD library for the Wine processes in the bottle: hides X11 outputs whose CRTC
 * has a 0x0 mode from Wine's monitor list, and keeps X errors about outputs and windows
 * that have just gone away from ending the process.
 *
 * When a monitor fails to come up (a dock re-authorizing its DisplayPort MST outputs: the
 * kernel rejects every mode), Hyprland keeps it "connected" at 0x0 and XWayland lists it
 * as a connected output whose CRTC has a 0x0 mode. Wine takes that for an active monitor,
 * and Proton's win32u (monitor_get_dpi() in sysparams.c) divides by its width: an integer
 * divide by zero in every process that asks about monitors. explorer.exe, which owns the
 * desktop, then faults over and over; Altium loses its windows, or never shows one
 * (docs/how-it-works.md §21).
 *
 * winex11.so gets every XRandR function with dlsym() from libXrandr's handle, which
 * LD_PRELOAD does not reach, so this library wraps dlsym(): for XRRGetOutputInfo it
 * returns a wrapper that reports such an output as disconnected. Every other lookup goes
 * to the real dlsym() as a tail call, so its RTLD_NEXT and caller-dependent lookups see
 * the original caller.
 *
 * An X error that Wine does not expect ends the process: winex11's error handler passes it
 * on to Xlib's default one, which exits. Unplugging the dock during a suspend made both
 * explorer.exe and Altium exit that way (docs/how-it-works.md §25): explorer asked about an
 * output that XWayland had just removed (BadRROutput from RRGetOutputInfo), and Altium then
 * unmapped the cursor clipping window, which explorer owns and which had died with it
 * (BadWindow from UnmapWindow). This library wraps XSetErrorHandler() so that the handler
 * Wine sets runs behind a filter, which drops RandR's errors for an output, CRTC, mode or
 * provider that no longer exists (the query returns NULL, which winex11 handles: it lists
 * the monitors again on the next RandR event) and BadWindow from mapping, unmapping,
 * destroying or reparenting a window that no longer exists. Each dropped error is logged.
 * RandR's error codes are learnt from winex11's XRRQueryExtension() call.
 *
 * Environment:
 *   ALTIUM_XRANDR_FILTER=off      do nothing
 *   ALTIUM_XRANDR_FILTER_LOG=FILE log file (default
 *                                 $WINEPREFIX/altium-on-linux/logs/xrandr-filter.log);
 *                                 every line also goes to stderr
 *
 * Build: scripts/build.sh. Loaded inside the Bottles Flatpak, so it uses only old glibc
 * symbols, like the fd guard.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define MAX_REPORTS 100

/* Xrandr.h's structures (public ABI), without pulling in the X11 headers. */
typedef unsigned long XID;
#define RR_Connected 0
#define RR_Disconnected 1

typedef struct {
    unsigned long timestamp;
    XID crtc;
    char *name;
    int nameLen;
    unsigned long mm_width, mm_height;
    unsigned short connection, subpixel_order;
    int ncrtc;
    XID *crtcs;
    int nclone;
    XID *clones;
    int nmode, npreferred;
    XID *modes;
} XRROutputInfo;

typedef struct {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    XID mode;
    unsigned short rotation;
    int noutput;
    XID *outputs;
    unsigned short rotations;
    int npossible;
    XID *possible;
} XRRCrtcInfo;

/* Xlib.h's XErrorEvent (public ABI). */
typedef struct {
    int type;
    void *display;
    XID resourceid;
    unsigned long serial;
    unsigned char error_code, request_code, minor_code;
} XErrorEvent;

#define BadWindow 3
#define X_DestroyWindow 4
#define X_ReparentWindow 7
#define X_MapWindow 8
#define X_UnmapWindow 10
#define RR_ERRORS 4 /* BadRROutput, BadRRCrtc, BadRRMode, BadRRProvider from RandR's error base */

typedef void *(*dlsym_fn)(void *, const char *);
typedef XRROutputInfo *(*get_output_info_fn)(void *dpy, void *resources, XID output);
typedef XRRCrtcInfo *(*get_crtc_info_fn)(void *dpy, void *resources, XID crtc);
typedef void (*free_crtc_info_fn)(XRRCrtcInfo *);
typedef int (*query_extension_fn)(void *dpy, int *event_base, int *error_base);
typedef int (*error_handler_fn)(void *dpy, XErrorEvent *event);
typedef error_handler_fn (*set_error_handler_fn)(error_handler_fn handler);

static dlsym_fn real_dlsym;
static get_output_info_fn real_get_output_info;
static get_crtc_info_fn real_get_crtc_info;
static free_crtc_info_fn real_free_crtc_info;
static query_extension_fn real_query_extension;
static error_handler_fn next_error_handler; /* the handler the process set */
static int rr_error_base;                   /* 0 until RandR has been queried */
static int off;
static unsigned reports;

__attribute__((constructor)) static void init(void)
{
    const char *m = getenv("ALTIUM_XRANDR_FILTER");
    off = m && (!strcmp(m, "off") || !strcmp(m, "0"));
}

/* One line, after "xrandr-filter HH:MM:SS: ", to stderr and the log file; at most MAX_REPORTS
 * per process. Only system calls besides formatting: it runs inside an X error handler too. */
static void report(const char *fmt, ...)
{
    if (__atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) >= MAX_REPORTS) return;
    char path[512] = "", line[400];
    const char *log = getenv("ALTIUM_XRANDR_FILTER_LOG"), *prefix = getenv("WINEPREFIX");
    if (log) snprintf(path, sizeof(path), "%s", log);
    else if (prefix) snprintf(path, sizeof(path), "%s/altium-on-linux/logs/xrandr-filter.log", prefix);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int n = snprintf(line, sizeof(line), "xrandr-filter %02d:%02d:%02d: ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    syscall(SYS_write, 2, line, n);
    int fd;
    if (path[0] && (fd = syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)) >= 0) {
        syscall(SYS_write, fd, line, n);
        syscall(SYS_close, fd);
    }
}

static const char *process_name(char name[17])
{
    name[0] = 0;
    int fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        long n = syscall(SYS_read, fd, name, 16);
        name[n > 0 ? n - 1 : 0] = 0;
        syscall(SYS_close, fd);
    }
    return name;
}

/* XRRGetOutputInfo, with a connected output whose CRTC has a 0x0 mode reported as
 * disconnected. An output without a CRTC (switched off) stays as it is: Wine handles it. */
static XRROutputInfo *get_output_info(void *dpy, void *resources, XID output)
{
    XRROutputInfo *info = real_get_output_info(dpy, resources, output);
    if (!info || info->connection != RR_Connected || !info->crtc || !real_get_crtc_info) return info;
    XRRCrtcInfo *crtc = real_get_crtc_info(dpy, resources, info->crtc);
    if (crtc && crtc->mode && (!crtc->width || !crtc->height)) {
        char proc[17];
        report("hid output %.*s from %s (pid %d): its CRTC is %ux%u at %d,%d\n",
               info->nameLen > 0 && info->nameLen < 64 ? info->nameLen : 1, info->name ? info->name : "?",
               process_name(proc), getpid(), crtc->width, crtc->height, crtc->x, crtc->y);
        info->connection = RR_Disconnected;
        info->crtc = 0;
    }
    if (crtc && real_free_crtc_info) real_free_crtc_info(crtc);
    return info;
}

static int query_extension(void *dpy, int *event_base, int *error_base)
{
    int ok = real_query_extension(dpy, event_base, error_base);
    if (ok && error_base) __atomic_store_n(&rr_error_base, *error_base, __ATOMIC_RELAXED);
    return ok;
}

/* What an error that is dropped is about, or NULL to hand it on. */
static const char *gone(const XErrorEvent *event)
{
    int base = __atomic_load_n(&rr_error_base, __ATOMIC_RELAXED);
    if (base && event->error_code >= base && event->error_code < base + RR_ERRORS) return "a RandR resource";
    if (event->error_code == BadWindow &&
        (event->request_code == X_DestroyWindow || event->request_code == X_ReparentWindow ||
         event->request_code == X_MapWindow || event->request_code == X_UnmapWindow))
        return "a window";
    return NULL;
}

static int error_filter(void *dpy, XErrorEvent *event)
{
    const char *what = gone(event);
    if (what) {
        char proc[17];
        report("dropped X error %u (request %u.%u, resource 0x%lx) in %s (pid %d): %s that no longer exists\n",
               event->error_code, event->request_code, event->minor_code, event->resourceid, process_name(proc),
               getpid(), what);
        return 0;
    }
    error_handler_fn next = __atomic_load_n(&next_error_handler, __ATOMIC_ACQUIRE);
    return next ? next(dpy, event) : 0;
}

/* The handler the caller sets runs behind error_filter; the caller gets back the handler it
 * would have without this library. NULL (Xlib's default) is set as it is. */
EXPORT error_handler_fn XSetErrorHandler(error_handler_fn handler)
{
    static set_error_handler_fn real;
    if (!real) real = (set_error_handler_fn)dlsym(RTLD_NEXT, "XSetErrorHandler");
    if (!real) return NULL;
    if (off) return real(handler);
    error_handler_fn prev_next = __atomic_exchange_n(&next_error_handler, handler, __ATOMIC_ACQ_REL);
    error_handler_fn prev = real(handler ? error_filter : NULL);
    return prev == error_filter ? prev_next : prev;
}

static dlsym_fn get_real_dlsym(void)
{
    dlsym_fn f = __atomic_load_n(&real_dlsym, __ATOMIC_ACQUIRE);
    if (f) return f;
    /* libc's dlsym (dlvsym skips unversioned definitions, such as this one), then through
     * it the next dlsym after this library: another preloaded one, or libc's again. */
    dlsym_fn libc = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"); /* glibc >= 2.34: dlsym is in libc */
    if (!libc) libc = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    f = libc ? (dlsym_fn)libc(RTLD_NEXT, "dlsym") : NULL;
    if (!f) f = libc;
    __atomic_store_n(&real_dlsym, f, __ATOMIC_RELEASE);
    return f;
}

EXPORT void *dlsym(void *handle, const char *name)
{
    dlsym_fn next = get_real_dlsym();
    if (!off && handle != RTLD_NEXT && handle != RTLD_DEFAULT && !strcmp(name, "XRRGetOutputInfo")) {
        void *f = next(handle, name);
        if (!f) return f;
        __atomic_store_n(&real_get_output_info, (get_output_info_fn)f, __ATOMIC_RELEASE);
        __atomic_store_n(&real_get_crtc_info, (get_crtc_info_fn)next(handle, "XRRGetCrtcInfo"), __ATOMIC_RELEASE);
        __atomic_store_n(&real_free_crtc_info, (free_crtc_info_fn)next(handle, "XRRFreeCrtcInfo"), __ATOMIC_RELEASE);
        return (void *)get_output_info;
    }
    if (!off && handle != RTLD_NEXT && handle != RTLD_DEFAULT && !strcmp(name, "XRRQueryExtension")) {
        void *f = next(handle, name);
        if (!f) return f;
        __atomic_store_n(&real_query_extension, (query_extension_fn)f, __ATOMIC_RELEASE);
        return (void *)query_extension;
    }
    /* A jump, not a call: glibc's dlsym() goes by its return address (RTLD_NEXT). */
    __attribute__((musttail)) return next(handle, name);
}
