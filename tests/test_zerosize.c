/*
 * Driver for tests/test_zerosize.py: makes the calls winex11.so makes, against the
 * stand-in libX11 (tests/zerosize_fakex11.c), linked directly as winex11.so links libX11.
 */
#include <stdio.h>

typedef unsigned long Window;
typedef struct {
    int x, y, width, height, border_width;
    Window sibling;
    int stack_mode;
} XWindowChanges;

int XMapWindow(void *dpy, Window w);
int XUnmapWindow(void *dpy, Window w);
int XWithdrawWindow(void *dpy, Window w, int screen);
int XDestroyWindow(void *dpy, Window w);
int XConfigureWindow(void *dpy, Window w, unsigned mask, XWindowChanges *c);
int XMoveResizeWindow(void *dpy, Window w, int x, int y, unsigned width, unsigned height);

int main(void)
{
    static int display; /* only its address matters */
    void *dpy = &display;
    XWindowChanges c = { 0 };

    printf("map 1x1 managed\n");
    XMapWindow(dpy, 1);
    printf("map 1x1 override-redirect\n");
    XMapWindow(dpy, 2);
    printf("map 400x300\n");
    XMapWindow(dpy, 3);
    printf("map 1x1 child\n");
    XMapWindow(dpy, 4);
    printf("map again while 1x1\n");
    XMapWindow(dpy, 1);
    printf("resize to 640x480\n");
    XMoveResizeWindow(dpy, 1, 10, 10, 640, 480);
    printf("unmap after the map\n");
    XUnmapWindow(dpy, 1);

    printf("map 1x1 managed, withdraw, destroy\n");
    XMapWindow(dpy, 5);
    XWithdrawWindow(dpy, 5, 0);
    XDestroyWindow(dpy, 5);

    printf("map 1x1 managed, widen, then heighten\n");
    XMapWindow(dpy, 6);
    c.width = 50;
    XConfigureWindow(dpy, 6, 1 << 2, &c);
    c.height = 40;
    XConfigureWindow(dpy, 6, 1 << 3, &c);
    return 0;
}
