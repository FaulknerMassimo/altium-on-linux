/*
 * cmd.exe shim for Altium Designer under Wine.
 *
 * Altium opens its sign-in page with `cmd /c start <url>`. Wine's start.exe
 * copies the URL into a MAX_PATH buffer in search_path() and crashes when the
 * URL is longer than ~260 chars, so the browser never opens. Placed next to
 * X2.exe, this shim is found before system32\cmd.exe. It opens
 * `/c start <http(s) url>` directly with ShellExecuteExW and hands every other
 * command line, unchanged, to the real cmd.exe.
 *
 * Build: scripts/build.sh (clang + lld-link + llvm-dlltool; no Windows SDK or CRT needed).
 */

typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef int BOOL;
typedef unsigned char BYTE;
typedef void *HANDLE;
typedef __WCHAR_TYPE__ WCHAR; /* matches L"" literals; 16-bit on the msvc target */

typedef struct {
    DWORD cb;
    WCHAR *lpReserved, *lpDesktop, *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;

typedef struct {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

typedef struct {
    DWORD cbSize;
    ULONG fMask;
    HANDLE hwnd;
    const WCHAR *lpVerb, *lpFile, *lpParameters, *lpDirectory;
    int nShow;
    HANDLE hInstApp;
    void *lpIDList;
    const WCHAR *lpClass;
    HANDLE hkeyClass;
    DWORD dwHotKey;
    HANDLE hIconOrMonitor;
    HANDLE hProcess;
} SHELLEXECUTEINFOW;

#define IMPORT __declspec(dllimport)
IMPORT WCHAR *GetCommandLineW(void);
IMPORT unsigned GetSystemDirectoryW(WCHAR *buf, unsigned size);
IMPORT void GetStartupInfoW(STARTUPINFOW *si);
IMPORT BOOL CreateProcessW(const WCHAR *app, WCHAR *cmdline, void *pa, void *ta, BOOL inherit,
                           DWORD flags, void *env, const WCHAR *dir, STARTUPINFOW *si,
                           PROCESS_INFORMATION *pi);
IMPORT DWORD WaitForSingleObject(HANDLE h, DWORD ms);
IMPORT BOOL GetExitCodeProcess(HANDLE h, DWORD *code);
IMPORT BOOL CloseHandle(HANDLE h);
IMPORT HANDLE GetProcessHeap(void);
IMPORT void *HeapAlloc(HANDLE heap, DWORD flags, unsigned long long size);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);
IMPORT BOOL ShellExecuteExW(SHELLEXECUTEINFOW *sei);

#define HEAP_ZERO_MEMORY 0x08
#define INFINITE 0xffffffff
#define SEE_MASK_NOASYNC 0x100
#define SEE_MASK_FLAG_NO_UI 0x400
#define SW_SHOWNORMAL 1

/* clang may lower struct zeroing to memset; there is no CRT to provide it. */
void *memset(void *dst, int c, unsigned long long n)
{
    volatile BYTE *p = dst;
    while (n--) *p++ = (BYTE)c;
    return dst;
}

static int is_space(WCHAR c) { return c == ' ' || c == '\t'; }
static WCHAR lower(WCHAR c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static unsigned long long wlen(const WCHAR *s)
{
    unsigned long long n = 0;
    while (s[n]) n++;
    return n;
}

static WCHAR *skip_spaces(WCHAR *p)
{
    while (is_space(*p)) p++;
    return p;
}

/* Case-insensitive match of a word followed by whitespace; returns the text after it. */
static WCHAR *match_word(WCHAR *p, const WCHAR *word)
{
    while (*word)
        if (lower(*p++) != *word++) return 0;
    return is_space(*p) ? skip_spaces(p) : 0;
}

static int starts_with_ci(const WCHAR *s, const WCHAR *prefix)
{
    while (*prefix)
        if (lower(*s++) != *prefix++) return 0;
    return 1;
}

/* Skip argv[0] the way the C runtime does (quoted or up to whitespace). */
static WCHAR *skip_program_name(WCHAR *p)
{
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p) p++;
    } else {
        while (*p && !is_space(*p)) p++;
    }
    return skip_spaces(p);
}

/*
 * If args are `/c start ["title"] <http(s) url>`, return the URL with cmd's
 * caret escapes and surrounding quotes removed; otherwise return 0.
 */
static WCHAR *extract_start_url(WCHAR *args)
{
    WCHAR *p = match_word(args, L"/c");
    WCHAR *url, *out;
    unsigned long long n;

    if (!p || !(p = match_word(p, L"start"))) return 0;
    if (p[0] == '"' && p[1] == '"' && is_space(p[2])) p = skip_spaces(p + 2); /* empty title */

    n = wlen(p);
    while (n && is_space(p[n - 1])) n--;
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { p++; n -= 2; }

    url = out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof(WCHAR));
    if (!url) return 0;
    for (unsigned long long i = 0; i < n; i++) {
        if (p[i] == '^' && i + 1 < n) i++;
        else if (is_space(p[i]) || p[i] == '"') return 0; /* more than a bare URL */
        *out++ = p[i];
    }
    *out = 0;

    if (!starts_with_ci(url, L"http://") && !starts_with_ci(url, L"https://")) return 0;
    return url;
}

static DWORD run_real_cmd(WCHAR *args)
{
    WCHAR sysdir[260];
    unsigned len = GetSystemDirectoryW(sysdir, 260);
    unsigned long long size = len + wlen(L"\\cmd.exe\" ") + wlen(args) + 2;
    WCHAR *exe, *cmdline, *q;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    exe = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 9) * sizeof(WCHAR));
    cmdline = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(WCHAR));
    if (!exe || !cmdline || !len) return 1;

    q = exe;
    for (unsigned i = 0; i < len; i++) *q++ = sysdir[i];
    for (const WCHAR *s = L"\\cmd.exe"; *s; s++) *q++ = *s;

    q = cmdline;
    *q++ = '"';
    for (const WCHAR *s = exe; *s; s++) *q++ = *s;
    *q++ = '"';
    *q++ = ' ';
    for (const WCHAR *s = args; *s; s++) *q++ = *s;

    GetStartupInfoW(&si);
    if (!CreateProcessW(exe, cmdline, 0, 0, 1, 0, 0, 0, &si, &pi)) return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

void start_entry(void)
{
    WCHAR *args = skip_program_name(GetCommandLineW());
    WCHAR *url = extract_start_url(args);

    if (url) {
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = url;
        sei.nShow = SW_SHOWNORMAL;
        ExitProcess(ShellExecuteExW(&sei) ? 0 : 1);
    }
    ExitProcess(run_real_cmd(args));
}
