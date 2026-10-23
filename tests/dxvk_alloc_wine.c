/*
 * Windows side of tests/test_dxvk_alloc_wine.py: DXVK's allocator under a swapchain that is
 * recreated over and over while another thread creates buffers (how-it-works §22).
 *
 * Loads the SHFolder.dll proxy (unless DXVK_TEST_PROXY_WIN is empty), then DXVK's d3d11.dll,
 * and checks that the proxy put a jump at the start of DXVK's importImageResource and
 * importBufferResource. Then one thread resizes a swapchain and presents, for DXVK_TEST_SECONDS
 * (default 60): each present after a resize makes DXVK's command thread create a Vulkan
 * swapchain and import its images, which takes from the allocator's pool. Two other threads
 * create and release buffers the whole time, which take from it and give back under the
 * allocator's lock. Without the lock on the imports the pool's free list gets corrupted and
 * the process crashes.
 *
 * Prints how many swapchains were made and buffers created, and "result: PASS" at the end.
 */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef long HRESULT;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HMODULE;
typedef __WCHAR_TYPE__ WCHAR;
typedef long long LRESULT;
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0 0
#define WS_POPUP 0x80000000u

IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned code);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
IMPORT HANDLE WINAPI CreateThread(void *attr, ULONGLONG stack, DWORD (WINAPI *start)(void *), void *arg, DWORD flags,
                                  DWORD *id);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD ms);
IMPORT DWORD WINAPI GetTickCount(void);
IMPORT void WINAPI Sleep(DWORD ms);
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT HWND WINAPI CreateWindowExW(DWORD ex, const WCHAR *cls, const WCHAR *title, DWORD style, int x, int y, int w,
                                   int h, HWND parent, void *menu, HMODULE inst, void *param);
IMPORT BOOL WINAPI SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags);
#define SWP_NOMOVE 0x02
#define SWP_NOZORDER 0x04
#define SWP_NOACTIVATE 0x10

typedef struct { UINT num, den; } DXGI_RATIONAL;
typedef struct {
    UINT width, height;
    DXGI_RATIONAL refresh;
    UINT format, scanline_ordering, scaling;
} DXGI_MODE_DESC;
typedef struct {
    DXGI_MODE_DESC buffer;
    UINT sample_count, sample_quality;
    UINT usage, buffer_count;
    HWND window;
    BOOL windowed;
    UINT swap_effect, flags;
} DXGI_SWAP_CHAIN_DESC;
typedef struct { UINT size, usage, bind_flags, cpu_access, misc, stride; } D3D11_BUFFER_DESC;

typedef HRESULT (WINAPI *create_device_fn)(void *adapter, UINT driver, HMODULE software, UINT flags, const UINT *levels,
                                           UINT count, UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc, void **swapchain,
                                           void **device, UINT *level, void **context);
typedef HRESULT (*present_fn)(void *swapchain, UINT sync, UINT flags);
typedef HRESULT (*resize_fn)(void *swapchain, UINT count, UINT width, UINT height, UINT format, UINT flags);
typedef HRESULT (*create_buffer_fn)(void *device, const D3D11_BUFFER_DESC *desc, const void *data, void **buffer);
typedef DWORD (*release_fn)(void *obj);
#define VTBL(obj) (*(void ***)(obj))
#define SLOT_RELEASE 2
#define SLOT_PRESENT 8
#define SLOT_RESIZE_BUFFERS 13
#define SLOT_CREATE_BUFFER 3

#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20
#define DXGI_SWAP_EFFECT_FLIP_DISCARD 4
#define D3D_DRIVER_TYPE_HARDWARE 1
#define D3D11_SDK_VERSION 7
#define D3D11_BIND_VERTEX_BUFFER 1

static void print(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &written, 0);
}

static void print_num(const char *before, long long v, const char *after)
{
    char buf[24], *p = buf + sizeof(buf) - 1;
    unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
    *p = 0;
    do *--p = '0' + u % 10; while (u /= 10);
    if (v < 0) *--p = '-';
    print(before);
    print(p);
    print(after);
}

/* The bytes of each function after the 14-byte jump the proxy writes, and where they start. */
static const struct { const char *name; BYTE after[16]; int offset; } dxvk_functions[] = {
    { "importImageResource", { 0x4a, 0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xd2, 0x48, 0x89, 0xcb, 0x49, 0x89, 0xd3, 0x66, 0x0f }, 14 },
    { "importBufferResource", { 0xf8, 0x4a, 0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xd2, 0x4c, 0x89, 0xcf, 0x48, 0x89, 0xcb, 0x49 }, 16 },
};

/* 1: starts with a jump into the proxy, 0: original code, -1: not found */
static int hooked(BYTE *base, int which, BYTE *proxy)
{
    BYTE *nt = base + *(DWORD *)(base + 0x3c);
    UINT size = *(UINT *)(nt + 0x50); /* SizeOfImage */
    const BYTE *after = dxvk_functions[which].after;
    int offset = dxvk_functions[which].offset;
    UINT proxy_size = proxy ? *(UINT *)(proxy + *(DWORD *)(proxy + 0x3c) + 0x50) : 0;

    for (UINT i = 0x1000 + offset; i + 16 < size; i++) {
        int j = 0;
        while (j < 16 && base[i + j] == after[j]) j++;
        if (j < 16) continue;
        BYTE *code = base + i - offset;
        if (code[0] != 0xff || code[1] != 0x25) return 0;
        BYTE *target = *(BYTE **)(code + 6);
        return proxy && target >= proxy && target < proxy + proxy_size ? 1 : 0;
    }
    return -1;
}

static void *device, *swapchain;
static volatile int stop;
static volatile long long buffers_made, buffer_failures, presents, resizes;

static DWORD WINAPI buffer_thread(void *arg)
{
    D3D11_BUFFER_DESC desc = { 256, 0 /* DEFAULT */, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    void *buffers[64];
    (void)arg;
    while (!stop) {
        for (int i = 0; i < 64; i++) {
            desc.size = 256 * (1 + (i & 7));
            if (((create_buffer_fn)VTBL(device)[SLOT_CREATE_BUFFER])(device, &desc, 0, &buffers[i]) < 0) {
                buffers[i] = 0;
                buffer_failures++;
            }
        }
        for (int i = 0; i < 64; i++)
            if (buffers[i]) ((release_fn)VTBL(buffers[i])[SLOT_RELEASE])(buffers[i]);
        __sync_fetch_and_add(&buffers_made, 64);
        Sleep(1); /* or the presenting thread starves on DXVK's initializer lock */
    }
    return 0;
}

void start_entry(void)
{
    WCHAR proxy_path[1024], value[32];
    DWORD seconds = 60, start;
    BYTE *proxy = 0, *d3d11;
    int pass = 1;

    if (GetEnvironmentVariableW(L"DXVK_TEST_PROXY_WIN", proxy_path, 1024) && proxy_path[0]) {
        if (!(proxy = LoadLibraryW(proxy_path))) {
            print("cannot load the proxy\n");
            ExitProcess(2);
        }
    }
    if (GetEnvironmentVariableW(L"DXVK_TEST_SECONDS", value, 32)) {
        seconds = 0;
        for (WCHAR *p = value; *p >= '0' && *p <= '9'; p++) seconds = seconds * 10 + (*p - '0');
    }
    if (!(d3d11 = LoadLibraryW(L"d3d11.dll"))) {
        print("cannot load d3d11.dll\n");
        ExitProcess(2);
    }
    for (int i = 0; i < 2; i++) {
        int h = hooked(d3d11, i, proxy);
        print(dxvk_functions[i].name);
        print(h > 0 ? ": locked by the proxy\n" : h == 0 ? ": original code\n" : ": not found (not DXVK 3.1?)\n");
        if (proxy && h <= 0) pass = 0;
    }

    HWND window = CreateWindowExW(0, L"Static", L"dxvk-alloc-test", WS_POPUP, 0, 0, 64, 64, 0, 0,
                                  GetModuleHandleW(0), 0);
    DXGI_SWAP_CHAIN_DESC desc = { { 64, 64, { 0, 1 }, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0 }, 1, 0,
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, window, 1, DXGI_SWAP_EFFECT_FLIP_DISCARD, 0 };
    create_device_fn create = (create_device_fn)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    void *context = 0;
    HRESULT hr;
    if (!window || !create
            || (hr = create(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &desc, &swapchain, &device, 0,
                            &context)) < 0) {
        print("cannot create the device and swapchain\n");
        ExitProcess(2);
    }

    HANDLE threads[2];
    int thread_count = 2;
    if (GetEnvironmentVariableW(L"DXVK_TEST_THREADS", value, 32)) thread_count = value[0] - '0';
    for (int i = 0; i < thread_count; i++) threads[i] = CreateThread(0, 0, buffer_thread, 0, 0, 0);
    /* DXVK makes a new Vulkan swapchain when the window's size changes. */
    start = GetTickCount();
    DWORD reported = start;
    for (UINT n = 0; GetTickCount() - start < seconds * 1000; n++) {
        int width = 64 + (n & 1) * 32;
        SetWindowPos(window, 0, 0, 0, width, 64, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (((resize_fn)VTBL(swapchain)[SLOT_RESIZE_BUFFERS])(swapchain, 0, width, 64, 0, 0) >= 0) resizes++;
        if (((present_fn)VTBL(swapchain)[SLOT_PRESENT])(swapchain, 0, 0) >= 0) presents++;
        if (GetTickCount() - reported >= 5000) {
            reported = GetTickCount();
            print_num("  ", (reported - start) / 1000, " s: ");
            print_num("", presents, " presents, ");
            print_num("", buffers_made, " buffers\n");
        }
    }
    stop = 1;
    for (int i = 0; i < thread_count; i++) WaitForSingleObject(threads[i], 10000);
    print_num("resizes: ", resizes, ", ");
    print_num("presents: ", presents, ", ");
    print_num("buffers: ", buffers_made, ", ");
    print_num("failed: ", buffer_failures, "\n");
    pass = pass && presents > 100 && buffers_made > 1000 && !buffer_failures;
    print(pass ? "result: PASS\n" : "result: FAIL\n");
    ExitProcess(pass ? 0 : 1);
}
