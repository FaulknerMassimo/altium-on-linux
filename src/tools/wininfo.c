/*
 * wininfo: list visible top-level windows (and the main window's children) as Wine
 * sees them: class, title, rect, style, exstyle, owner. Diagnostic for Altium under
 * Wine; no SDK or CRT needed (built by scripts/build.sh).
 */

typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef long long LONG_PTR;
typedef struct { int left, top, right, bottom; } RECT;

#define IMPORT __declspec(dllimport)
IMPORT BOOL EnumWindows(BOOL (*cb)(HWND, LONG_PTR), LONG_PTR param);
IMPORT BOOL EnumChildWindows(HWND parent, BOOL (*cb)(HWND, LONG_PTR), LONG_PTR param);
IMPORT int GetClassNameA(HWND hwnd, char *buf, int len);
IMPORT int GetWindowTextA(HWND hwnd, char *buf, int len);
IMPORT BOOL GetWindowRect(HWND hwnd, RECT *rect);
IMPORT BOOL IsWindowVisible(HWND hwnd);
IMPORT LONG_PTR GetWindowLongPtrA(HWND hwnd, int index);
IMPORT HWND GetWindow(HWND hwnd, unsigned cmd);
IMPORT HWND GetParent(HWND hwnd);
IMPORT DWORD GetWindowThreadProcessId(HWND hwnd, DWORD *pid);
IMPORT int wsprintfA(char *buf, const char *fmt, ...);
IMPORT HANDLE GetStdHandle(DWORD id);
IMPORT BOOL WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);
IMPORT DWORD GetCurrentProcessId(void);

#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GW_OWNER 4
#define STD_OUTPUT_HANDLE ((DWORD)-11)

static HWND main_window;

static void out(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

static void describe(HWND hwnd, const char *indent)
{
    char cls[128], title[128], line[512];
    RECT r;
    DWORD pid;
    GetClassNameA(hwnd, cls, sizeof(cls));
    title[0] = 0;
    GetWindowTextA(hwnd, title, sizeof(title));
    GetWindowRect(hwnd, &r);
    GetWindowThreadProcessId(hwnd, &pid);
    wsprintfA(line, "%s%08x pid=%u vis=%d [%d,%d %dx%d] style=%08x ex=%08x owner=%08x parent=%08x cls=%s \"%s\"\r\n",
              indent, (DWORD)(LONG_PTR)hwnd, pid, IsWindowVisible(hwnd), r.left, r.top, r.right - r.left, r.bottom - r.top,
              (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE), (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE),
              (DWORD)(LONG_PTR)GetWindow(hwnd, GW_OWNER), (DWORD)(LONG_PTR)GetParent(hwnd), cls, title);
    out(line);
}

static BOOL child_cb(HWND hwnd, LONG_PTR param)
{
    (void)param;
    if (IsWindowVisible(hwnd)) describe(hwnd, "    ");
    return 1;
}

static BOOL top_cb(HWND hwnd, LONG_PTR param)
{
    char title[128];
    DWORD pid;
    (void)param;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() || !IsWindowVisible(hwnd)) return 1;
    describe(hwnd, "");
    title[0] = 0;
    GetWindowTextA(hwnd, title, sizeof(title));
    for (int i = 0; title[i]; i++)
        if (title[i] == '-' && title[i + 2] == 'A' && title[i + 3] == 'l' && title[i + 4] == 't') main_window = hwnd;
    return 1;
}

void start_entry(void)
{
    EnumWindows(top_cb, 0);
    if (main_window) {
        out("--- visible children of the Altium main window:\r\n");
        EnumChildWindows(main_window, child_cb, 0);
    }
    ExitProcess(0);
}
