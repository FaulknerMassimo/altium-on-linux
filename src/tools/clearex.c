/* clearex <hex hwnd> <hex bits>: clear extended-style bits on a window (diagnostic; built by scripts/build.sh). */
typedef void *HWND;
typedef long long LONG_PTR;
#define IMPORT __declspec(dllimport)
IMPORT LONG_PTR GetWindowLongPtrA(HWND hwnd, int index);
IMPORT LONG_PTR SetWindowLongPtrA(HWND hwnd, int index, LONG_PTR value);
IMPORT int SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, unsigned flags);
IMPORT int RedrawWindow(HWND hwnd, const void *rect, void *rgn, unsigned flags);
IMPORT __WCHAR_TYPE__ *GetCommandLineW(void);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);

static unsigned long long hex(__WCHAR_TYPE__ **p)
{
    unsigned long long v = 0;
    while (**p == ' ') (*p)++;
    for (;; (*p)++) {
        __WCHAR_TYPE__ c = **p;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

void start_entry(void)
{
    __WCHAR_TYPE__ *p = GetCommandLineW();
    HWND hwnd;
    unsigned long long bits;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; } else while (*p && *p != ' ') p++;
    hwnd = (HWND)hex(&p);
    bits = hex(&p);
    SetWindowLongPtrA(hwnd, -20, GetWindowLongPtrA(hwnd, -20) & ~(LONG_PTR)bits);
    SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020); /* NOSIZE|NOMOVE|NOZORDER|NOACTIVATE|FRAMECHANGED */
    RedrawWindow(hwnd, 0, 0, 0x0001 | 0x0004 | 0x0080 | 0x0100); /* INVALIDATE|ERASE|ALLCHILDREN|UPDATENOW */
    ExitProcess(0);
}
