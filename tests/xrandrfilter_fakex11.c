/* Stand-in for libX11 in tests/test_xrandrfilter.py: XSetErrorHandler() as Xlib has it (the
 * previous handler back, NULL for the default), and fake_x_error() to deliver an error to
 * the handler that is set. The default handler prints instead of exiting. */
#include <stdio.h>

typedef unsigned long XID;
typedef struct {
    int type;
    void *display;
    XID resourceid;
    unsigned long serial;
    unsigned char error_code, request_code, minor_code;
} XErrorEvent;
typedef int (*error_handler_fn)(void *dpy, XErrorEvent *event);

int fake_default_handler(void *dpy, XErrorEvent *event)
{
    (void)dpy;
    printf("default handler: error %u request %u.%u\n", event->error_code, event->request_code, event->minor_code);
    return 0;
}

static error_handler_fn handler = fake_default_handler;

error_handler_fn XSetErrorHandler(error_handler_fn new_handler)
{
    error_handler_fn prev = handler;
    handler = new_handler ? new_handler : fake_default_handler;
    return prev;
}

void fake_x_error(unsigned char error_code, unsigned char request_code, unsigned char minor_code)
{
    XErrorEvent event = { 0, NULL, 0x123, 1, error_code, request_code, minor_code };
    handler(NULL, &event);
}
