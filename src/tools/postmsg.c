/*
 * postmsg <hex hwnd> <hex msg> <hex wparam> <hex lparam> [count] [delay ms]
 * Post a window message `count` times (default 1), `delay` ms apart. Drives Altium's views
 * (mouse wheel, close) without touching the real keyboard and mouse, so tests do not steal
 * focus from whatever the user is doing (diagnostic; built by scripts/build.sh).
 */
typedef void *HWND;
#define IMPORT __declspec(dllimport)
IMPORT int PostMessageW(HWND hwnd, unsigned msg, unsigned long long wparam, long long lparam);
IMPORT void Sleep(unsigned ms);
IMPORT __WCHAR_TYPE__ *GetCommandLineW(void);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);

static unsigned long long number(__WCHAR_TYPE__ **p, unsigned base, unsigned long long missing)
{
    unsigned long long v = 0;
    int found = 0;
    while (**p == ' ') (*p)++;
    for (;; (*p)++, found = 1) {
        __WCHAR_TYPE__ c = **p;
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * base + d;
    }
    return found ? v : missing;
}

void start_entry(void)
{
    __WCHAR_TYPE__ *p = GetCommandLineW();
    HWND hwnd;
    unsigned msg, count, delay;
    unsigned long long wparam;
    long long lparam;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; } else while (*p && *p != ' ') p++;
    hwnd = (HWND)number(&p, 16, 0);
    msg = (unsigned)number(&p, 16, 0);
    wparam = number(&p, 16, 0);
    lparam = (long long)number(&p, 16, 0);
    count = (unsigned)number(&p, 10, 1);
    delay = (unsigned)number(&p, 10, 0);
    for (unsigned i = 0; i < count; i++) {
        if (!PostMessageW(hwnd, msg, wparam, lparam)) ExitProcess(1);
        if (delay && i + 1 < count) Sleep(delay);
    }
    ExitProcess(0);
}
