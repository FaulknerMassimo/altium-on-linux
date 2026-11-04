/*
 * start.exe shim for 32-bit programs in the bottle (installed as syswow64\start.exe).
 *
 * The Altium Designer installer signs in through Authorizer.exe, a 32-bit program it
 * unpacks into a temporary folder. Like Altium, it opens the sign-in page with
 * `cmd /c start <url>`, and Wine's start.exe crashes on the URL: search_path() treats it
 * as a file name and concatenates its last component (~240 chars of OAuth query) and the
 * current directory into a MAX_PATH buffer. No browser opens and the sign-in times out.
 * The cmd.exe shim next to X2.EXE cannot help here, as Authorizer.exe is found in a new
 * temporary folder each time; but 32-bit cmd.exe always runs `start` as
 * C:\windows\syswow64\start.exe, so this shim goes there.
 *
 * It opens `start ["title"] [options] <http(s) url>` itself with ShellExecuteExW, and
 * hands every other command line, unchanged, to Wine's 64-bit start.exe in system32 (this
 * shim is a 64-bit program, so system32 is the 64-bit folder), waiting for it and
 * returning its exit code, as cmd's `start` waits for start.exe.
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
IMPORT DWORD GetModuleFileNameW(HANDLE module, WCHAR *buf, DWORD size);
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
IMPORT WCHAR **CommandLineToArgvW(const WCHAR *cmdline, int *argc);
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

static int equals_ci(const WCHAR *a, const WCHAR *b)
{
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return lower(*a) == lower(*b);
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
    while (is_space(*p)) p++;
    return p;
}

/*
 * The URL to open if the command line is `start ["title"] [options] <http(s) url>` with
 * nothing after the URL, else 0. Mirrors the option parsing of Wine's start.exe; its own
 * extensions (/unix, /exec, /progIDOpen) are left to it. cmd's caret escapes are dropped,
 * in case they reach start.exe (a URL has no literal ^).
 */
static WCHAR *start_url(void)
{
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR *url, *out;
    const WCHAR *p;
    int have_title = 0;

    if (!argv) return 0;
    for (i = 1; i < argc; i++) {
        WCHAR *a = argv[i];
        if (!have_title && a[0] == '"') { have_title = 1; continue; } /* cmd passes "\"title\"" */
        if (a[0] != '/') break;
        if (equals_ci(a, L"/unix") || equals_ci(a, L"/exec") || equals_ci(a, L"/progIDOpen")) return 0;
        if ((lower(a[1]) == 'd' && !a[2]) || equals_ci(a, L"/node") || equals_ci(a, L"/affinity")
            || equals_ci(a, L"/machine"))
            i++; /* option with a separate argument */
    }
    if (i != argc - 1) return 0; /* no file, or parameters after it */

    p = argv[i];
    url = out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (wlen(p) + 1) * sizeof(WCHAR));
    if (!url) return 0;
    for (; *p; p++) {
        if (*p == '^' && p[1]) p++;
        if (is_space(*p) || *p == '"') return 0;
        *out++ = *p;
    }
    *out = 0;
    if (!starts_with_ci(url, L"http://") && !starts_with_ci(url, L"https://")) return 0;
    return url;
}

/* Run Wine's own start.exe (64-bit, in system32) with our arguments, unchanged. */
static DWORD run_wine_start(WCHAR *args)
{
    WCHAR sysdir[260], self[260];
    unsigned len = GetSystemDirectoryW(sysdir, 260);
    unsigned long long size = len + wlen(L"\\start.exe\" ") + wlen(args) + 2;
    WCHAR *exe, *cmdline, *q;
    const WCHAR *s;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (!len || len > 240) return 1;
    exe = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 11) * sizeof(WCHAR));
    cmdline = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(WCHAR));
    if (!exe || !cmdline) return 1;

    q = exe;
    for (unsigned i = 0; i < len; i++) *q++ = sysdir[i];
    for (s = L"\\start.exe"; *s; s++) *q++ = *s;

    /* Installed by mistake as system32\start.exe, this would run itself forever. */
    if (GetModuleFileNameW(0, self, 260) && equals_ci(self, exe)) return 1;

    q = cmdline;
    *q++ = '"';
    for (s = exe; *s; s++) *q++ = *s;
    *q++ = '"';
    *q++ = ' ';
    for (s = args; *s; s++) *q++ = *s;

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
    WCHAR *url = start_url();

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
    ExitProcess(run_wine_start(skip_program_name(GetCommandLineW())));
}
