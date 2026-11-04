/*
 * A stand-in libX11 for tests/test_zerosize.py: a few windows with a size, a parent and an
 * override-redirect flag, and the calls the zero-size filter wraps or uses. Each call that
 * reaches it is printed, so the test sees which ones the filter let through.
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long Window;
#define ROOT 0x100

struct win {
    Window id, parent;
    int width, height, override_redirect;
};

static struct win wins[] = {
    { 1, ROOT, 1, 1, 0 },  /* a Delphi TApplication window: managed, 1x1 */
    { 2, ROOT, 1, 1, 1 },  /* Wine's dummy parent: override-redirect, 1x1 */
    { 3, ROOT, 400, 300, 0 },
    { 4, 3, 1, 1, 0 },     /* a child window */
    { 5, ROOT, 1, 1, 0 },
    { 6, ROOT, 1, 1, 0 },
};

static struct win *find(Window id)
{
    for (unsigned i = 0; i < sizeof(wins) / sizeof(wins[0]); i++)
        if (wins[i].id == id) return &wins[i];
    return NULL;
}

typedef struct {
    int x, y, width, height, border_width, depth;
    void *visual;
    Window root;
    int class_, bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    unsigned long colormap;
    int map_installed, map_state;
    long all_event_masks, your_event_mask, do_not_propagate_mask;
    int override_redirect;
    void *screen;
} XWindowAttributes;

typedef struct {
    int x, y, width, height, border_width;
    Window sibling;
    int stack_mode;
} XWindowChanges;

int XMapWindow(void *dpy, Window w) { (void)dpy; printf("  X: map %lu\n", w); return 1; }
int XUnmapWindow(void *dpy, Window w) { (void)dpy; printf("  X: unmap %lu\n", w); return 1; }
int XWithdrawWindow(void *dpy, Window w, int screen) { (void)dpy, (void)screen; printf("  X: withdraw %lu\n", w); return 1; }
int XDestroyWindow(void *dpy, Window w) { (void)dpy; printf("  X: destroy %lu\n", w); return 1; }

int XConfigureWindow(void *dpy, Window w, unsigned mask, XWindowChanges *c)
{
    struct win *win = find(w);
    (void)dpy;
    if (win && (mask & (1 << 2))) win->width = c->width;
    if (win && (mask & (1 << 3))) win->height = c->height;
    printf("  X: configure %lu\n", w);
    return 1;
}

int XMoveResizeWindow(void *dpy, Window w, int x, int y, unsigned width, unsigned height)
{
    struct win *win = find(w);
    (void)dpy, (void)x, (void)y;
    if (win) win->width = width, win->height = height;
    printf("  X: move-resize %lu %ux%u\n", w, width, height);
    return 1;
}

int XResizeWindow(void *dpy, Window w, unsigned width, unsigned height)
{
    return XMoveResizeWindow(dpy, w, 0, 0, width, height);
}

int XGetWindowAttributes(void *dpy, Window w, XWindowAttributes *attr)
{
    struct win *win = find(w);
    (void)dpy;
    if (!win) return 0;
    attr->width = win->width;
    attr->height = win->height;
    attr->override_redirect = win->override_redirect;
    attr->root = ROOT;
    return 1;
}

int XQueryTree(void *dpy, Window w, Window *root, Window *parent, Window **children, unsigned *n)
{
    struct win *win = find(w);
    (void)dpy;
    if (!win) return 0;
    *root = ROOT;
    *parent = win->parent;
    *children = malloc(sizeof(Window)); /* the filter must XFree it */
    *n = 0;
    return 1;
}

int XGetGeometry(void *dpy, Window w, Window *root, int *x, int *y, unsigned *width, unsigned *height,
                 unsigned *border, unsigned *depth)
{
    struct win *win = find(w);
    (void)dpy;
    if (!win) return 0;
    *root = ROOT;
    *x = *y = 0;
    *width = win->width;
    *height = win->height;
    *border = 0;
    *depth = 24;
    return 1;
}

int XFree(void *p) { free(p); return 1; }
