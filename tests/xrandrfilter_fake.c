/* Stand-in for libXrandr in tests/test_xrandrfilter.py: four outputs on one screen.
 * 1: connected, CRTC 11 at 1920x1080; 2: connected, CRTC 12 with a 0x0 mode (a monitor
 * Hyprland could not bring up, as XWayland lists it); 3: connected, no CRTC (switched
 * off); 4: disconnected. */
#include <stdlib.h>
#include <string.h>

typedef unsigned long XID;
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

int crtc_frees;

XRROutputInfo *XRRGetOutputInfo(void *dpy, void *resources, XID output)
{
    static const char *names[] = { "", "DP-1", "DP-4", "DP-5", "DP-6" };
    (void)dpy;
    (void)resources;
    if (output < 1 || output > 4) return NULL;
    XRROutputInfo *info = calloc(1, sizeof(*info));
    info->name = strdup(names[output]);
    info->nameLen = strlen(info->name);
    info->connection = output == 4;
    info->crtc = output <= 2 ? 10 + output : 0;
    return info;
}

XRRCrtcInfo *XRRGetCrtcInfo(void *dpy, void *resources, XID crtc)
{
    (void)dpy;
    (void)resources;
    XRRCrtcInfo *info = calloc(1, sizeof(*info));
    info->mode = 100 + crtc;
    if (crtc == 11) { info->width = 1920; info->height = 1080; }
    return info;
}

void XRRFreeCrtcInfo(XRRCrtcInfo *info) { crtc_frees++; free(info); }
void XRRFreeOutputInfo(XRROutputInfo *info) { free(info->name); free(info); }
