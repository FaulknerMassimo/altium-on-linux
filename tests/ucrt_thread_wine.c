/*
 * Windows side of tests/test_ucrt_thread_wine.py: threads that start and end while another
 * thread holds the CRT's locale lock and calls LoadLibrary, with Altium's own ucrtbase.dll
 * (how-it-works §29).
 *
 * Altium's ucrtbase.dll builds a new thread's CRT data in DllMain(DLL_THREAD_ATTACH) and frees it
 * in DLL_THREAD_DETACH, both under its locale lock, and Wine calls DllMain with its loader lock
 * held. So while the main thread holds the locale lock (_lock_locales, as the C++ library does),
 * a thread that starts or ends takes the loader lock and waits for the locale lock, and the
 * main thread's next LoadLibrary never returns. The SHFolder.dll proxy turns ucrtbase's thread
 * calls off.
 *
 * UCRT_TEST_ORDER: none (no proxy: the control, which deadlocks), proxy-first (the proxy, then
 * ucrtbase.dll: the proxy's load notification) or ucrt-first (ucrtbase.dll, then the proxy: its
 * DllMain). UCRT_TEST_PROXY_WIN is the proxy's path. Two phases, each with the locale lock held
 * twice:
 *   attach  a thread starts and uses the CRT (errno); then LoadLibrary
 *   exit    a thread that has used the CRT ends; then LoadLibrary
 * Prints "result: PASS" when both LoadLibrary calls return, the threads did their CRT work, and
 * the ending thread freed its CRT data (it waits for the locale lock to do so).
 */
typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef __WCHAR_TYPE__ WCHAR;
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define STD_OUTPUT_HANDLE ((DWORD)-11)

IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned code);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
IMPORT DWORD WINAPI GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
IMPORT HANDLE WINAPI CreateThread(void *attr, ULONGLONG stack, DWORD (WINAPI *start)(void *), void *arg, DWORD flags,
                                  DWORD *id);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD ms);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT void WINAPI Sleep(DWORD ms);

static void out(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

static __declspec(noreturn) void fail(const char *why)
{
    out(why);
    out("\nresult: FAIL\n");
    ExitProcess(1);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static void (*lock_locales)(void), (*unlock_locales)(void);
static int *(*crt_errno)(void);
static volatile int attach_errno, exit_ready, exit_go;

/* The first errno access builds this thread's CRT data (construct_ptd, under the locale lock). */
static DWORD WINAPI attach_worker(void *arg)
{
    (void)arg;
    *crt_errno() = 42;
    attach_errno = *crt_errno();
    return 0;
}

/* Has CRT data, then ends while the main thread holds the locale lock: freeing it needs the lock. */
static DWORD WINAPI exit_worker(void *arg)
{
    (void)arg;
    *crt_errno() = 7;
    exit_ready = *crt_errno() == 7;
    while (!exit_go) Sleep(1);
    return 0;
}

static HMODULE load_ucrtbase(void)
{
    static const char sig[] = "Wine builtin DLL";
    HMODULE ucrt = LoadLibraryW(L"ucrtbase.dll");
    unsigned i = 0;

    if (!ucrt) fail("cannot load ucrtbase.dll");
    while (i < sizeof(sig) - 1 && ((BYTE *)ucrt)[0x40 + i] == (BYTE)sig[i]) i++;
    if (i == sizeof(sig) - 1) fail("ucrtbase.dll is Wine's builtin, not Altium's copy");
    return ucrt;
}

static void load_proxy(void)
{
    WCHAR proxy[1024];
    if (!GetEnvironmentVariableW(L"UCRT_TEST_PROXY_WIN", proxy, 1024) || !LoadLibraryW(proxy))
        fail("cannot load the proxy (UCRT_TEST_PROXY_WIN)");
}

static void lock_twice(void) { lock_locales(); lock_locales(); }
static void unlock_twice(void) { unlock_locales(); unlock_locales(); }

void start_entry(void)
{
    char order[32] = "";
    HMODULE ucrt = 0;
    HANDLE thread;

    GetEnvironmentVariableA("UCRT_TEST_ORDER", order, sizeof(order));
    if (streq(order, "proxy-first")) {
        load_proxy();
        ucrt = load_ucrtbase();
    } else if (streq(order, "ucrt-first")) {
        ucrt = load_ucrtbase();
        load_proxy();
    } else if (streq(order, "none")) {
        ucrt = load_ucrtbase();
    } else {
        fail("unknown UCRT_TEST_ORDER");
    }
    out("ucrtbase.dll: Altium's copy\n");
    if (!(lock_locales = (void (*)(void))GetProcAddress(ucrt, "_lock_locales"))
            || !(unlock_locales = (void (*)(void))GetProcAddress(ucrt, "_unlock_locales"))
            || !(crt_errno = (int *(*)(void))GetProcAddress(ucrt, "_errno")))
        fail("ucrtbase.dll lacks _lock_locales, _unlock_locales or _errno");

    lock_twice();
    if (!(thread = CreateThread(0, 0, attach_worker, 0, 0, 0))) fail("CreateThread failed");
    Sleep(500);
    out("attach: locale lock held, a thread started; calling LoadLibrary\n");
    LoadLibraryW(L"version.dll");
    out("attach: LoadLibrary returned\n");
    unlock_twice();
    WaitForSingleObject(thread, 0xffffffff);
    CloseHandle(thread);
    if (attach_errno != 42) fail("attach: the thread's errno did not work");
    out("attach: the thread's CRT data works (errno)\n");

    if (!(thread = CreateThread(0, 0, exit_worker, 0, 0, 0))) fail("CreateThread failed");
    while (!exit_ready) Sleep(1);
    lock_twice();
    exit_go = 1;
    Sleep(500);
    out("exit: locale lock held, a thread with CRT data ended; calling LoadLibrary\n");
    LoadLibraryW(L"wtsapi32.dll");
    out("exit: LoadLibrary returned\n");
    /* Freeing its CRT data (destroy_fls) takes the locale lock: the thread is still ending. */
    if (WaitForSingleObject(thread, 0) == 0) fail("exit: the thread ended without freeing its CRT data");
    out("exit: the ending thread waits for the locale lock to free its CRT data\n");
    unlock_twice();
    if (WaitForSingleObject(thread, 10000)) fail("exit: the thread did not end");
    CloseHandle(thread);
    out("exit: the thread ended\n");
    out("result: PASS\n");
    ExitProcess(0);
}
