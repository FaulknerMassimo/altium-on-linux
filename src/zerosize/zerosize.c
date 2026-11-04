/*
 * LD_PRELOAD library for the Wine processes in the bottle: keeps managed top-level X11
 * windows smaller than 2x2 unmapped until they get a real size.
 *
 * Delphi programs, the Altium installer among them, create their TApplication window
 * visible but 0x0 at the centre of the screen; on Windows nothing shows. Wine maps it as a
 * 1x1 X11 window, and Hyprland configures every managed window smaller than 2x2 that asks
 * to be mapped to 800x800 (CXWM::handleMapRequest, "default to 800 x 800"). Wine takes the
 * new size from the ConfigureNotify, so the installer gets an empty 800x800 white box
 * that does nothing (docs/how-it-works.md §28).
 *
 * winex11.so links XMapWindow and friends from libX11 directly, so plain interposition
 * reaches them:
 *   XMapWindow     a managed (not override-redirect) child of the root smaller than 2x2 is
 *                  not mapped, only remembered. Override-redirect windows are left alone:
 *                  Hyprland does not resize them, and Wine maps its own 1x1 dummy parent
 *                  window, which must stay mapped.
 *   XConfigureWindow, XMoveResizeWindow, XResizeWindow
 *                  once a remembered window is 2x2 or larger, it is mapped.
 *   XUnmapWindow, XWithdrawWindow, XDestroyWindow
 *                  a remembered window is forgotten (and, as it was never mapped, not
 *                  unmapped or withdrawn).
 * Wine waits for the window manager's WM_STATE after a map, so until a remembered window
 * is mapped Wine ignores window-manager changes to it and holds back its own state changes
 * (window_set_wm_state()); none of that matters for a window nobody can see. Requests
 * Wine makes on the unmapped window that need it viewable (XSetInputFocus) fail with
 * BadMatch, which Wine's error handler ignores.
 *
 * The installer is 32-bit, and this runner runs 32-bit programs in 32-bit Linux processes,
 * so the library is built twice (x86_64 and i686); LD_PRELOAD names it through $LIB, which
 * the dynamic loader expands to the process's own library folder. It includes no libc header and
 * links no libc, so the 32-bit build needs no 32-bit development files: the few libc
 * functions it calls are declared below and bound when the library loads.
 *
 * Environment:
 *   ALTIUM_ZERO_SIZE=off      do nothing
 *   ALTIUM_ZERO_SIZE_LOG=FILE log file (default $WINEPREFIX/altium-on-linux/logs/zero-size.log);
 *                             every line also goes to stderr
 *
 * Build: scripts/build.sh. Loaded inside the Bottles Flatpak, so it uses only old glibc
 * symbols, like the fd guard.
 */

/* The libc functions used (glibc ABI). */
#define RTLD_NEXT ((void *)-1l)
#define O_WRONLY 01
#define O_CREAT 0100
#define O_APPEND 02000
#define O_CLOEXEC 02000000
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
void *dlsym(void *handle, const char *name);
char *getenv(const char *name);
long time(long *t);
struct tm *localtime_r(const long *t, struct tm *tm);
int getpid(void);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
long write(int fd, const void *buf, unsigned long n);
int close(int fd);

#define EXPORT __attribute__((visibility("default")))
#define MAX_REPORTS 200
#define MAX_WINDOWS 256

/* Xlib.h's types (public ABI), without pulling in the X11 headers. */
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef int Status;

typedef struct {
    int x, y;
    int width, height;
    int border_width;
    int depth;
    void *visual;
    Window root;
    int class_;
    int bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    unsigned long colormap;
    int map_installed, map_state;
    long all_event_masks, your_event_mask, do_not_propagate_mask;
    int override_redirect;
    void *screen;
} XWindowAttributes;

typedef struct {
    int x, y;
    int width, height;
    int border_width;
    Window sibling;
    int stack_mode;
} XWindowChanges;

#define CWWidth (1 << 2)
#define CWHeight (1 << 3)

typedef int (*map_fn)(Display *, Window);
typedef int (*withdraw_fn)(Display *, Window, int);
typedef int (*configure_fn)(Display *, Window, unsigned, XWindowChanges *);
typedef int (*move_resize_fn)(Display *, Window, int, int, unsigned, unsigned);
typedef int (*resize_fn)(Display *, Window, unsigned, unsigned);
typedef Status (*get_attributes_fn)(Display *, Window, XWindowAttributes *);
typedef Status (*query_tree_fn)(Display *, Window, Window *, Window *, Window **, unsigned *);
typedef Status (*get_geometry_fn)(Display *, Window, Window *, int *, int *, unsigned *, unsigned *, unsigned *,
                                  unsigned *);
typedef int (*free_fn)(void *);

static int off = -1; /* -1: not read yet */
static unsigned reports;

static struct {
    Display *dpy;
    Window win;
} held[MAX_WINDOWS];
static int nheld; /* entries in use are packed at the start */
static int lock;

/* Look the real function up once (from libX11, next after this library). */
#define REAL(type, name)                                                          \
    static type real;                                                             \
    type f = __atomic_load_n(&real, __ATOMIC_ACQUIRE);                            \
    if (!f) {                                                                     \
        f = (type)dlsym(RTLD_NEXT, name);                                         \
        __atomic_store_n(&real, f, __ATOMIC_RELEASE);                             \
    }

static int equals(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* ALTIUM_ZERO_SIZE=off? Read on first use: the library links no libc, so a constructor
 * could run before libc has set up the environment. */
static int is_off(void)
{
    int v = __atomic_load_n(&off, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *m = getenv("ALTIUM_ZERO_SIZE");
        v = m && (equals(m, "off") || equals(m, "0"));
        __atomic_store_n(&off, v, __ATOMIC_RELAXED);
    }
    return v;
}

/* A tiny formatter for the log line: text, decimals, hex. */
struct buf {
    char *p, *end;
};

static void put(struct buf *b, const char *s)
{
    while (*s && b->p < b->end) *b->p++ = *s++;
}

static void put_num(struct buf *b, unsigned long v, unsigned base, int width)
{
    char tmp[24];
    int n = 0;
    do tmp[n++] = "0123456789abcdef"[v % base], v /= base;
    while (v);
    while (n < width) tmp[n++] = '0';
    while (n && b->p < b->end) *b->p++ = tmp[--n];
}

static void put_int(struct buf *b, int v)
{
    if (v < 0) put(b, "-");
    put_num(b, v < 0 ? -(unsigned long)v : (unsigned long)v, 10, 0);
}

static void report(const char *what, Window win, int width, int height)
{
    if (__atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) >= MAX_REPORTS) return;
    char line[300], proc[17] = "", path[512];
    struct buf b = { line, line + sizeof(line) - 1 };
    struct tm tm;
    long now = time(0);
    int fd = open("/proc/self/comm", O_CLOEXEC);
    if (fd >= 0) {
        long n = read(fd, proc, sizeof(proc) - 1);
        proc[n > 0 ? n - 1 : 0] = 0;
        close(fd);
    }
    put(&b, "zero-size ");
    if (localtime_r(&now, &tm)) {
        put_num(&b, tm.tm_hour, 10, 2), put(&b, ":"), put_num(&b, tm.tm_min, 10, 2), put(&b, ":");
        put_num(&b, tm.tm_sec, 10, 2), put(&b, ": ");
    }
    put(&b, what), put(&b, " window 0x"), put_num(&b, win, 16, 0), put(&b, " (");
    put_int(&b, width), put(&b, "x"), put_int(&b, height), put(&b, ") in "), put(&b, proc);
    put(&b, " (pid "), put_int(&b, getpid()), put(&b, ")\n");
    write(2, line, b.p - line);

    const char *log = getenv("ALTIUM_ZERO_SIZE_LOG"), *prefix = getenv("WINEPREFIX");
    struct buf pb = { path, path + sizeof(path) - 1 };
    if (log) put(&pb, log);
    else if (prefix) put(&pb, prefix), put(&pb, "/altium-on-linux/logs/zero-size.log");
    else return;
    *pb.p = 0;
    if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)) >= 0) {
        write(fd, line, b.p - line);
        close(fd);
    }
}

static void lock_held(void)
{
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}

static void unlock_held(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

/* Remember a window; 0 if the table is full (the window is then mapped as usual). */
static int hold(Display *dpy, Window win)
{
    int ok = 0;
    lock_held();
    if (nheld < MAX_WINDOWS) {
        held[nheld].dpy = dpy;
        held[nheld].win = win;
        nheld++;
        ok = 1;
    }
    unlock_held();
    return ok;
}

/* Forget a window; 1 if it was remembered. */
static int release(Display *dpy, Window win)
{
    int found = 0;
    if (!__atomic_load_n(&nheld, __ATOMIC_RELAXED)) return 0; /* the common case: nothing held */
    lock_held();
    for (int i = 0; i < nheld; i++) {
        if (held[i].dpy == dpy && held[i].win == win) {
            held[i] = held[--nheld];
            found = 1;
            break;
        }
    }
    unlock_held();
    return found;
}

static int is_held(Display *dpy, Window win)
{
    int found = 0;
    if (!__atomic_load_n(&nheld, __ATOMIC_RELAXED)) return 0;
    lock_held();
    for (int i = 0; i < nheld && !found; i++) found = held[i].dpy == dpy && held[i].win == win;
    unlock_held();
    return found;
}

static int real_map(Display *dpy, Window win)
{
    REAL(map_fn, "XMapWindow");
    return f ? f(dpy, win) : 0;
}

/* A managed child of the root window smaller than 2x2? */
static int is_tiny_toplevel(Display *dpy, Window win, int *width, int *height)
{
    REAL(get_attributes_fn, "XGetWindowAttributes");
    XWindowAttributes attr;
    if (!f || !f(dpy, win, &attr)) return 0;
    *width = attr.width;
    *height = attr.height;
    if (attr.override_redirect || (attr.width >= 2 && attr.height >= 2)) return 0;

    static query_tree_fn query_tree;
    static free_fn xfree;
    if (!query_tree) query_tree = (query_tree_fn)dlsym(RTLD_NEXT, "XQueryTree");
    if (!xfree) xfree = (free_fn)dlsym(RTLD_NEXT, "XFree");
    Window root, parent, *children = 0;
    unsigned n;
    if (!query_tree || !query_tree(dpy, win, &root, &parent, &children, &n)) return 0;
    if (children && xfree) xfree(children);
    return parent == root;
}

/* After a size change of a remembered window: map it once it is 2x2 or larger. */
static void resized(Display *dpy, Window win, int width, int height)
{
    if (width < 0 || height < 0) {
        REAL(get_geometry_fn, "XGetGeometry");
        Window root;
        int x, y;
        unsigned w, h, border, depth;
        if (!f || !f(dpy, win, &root, &x, &y, &w, &h, &border, &depth)) return;
        width = w;
        height = h;
    }
    if (width < 2 || height < 2 || !release(dpy, win)) return;
    report("mapped, now", win, width, height);
    real_map(dpy, win);
}

EXPORT int XMapWindow(Display *dpy, Window win)
{
    int width, height;
    if (is_held(dpy, win)) return 1; /* still too small */
    if (!is_off() && is_tiny_toplevel(dpy, win, &width, &height) && hold(dpy, win)) {
        report("kept unmapped", win, width, height);
        return 1;
    }
    return real_map(dpy, win);
}

EXPORT int XUnmapWindow(Display *dpy, Window win)
{
    REAL(map_fn, "XUnmapWindow");
    if (release(dpy, win)) return 1;
    return f ? f(dpy, win) : 0;
}

EXPORT Status XWithdrawWindow(Display *dpy, Window win, int screen)
{
    REAL(withdraw_fn, "XWithdrawWindow");
    if (release(dpy, win)) return 1;
    return f ? f(dpy, win, screen) : 0;
}

EXPORT int XDestroyWindow(Display *dpy, Window win)
{
    REAL(map_fn, "XDestroyWindow");
    release(dpy, win);
    return f ? f(dpy, win) : 0;
}

EXPORT int XConfigureWindow(Display *dpy, Window win, unsigned mask, XWindowChanges *changes)
{
    REAL(configure_fn, "XConfigureWindow");
    int ret = f ? f(dpy, win, mask, changes) : 0;
    if ((mask & (CWWidth | CWHeight)) && changes && is_held(dpy, win)) {
        if ((mask & (CWWidth | CWHeight)) == (CWWidth | CWHeight)) resized(dpy, win, changes->width, changes->height);
        else resized(dpy, win, -1, -1);
    }
    return ret;
}

EXPORT int XMoveResizeWindow(Display *dpy, Window win, int x, int y, unsigned width, unsigned height)
{
    REAL(move_resize_fn, "XMoveResizeWindow");
    int ret = f ? f(dpy, win, x, y, width, height) : 0;
    if (is_held(dpy, win)) resized(dpy, win, width, height);
    return ret;
}

EXPORT int XResizeWindow(Display *dpy, Window win, unsigned width, unsigned height)
{
    REAL(resize_fn, "XResizeWindow");
    int ret = f ? f(dpy, win, width, height) : 0;
    if (is_held(dpy, win)) resized(dpy, win, width, height);
    return ret;
}
