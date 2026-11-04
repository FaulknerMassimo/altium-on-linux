/*
 * onscreen: move every visible top-level window that lies on no monitor onto the nearest one.
 * When a monitor goes away or the primary changes, the origin of Wine's coordinates moves
 * but Wine keeps each window's position in them, so a window can end up off every monitor,
 * and winex11 unmaps a window that is entirely off-screen: Altium's main window disappears
 * (docs/how-it-works.md §24). Windows does this move itself. The moved windows are then redrawn:
 * their program never saw them hidden and would not repaint them. The SHFolder.dll proxy does the
 * same inside Altium within a quarter of a second (its item 10); this covers other programs and
 * monitor lists that only scripts/refresh-displays.sh repairs. Built by scripts/build.sh, run by
 * scripts/refresh-displays.sh after Wine's monitor list has been rebuilt.
 */

typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HMONITOR;
typedef long long LONG_PTR;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;

#define IMPORT __declspec(dllimport)
IMPORT BOOL EnumWindows(BOOL (*cb)(HWND, LONG_PTR), LONG_PTR param);
IMPORT int GetClassNameA(HWND hwnd, char *buf, int len);
IMPORT int GetWindowTextA(HWND hwnd, char *buf, int len);
IMPORT BOOL GetWindowRect(HWND hwnd, RECT *rect);
IMPORT BOOL IsWindowVisible(HWND hwnd);
IMPORT BOOL IsIconic(HWND hwnd);
IMPORT HMONITOR MonitorFromRect(const RECT *rect, DWORD flags);
IMPORT BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO *info);
IMPORT BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, unsigned flags);
IMPORT BOOL RedrawWindow(HWND hwnd, const RECT *rect, void *region, unsigned flags);
IMPORT BOOL SetProcessDPIAware(void);
IMPORT DWORD GetWindowThreadProcessId(HWND hwnd, DWORD *pid);
IMPORT int wsprintfA(char *buf, const char *fmt, ...);
IMPORT HANDLE GetStdHandle(DWORD id);
IMPORT BOOL WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT void Sleep(DWORD ms);

#define MONITOR_DEFAULTTONULL 0
#define MONITOR_DEFAULTTONEAREST 2
#define SWP_NOSIZE 0x0001
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_ALLCHILDREN 0x0080
#define RDW_FRAME 0x0400
#define MAX_MOVED 64
#define STD_OUTPUT_HANDLE ((DWORD)-11)

static HWND moved_windows[MAX_MOVED];
static int moved;

static void out(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

/* Keep the window's size; move it as little as needed to fit the monitor's work area. */
static int clamp(int pos, int size, int lo, int hi)
{
    if (pos > hi - size) pos = hi - size;
    if (pos < lo) pos = lo;
    return pos;
}

static BOOL top_cb(HWND hwnd, LONG_PTR param)
{
    char cls[128], title[128], line[512];
    MONITORINFO mi;
    RECT r;
    DWORD pid;
    int x, y;
    (void)param;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &r)) return 1;
    /* Empty windows (Delphi's TApplication) are never mapped; partly visible ones are left alone,
     * and so are minimized ones (Windows parks them at -32000). */
    if (r.right <= r.left || r.bottom <= r.top || MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return 1;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return 1;
    x = clamp(r.left, r.right - r.left, mi.rcWork.left, mi.rcWork.right);
    y = clamp(r.top, r.bottom - r.top, mi.rcWork.top, mi.rcWork.bottom);
    GetClassNameA(hwnd, cls, sizeof(cls));
    title[0] = 0;
    GetWindowTextA(hwnd, title, sizeof(title));
    wsprintfA(line, "moved %08x pid=%u [%d,%d %dx%d] to %d,%d cls=%s \"%s\"\r\n", (DWORD)(LONG_PTR)hwnd, pid,
              r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, cls, title);
    out(line);
    SetWindowPos(hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (moved < MAX_MOVED) moved_windows[moved++] = hwnd;
    return 1;
}

/* Hyprland maps and tiles a moved window some time after the move. */
static void redraw_moved(void)
{
    for (int i = 0; i < moved; i++)
        RedrawWindow(moved_windows[i], 0, 0, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void start_entry(void)
{
    char line[64];
    SetProcessDPIAware(); /* physical pixels, like Altium (system DPI aware) */
    EnumWindows(top_cb, 0);
    wsprintfA(line, "onscreen: moved %d windows\r\n", moved);
    out(line);
    if (moved) {
        Sleep(500);
        redraw_moved();
        Sleep(1500);
        redraw_moved();
    }
    ExitProcess(0);
}
