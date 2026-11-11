/*
 * Windows side of tests/test_polydraw_wine.py: llPDFLib's PolyDraw handlers, taken from Altium's
 * own modules, before and after the SHFolder.dll proxy fixes them (how-it-works §30).
 *
 * POLYDRAW_TEST_MODULES lists the modules' Windows paths, separated by ';'; POLYDRAW_TEST_PROXY_WIN
 * is the proxy's path. Each module is mapped without resolving its imports or running its DllMain
 * (DONT_RESOLVE_DLL_REFERENCES), and its EMR_POLYDRAW16 and EMR_POLYDRAW handlers are found by
 * size in its function table and by their code. Then the proxy is loaded (its DllMain fixes the
 * modules already loaded), and each handler is checked twice, as it was and as it is now. Each
 * check runs a copy of the handler in a buffer of its own, with its calls into the rest of
 * llPDFLib (start a path, the two coordinate transforms, MoveTo, LineTo, CurveTo, a pointer into
 * the record, stroke) sent to stubs that log them:
 *
 *   as it was   a 2-point line, as Wine's GDI+ records them, must not return: Altium's freeze
 *   fixed       records like Wine's GDI+ writes (lines, closed polygons, circles of four Béziers,
 *               two figures in one record, a line then a closed Bézier, a lone MoveTo), inside a
 *               path and outside one, must give exactly the calls PolyDraw means, and leave the
 *               last point drawn (or the figure's start, after a close) as the current position
 *
 * Prints "result: PASS" when all of that holds for every module.
 */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef __WCHAR_TYPE__ WCHAR;
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define DONT_RESOLVE_DLL_REFERENCES 0x1
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_EXECUTE_READWRITE 0x40
#define WAIT_TIMEOUT 258

IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned code);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT HMODULE WINAPI LoadLibraryExW(const WCHAR *path, HANDLE file, DWORD flags);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
IMPORT HANDLE WINAPI CreateThread(void *attr, ULONGLONG stack, DWORD (WINAPI *start)(void *), void *arg, DWORD flags,
                                  DWORD *id);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD ms);
IMPORT BOOL WINAPI TerminateThread(HANDLE thread, DWORD code);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT void *WINAPI VirtualAlloc(void *addr, ULONGLONG size, DWORD type, DWORD protect);

int _fltused; /* float code without a CRT */

void *memset(void *dst, int c, ULONGLONG n)
{
    volatile BYTE *p = dst;
    while (n--) *p++ = (BYTE)c;
    return dst;
}

void *memcpy(void *dst, const void *src, ULONGLONG n)
{
    volatile BYTE *d = dst;
    const BYTE *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static void out(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

static void out_num(long long v)
{
    char buf[24];
    int i = sizeof(buf) - 1, neg = v < 0;
    buf[i] = 0;
    if (neg) v = -v;
    do buf[--i] = (char)('0' + v % 10); while (v /= 10);
    if (neg) buf[--i] = '-';
    out(buf + i);
}

static void out_hex(ULONGLONG v)
{
    char buf[20];
    int i = sizeof(buf) - 1;
    buf[i] = 0;
    do buf[--i] = "0123456789abcdef"[v & 15]; while (v >>= 4);
    out("0x");
    out(buf + i);
}

static void out_w(const WCHAR *s)
{
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        buf[0] = *s < 128 ? (char)*s : '?';
        out(buf);
    }
}

static int failures;

static void check(int ok, const char *what)
{
    out(ok ? "    ok   " : "    FAIL ");
    out(what);
    out("\n");
    failures += !ok;
}

/* ---- The module's function table ------------------------------------------- */

typedef struct { DWORD begin, end, unwind; } RUNTIME_FUNCTION;

static RUNTIME_FUNCTION *functions(BYTE *base, DWORD *count)
{
    BYTE *nt = base + *(unsigned *)(base + 0x3c);
    unsigned rva = *(unsigned *)(nt + 0x88 + 8 * 3), size = *(unsigned *)(nt + 0x8c + 8 * 3);
    *count = size / sizeof(RUNTIME_FUNCTION);
    return rva ? (RUNTIME_FUNCTION *)(base + rva) : 0;
}

static int is_function_start(BYTE *base, BYTE *addr)
{
    DWORD count;
    RUNTIME_FUNCTION *f = functions(base, &count);
    for (DWORD i = 0; f && i < count; i++)
        if (base + f[i].begin == addr) return 1;
    return 0;
}

static int bytes_at(const BYTE *code, const char *hex)
{
    for (; *hex; hex += hex[2] ? 3 : 2, code++) {
        int hi = hex[0] <= '9' ? hex[0] - '0' : hex[0] - 'a' + 10, lo = hex[1] <= '9' ? hex[1] - '0' : hex[1] - 'a' + 10;
        if (*code != (BYTE)(hi << 4 | lo)) return 0;
    }
    return 1;
}

enum { POLYDRAW16, POLYDRAW };

static const struct {
    const char *name;
    DWORD size;           /* of the function */
    const char *code;     /* at +0xcd: types = rec + n * (cpts - 1) + 0x1f + n */
    unsigned point;       /* bytes per point */
    DWORD emr;            /* the record type */
} handlers[] = {
    { "EMR_POLYDRAW16", 0x4d8, "8b 40 18 83 e8 01 03 c0 03 c0 83 c0 23 89 45 78", 4, 92 },
    { "EMR_POLYDRAW", 0x4ae, "8b 40 18 83 e8 01 03 c0 03 c0 03 c0 83 c0 27 89 45 78", 8, 56 },
};

static BYTE *find_handler(BYTE *base, int which)
{
    DWORD count;
    RUNTIME_FUNCTION *f = functions(base, &count);
    BYTE *found = 0;
    for (DWORD i = 0; f && i < count; i++) {
        if (f[i].end - f[i].begin != handlers[which].size || !bytes_at(base + f[i].begin + 0xcd, handlers[which].code))
            continue;
        if (found) return 0; /* not unique */
        found = base + f[i].begin;
    }
    return found;
}

/* ---- Running a copy of a handler, its calls sent to stubs -------------------- */

enum { BEGIN, XFORM_X, XFORM_Y, MOVETO, OFFSET, LINETO, CURVETO, STROKE, CALLEES };
static const char *const callee_names[] = { "begin", "x", "y", "MoveTo", "offset", "LineTo", "CurveTo", "stroke" };

struct call { int op; double v[6]; };
static struct call calls[256];
static volatile int ncalls;

static void log_call(int op, double a, double b, double c, double d, double e, double f)
{
    if (ncalls >= (int)(sizeof(calls) / sizeof(calls[0]))) return;
    calls[ncalls].op = op;
    calls[ncalls].v[0] = a, calls[ncalls].v[1] = b, calls[ncalls].v[2] = c;
    calls[ncalls].v[3] = d, calls[ncalls].v[4] = e, calls[ncalls].v[5] = f;
    ncalls++;
}

static BYTE canvas[16];

static void stub_begin(void *c) { if (c == canvas) log_call(BEGIN, 0, 0, 0, 0, 0, 0); }
static double stub_x(void *self, double v, BYTE flag) { (void)self, (void)flag; return v; }
static double stub_y(void *self, double v, BYTE flag) { (void)self, (void)flag; return v; }
static void stub_moveto(void *c, double x, double y) { if (c == canvas) log_call(MOVETO, x, y, 0, 0, 0, 0); }
static void stub_lineto(void *c, double x, double y) { if (c == canvas) log_call(LINETO, x, y, 0, 0, 0, 0); }
static void stub_curveto(void *c, double x1, double y1, double x2, double y2, double x3, double y3)
{
    if (c == canvas) log_call(CURVETO, x1, y1, x2, y2, x3, y3);
}
static BYTE *stub_offset(BYTE *rec, int n) { return rec + n; }
static void stub_stroke(void *self) { (void)self; log_call(STROKE, 0, 0, 0, 0, 0, 0); }

static void *const stubs[CALLEES] = {
    (void *)stub_begin, (void *)stub_x, (void *)stub_y, (void *)stub_moveto,
    (void *)stub_offset, (void *)stub_lineto, (void *)stub_curveto, (void *)stub_stroke,
};

typedef void (*handler_fn)(BYTE *self, BYTE *record);

/* A runnable copy of bytes, the code of the handler at `at` (in module base). Its calls go,
 * through jumps placed after it, to the stubs: the callees, in the order of their first call,
 * are always start a path, x, y, MoveTo, pointer into the record, LineTo, CurveTo, stroke. */
static handler_fn copy_handler(BYTE *base, BYTE *at, const BYTE *bytes, DWORD size)
{
    BYTE *copy = VirtualAlloc(0, size + CALLEES * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE *targets[CALLEES];
    int distinct = 0;

    if (!copy) return 0;
    memcpy(copy, bytes, size);
    for (DWORD i = 0; i + 5 <= size; i++) {
        BYTE *target;
        int k;
        if (bytes[i] != 0xe8) continue;
        target = at + i + 5 + *(const int *)(bytes + i + 1);
        if (target >= at && target < at + size) continue;
        if (!is_function_start(base, target)) continue;
        for (k = 0; k < distinct && targets[k] != target; k++);
        if (k == distinct) {
            if (distinct == CALLEES) return 0;
            targets[distinct++] = target;
        }
        *(int *)(copy + i + 1) = (int)((copy + size + 16 * k) - (copy + i + 5));
    }
    if (distinct != CALLEES) {
        out("    callees found: ");
        out_num(distinct);
        out("\n");
        return 0;
    }
    for (int k = 0; k < CALLEES; k++) {
        BYTE *thunk = copy + size + 16 * k;
        thunk[0] = 0xff, thunk[1] = 0x25; /* jmp [rip+0] */
        *(int *)(thunk + 2) = 0;
        memcpy(thunk + 6, &stubs[k], 8);
    }
    return (handler_fn)copy;
}

/* ---- Records and what they mean ----------------------------------------------- */

#define PT_CLOSEFIGURE 1
#define PT_LINETO 2
#define PT_BEZIERTO 4
#define PT_MOVETO 6

struct shape { const char *name; int count; int pts[13][2]; BYTE types[13]; };

static const struct shape shapes[] = {
    { "2-point line", 2, { { 863, 2280 }, { 863, 2220 } }, { 6, 2 } },
    { "closed triangle", 3, { { 10, 10 }, { 50, 10 }, { 30, 40 } }, { 6, 2, 3 } },
    { "closed quad", 4, { { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 } }, { 6, 2, 2, 3 } },
    { "circle", 13,
      { { 100, 50 }, { 100, 78 }, { 78, 100 }, { 50, 100 }, { 22, 100 }, { 0, 78 }, { 0, 50 },
        { 0, 22 }, { 22, 0 }, { 50, 0 }, { 78, 0 }, { 100, 22 }, { 100, 50 } },
      { 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5 } },
    { "two figures", 5, { { 1, 2 }, { 3, 4 }, { 5, 6 }, { 7, 8 }, { 9, 10 } }, { 6, 2, 6, 2, 2 } },
    { "line, then a closed Bezier", 5, { { 0, 0 }, { 10, 0 }, { 15, 5 }, { 15, 15 }, { 10, 20 } }, { 6, 2, 4, 4, 5 } },
    { "lone MoveTo", 1, { { 42, 43 } }, { 6 } },
};

#define START_X 7
#define START_Y 9

static int make_record(BYTE *rec, int which, const struct shape *s, int scale)
{
    unsigned point = handlers[which].point, size = (0x1c + (point + 1) * s->count + 3) & ~3u;
    memset(rec, 0, size);
    *(DWORD *)rec = handlers[which].emr;
    *(DWORD *)(rec + 4) = size;
    *(DWORD *)(rec + 0x18) = s->count;
    for (int i = 0; i < s->count; i++) {
        int x = s->pts[i][0] * scale, y = s->pts[i][1] * scale;
        if (point == 4) {
            *(short *)(rec + 0x1c + 4 * i) = (short)x;
            *(short *)(rec + 0x1e + 4 * i) = (short)y;
        } else {
            *(int *)(rec + 0x1c + 8 * i) = x;
            *(int *)(rec + 0x20 + 8 * i) = y;
        }
    }
    memcpy(rec + 0x1c + point * s->count, s->types, s->count);
    return size;
}

/* The calls PolyDraw means, as llPDFLib makes them: outside a path, start one and stroke it at
 * the end; a MoveTo to the current position first; a close as a LineTo to the figure's start. */
static void add(struct call *want, int *n, int op, double a, double b, double c, double d, double e, double f)
{
    struct call *w = &want[(*n)++];
    w->op = op;
    w->v[0] = a, w->v[1] = b, w->v[2] = c, w->v[3] = d, w->v[4] = e, w->v[5] = f;
}

static int expected_calls(const struct shape *s, int scale, int in_path, struct call *want, int *end_x, int *end_y)
{
    int n = 0, cx = START_X, cy = START_Y, sx = cx, sy = cy;
    int x[13], y[13];

    for (int i = 0; i < s->count; i++) x[i] = s->pts[i][0] * scale, y[i] = s->pts[i][1] * scale;
    if (!in_path) add(want, &n, BEGIN, 0, 0, 0, 0, 0, 0);
    add(want, &n, MOVETO, cx, cy, 0, 0, 0, 0);
    for (int i = 0; i < s->count;) {
        BYTE t = s->types[i];
        if (t == PT_MOVETO) {
            add(want, &n, MOVETO, x[i], y[i], 0, 0, 0, 0);
            sx = cx = x[i], sy = cy = y[i];
            i++;
        } else if (t & PT_LINETO) {
            add(want, &n, LINETO, x[i], y[i], 0, 0, 0, 0);
            cx = x[i], cy = y[i];
            i++;
            if (t & PT_CLOSEFIGURE) add(want, &n, LINETO, sx, sy, 0, 0, 0, 0), cx = sx, cy = sy;
        } else {
            add(want, &n, CURVETO, x[i], y[i], x[i + 1], y[i + 1], x[i + 2], y[i + 2]);
            cx = x[i + 2], cy = y[i + 2];
            i += 3;
            if (s->types[i - 1] & PT_CLOSEFIGURE) add(want, &n, LINETO, sx, sy, 0, 0, 0, 0), cx = sx, cy = sy;
        }
    }
    if (!in_path) add(want, &n, STROKE, 0, 0, 0, 0, 0, 0);
    *end_x = cx, *end_y = cy;
    return n;
}

/* The object the handlers work on: the canvas at +0x10, the current position at +0x50, "in a
 * path" at +0x11a. */
static __attribute__((aligned(16))) BYTE self[0x200];
static __attribute__((aligned(16))) BYTE record[0x200];

static void reset(int in_path)
{
    memset(self, 0, sizeof(self));
    *(BYTE **)(self + 0x10) = canvas;
    *(int *)(self + 0x50) = START_X;
    *(int *)(self + 0x54) = START_Y;
    self[0x11a] = (BYTE)in_path;
    ncalls = 0;
}

static handler_fn running;

static DWORD WINAPI run_thread(void *arg)
{
    (void)arg;
    running(self, record);
    return 0;
}

/* Runs the handler on the record in a thread; 1 if it returned within ms. */
static int run(handler_fn fn, DWORD ms)
{
    HANDLE thread;
    running = fn;
    thread = CreateThread(0, 0, run_thread, 0, 0, 0);
    if (!thread) return 0;
    if (WaitForSingleObject(thread, ms) == WAIT_TIMEOUT) {
        TerminateThread(thread, 1);
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

static void print_call(const struct call *c)
{
    int args = c->op == CURVETO ? 6 : c->op == MOVETO || c->op == LINETO ? 2 : 0;
    out(callee_names[c->op]);
    out("(");
    for (int i = 0; i < args; i++) {
        if (i) out(", ");
        out_num((long long)c->v[i]);
    }
    out(") ");
}

static int same_calls(const struct call *want, int n)
{
    if (ncalls != n) return 0;
    for (int i = 0; i < n; i++) {
        if (calls[i].op != want[i].op) return 0;
        for (int k = 0; k < 6; k++)
            if (calls[i].v[k] != want[i].v[k]) return 0;
    }
    return 1;
}

static void check_fixed(handler_fn fn, int which)
{
    /* EMR_POLYDRAW is what Wine records when a coordinate does not fit in a short. */
    int scale = handlers[which].point == 8 ? 1000 : 1;
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++)
        for (int in_path = 0; in_path < 2; in_path++) {
            struct call want[64];
            int end_x, end_y, n = expected_calls(&shapes[s], scale, in_path, want, &end_x, &end_y), ok;
            make_record(record, which, &shapes[s], scale);
            reset(in_path);
            ok = run(fn, 5000) && same_calls(want, n) && *(int *)(self + 0x50) == end_x
                 && *(int *)(self + 0x54) == end_y;
            out(ok ? "    ok   " : "    FAIL ");
            out(shapes[s].name);
            out(in_path ? ", in a path\n" : ", stroked\n");
            failures += !ok;
            if (!ok) {
                out("      want: ");
                for (int i = 0; i < n; i++) print_call(&want[i]);
                out("\n      got:  ");
                for (int i = 0; i < ncalls; i++) print_call(&calls[i]);
                out("\n      current position: ");
                out_num(*(int *)(self + 0x50));
                out(", ");
                out_num(*(int *)(self + 0x54));
                out("\n");
            }
        }
}

/* ---- Main ------------------------------------------------------------------- */

#define MAX_MODULES 4

static WCHAR paths[1024];

void start_entry(void)
{
    WCHAR proxy[512], *names[MAX_MODULES];
    BYTE *bases[MAX_MODULES], *code[MAX_MODULES][2], *before[MAX_MODULES][2];
    int modules = 0;

    if (!GetEnvironmentVariableW(L"POLYDRAW_TEST_MODULES", paths, 1024)
            || !GetEnvironmentVariableW(L"POLYDRAW_TEST_PROXY_WIN", proxy, 512)) {
        out("POLYDRAW_TEST_MODULES and POLYDRAW_TEST_PROXY_WIN must be set\nresult: FAIL\n");
        ExitProcess(1);
    }
    for (WCHAR *p = paths; *p && modules < MAX_MODULES;) {
        WCHAR *end = p;
        while (*end && *end != ';') end++;
        names[modules++] = p;
        if (!*end) break;
        *end = 0;
        p = end + 1;
    }

    /* As Altium's modules are on disk */
    for (int m = 0; m < modules; m++) {
        out_w(names[m]);
        out("\n");
        bases[m] = LoadLibraryExW(names[m], 0, DONT_RESOLVE_DLL_REFERENCES);
        check(bases[m] != 0, "mapped");
        if (!bases[m]) ExitProcess(1);
        for (int h = 0; h < 2; h++) {
            code[m][h] = find_handler(bases[m], h);
            out(code[m][h] ? "    ok   " : "    FAIL ");
            out(handlers[h].name);
            out(" handler");
            if (code[m][h]) out(" at +"), out_hex(code[m][h] - bases[m]);
            out("\n");
            if (!code[m][h]) ExitProcess(1);
            before[m][h] = VirtualAlloc(0, handlers[h].size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            memcpy(before[m][h], code[m][h], handlers[h].size);
        }
    }

    out("loading the proxy\n");
    check(LoadLibraryW(proxy) != 0, "proxy loaded");

    for (int m = 0; m < modules; m++) {
        out_w(names[m]);
        out("\n");
        for (int h = 0; h < 2; h++) {
            handler_fn original = copy_handler(bases[m], code[m][h], before[m][h], handlers[h].size);
            handler_fn fixed = copy_handler(bases[m], code[m][h], code[m][h], handlers[h].size);
            int changed = 0;

            out("  ");
            out(handlers[h].name);
            out("\n");
            for (DWORD i = 0; i < handlers[h].size; i++) changed += before[m][h][i] != code[m][h][i];
            out("    ");
            out_num(changed);
            out(" bytes changed by the proxy\n");
            check(changed > 0, "the proxy changed the handler");
            check(original && fixed, "its calls into llPDFLib identified");
            if (!original || !fixed) continue;

            make_record(record, h, &shapes[0], 1);
            reset(0);
            check(!run(original, 2000), "as it was: a 2-point line never returns (Altium's freeze)");
            check_fixed(fixed, h);
        }
    }
    out(failures ? "result: FAIL\n" : "result: PASS\n");
    ExitProcess(failures ? 1 : 0);
}
