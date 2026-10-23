/*
 * Windows side of tests/test_layered_wine.py: checks the image the SHFolder.dll proxy hands
 * to Wine for UpdateLayeredWindow with per-pixel alpha (src/shfolderproxy/shfolderproxy.c, 4).
 *
 * Before loading the proxy, points user32's import of win32u!NtUserUpdateLayeredWindow at
 * record(); the proxy then wraps that slot and calls record() as Wine. So each
 * UpdateLayeredWindow call here shows what Wine would get, and no window is needed. Each case
 * fills a 7x5 DIB section, top-down or bottom-up, with a different colour per pixel and
 * passes all of it or a part. Where that area has pixels with alpha 0, Wine must get the
 * same area the right way up, with those pixels black at alpha 1; where it has none, the
 * caller's own DC and position. record() reads the image with GetDIBits, which knows the
 * row order (GetObject reports every DIB section's height as positive).
 *
 * Environment: LAYERED_PROXY_WIN: Windows path of the proxy to test.
 */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HDC;
typedef void *HGDIOBJ;
typedef void *HMODULE;
typedef __WCHAR_TYPE__ WCHAR;
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define PAGE_READWRITE 4
#define OBJ_BITMAP 7
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define ULW_ALPHA 2
#define AC_SRC_ALPHA 1

typedef struct { int x, y; } POINT;
typedef struct { int cx, cy; } SIZE;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { BYTE op, flags, constant_alpha, format; } BLENDFUNCTION;
typedef struct {
    DWORD size;
    int width, height;
    WORD planes, bit_count;
    DWORD compression, size_image;
    int x_pels_per_meter, y_pels_per_meter;
    DWORD clr_used, clr_important;
} BITMAPINFOHEADER;
typedef struct {
    int type, width, height, width_bytes;
    WORD planes, bits_pixel;
    void *bits;
} BITMAP;
typedef struct {
    DWORD original_first_thunk, timestamp, forwarder_chain, name, first_thunk;
} IMPORT_DESCRIPTOR;

IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned code);
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT BOOL WINAPI VirtualProtect(void *addr, ULONGLONG size, DWORD protect, DWORD *old);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
IMPORT BOOL WINAPI UpdateLayeredWindow(HWND hwnd, HDC dst, POINT *dst_pos, SIZE *size, HDC src, POINT *src_pos,
                                       DWORD key, BLENDFUNCTION *blend, DWORD flags);
IMPORT HGDIOBJ WINAPI CreateDIBSection(HDC hdc, const void *info, unsigned usage, void **bits, HANDLE section,
                                       DWORD offset);
IMPORT HDC WINAPI CreateCompatibleDC(HDC hdc);
IMPORT HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ obj);
IMPORT BOOL WINAPI DeleteDC(HDC hdc);
IMPORT BOOL WINAPI DeleteObject(HGDIOBJ obj);
IMPORT HGDIOBJ WINAPI GetCurrentObject(HDC hdc, unsigned type);
IMPORT int WINAPI GetObjectW(HGDIOBJ obj, int size, void *buffer);
IMPORT int WINAPI GetDIBits(HDC hdc, HGDIOBJ bitmap, unsigned start, unsigned lines, void *bits, void *info,
                            unsigned usage);
IMPORT BOOL WINAPI GdiFlush(void);

#define W 7
#define H 5

static void print(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &written, 0);
}

static void print_hex(const char *before, DWORD v, const char *after)
{
    char buf[9];
    for (int i = 7; i >= 0; i--, v >>= 4) buf[i] = "0123456789abcdef"[v & 15];
    buf[8] = 0;
    print(before);
    print(buf);
    print(after);
}

static void print_num(const char *before, int v, const char *after)
{
    char buf[12], *p = buf + sizeof(buf) - 1;
    *p = 0;
    do *--p = '0' + v % 10; while (v /= 10);
    print(before);
    print(p);
    print(after);
}

static int same(const char *a, const char *b, int ignore_case)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ignore_case && ca >= 'A' && ca <= 'Z') ca += 32;
        if (ignore_case && cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

/* The import address slot of dll!func in the module at base. */
static ULONGLONG *import_slot(BYTE *base, const char *dll, const char *func)
{
    BYTE *nt = base + *(DWORD *)(base + 0x3c);
    DWORD rva = *(DWORD *)(nt + 0x88 + 8 * 1); /* IMAGE_DIRECTORY_ENTRY_IMPORT */

    for (IMPORT_DESCRIPTOR *desc = (IMPORT_DESCRIPTOR *)(base + rva); rva && desc->name; desc++) {
        if (!desc->original_first_thunk || !same((const char *)base + desc->name, dll, 1)) continue;
        ULONGLONG *names = (ULONGLONG *)(base + desc->original_first_thunk);
        ULONGLONG *slots = (ULONGLONG *)(base + desc->first_thunk);
        for (; *names; names++, slots++)
            if (!(*names >> 63) && same((const char *)base + (DWORD)*names + 2, func, 0)) return slots;
    }
    return 0;
}

/* What the last UpdateLayeredWindow handed to "Wine". */
static struct {
    int calls, read;
    HDC src;
    POINT pos;
    SIZE size;
    DWORD pixels[W * H]; /* the area at pos, top row first */
} got;

/* The source area as GetDIBits sees it, top row first. */
static int read_area(HDC src, POINT pos, SIZE size, DWORD *out)
{
    HGDIOBJ bitmap = GetCurrentObject(src, OBJ_BITMAP);
    BITMAPINFOHEADER info = { .size = sizeof(info), .planes = 1, .bit_count = 32, .compression = BI_RGB };
    static DWORD all[W * H];
    BITMAP bm;

    if (GetObjectW(bitmap, sizeof(bm), &bm) != sizeof(bm) || bm.width * bm.height > W * H) return 0;
    if (pos.x < 0 || pos.y < 0 || size.cx > bm.width - pos.x || size.cy > bm.height - pos.y) return 0;
    info.width = bm.width;
    info.height = -bm.height;
    GdiFlush();
    if (GetDIBits(src, bitmap, 0, bm.height, all, &info, DIB_RGB_COLORS) != bm.height) return 0;
    for (int y = 0; y < size.cy; y++)
        for (int x = 0; x < size.cx; x++) out[y * size.cx + x] = all[(pos.y + y) * bm.width + pos.x + x];
    return 1;
}

static BOOL record(HWND hwnd, HDC dst, const POINT *dst_pos, const SIZE *size, HDC src, const POINT *src_pos,
                   DWORD key, const BLENDFUNCTION *blend, DWORD flags, const RECT *dirty)
{
    (void)hwnd; (void)dst; (void)dst_pos; (void)key; (void)blend; (void)flags; (void)dirty;
    got.calls++;
    got.src = src;
    got.pos = src_pos ? *src_pos : (POINT){ 0, 0 };
    got.size = *size;
    got.read = got.size.cx <= W && got.size.cy <= H && read_area(src, got.pos, got.size, got.pixels);
    return 1;
}

static DWORD colour(int x, int y)
{
    return 0xff000000 | (DWORD)(0x20 + y * 0x20) << 16 | (DWORD)(0x10 + x * 0x20) << 8 | 0x5a;
}

struct test {
    const char *name;
    int top_down, sx, sy, cx, cy;
    int tx, ty; /* a pixel with alpha 0, or -1 */
};

static int run(const struct test *t)
{
    BITMAPINFOHEADER info = { .size = sizeof(info), .width = W, .planes = 1, .bit_count = 32,
                              .compression = BI_RGB };
    BLENDFUNCTION blend = { 0, 0, 255, AC_SRC_ALPHA };
    POINT pos = { t->sx, t->sy };
    SIZE size = { t->cx, t->cy };
    DWORD *bits, want[W * H];
    int copied = t->tx >= t->sx && t->tx < t->sx + t->cx && t->ty >= t->sy && t->ty < t->sy + t->cy;
    HGDIOBJ bitmap, old;
    HDC src;

    info.height = t->top_down ? -H : H;
    if (!(bitmap = CreateDIBSection(0, &info, DIB_RGB_COLORS, (void **)&bits, 0, 0)) || !(src = CreateCompatibleDC(0))) {
        print("FAIL ");
        print(t->name);
        print(": no DIB section\n");
        return 0;
    }
    for (int y = 0; y < H; y++) {
        DWORD *row = bits + (t->top_down ? y : H - 1 - y) * W;
        for (int x = 0; x < W; x++) row[x] = x == t->tx && y == t->ty ? colour(x, y) & 0xffffff : colour(x, y);
    }
    for (int y = 0; y < t->cy; y++)
        for (int x = 0; x < t->cx; x++) {
            DWORD c = colour(t->sx + x, t->sy + y);
            if (t->sx + x == t->tx && t->sy + y == t->ty) c = copied ? 0x01000000 : c & 0xffffff;
            want[y * t->cx + x] = c;
        }
    old = SelectObject(src, bitmap);
    got.calls = 0;
    UpdateLayeredWindow(0, 0, 0, &size, src, &pos, 0, &blend, ULW_ALPHA);

    const char *why = 0;
    int bad = -1;
    if (got.calls != 1) why = "Wine was not called once";
    else if (copied && (got.src == src || got.pos.x || got.pos.y)) why = "Wine got the caller's DC, not a copy";
    else if (!copied && (got.src != src || got.pos.x != t->sx || got.pos.y != t->sy))
        why = "Wine got a copy, but there is no pixel with alpha 0 to replace";
    else if (got.size.cx != t->cx || got.size.cy != t->cy) why = "Wine got another size";
    else if (!got.read) why = "cannot read what Wine got";
    else
        for (int i = 0; i < t->cx * t->cy && bad < 0; i++)
            if (got.pixels[i] != want[i]) bad = i;
    if (bad >= 0) why = "Wine got other pixels";

    print(why ? "FAIL " : "ok   ");
    print(t->name);
    if (why) {
        print(": ");
        print(why);
        if (bad >= 0) {
            print_num(" (at ", bad % t->cx, ",");
            print_num("", bad / t->cx, ": ");
            print_hex("", got.pixels[bad], " instead of ");
            print_hex("", want[bad], ")");
        }
    }
    print("\n");
    SelectObject(src, old);
    DeleteDC(src);
    DeleteObject(bitmap);
    return !why;
}

void start_entry(void)
{
    static const struct test tests[] = {
        { "top-down, whole image", 1, 0, 0, W, H, 1, 0 },
        { "bottom-up, whole image", 0, 0, 0, W, H, 1, 0 },
        { "top-down, part", 1, 2, 1, 4, 3, 3, 1 },
        { "bottom-up, part", 0, 2, 1, 4, 3, 3, 1 },
        { "top-down, no alpha 0", 1, 0, 0, W, H, -1, -1 },
        { "top-down, alpha 0 outside the part", 1, 2, 0, 4, 2, 3, 4 },
        { "bottom-up, alpha 0 outside the part", 0, 2, 0, 4, 2, 3, 4 },
    };
    WCHAR proxy[1024];
    BYTE *user32 = (BYTE *)GetModuleHandleW(L"user32.dll");
    ULONGLONG *slot = user32 ? import_slot(user32, "win32u.dll", "NtUserUpdateLayeredWindow") : 0;
    DWORD old;
    int passed = 0, count = sizeof(tests) / sizeof(tests[0]);

    if (!GetEnvironmentVariableW(L"LAYERED_PROXY_WIN", proxy, 1024)) {
        print("LAYERED_PROXY_WIN must be set\n");
        ExitProcess(2);
    }
    if (!slot || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        print("user32 does not import NtUserUpdateLayeredWindow from win32u\n");
        ExitProcess(2);
    }
    *slot = (ULONGLONG)record;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    if (!LoadLibraryW(proxy) || *slot == (ULONGLONG)record) {
        print("the proxy did not load, or did not wrap UpdateLayeredWindow\n");
        ExitProcess(2);
    }
    for (int i = 0; i < count; i++) passed += run(&tests[i]);
    print(passed == count ? "all passed\n" : "some failed\n");
    ExitProcess(passed == count ? 0 : 1);
}
