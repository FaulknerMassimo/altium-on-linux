/*
 * Windows side of tests/test_fdguard_wine.py: run in the bottle with the fd guard and
 * tests/fdguard_poke.c preloaded.
 *
 * Starts and ends 320 threads that each talk to wineserver, then keeps a worker thread
 * doing wineserver calls (CreateEventW/CloseHandle) while the poke library closes every
 * pipe in the process, as a native double close would. Then checks that the worker, the
 * main thread and new threads can still talk to wineserver. Without the guard, the
 * threads lose their pipes and die or hang.
 *
 * Environment: FDPOKE_TRIGGER_WIN, FDPOKE_DONE_WIN: Windows paths of the files that start
 * the poke (created here) and report it done (created by the poke library);
 * FDPOKE_RESULT_WIN: where to write what happened (not stdout: when it is a pipe, the poke
 * closes Wine's copy of it too, which the guard rightly leaves alone).
 */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef __WCHAR_TYPE__ WCHAR;
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2
#define WAIT_OBJECT_0 0
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT int WINAPI WriteFile(HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned code);
IMPORT void WINAPI Sleep(DWORD ms);
IMPORT HANDLE WINAPI CreateThread(void *attr, unsigned long long stack, DWORD (WINAPI *start)(void *), void *arg,
                                  DWORD flags, DWORD *id);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD ms);
IMPORT int WINAPI CloseHandle(HANDLE handle);
IMPORT HANDLE WINAPI CreateEventW(void *attr, int manual, int initial, const WCHAR *name);
IMPORT int WINAPI SetEvent(HANDLE event);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
IMPORT DWORD WINAPI GetFileAttributesW(const WCHAR *path);
IMPORT HANDLE WINAPI CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *attr, DWORD disposition,
                                 DWORD flags, HANDLE template_file);

static HANDLE result = (HANDLE)-1;

static void print(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(result != (HANDLE)-1 ? result : GetStdHandle(STD_OUTPUT_HANDLE), s, n, &written, 0);
}

static void print_num(const char *before, long v, const char *after)
{
    char buf[24], *p = buf + sizeof(buf) - 1;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    *p = 0;
    do *--p = '0' + u % 10; while (u /= 10);
    if (neg) *--p = '-';
    print(before);
    print(p);
    print(after);
}

/* One round trip: create an event and close it again. */
static int server_call(void)
{
    HANDLE event = CreateEventW(0, 1, 0, 0);
    if (!event) return 0;
    SetEvent(event);
    int ok = WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
    return CloseHandle(event) && ok;
}

static DWORD WINAPI short_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) if (!server_call()) return 1;
    Sleep(1);
    return 0;
}

static volatile int stop_worker;
static volatile long worker_calls, worker_failures;

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    while (!stop_worker) {
        if (server_call()) worker_calls++;
        else worker_failures++;
        Sleep(2);
    }
    return 0;
}

/* Start `count` threads, `batch` at a time; how many ended normally. */
static int churn(int count, int batch)
{
    int ok = 0;
    for (int done = 0; done < count; done += batch) {
        HANDLE threads[64];
        for (int i = 0; i < batch; i++) threads[i] = CreateThread(0, 0, short_thread, 0, 0, 0);
        for (int i = 0; i < batch; i++) {
            if (!threads[i]) continue;
            if (WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0) ok++;
            CloseHandle(threads[i]);
        }
    }
    return ok;
}

void start_entry(void)
{
    WCHAR trigger[512], done[512], path[512];
    if (!GetEnvironmentVariableW(L"FDPOKE_TRIGGER_WIN", trigger, 512) ||
        !GetEnvironmentVariableW(L"FDPOKE_DONE_WIN", done, 512) || !GetEnvironmentVariableW(L"FDPOKE_RESULT_WIN", path, 512)) {
        print("FDPOKE_TRIGGER_WIN, FDPOKE_DONE_WIN and FDPOKE_RESULT_WIN must be set\n");
        ExitProcess(2);
    }
    result = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    print_num("churn: ", churn(320, 32), " of 320 threads ok\n");

    HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
    Sleep(200);
    long before = worker_calls;
    HANDLE file = CreateFileW(trigger, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (file == (HANDLE)-1) { print("cannot create the trigger file\n"); ExitProcess(2); }
    CloseHandle(file);
    int waited = 0;
    while (GetFileAttributesW(done) == INVALID_FILE_ATTRIBUTES && waited < 30000) { Sleep(10); waited += 10; }
    print(waited < 30000 ? "poke: done\n" : "poke: never happened\n");
    long at_poke = worker_calls;
    Sleep(500);
    int main_ok = 0;
    for (int i = 0; i < 20; i++) main_ok += server_call();
    int after = churn(64, 16);
    stop_worker = 1;
    int worker_ended = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
    print_num("worker: ", before > 0, " before the poke, ");
    print_num("", worker_calls - at_poke, " calls after it, ");
    print_num("", worker_failures, " failed, ");
    print(worker_ended ? "ended normally\n" : "did not end\n");
    print_num("main: ", main_ok, " of 20 calls ok after the poke\n");
    print_num("new threads: ", after, " of 64 ok after the poke\n");
    int pass = waited < 30000 && before > 0 && worker_calls - at_poke > 50 && !worker_failures && worker_ended &&
               main_ok == 20 && after == 64;
    print(pass ? "result: PASS\n" : "result: FAIL\n");
    ExitProcess(pass ? 0 : 1);
}
