/*
 * SHFolder.dll proxy for Altium Designer under Wine.
 *
 * X2.EXE imports SHFolder.dll at startup, so a copy of this DLL placed next to
 * X2.EXE is loaded early. Its exports forward to shell32 like Wine's own
 * shfolder. On load it works around six Wine gaps, in X2.EXE and in every DLL
 * loaded later (via LdrRegisterDllNotification), one VCL bug that shows up
 * under Wine (7), and a Wine-on-DXVK slowdown (8):
 *
 * 1. Missing user32 functions. Altium delay-loads a few newer user32 functions
 *    that Wine does not export (InheritWindowMonitor is still a stub in upstream
 *    Wine 11.19). The first call raises "External exception C06D007F", for
 *    example when the File menu or a project context menu opens. Their
 *    delay-import slots are pointed at small fallbacks, but only when the
 *    function is really missing, so a Wine that implements it takes over.
 *
 * 2. Raw RSA decryption. The Simbeor SDK (simbeor_sdk.dll, used by the PCB
 *    editor) checks its license with BCryptDecrypt(BCRYPT_PAD_NONE) on an
 *    imported RSA private key. Wine 11.0's GnuTLS bcrypt ignores PAD_NONE and
 *    applies PKCS#1 unpadding, which fails ("GnuTLS error: Decryption has
 *    failed"); Simbeor then throws and Altium dies with c0000409. For that DLL
 *    only, BCryptImportKeyPair/BCryptDecrypt/BCryptDestroyKey are wrapped: raw
 *    decryption with a remembered private key is done here (rsa_raw.c), every
 *    other call goes straight to Wine's bcrypt.
 *
 * 3. Schematic text through Wine's Direct2D. The schematic editor draws with
 *    Direct2D and builds its text in "world" units (a 10 pt label has an em size
 *    of ~900) that a zoom transform scales down. Wine's d2d1 picks the rendering
 *    mode from the untransformed em size, gets OUTLINE (>= 100) and fills every
 *    glyph outline as geometry, which Wine draws without antialiasing (strokes
 *    and underscores vanish) and triangulates on the CPU every frame. Once Wine's
 *    builtin dwrite.dll is loaded, two of its vtable slots are wrapped, for calls
 *    coming from Wine's builtin d2d1.dll only: GetRecommendedRenderingMode turns
 *    OUTLINE into NATURAL_SYMMETRIC (antialiased bitmaps), and
 *    CreateGlyphRunAnalysis folds a uniform scale into the em size, advances and
 *    origin, because dwrite bypasses its glyph cache for any transform.
 *
 * 4. Black areas around layered windows (the splash screen). For a layered
 *    window drawn with per-pixel alpha (UpdateLayeredWindow), Wine cuts the
 *    pixels with alpha 0 out of the X window with an X shape, which it sets
 *    before painting, so those pixels are never written. On Hyprland
 *    (XWayland) the shape is not applied, and they show whatever the window's
 *    buffer held, often opaque black. Wine's user32 passes
 *    UpdateLayeredWindow(Indirect) on to win32u's NtUserUpdateLayeredWindow,
 *    however the caller found it (the splash does not use X2.EXE's import of
 *    UpdateLayeredWindow); that import of user32 is wrapped and gets a copy of
 *    the bitmap in which alpha-0 pixels have alpha 1 (invisible), so Wine
 *    paints every pixel.
 *
 * 5. Layered child windows (the PCB editor's heads-up display). Windows 8 and
 *    later let a child window be layered; the HUD is one, inside the Direct3D
 *    PCB view. For UpdateLayeredWindow on a child, Wine 11 switches the window
 *    to an ARGB visual, which gives it its own top-level X window at its
 *    parent-relative position: the HUD shows in the corner of the screen, on
 *    every workspace, while its place in the view (clipped out of the view's
 *    Direct3D frames) keeps whatever was last drawn there. For a child, the
 *    same wrapper does not call Wine: it keeps the image composited over black
 *    and draws it into the window, which paints into the main window like any
 *    child, and redraws it whenever the window is repainted, moved or shown.
 *
 * 6. Arcs in Direct2D paths (schematic symbols such as inductors). Wine's d2d1 does not
 *    implement ID2D1GeometrySink::AddArc: it adds the end point without a segment to it, so
 *    the schematic's arcs (all but full circles) are not drawn. Once Wine's builtin d2d1.dll
 *    is loaded, and if its AddArc still leaves arcs out, that vtable slot is replaced: the arc
 *    goes in as quadratic curves (d2d_arc.c), from the current point that Wine keeps next to
 *    the sink.
 *
 * 7. "Control 'View…' has no parent window". A panel form can keep a removed view as its
 *    ActiveControl. VCL's DoNestedActivation, called whenever the form is activated or
 *    deactivated, then asks that view for its window handle, and creating a window for a
 *    control without a parent raises. The routine is found in X2.EXE by its code and
 *    replaced: for a control that is in no form any more, the form's stale pointers are
 *    cleared, as VCL does when a control is removed, instead of creating a window.
 *
 * 8. Slow schematic redraws. Wine's d2d1 swaps its own Direct3D state into the context
 *    before every shape it draws and the application's back after it, and in DXVK each swap
 *    copies and rebinds the whole context state. Once d2d1 has created a render target, the
 *    swap back and the next swap in are skipped while nothing else uses the context; the
 *    application's state goes back before any other caller uses the context, and at
 *    EndDraw. ALTIUM_KEEP_D2D_SWAPS=1 turns this off.
 *
 * Build: scripts/build.sh (clang + lld-link + llvm-dlltool; no Windows SDK or CRT needed).
 */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD; /* 32-bit; the msvc target is LLP64 */
typedef int BOOL;
typedef long NTSTATUS;
typedef unsigned long long ULONGLONG;
typedef unsigned long long SIZE_T;
typedef void *HANDLE;
typedef void *HMODULE;
typedef __WCHAR_TYPE__ WCHAR;

#define IMPORT __declspec(dllimport)
IMPORT HMODULE GetModuleHandleW(const WCHAR *name);
IMPORT void *GetProcAddress(HMODULE module, const char *name);
IMPORT BOOL VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
IMPORT void SetLastError(DWORD err);
IMPORT void OutputDebugStringA(const char *msg);
IMPORT HANDLE CreateThread(void *attrs, SIZE_T stack, DWORD (*start)(void *), void *arg, DWORD flags, DWORD *tid);
IMPORT NTSTATUS LdrRegisterDllNotification(DWORD flags, void *callback, void *context, void **cookie);
IMPORT HANDLE GetCurrentProcess(void);
IMPORT BOOL FlushInstructionCache(HANDLE process, const void *addr, SIZE_T size);
IMPORT DWORD GetCurrentThreadId(void);
IMPORT DWORD GetEnvironmentVariableA(const char *name, char *value, DWORD size);

#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define ERROR_NOT_SUPPORTED 50
#define DLL_PROCESS_ATTACH 1
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_ORDINAL_FLAG64 0x8000000000000000ULL
#define STATUS_SUCCESS 0
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define BCRYPT_PAD_NONE 0x00000001

/* There is no CRT; clang may lower struct/array initialisation to these. */
void *memset(void *dst, int c, SIZE_T n)
{
    volatile BYTE *p = dst;
    while (n--) *p++ = (BYTE)c;
    return dst;
}

void *memcpy(void *dst, const void *src, SIZE_T n)
{
    volatile BYTE *d = dst;
    const BYTE *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

#include "rsa_raw.c"

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static int wstreq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int streq_ci(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return !*a && !*b;
}

/* Case-insensitive match of a counted UTF-16 name against ASCII. */
static int wname_is(const WCHAR *name, unsigned len, const char *ascii)
{
    unsigned i = 0;
    for (; i < len && ascii[i]; i++) {
        WCHAR c = name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != (WCHAR)lower(ascii[i])) return 0;
    }
    return i == len && !ascii[i];
}

static void set_slot(ULONGLONG *slot, void *value)
{
    DWORD old;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        *slot = (ULONGLONG)value;
        VirtualProtect(slot, sizeof(*slot), old, &old);
    }
}

/* ---- 1. Missing user32 functions -------------------------------------- */

/* Real Windows makes hwnd use hwndInherit's monitor for DPI; Wine has no per-monitor DPI to inherit. */
static BOOL InheritWindowMonitor_fallback(HANDLE hwnd, HANDLE inherit)
{
    (void)hwnd; (void)inherit;
    return 1;
}

/* Pen/touch pointer queries: report "no pointer data", as on a machine without that input. */
static BOOL pointer_query_fallback(void)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

/* The x64 calling convention makes extra/unused args harmless. */
static const struct { const char *name; void *fallback; } user32_fallbacks[] = {
    { "InheritWindowMonitor", (void *)InheritWindowMonitor_fallback },
    { "GetPointerInputTransform", (void *)pointer_query_fallback },
    { "GetPointerFrameInfoHistory", (void *)pointer_query_fallback },
};

/* ---- 2. Raw RSA decryption for simbeor_sdk.dll ------------------------- */

typedef NTSTATUS (*import_key_pair_fn)(HANDLE alg, HANDLE import_key, const WCHAR *type, HANDLE *key,
                                       BYTE *input, DWORD input_len, DWORD flags);
typedef NTSTATUS (*decrypt_fn)(HANDLE key, BYTE *input, DWORD input_len, void *padding, BYTE *iv, DWORD iv_len,
                               BYTE *output, DWORD output_len, DWORD *result_len, DWORD flags);
typedef NTSTATUS (*destroy_key_fn)(HANDLE key);

static import_key_pair_fn real_import_key_pair;
static decrypt_fn real_decrypt;
static destroy_key_fn real_destroy_key;

#define RSA_KEY_SLOTS 16
#define RSA_BLOB_MAX (RSA_BLOB_HEADER + 4 + 3 * RSA_MAX_BYTES)
static struct { HANDLE key; unsigned len; BYTE blob[RSA_BLOB_MAX]; } rsa_keys[RSA_KEY_SLOTS];
static volatile long rsa_keys_lock;

static void lock(void) { while (__sync_lock_test_and_set(&rsa_keys_lock, 1)) ; }
static void unlock(void) { __sync_lock_release(&rsa_keys_lock); }

static NTSTATUS import_key_pair_hook(HANDLE alg, HANDLE import_key, const WCHAR *type, HANDLE *key,
                                     BYTE *input, DWORD input_len, DWORD flags)
{
    NTSTATUS status = real_import_key_pair(alg, import_key, type, key, input, input_len, flags);
    unsigned need;

    if (status || !key || !type || !input) return status;
    if (!wstreq(type, L"RSAPRIVATEBLOB") && !wstreq(type, L"RSAFULLPRIVATEBLOB")) return status;
    if (!(need = rsa_blob_needed(input, input_len))) return status;

    lock();
    for (int i = 0; i < RSA_KEY_SLOTS; i++) {
        if (rsa_keys[i].key) continue;
        rsa_keys[i].key = *key;
        rsa_keys[i].len = need;
        memcpy(rsa_keys[i].blob, input, need);
        break;
    }
    unlock();
    return status;
}

static NTSTATUS decrypt_hook(HANDLE key, BYTE *input, DWORD input_len, void *padding, BYTE *iv, DWORD iv_len,
                             BYTE *output, DWORD output_len, DWORD *result_len, DWORD flags)
{
    static int logged;
    BYTE blob[RSA_BLOB_MAX];
    int found = 0;
    unsigned size;

    if (flags == BCRYPT_PAD_NONE && key && input && result_len) {
        lock();
        for (int i = 0; i < RSA_KEY_SLOTS; i++) {
            if (rsa_keys[i].key != key) continue;
            memcpy(blob, rsa_keys[i].blob, rsa_keys[i].len);
            found = 1;
            break;
        }
        unlock();
    }
    if (!found) return real_decrypt(key, input, input_len, padding, iv, iv_len, output, output_len, result_len, flags);

    size = rsa_blob_modulus_bytes(blob);
    *result_len = size;
    if (!output) return STATUS_SUCCESS;
    if (output_len < size) return STATUS_BUFFER_TOO_SMALL;
    if (!rsa_raw_decrypt(blob, input, input_len, output))
        return real_decrypt(key, input, input_len, padding, iv, iv_len, output, output_len, result_len, flags);
    if (!logged) {
        logged = 1;
        OutputDebugStringA("shfolderproxy: raw RSA BCryptDecrypt(BCRYPT_PAD_NONE) handled for simbeor_sdk.dll\n");
    }
    return STATUS_SUCCESS;
}

static NTSTATUS destroy_key_hook(HANDLE key)
{
    lock();
    for (int i = 0; i < RSA_KEY_SLOTS; i++)
        if (rsa_keys[i].key == key) rsa_keys[i].key = 0;
    unlock();
    return real_destroy_key(key);
}

struct hook { const char *name; void *hook; void **real; };

static const struct hook bcrypt_hooks[] = {
    { "BCryptImportKeyPair", (void *)import_key_pair_hook, (void **)&real_import_key_pair },
    { "BCryptDecrypt", (void *)decrypt_hook, (void **)&real_decrypt },
    { "BCryptDestroyKey", (void *)destroy_key_hook, (void **)&real_destroy_key },
};

/* ---- 3. Direct2D text in the schematic editor -------------------------- */

typedef long HRESULT;
typedef struct { DWORD d1; WORD d2, d3; BYTE d4[8]; } GUID;
typedef struct { float m11, m12, m21, m22, dx, dy; } DWRITE_MATRIX;
typedef struct { float advance_offset, ascender_offset; } DWRITE_GLYPH_OFFSET;
typedef struct {
    void *font_face;
    float em_size;
    DWORD count;
    const WORD *indices;
    const float *advances;
    const DWRITE_GLYPH_OFFSET *offsets;
    BOOL sideways;
    DWORD bidi_level;
} DWRITE_GLYPH_RUN;

int _fltused; /* float code without a CRT */

#define DWRITE_MEASURING_MODE_NATURAL 0
#define DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC 5
#define DWRITE_RENDERING_MODE_OUTLINE 6
#define GLYPHS_MAX 256 /* longer runs keep their transform (stack arrays below stay under a page) */

/* COM vtable slots (fixed by the interfaces, the same in every Wine version) */
#define SLOT_RELEASE 2
#define SLOT_FACTORY_GET_SYSTEM_FONT_COLLECTION 3
#define SLOT_FACTORY2_CREATE_GLYPH_RUN_ANALYSIS 30
#define SLOT_COLLECTION_GET_FONT_FAMILY 4
#define SLOT_FAMILY_GET_FONT 5
#define SLOT_FONT_CREATE_FONT_FACE 13
#define SLOT_FONT_FACE_GET_RECOMMENDED_RENDERING_MODE 15

static const GUID IID_IDWriteFactory2 = { 0x0439fc60, 0xca44, 0x4994, { 0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec } };

typedef HRESULT (*recommend_fn)(void *face, float em_size, float pixels_per_dip, int measuring_mode,
                                void *params, int *mode);
typedef HRESULT (*analysis_fn)(void *factory, const DWRITE_GLYPH_RUN *run, const DWRITE_MATRIX *m,
                               int rendering_mode, int measuring_mode, int grid_fit_mode, int aa_mode,
                               float x, float y, void **analysis);

static recommend_fn real_recommend;
static analysis_fn real_analysis;

/* Wine marks its own PE DLLs with this string after the DOS header. */
static int is_wine_builtin(const BYTE *base)
{
    static const char sig[] = "Wine builtin DLL";
    if (!base || base[0] != 'M' || base[1] != 'Z') return 0;
    for (unsigned i = 0; i < sizeof(sig) - 1; i++)
        if (base[0x40 + i] != (BYTE)sig[i]) return 0;
    return 1;
}

static DWORD image_size(const BYTE *base)
{
    return *(const DWORD *)(base + *(const DWORD *)(base + 0x3c) + 0x50); /* SizeOfImage */
}

static int in_image(const BYTE *base, const void *addr)
{
    return base && (const BYTE *)addr >= base && (const BYTE *)addr < base + image_size(base);
}

/* Only Wine's d2d1 makes the em-size mistake; leave every other caller alone. */
static int from_wine_d2d1(const void *return_address)
{
    static const BYTE *d2d1;
    if (!d2d1) {
        const BYTE *base = GetModuleHandleW(L"d2d1.dll");
        if (!is_wine_builtin(base)) return 0;
        d2d1 = base;
    }
    return in_image(d2d1, return_address);
}

static HRESULT recommend_hook(void *face, float em_size, float pixels_per_dip, int measuring_mode,
                              void *params, int *mode)
{
    HRESULT hr = real_recommend(face, em_size, pixels_per_dip, measuring_mode, params, mode);
    if (hr >= 0 && *mode == DWRITE_RENDERING_MODE_OUTLINE && from_wine_d2d1(__builtin_return_address(0)))
        *mode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    return hr;
}

/* A run drawn through m = scale s plus translation lands on the same pixels as the run with
 * em size, advances and offsets times s, the origin mapped through m, and no transform
 * (glyphs are then hinted at their on-screen size). With GDI measuring modes dwrite rounds
 * default advances through the transform, so only NATURAL runs are folded. */
static HRESULT analysis_hook(void *factory, const DWRITE_GLYPH_RUN *run, const DWRITE_MATRIX *m,
                             int rendering_mode, int measuring_mode, int grid_fit_mode, int aa_mode,
                             float x, float y, void **analysis)
{
    float advances[GLYPHS_MAX];
    DWRITE_GLYPH_OFFSET offsets[GLYPHS_MAX];
    DWRITE_GLYPH_RUN scaled;
    float s;

    if (!m || !run || !run->count || run->count > GLYPHS_MAX || measuring_mode != DWRITE_MEASURING_MODE_NATURAL
            || m->m12 != 0.0f || m->m21 != 0.0f
            || m->m11 != m->m22 || m->m11 <= 0.0f || m->m11 == 1.0f || !from_wine_d2d1(__builtin_return_address(0)))
        return real_analysis(factory, run, m, rendering_mode, measuring_mode, grid_fit_mode, aa_mode, x, y, analysis);

    s = m->m11;
    scaled = *run;
    scaled.em_size = run->em_size * s;
    if (run->advances) {
        for (DWORD i = 0; i < run->count; i++) advances[i] = run->advances[i] * s;
        scaled.advances = advances;
    }
    if (run->offsets) {
        for (DWORD i = 0; i < run->count; i++) {
            offsets[i].advance_offset = run->offsets[i].advance_offset * s;
            offsets[i].ascender_offset = run->offsets[i].ascender_offset * s;
        }
        scaled.offsets = offsets;
    }
    /* dwrite reads advances and offsets only while it creates the analysis */
    return real_analysis(factory, &scaled, 0, rendering_mode, measuring_mode, grid_fit_mode, aa_mode,
                         x * s + m->dx, y * s + m->dy, analysis);
}

#define VTBL(obj) (*(void ***)(obj))
typedef HRESULT (*create_factory_fn)(int type, const GUID *iid, void **factory);
typedef HRESULT (*get_collection_fn)(void *factory, void **collection, BOOL check_updates);
typedef HRESULT (*get_indexed_fn)(void *obj, DWORD index, void **out);
typedef HRESULT (*create_face_fn)(void *font, void **face);
typedef DWORD (*release_fn)(void *obj);

static void release(void *obj)
{
    if (obj) ((release_fn)VTBL(obj)[SLOT_RELEASE])(obj);
}

static void wrap_slot(const BYTE *module, void **slot, void *hook, void **real)
{
    if (*slot == hook || !in_image(module, *slot)) return; /* already wrapped, or not Wine's layout */
    *real = *slot;
    __sync_synchronize(); /* the real pointer is visible before the slot changes */
    set_slot((ULONGLONG *)slot, hook);
}

/* Runs on its own thread: dwrite cannot be used from the load notification (loader lock,
 * and its DllMain has not run yet); the thread starts once the load has finished. */
static DWORD patch_dwrite(void *module)
{
    const BYTE *dwrite = module;
    create_factory_fn create = (create_factory_fn)GetProcAddress(module, "DWriteCreateFactory");
    void *factory = 0, *collection = 0, *family = 0, *font = 0, *face = 0;

    if (!create || create(0 /* SHARED */, &IID_IDWriteFactory2, &factory) < 0) return 1;
    wrap_slot(dwrite, &VTBL(factory)[SLOT_FACTORY2_CREATE_GLYPH_RUN_ANALYSIS], (void *)analysis_hook, (void **)&real_analysis);

    /* The font-face vtable is shared by every font face; any face gives access to it. */
    if (((get_collection_fn)VTBL(factory)[SLOT_FACTORY_GET_SYSTEM_FONT_COLLECTION])(factory, &collection, 0) >= 0
            && ((get_indexed_fn)VTBL(collection)[SLOT_COLLECTION_GET_FONT_FAMILY])(collection, 0, &family) >= 0
            && ((get_indexed_fn)VTBL(family)[SLOT_FAMILY_GET_FONT])(family, 0, &font) >= 0
            && ((create_face_fn)VTBL(font)[SLOT_FONT_CREATE_FONT_FACE])(font, &face) >= 0)
        wrap_slot(dwrite, &VTBL(face)[SLOT_FONT_FACE_GET_RECOMMENDED_RENDERING_MODE], (void *)recommend_hook, (void **)&real_recommend);

    release(face);
    release(font);
    release(family);
    release(collection);
    release(factory);
    if (real_analysis && real_recommend)
        OutputDebugStringA("shfolderproxy: Direct2D text hooks installed in dwrite.dll\n");
    return 0;
}

static void dwrite_loaded(BYTE *base)
{
    static volatile long started;
    if (!is_wine_builtin(base) || __sync_lock_test_and_set(&started, 1)) return;
    CreateThread(0, 0, patch_dwrite, base, 0, 0);
}

/* ---- 4. Fully transparent pixels of layered windows --------------------- */

typedef void *HWND;
typedef void *HDC;
typedef void *HGDIOBJ;
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
    int type, width, height, width_bytes; /* BITMAP */
    WORD planes, bits_pixel;
    BYTE *bits;
    BITMAPINFOHEADER header;
    DWORD bitfields[3];
    HANDLE section;
    DWORD offset;
} DIBSECTION;
_Static_assert(sizeof(DIBSECTION) == 104, "DIBSECTION layout");

IMPORT HGDIOBJ GetCurrentObject(HDC hdc, unsigned type);
IMPORT int GetObjectW(HGDIOBJ obj, int size, void *buffer);
IMPORT HGDIOBJ CreateDIBSection(HDC hdc, const void *info, unsigned usage, void **bits, HANDLE section, DWORD offset);
IMPORT HDC CreateCompatibleDC(HDC hdc);
IMPORT HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj);
IMPORT BOOL DeleteDC(HDC hdc);
IMPORT BOOL DeleteObject(HGDIOBJ obj);
IMPORT BOOL GdiFlush(void);

#define ULW_ALPHA 2
#define AC_SRC_ALPHA 1
#define OBJ_BITMAP 7
#define BI_RGB 0
#define DIB_RGB_COLORS 0

/* win32u's NtUserUpdateLayeredWindow: UpdateLayeredWindow plus a dirty rectangle */
typedef BOOL (*update_layered_fn)(HWND hwnd, HDC dst, const POINT *dst_pos, const SIZE *size, HDC src,
                                  const POINT *src_pos, DWORD key, const BLENDFUNCTION *blend, DWORD flags,
                                  const RECT *dirty);

static update_layered_fn real_update_layered;

/* Row y (from the top) of a 32-bit DIB section. */
static const DWORD *dib_row(const DIBSECTION *dib, int y)
{
    if (dib->header.height > 0) y = dib->header.height - 1 - y; /* bottom-up */
    return (const DWORD *)(dib->bits + (SIZE_T)y * dib->width_bytes);
}

/* For per-pixel alpha from a 32-bit DIB section, where alpha 0 means invisible: a top-down copy
 * of the source area with alpha 1 (and no colour) in place of alpha 0. 0 if there is none to
 * replace, or for any other source. */
static HGDIOBJ copy_without_transparent_pixels(HDC src, const POINT *src_pos, const SIZE *size,
                                               const BLENDFUNCTION *blend, DWORD flags)
{
    BITMAPINFOHEADER header = { .size = sizeof(header), .planes = 1, .bit_count = 32, .compression = BI_RGB };
    int x, y, sx = src_pos ? src_pos->x : 0, sy = src_pos ? src_pos->y : 0, height, transparent = 0;
    DIBSECTION dib;
    HGDIOBJ copy;
    DWORD *bits;

    if (!src || !size || !blend || !(flags & ULW_ALPHA) || !(blend->format & AC_SRC_ALPHA)
            || GetObjectW(GetCurrentObject(src, OBJ_BITMAP), sizeof(dib), &dib) != sizeof(dib)
            || !dib.bits || dib.header.bit_count != 32 || dib.header.compression != BI_RGB)
        return 0;
    height = dib.header.height < 0 ? -dib.header.height : dib.header.height;
    if (sx < 0 || sy < 0 || size->cx <= 0 || size->cy <= 0
            || size->cx > dib.header.width - sx || size->cy > height - sy)
        return 0;

    GdiFlush();
    for (y = 0; y < size->cy && !transparent; y++) {
        const DWORD *row = dib_row(&dib, sy + y) + sx;
        for (x = 0; x < size->cx && !transparent; x++) transparent = !(row[x] >> 24);
    }
    header.width = size->cx;
    header.height = -size->cy;
    if (!transparent || !(copy = CreateDIBSection(0, &header, DIB_RGB_COLORS, (void **)&bits, 0, 0))) return 0;

    for (y = 0; y < size->cy; y++) {
        const DWORD *row = dib_row(&dib, sy + y) + sx;
        for (x = 0; x < size->cx; x++) *bits++ = row[x] >> 24 ? row[x] : 0x01000000;
    }
    return copy;
}

/* ---- 5. Layered child windows ------------------------------------------- */

typedef long long LONG_PTR;
typedef unsigned long long WPARAM;
typedef long long LPARAM;
typedef long long LRESULT;
typedef LRESULT (*WNDPROC)(HWND hwnd, unsigned msg, WPARAM wp, LPARAM lp);

/* user32 is resolved at run time (user32_ready) rather than imported: in testing, both runs of
 * a build that imported it lost a thread's wineserver connection and then hung on exit, and
 * the runs without the import did not (docs/known-issues.md). Called through these pointers
 * under their usual names. */
static LONG_PTR (*GetWindowLongPtrW)(HWND hwnd, int index);
static LONG_PTR (*SetWindowLongPtrW)(HWND hwnd, int index, LONG_PTR value);
static LRESULT (*CallWindowProcW)(WNDPROC proc, HWND hwnd, unsigned msg, WPARAM wp, LPARAM lp);
static BOOL (*GetLayeredWindowAttributes)(HWND hwnd, DWORD *key, BYTE *alpha, DWORD *flags);
static HWND (*GetAncestor)(HWND hwnd, unsigned flags);
static BOOL (*GetWindowRect)(HWND hwnd, RECT *rect);
static int (*MapWindowPoints)(HWND from, HWND to, POINT *points, unsigned count);
static BOOL (*SetWindowPos)(HWND hwnd, HWND after, int x, int y, int cx, int cy, unsigned flags);
static BOOL (*IsWindowVisible)(HWND hwnd);
static BOOL (*RedrawWindow)(HWND hwnd, const RECT *rect, void *region, unsigned flags);
static HDC (*GetWindowDC)(HWND hwnd);
static int (*ReleaseDC)(HWND hwnd, HDC hdc);

static int user32_ready(void)
{
    static const struct { const char *name; void **fn; } fns[] = {
        { "GetWindowLongPtrW", (void **)&GetWindowLongPtrW },
        { "SetWindowLongPtrW", (void **)&SetWindowLongPtrW },
        { "CallWindowProcW", (void **)&CallWindowProcW },
        { "GetLayeredWindowAttributes", (void **)&GetLayeredWindowAttributes },
        { "GetAncestor", (void **)&GetAncestor },
        { "GetWindowRect", (void **)&GetWindowRect },
        { "MapWindowPoints", (void **)&MapWindowPoints },
        { "SetWindowPos", (void **)&SetWindowPos },
        { "IsWindowVisible", (void **)&IsWindowVisible },
        { "RedrawWindow", (void **)&RedrawWindow },
        { "GetWindowDC", (void **)&GetWindowDC },
        { "ReleaseDC", (void **)&ReleaseDC },
    };
    static volatile int ready = -1;
    HMODULE user32;
    int ok;

    if (ready >= 0) return ready;
    ok = !!(user32 = GetModuleHandleW(L"user32.dll"));
    for (unsigned i = 0; ok && i < sizeof(fns) / sizeof(fns[0]); i++) ok = !!(*fns[i].fn = GetProcAddress(user32, fns[i].name));
    return ready = ok;
}

IMPORT BOOL BitBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int sx, int sy, DWORD rop);
IMPORT BOOL PatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop);
IMPORT BOOL GdiAlphaBlend(HDC dst, int x, int y, int cx, int cy, HDC src, int sx, int sy, int scx, int scy,
                          BLENDFUNCTION blend);
IMPORT BOOL GdiTransparentBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int sx, int sy, int scx, int scy,
                              DWORD key);

#define GWLP_WNDPROC (-4)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_CHILD 0x40000000
#define WS_EX_LAYERED 0x00080000
#define GA_PARENT 1
#define ULW_COLORKEY 1
#define ULW_OPAQUE 4
#define ULW_EX_NORESIZE 8
#define SWP_NOSIZE 0x01
#define SWP_NOMOVE 0x02
#define SWP_NOZORDER 0x04
#define SWP_NOACTIVATE 0x10
#define SWP_SHOWWINDOW 0x40
#define SWP_HIDEWINDOW 0x80
#define RDW_INVALIDATE 0x01
#define WM_PAINT 0x0f
#define WM_ERASEBKGND 0x14
#define WM_WINDOWPOSCHANGED 0x47
#define WM_NCDESTROY 0x82
#define SRCCOPY 0x00cc0020
#define BLACKNESS 0x00000042
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INCORRECT_SIZE 1462

typedef struct { HWND hwnd, after; int x, y, cx, cy; unsigned flags; } WINDOWPOS;

/* What a layered child shows: its last UpdateLayeredWindow image, over black. Windows
 * composites it over its parent; here the parent is a Direct3D view that is drawn on the X
 * server and cannot be read back, and the HUD is mostly opaque (alpha 217) and dark, so
 * black is close to what Windows shows over the PCB's dark background. */
struct layered_child {
    HWND hwnd;
    HGDIOBJ bitmap; /* 32-bit DIB section, width x height */
    int width, height;
    WNDPROC old_proc;
    HWND shown_parent; /* where it was last drawn, in that parent's client coordinates */
    RECT shown;
};

#define LAYERED_CHILDREN_MAX 16
static struct layered_child layered_children[LAYERED_CHILDREN_MAX];
static volatile long layered_children_lock;

/* The entry for hwnd; with create, a free one if it has none (0 if full). */
static struct layered_child *find_layered_child(HWND hwnd, int create)
{
    struct layered_child *found = 0, *unused = 0;
    while (__sync_lock_test_and_set(&layered_children_lock, 1)) ;
    for (int i = 0; i < LAYERED_CHILDREN_MAX && !found; i++) {
        if (layered_children[i].hwnd == hwnd) found = &layered_children[i];
        else if (!layered_children[i].hwnd && !unused) unused = &layered_children[i];
    }
    if (!found && create && unused) {
        unused->hwnd = hwnd;
        found = unused;
    }
    __sync_lock_release(&layered_children_lock);
    return found;
}

/* The window rectangle in its parent's client coordinates. */
static BOOL child_rect(HWND hwnd, HWND parent, RECT *rect)
{
    if (!GetWindowRect(hwnd, rect)) return 0;
    MapWindowPoints(0, parent, (POINT *)rect, 2);
    return 1;
}

/* The parent clips its children out of what it draws, so where the child no longer is, the
 * parent shows whatever was drawn there last (the parent is not repainted for these windows):
 * have it repaint the area the child was last drawn in, unless the child is still there. */
static void uncover_layered_child(struct layered_child *child)
{
    RECT now;

    if (!child->shown_parent) return;
    if (IsWindowVisible(child->hwnd) && GetAncestor(child->hwnd, GA_PARENT) == child->shown_parent
            && child_rect(child->hwnd, child->shown_parent, &now) && now.left == child->shown.left
            && now.top == child->shown.top && now.right == child->shown.right && now.bottom == child->shown.bottom)
        return;
    RedrawWindow(child->shown_parent, &child->shown, 0, RDW_INVALIDATE);
    child->shown_parent = 0;
}

static void draw_layered_child(struct layered_child *child)
{
    HWND parent = GetAncestor(child->hwnd, GA_PARENT);
    HDC hdc, mem;
    HGDIOBJ old;

    uncover_layered_child(child);
    if (!child->bitmap || !IsWindowVisible(child->hwnd) || !(hdc = GetWindowDC(child->hwnd))) return;
    if ((mem = CreateCompatibleDC(hdc))) {
        old = SelectObject(mem, child->bitmap);
        BitBlt(hdc, 0, 0, child->width, child->height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    ReleaseDC(child->hwnd, hdc);
    if (child_rect(child->hwnd, parent, &child->shown)) child->shown_parent = parent;
}

/* Windows does not send WM_PAINT to a window drawn with UpdateLayeredWindow; Wine does when the
 * window is invalidated, and VCL then fills it with the form's colour. Let the form paint, then
 * draw the image again. Hiding, moving and resizing these windows repaints nothing, neither the
 * window nor its parent. */
static LRESULT layered_child_proc(HWND hwnd, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct layered_child *child = find_layered_child(hwnd, 0);
    WNDPROC old_proc;
    LRESULT ret;

    if (!child) return 0; /* not reached: the entry lives until WM_NCDESTROY */
    old_proc = child->old_proc;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ret = CallWindowProcW(old_proc, hwnd, msg, wp, lp);
        draw_layered_child(child);
        return ret;
    case WM_WINDOWPOSCHANGED:
        ret = CallWindowProcW(old_proc, hwnd, msg, wp, lp);
        if (((const WINDOWPOS *)lp)->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)
                || (~((const WINDOWPOS *)lp)->flags & (SWP_NOMOVE | SWP_NOSIZE)))
            draw_layered_child(child);
        return ret;
    case WM_NCDESTROY:
        uncover_layered_child(child);
        if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == (LONG_PTR)layered_child_proc)
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
        if (child->bitmap) DeleteObject(child->bitmap);
        child->bitmap = 0;
        child->hwnd = 0;
        return CallWindowProcW(old_proc, hwnd, msg, wp, lp);
    }
    return CallWindowProcW(old_proc, hwnd, msg, wp, lp);
}

/* Composites the source over black into the child's image (within dirty, if given). */
static void compose_layered_child(struct layered_child *child, HDC src, const POINT *src_pos, DWORD key,
                                  const BLENDFUNCTION *blend, DWORD flags, const RECT *dirty)
{
    RECT r = { 0, 0, child->width, child->height };
    int sx = src_pos ? src_pos->x : 0, sy = src_pos ? src_pos->y : 0, cx, cy;
    HGDIOBJ old;
    HDC mem;

    if (dirty) {
        if (dirty->left > r.left) r.left = dirty->left;
        if (dirty->top > r.top) r.top = dirty->top;
        if (dirty->right < r.right) r.right = dirty->right;
        if (dirty->bottom < r.bottom) r.bottom = dirty->bottom;
    }
    if ((cx = r.right - r.left) <= 0 || (cy = r.bottom - r.top) <= 0 || !(mem = CreateCompatibleDC(src))) return;
    old = SelectObject(mem, child->bitmap);
    PatBlt(mem, r.left, r.top, cx, cy, BLACKNESS);
    if ((flags & ULW_ALPHA) && blend)
        GdiAlphaBlend(mem, r.left, r.top, cx, cy, src, sx + r.left, sy + r.top, cx, cy, *blend);
    else if (flags & ULW_COLORKEY)
        GdiTransparentBlt(mem, r.left, r.top, cx, cy, src, sx + r.left, sy + r.top, cx, cy, key);
    else
        BitBlt(mem, r.left, r.top, cx, cy, src, sx + r.left, sy + r.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

/* UpdateLayeredWindow for a child window: what win32u does (position, size, image), but drawn
 * into the window instead of a layered surface. Returns -1 to leave the call to Wine. */
static int update_layered_child(HWND hwnd, const POINT *dst_pos, const SIZE *size, HDC src,
                                const POINT *src_pos, DWORD key, const BLENDFUNCTION *blend, DWORD flags,
                                const RECT *dirty)
{
    BITMAPINFOHEADER header = { .size = sizeof(header), .planes = 1, .bit_count = 32, .compression = BI_RGB };
    struct layered_child *child;
    unsigned swp = SWP_NOZORDER | SWP_NOACTIVATE;
    int width, height;
    static int logged;
    void *bits;
    RECT rect;

    if (!user32_ready() || !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)
            || !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)
            || (flags & ~(ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE | ULW_EX_NORESIZE))
            || GetLayeredWindowAttributes(hwnd, 0, 0, 0) /* uses SetLayeredWindowAttributes: Wine fails it */
            || !child_rect(hwnd, GetAncestor(hwnd, GA_PARENT), &rect))
        return -1;
    width = size ? size->cx : rect.right - rect.left;
    height = size ? size->cy : rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ULW_EX_NORESIZE) && (width != rect.right - rect.left || height != rect.bottom - rect.top)) {
        SetLastError(ERROR_INCORRECT_SIZE);
        return 0;
    }
    if (!(child = find_layered_child(hwnd, 1))) return -1;

    if (!child->old_proc) {
        child->old_proc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)layered_child_proc);
    }
    if (!child->bitmap || child->width != width || child->height != height) {
        header.width = width;
        header.height = -height;
        if (child->bitmap) DeleteObject(child->bitmap);
        child->bitmap = CreateDIBSection(0, &header, DIB_RGB_COLORS, &bits, 0, 0); /* zeroed: black */
        child->width = width;
        child->height = height;
    }
    if (src && child->bitmap) compose_layered_child(child, src, src_pos, key, blend, flags, dirty);

    if (!dst_pos || (dst_pos->x == rect.left && dst_pos->y == rect.top)) swp |= SWP_NOMOVE;
    if (width == rect.right - rect.left && height == rect.bottom - rect.top) swp |= SWP_NOSIZE;
    if ((swp & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
        SetWindowPos(hwnd, 0, dst_pos ? dst_pos->x : rect.left, dst_pos ? dst_pos->y : rect.top, width, height, swp);
    draw_layered_child(child);
    if (!logged) {
        logged = 1;
        OutputDebugStringA("shfolderproxy: layered child window drawn into its parent\n");
    }
    return 1;
}

/* ---- The UpdateLayeredWindow hook (4 and 5) ------------------------------ */

static BOOL update_layered_hook(HWND hwnd, HDC dst, const POINT *dst_pos, const SIZE *size, HDC src,
                                const POINT *src_pos, DWORD key, const BLENDFUNCTION *blend, DWORD flags,
                                const RECT *dirty)
{
    static const POINT origin = { 0, 0 };
    static int logged;
    int child = update_layered_child(hwnd, dst_pos, size, src, src_pos, key, blend, flags, dirty);
    HGDIOBJ copy, old;
    HDC hdc;
    BOOL ret;

    if (child >= 0) return child;
    copy = copy_without_transparent_pixels(src, src_pos, size, blend, flags);
    hdc = copy ? CreateCompatibleDC(src) : 0;
    if (!hdc) {
        if (copy) DeleteObject(copy);
        return real_update_layered(hwnd, dst, dst_pos, size, src, src_pos, key, blend, flags, dirty);
    }
    old = SelectObject(hdc, copy);
    ret = real_update_layered(hwnd, dst, dst_pos, size, hdc, &origin, key, blend, flags, dirty);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    DeleteObject(copy);
    if (!logged) {
        logged = 1;
        OutputDebugStringA("shfolderproxy: layered window drawn without fully transparent pixels\n");
    }
    return ret;
}

/* Imports of Wine's builtin user32.dll */
static const struct hook win32u_hooks[] = {
    { "NtUserUpdateLayeredWindow", (void *)update_layered_hook, (void **)&real_update_layered },
};

/* ---- 6. Arcs in Direct2D paths ------------------------------------------- */

IMPORT double sin(double x);
IMPORT double cos(double x);
IMPORT double atan2(double y, double x);
IMPORT double sqrt(double x);

#include "d2d_arc.c"

typedef struct {
    arc_point point;
    float width, height; /* D2D1_SIZE_F */
    float rotation;
    int sweep_direction, arc_size;
} D2D1_ARC_SEGMENT;
typedef struct { float left, top, right, bottom; } D2D1_RECT_F;

#define D2D1_SWEEP_DIRECTION_CLOCKWISE 1
#define D2D1_ARC_SIZE_LARGE 1
#define D2D1_FIGURE_BEGIN_FILLED 0
#define D2D1_FIGURE_END_CLOSED 1

#define SLOT_D2D_FACTORY_CREATE_PATH_GEOMETRY 10
#define SLOT_GEOMETRY_GET_BOUNDS 4
#define SLOT_PATH_GEOMETRY_OPEN 17
#define SLOT_SINK_BEGIN_FIGURE 5
#define SLOT_SINK_END_FIGURE 8
#define SLOT_SINK_CLOSE 9
#define SLOT_SINK_ADD_LINE 10
#define SLOT_SINK_ADD_QUADRATIC_BEZIERS 13
#define SLOT_SINK_ADD_ARC 14

static const GUID IID_ID2D1Factory = { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };

/* A path geometry being built in Wine's d2d1 (struct d2d_geometry and struct d2d_figure in
 * dlls/d2d1): the ID2D1GeometrySink is followed by the figures, and the current point is the
 * last vertex of the last figure. Checked on a test path before AddArc is replaced. */
struct wine_d2d_figure {
    arc_point *vertices;
    SIZE_T vertices_size;
    void *vertex_types;
    SIZE_T vertex_types_size;
    SIZE_T vertex_count;
    void *bezier_controls;
    SIZE_T bezier_controls_size, bezier_control_count;
    void *original_bezier_controls;
    SIZE_T original_bezier_controls_size, original_bezier_control_count;
    D2D1_RECT_F bounds;
    unsigned flags;
};
_Static_assert(sizeof(struct wine_d2d_figure) == 112, "d2d_figure layout");

struct wine_d2d_path_sink {
    void *vtbl;
    struct wine_d2d_figure *figures;
    SIZE_T figures_size, figure_count;
    int state;
};

#define WINE_D2D_STATE_FIGURE 4 /* D2D_GEOMETRY_STATE_FIGURE: between BeginFigure and EndFigure */

static int current_point(void *sink, arc_point *point)
{
    const struct wine_d2d_path_sink *path = sink;
    const struct wine_d2d_figure *figure;

    if (path->state != WINE_D2D_STATE_FIGURE || !path->figure_count || path->figure_count > path->figures_size)
        return 0;
    figure = &path->figures[path->figure_count - 1];
    if (!figure->vertex_count) return 0;
    *point = figure->vertices[figure->vertex_count - 1];
    return 1;
}

typedef void (*add_arc_fn)(void *sink, const D2D1_ARC_SEGMENT *arc);
typedef void (*add_line_fn)(void *sink, arc_point point);
typedef void (*add_quadratics_fn)(void *sink, const arc_quadratic *curves, DWORD count);

static add_arc_fn real_add_arc;

static void add_arc_hook(void *sink, const D2D1_ARC_SEGMENT *arc)
{
    arc_quadratic curves[ARC_QUADRATICS_MAX];
    arc_point from;
    unsigned n;

    if (!current_point(sink, &from)) {
        real_add_arc(sink, arc); /* outside a figure: Wine's error */
        return;
    }
    n = arc_to_quadratics(from, arc->point, arc->width, arc->height, arc->rotation,
                          arc->sweep_direction == D2D1_SWEEP_DIRECTION_CLOCKWISE,
                          arc->arc_size == D2D1_ARC_SIZE_LARGE, curves);
    if (n)
        ((add_quadratics_fn)VTBL(sink)[SLOT_SINK_ADD_QUADRATIC_BEZIERS])(sink, curves, n);
    else /* a zero radius: a straight line, as in SVG */
        ((add_line_fn)VTBL(sink)[SLOT_SINK_ADD_LINE])(sink, arc->point);
}

typedef HRESULT (*d2d_create_factory_fn)(int type, const GUID *iid, const void *options, void **factory);
typedef HRESULT (*create_fn)(void *obj, void **out);
typedef void (*begin_figure_fn)(void *sink, arc_point start, int figure_begin);
typedef void (*end_figure_fn)(void *sink, int figure_end);
typedef HRESULT (*close_fn)(void *sink);
typedef HRESULT (*get_bounds_fn)(void *geometry, const void *transform, D2D1_RECT_F *bounds);

/* On a new path: a triangle, then a 10 x 4 box whose right side is a half circle of radius 2
 * bulging to the right. True if the current point is found where current_point() looks (in
 * the second figure, so the figure size is checked too) and Wine's AddArc leaves the half
 * circle out (the box ends at x = 10, not 12). */
static int add_arc_missing(void *path, void *sink)
{
    static const arc_point triangle[] = { { -10, 0 }, { -6, 0 }, { -6, 4 } }, box[] = { { 0, 0 }, { 10, 0 }, { 0, 4 } };
    static const D2D1_ARC_SEGMENT half_circle = { { 10, 4 }, 2, 2, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, 0 };
    void **sink_vtbl = VTBL(sink);
    begin_figure_fn begin = (begin_figure_fn)sink_vtbl[SLOT_SINK_BEGIN_FIGURE];
    add_line_fn add_line = (add_line_fn)sink_vtbl[SLOT_SINK_ADD_LINE];
    end_figure_fn end = (end_figure_fn)sink_vtbl[SLOT_SINK_END_FIGURE];
    D2D1_RECT_F bounds;
    arc_point at;

    begin(sink, triangle[0], D2D1_FIGURE_BEGIN_FILLED);
    add_line(sink, triangle[1]);
    add_line(sink, triangle[2]);
    end(sink, D2D1_FIGURE_END_CLOSED);
    begin(sink, box[0], D2D1_FIGURE_BEGIN_FILLED);
    add_line(sink, box[1]);
    if (((struct wine_d2d_path_sink *)sink)->figure_count != 2 || !current_point(sink, &at)
            || at.x != box[1].x || at.y != box[1].y)
        return 0;
    ((add_arc_fn)sink_vtbl[SLOT_SINK_ADD_ARC])(sink, &half_circle);
    add_line(sink, box[2]);
    end(sink, D2D1_FIGURE_END_CLOSED);
    return ((close_fn)sink_vtbl[SLOT_SINK_CLOSE])(sink) >= 0
            && ((get_bounds_fn)VTBL(path)[SLOT_GEOMETRY_GET_BOUNDS])(path, 0, &bounds) >= 0
            && bounds.right == box[1].x;
}

/* ---- 7. Stale ActiveControl of a panel form ------------------------------- */

/* VCL's DoNestedActivation (Vcl.Forms), as compiled into X2.EXE (Delphi 12, x64), followed by
 * the start of TCustomForm.Activate, which calls it with the form's ActiveControl:
 *
 *   procedure DoNestedActivation(Msg: Cardinal; Control: TWinControl; Form: TCustomForm);
 *   begin
 *     if Control = nil then Exit;
 *     while (Control.Parent <> nil) and not (Control is TCustomForm) do Control := Control.Parent;
 *     if (Control <> nil) and (Control <> Form) then SendMessage(Control.Handle, Msg, 0, 0);
 *   end;
 *
 * ?? are rel32 operands, read from the match: the TCustomForm class (a pointer to it), @IsClass,
 * TWinControl.GetHandle, SendMessage, and in Activate the call back to the routine. */
static const char nested_activation_code[] =
    "57 56 53 48 83 ec 20 89 cb 48 89 d6 4c 89 c7 48 85 f6 74 42 eb 04 48 8b 76 78 48 83 7e 78 00 74 "
    "13 48 89 f1 48 8b 15 ?? ?? ?? ?? e8 ?? ?? ?? ?? 84 c0 74 e2 48 85 f6 74 1d 48 3b f7 74 18 48 89 "
    "f1 e8 ?? ?? ?? ?? 48 89 c1 89 da 4d 33 c0 4d 33 c9 e8 ?? ?? ?? ?? 48 83 c4 20 5b 5e 5f c3 cc cc "
    /* Activate: */ "56 53 48 83 ec 28 48 89 cb c7 c1 00 b0 00 00 48 8b 93 b0 04 00 00 49 89 d8 e8 ?? ?? ?? ??";
#define NESTED_ACTIVATION_RVA 0x3512d0 /* Altium 26.10.1; other builds are searched for */
#define NA_FORM_CLASS 39 /* offsets of the rel32 operands */
#define NA_IS_CLASS 44
#define NA_GET_HANDLE 66
#define NA_SEND_MESSAGE 82
#define NA_ACTIVATE_CALL 122

#define CONTROL_PARENT 0x78        /* TControl.FParent (in the pattern) */
#define FORM_ACTIVE_CONTROL 0x4b0  /* TCustomForm.FActiveControl (in the pattern) */
#define FORM_FOCUSED_CONTROL 0x4b8 /* TCustomForm.FFocusedControl, declared right after it */
#define FIELD(obj, offset) (*(BYTE **)((obj) + (offset)))

typedef BYTE (*is_class_fn)(const BYTE *obj, const void *cls);
typedef HWND (*get_handle_fn)(BYTE *control);
typedef LRESULT (*send_message_fn)(HWND hwnd, unsigned msg, WPARAM wp, LPARAM lp);

static void *const *form_class;
static is_class_fn vcl_is_class;
static get_handle_fn vcl_get_handle;
static send_message_fn vcl_send_message;

static int is_form(const BYTE *control) { return vcl_is_class(control, *form_class); }

/* DoNestedActivation, except for an ActiveControl that is in no form any more: the walk up ends
 * at a control without a parent that is not a form. VCL would ask it for its window handle,
 * which creates the window, which needs a parent: "Control 'View…' has no parent window". Such
 * a control has been removed from the form, so the form's pointers to it are cleared the way
 * TCustomForm.DefocusControl clears them when a control is removed, and nothing is sent. */
static void nested_activation_hook(DWORD msg, BYTE *control, BYTE *form)
{
    static int logged;
    BYTE *top = control, *p;

    if (!control) return;
    while (FIELD(top, CONTROL_PARENT) && !is_form(top)) top = FIELD(top, CONTROL_PARENT);
    if (FIELD(top, CONTROL_PARENT) || is_form(top)) {
        if (top != form) vcl_send_message(vcl_get_handle(top), msg, 0, 0);
        return;
    }
    if (form && FIELD(form, FORM_ACTIVE_CONTROL) == control) {
        p = FIELD(form, FORM_FOCUSED_CONTROL);
        while (p && p != top) p = FIELD(p, CONTROL_PARENT);
        if (p) FIELD(form, FORM_FOCUSED_CONTROL) = 0;
        FIELD(form, FORM_ACTIVE_CONTROL) = 0;
    }
    if (!logged) {
        logged = 1;
        OutputDebugStringA("shfolderproxy: stale ActiveControl (a removed control) cleared\n");
    }
}

static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* True if code matches pattern ("xx" or "??" per byte, space-separated) within limit bytes. */
static int code_matches(const BYTE *code, SIZE_T limit, const char *pattern)
{
    for (SIZE_T i = 0; *pattern; i++, pattern += pattern[2] ? 3 : 2) {
        if (i >= limit) return 0;
        if (pattern[0] != '?' && code[i] != (BYTE)(hex_digit(pattern[0]) << 4 | hex_digit(pattern[1]))) return 0;
    }
    return 1;
}

static BYTE *rel32_target(BYTE *operand) { return operand + 4 + *(const int *)operand; }

/* The first executable section of a module. */
static BYTE *code_section(BYTE *base, DWORD *size)
{
    BYTE *nt = base + *(DWORD *)(base + 0x3c), *section;
    WORD count = *(WORD *)(nt + 6);

    section = nt + 0x18 + *(WORD *)(nt + 0x14); /* after the optional header */
    for (; count--; section += 40)
        if (*(DWORD *)(section + 36) & 0x20000000) { /* IMAGE_SCN_MEM_EXECUTE */
            *size = *(DWORD *)(section + 8);
            return base + *(DWORD *)(section + 12);
        }
    return 0;
}

static int is_nested_activation(BYTE *code, BYTE *end)
{
    return code_matches(code, end - code, nested_activation_code)
            && rel32_target(code + NA_ACTIVATE_CALL) == code;
}

/* Runs before X2.EXE's own code: SHFolder.dll is one of its imports. */
static void patch_nested_activation(BYTE *exe)
{
    BYTE *text, *end, *code = 0, jump[14] = { 0xff, 0x25 }; /* jmp [rip+0]; dq target */
    void *target = (void *)nested_activation_hook;
    DWORD size, old;

    if (!exe || exe[0] != 'M' || exe[1] != 'Z' || !(text = code_section(exe, &size))) return;
    end = text + size;
    if (exe + NESTED_ACTIVATION_RVA >= text && exe + NESTED_ACTIVATION_RVA < end
            && is_nested_activation(exe + NESTED_ACTIVATION_RVA, end))
        code = exe + NESTED_ACTIVATION_RVA;
    for (BYTE *p = text; !code && p < end; p++)
        if (*p == 0x57 && is_nested_activation(p, end)) code = p;
    if (!code) return;

    form_class = (void *const *)rel32_target(code + NA_FORM_CLASS);
    vcl_is_class = (is_class_fn)rel32_target(code + NA_IS_CLASS);
    vcl_get_handle = (get_handle_fn)rel32_target(code + NA_GET_HANDLE);
    vcl_send_message = (send_message_fn)rel32_target(code + NA_SEND_MESSAGE);
    memcpy(jump + 6, &target, sizeof(target));
    if (!VirtualProtect(code, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(code, jump, sizeof(jump));
    VirtualProtect(code, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(jump));
    OutputDebugStringA("shfolderproxy: VCL DoNestedActivation replaced in X2.EXE\n");
}

/* ---- 8. Direct3D state swaps around every Direct2D draw -------------------- */

/* Wine's d2d1 draws every shape (each line, fill and glyph bitmap of a schematic) in
 * d2d_device_context_draw: it swaps its own Direct3D state object into the immediate context
 * (ID3D11DeviceContext1::SwapDeviceContextState), sets its shaders and buffers, draws, and
 * swaps the application's state back. In DXVK a swap copies the whole context state, with a
 * reference count change for every slot, and binds all of it again: about half of the
 * schematic editor's redraw time.
 *
 * Swapping the application's state back and d2d1's in again changes nothing if nothing else
 * uses the context in between. So the swap back is held back (the application's state object
 * is kept, with a reference), and d2d1's next swap to the same state of its own is skipped:
 * d2d1 gets the held state as the previous one. The held state goes into the context before
 * any other caller uses the context (every method of the immediate context goes through a
 * thunk that checks, below), and at the end of d2d1's EndDraw and Flush, on the thread that
 * drew. d2d1's own calls between draws (Map and Unmap of its constant buffers, bitmap
 * uploads) and DXVK's calls to itself do not depend on the bound state and pass through.
 *
 * Installed when d2d1 creates its first render target on a DXGI surface (as the schematic
 * editor does): the context's vtable is shared by every immediate context of that
 * d3d11.dll. ALTIUM_KEEP_D2D_SWAPS=1 leaves it out. */

#define SLOT_ADDREF 1
#define SLOT_D2D_FACTORY_CREATE_DXGI_SURFACE_RENDER_TARGET 15
#define SLOT_RENDER_TARGET_FLUSH 42
#define SLOT_RENDER_TARGET_END_DRAW 49
#define SLOT_DXGI_SURFACE_GET_DEVICE 7
#define SLOT_D3D11_DEVICE_GET_IMMEDIATE_CONTEXT 40
#define SLOT_CONTEXT_SWAP_DEVICE_CONTEXT_STATE 131
#define CONTEXT_SLOTS 149 /* ID3D11DeviceContext4 */
#define SWAP_CONTEXTS_MAX 16

static const GUID IID_ID3D11Device = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };

typedef void (*swap_state_fn)(void *context, void *state, void **previous);
typedef HRESULT (*create_dxgi_rt_fn)(void *factory, void *surface, const void *desc, void **rt);
typedef HRESULT (*end_draw_fn)(void *rt, ULONGLONG *tag1, ULONGLONG *tag2);
typedef HRESULT (*get_device_fn)(void *obj, const GUID *iid, void **device);
typedef void (*get_context_fn)(void *device, void **context);
typedef DWORD (*addref_fn)(void *obj);

struct swap_context {
    void *context;   /* NULL: free */
    void *app_state; /* swapped back by d2d1, not yet into the context; one reference held */
    void *d2d_state; /* d2d1's state, in the context */
    DWORD thread;    /* the thread that held app_state back */
};

static struct swap_context swap_contexts[SWAP_CONTEXTS_MAX];
static volatile long swap_contexts_lock;
static const BYTE *d3d11_module;
static create_dxgi_rt_fn real_create_dxgi_rt;
static end_draw_fn real_end_draw, real_flush;

/* Read by the thunks. */
volatile long d2d_swaps_pending; /* held application states, all contexts */
void *d3d_context_real[CONTEXT_SLOTS];

static void lock_swap_contexts(void) { while (__sync_lock_test_and_set(&swap_contexts_lock, 1)) ; }
static void unlock_swap_contexts(void) { __sync_lock_release(&swap_contexts_lock); }

/* With the lock held. A full table gives up an entry holding nothing back. */
static struct swap_context *find_swap_context(void *context, int create)
{
    struct swap_context *spare = 0;
    for (int i = 0; i < SWAP_CONTEXTS_MAX; i++) {
        struct swap_context *c = &swap_contexts[i];
        if (c->context == context) return c;
        if (!c->app_state && (!spare || (spare->context && !c->context))) spare = c; /* prefer a free one */
    }
    if (!create || !spare) return 0;
    spare->context = context;
    spare->d2d_state = 0;
    return spare;
}

static void swap_state(void *context, void *state, void **previous)
{
    ((swap_state_fn)d3d_context_real[SLOT_CONTEXT_SWAP_DEVICE_CONTEXT_STATE])(context, state, previous);
}

/* Put the held application state into the context, as d2d1 would have done after its last
 * draw, and forget which state d2d1 left in it. */
static void restore_app_state(void *context)
{
    struct swap_context *c;
    void *held = 0;

    lock_swap_contexts();
    if ((c = find_swap_context(context, 0))) {
        if ((held = c->app_state)) d2d_swaps_pending--;
        c->app_state = c->d2d_state = 0;
    }
    unlock_swap_contexts();
    if (held) {
        swap_state(context, held, 0);
        release(held);
    }
}

static void restore_thread_app_states(void)
{
    struct { void *context, *state; } held[SWAP_CONTEXTS_MAX];
    unsigned count = 0;
    DWORD thread;

    if (!d2d_swaps_pending) return;
    thread = GetCurrentThreadId();
    lock_swap_contexts();
    for (int i = 0; i < SWAP_CONTEXTS_MAX; i++) {
        struct swap_context *c = &swap_contexts[i];
        if (!c->app_state || c->thread != thread) continue;
        held[count].context = c->context;
        held[count++].state = c->app_state;
        c->app_state = c->d2d_state = 0;
        d2d_swaps_pending--;
    }
    unlock_swap_contexts();
    for (unsigned i = 0; i < count; i++) {
        swap_state(held[i].context, held[i].state, 0);
        release(held[i].state);
    }
}

/* Called by the thunks before a context method runs, while some state is held back. */
void d3d_context_called(void *context, const void *caller)
{
    if (!from_wine_d2d1(caller) && !in_image(d3d11_module, caller)) restore_app_state(context);
}

static void swap_state_hook(void *context, void *state, void **previous)
{
    const void *caller = __builtin_return_address(0);
    struct swap_context *c;
    void *held;

    if (!state || !from_wine_d2d1(caller)) { /* whoever swaps, the held state goes in first */
        restore_app_state(context);
        swap_state(context, state, previous);
        return;
    }
    lock_swap_contexts();
    c = find_swap_context(context, 1);
    if (previous) { /* d2d1 swaps its state in before a draw */
        if (c && c->app_state && c->d2d_state == state) {
            *previous = c->app_state; /* with the reference held, which d2d1 releases */
            c->app_state = 0;
            d2d_swaps_pending--;
            unlock_swap_contexts();
            return;
        }
        held = c ? c->app_state : 0;
        if (held) d2d_swaps_pending--;
        if (c) {
            c->app_state = 0;
            c->d2d_state = state;
        }
        unlock_swap_contexts();
        if (held) {
            swap_state(context, held, 0);
            release(held);
        }
        swap_state(context, state, previous);
        return;
    }
    /* d2d1 swaps the application's state back after a draw */
    if (c && c->d2d_state && !c->app_state) {
        ((addref_fn)VTBL(state)[SLOT_ADDREF])(state);
        c->app_state = state;
        c->thread = GetCurrentThreadId();
        d2d_swaps_pending++;
        unlock_swap_contexts();
        return;
    }
    if (c) c->d2d_state = 0;
    unlock_swap_contexts();
    swap_state(context, state, 0);
}

static HRESULT end_draw_hook(void *rt, ULONGLONG *tag1, ULONGLONG *tag2)
{
    HRESULT hr = real_end_draw(rt, tag1, tag2);
    restore_thread_app_states();
    return hr;
}

static HRESULT flush_hook(void *rt, ULONGLONG *tag1, ULONGLONG *tag2)
{
    HRESULT hr = real_flush(rt, tag1, tag2);
    restore_thread_app_states();
    return hr;
}

/* One thunk per slot of the context's vtable: straight to the real method while nothing is
 * held back, otherwise through d3d_context_called first, with the arguments kept (rcx, rdx,
 * r8, r9, xmm0-3; the rest are on the stack, untouched). */
#define CONTEXT_THUNK(n) __asm__(                                         \
    ".text\n"                                                             \
    "d3d_context_thunk_" #n ":\n"                                         \
    "    cmpl $0, d2d_swaps_pending(%rip)\n"                              \
    "    jne 1f\n"                                                        \
    "    jmpq *d3d_context_real+8*" #n "(%rip)\n"                         \
    "1:  movl $" #n ", %eax\n"                                            \
    "    jmp d3d_context_thunk_slow\n");
#define DECLARE_CONTEXT_THUNK(n) void d3d_context_thunk_##n(void);
#define CONTEXT_THUNK_ADDRESS(n) (void *)d3d_context_thunk_##n,
#define TEN_SLOTS(X, t) X(t##0) X(t##1) X(t##2) X(t##3) X(t##4) X(t##5) X(t##6) X(t##7) X(t##8) X(t##9)
#define CONTEXT_SLOT_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9)                     \
    TEN_SLOTS(X, 1) TEN_SLOTS(X, 2) TEN_SLOTS(X, 3) TEN_SLOTS(X, 4) TEN_SLOTS(X, 5) TEN_SLOTS(X, 6) \
    TEN_SLOTS(X, 7) TEN_SLOTS(X, 8) TEN_SLOTS(X, 9) TEN_SLOTS(X, 10) TEN_SLOTS(X, 11)               \
    TEN_SLOTS(X, 12) TEN_SLOTS(X, 13) X(140) X(141) X(142) X(143) X(144) X(145) X(146) X(147) X(148)

CONTEXT_SLOT_LIST(CONTEXT_THUNK)
CONTEXT_SLOT_LIST(DECLARE_CONTEXT_THUNK)
static void *const d3d_context_thunks[CONTEXT_SLOTS] = { CONTEXT_SLOT_LIST(CONTEXT_THUNK_ADDRESS) };

/* eax = the slot. Saves the argument registers, calls d3d_context_called(context, the
 * caller's return address) and jumps to the real method. */
__asm__(
    ".text\n"
    ".seh_proc d3d_context_thunk_slow\n"
    "d3d_context_thunk_slow:\n"
    "    pushq %rcx\n"
    "    .seh_pushreg %rcx\n"
    "    pushq %rdx\n"
    "    .seh_pushreg %rdx\n"
    "    pushq %r8\n"
    "    .seh_pushreg %r8\n"
    "    pushq %r9\n"
    "    .seh_pushreg %r9\n"
    "    subq $0x68, %rsp\n"
    "    .seh_stackalloc 0x68\n"
    "    .seh_endprologue\n"
    "    movdqu %xmm0, 0x20(%rsp)\n"
    "    movdqu %xmm1, 0x30(%rsp)\n"
    "    movdqu %xmm2, 0x40(%rsp)\n"
    "    movdqu %xmm3, 0x50(%rsp)\n"
    "    movl %eax, 0x60(%rsp)\n"
    "    movq 0x88(%rsp), %rdx\n" /* return address, above the 4 pushes and 0x68 bytes */
    "    callq d3d_context_called\n"
    "    movl 0x60(%rsp), %eax\n"
    "    movdqu 0x20(%rsp), %xmm0\n"
    "    movdqu 0x30(%rsp), %xmm1\n"
    "    movdqu 0x40(%rsp), %xmm2\n"
    "    movdqu 0x50(%rsp), %xmm3\n"
    "    addq $0x68, %rsp\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    leaq d3d_context_real(%rip), %r11\n"
    "    jmpq *(%r11,%rax,8)\n"
    ".seh_endproc\n");

/* Point every slot but IUnknown's at its thunk, and SwapDeviceContextState at the hook. */
static void install_swap_hooks(void *surface, void *rt)
{
    static volatile long tried;
    const BYTE *d2d1 = GetModuleHandleW(L"d2d1.dll");
    void *device = 0, *context = 0;
    volatile ULONGLONG *vtbl;
    DWORD old;
    char keep[4];

    if (__sync_lock_test_and_set(&tried, 1)) return;
    if (GetEnvironmentVariableA("ALTIUM_KEEP_D2D_SWAPS", keep, sizeof(keep)) == 1 && keep[0] == '1') return;
    if (((get_device_fn)VTBL(surface)[SLOT_DXGI_SURFACE_GET_DEVICE])(surface, &IID_ID3D11Device, &device) < 0) return;
    ((get_context_fn)VTBL(device)[SLOT_D3D11_DEVICE_GET_IMMEDIATE_CONTEXT])(device, &context);
    release(device);
    if (!context) return;
    vtbl = (volatile ULONGLONG *)VTBL(context);
    d3d11_module = GetModuleHandleW(L"d3d11.dll");
    for (int i = 0; i < CONTEXT_SLOTS; i++)
        if (!in_image(d3d11_module, (void *)vtbl[i])) goto done; /* not d3d11.dll's immediate context */
    for (int i = 0; i < CONTEXT_SLOTS; i++) d3d_context_real[i] = (void *)vtbl[i];

    wrap_slot(d2d1, &VTBL(rt)[SLOT_RENDER_TARGET_END_DRAW], (void *)end_draw_hook, (void **)&real_end_draw);
    wrap_slot(d2d1, &VTBL(rt)[SLOT_RENDER_TARGET_FLUSH], (void *)flush_hook, (void **)&real_flush);
    if (!real_end_draw || !real_flush) goto done;

    __sync_synchronize(); /* the real methods are visible before the slots change */
    if (!VirtualProtect((void *)vtbl, CONTEXT_SLOTS * sizeof(*vtbl), PAGE_READWRITE, &old)) goto done;
    for (int i = SLOT_ADDREF + 2; i < CONTEXT_SLOTS; i++)
        vtbl[i] = (ULONGLONG)(i == SLOT_CONTEXT_SWAP_DEVICE_CONTEXT_STATE ? (void *)swap_state_hook : d3d_context_thunks[i]);
    VirtualProtect((void *)vtbl, CONTEXT_SLOTS * sizeof(*vtbl), old, &old);
    OutputDebugStringA("shfolderproxy: Direct2D state-swap hooks installed in d3d11.dll\n");
done:
    release(context);
}

static HRESULT create_dxgi_rt_hook(void *factory, void *surface, const void *desc, void **rt)
{
    HRESULT hr = real_create_dxgi_rt(factory, surface, desc, rt);
    if (hr >= 0 && surface && rt && *rt) install_swap_hooks(surface, *rt);
    return hr;
}

/* ---- Hooks in Wine's d2d1.dll (6 and 8) ------------------------------------ */

/* On its own thread, like patch_dwrite: d2d1 cannot be used from the load notification. The
 * factory's vtable is shared by every factory. */
static DWORD patch_d2d1(void *module)
{
    d2d_create_factory_fn create = (d2d_create_factory_fn)GetProcAddress(module, "D2D1CreateFactory");
    void *factory = 0, *path = 0, *sink = 0;

    if (!create || create(0 /* SINGLE_THREADED */, &IID_ID2D1Factory, 0, &factory) < 0) return 1;
    /* The sink's vtable is shared by every path geometry. */
    if (((create_fn)VTBL(factory)[SLOT_D2D_FACTORY_CREATE_PATH_GEOMETRY])(factory, &path) >= 0
            && ((create_fn)VTBL(path)[SLOT_PATH_GEOMETRY_OPEN])(path, &sink) >= 0
            && add_arc_missing(path, sink))
        wrap_slot(module, &VTBL(sink)[SLOT_SINK_ADD_ARC], (void *)add_arc_hook, (void **)&real_add_arc);
    wrap_slot(module, &VTBL(factory)[SLOT_D2D_FACTORY_CREATE_DXGI_SURFACE_RENDER_TARGET],
              (void *)create_dxgi_rt_hook, (void **)&real_create_dxgi_rt);

    release(sink);
    release(path);
    release(factory);
    if (real_add_arc) OutputDebugStringA("shfolderproxy: Direct2D arc hook installed in d2d1.dll\n");
    return 0;
}

static void d2d1_loaded(BYTE *base)
{
    static volatile long started;
    if (!is_wine_builtin(base) || __sync_lock_test_and_set(&started, 1)) return;
    CreateThread(0, 0, patch_d2d1, base, 0, 0);
}

/* ---- Import table patching ---------------------------------------------- */

static BYTE *data_directory(BYTE *base, int index, DWORD *size)
{
    BYTE *nt;
    DWORD rva;

    if (!base || base[0] != 'M' || base[1] != 'Z') return 0;
    nt = base + *(DWORD *)(base + 0x3c);
    if (*(DWORD *)nt != 0x4550 || *(WORD *)(nt + 0x18) != 0x20b) return 0; /* PE32+ only */
    if (*(DWORD *)(nt + 0x84) <= (DWORD)index) return 0; /* NumberOfRvaAndSizes */
    rva = *(DWORD *)(nt + 0x88 + 8 * index);
    *size = *(DWORD *)(nt + 0x8c + 8 * index);
    return rva && *size ? base + rva : 0;
}

typedef struct {
    DWORD attrs, dll_name, module_handle, iat, int_table, bound_iat, unload_iat, timestamp;
} DELAY_DESCRIPTOR;

static void patch_user32_delay_imports(BYTE *base)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    DWORD size;
    DELAY_DESCRIPTOR *desc = (DELAY_DESCRIPTOR *)data_directory(base, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, &size);

    if (!user32) return;
    for (; desc && desc->dll_name; desc++) {
        ULONGLONG *names, *slots;
        if (!(desc->attrs & 1) || !streq_ci((const char *)(base + desc->dll_name), "user32.dll")) continue;
        names = (ULONGLONG *)(base + desc->int_table);
        slots = (ULONGLONG *)(base + desc->iat);
        for (; *names; names++, slots++) {
            const char *name;
            if (*names & IMAGE_ORDINAL_FLAG64) continue;
            name = (const char *)(base + (DWORD)*names + 2); /* skip the WORD hint */
            for (unsigned i = 0; i < sizeof(user32_fallbacks) / sizeof(user32_fallbacks[0]); i++)
                if (streq(name, user32_fallbacks[i].name) && !GetProcAddress(user32, name))
                    set_slot(slots, user32_fallbacks[i].fallback);
        }
    }
}

typedef struct {
    DWORD original_first_thunk, timestamp, forwarder_chain, name, first_thunk;
} IMPORT_DESCRIPTOR;

/* Points the module's imports of the hooked functions from `dll` at the hooks. Called after
 * Wine has resolved the module's imports (see process_attach in ntdll/loader.c). */
static void patch_imports(BYTE *base, const char *dll, const struct hook *hooks, unsigned count)
{
    DWORD size;
    IMPORT_DESCRIPTOR *desc = (IMPORT_DESCRIPTOR *)data_directory(base, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);

    for (; desc && desc->name; desc++) {
        ULONGLONG *names, *slots;
        if (!desc->original_first_thunk || !streq_ci((const char *)(base + desc->name), dll)) continue;
        names = (ULONGLONG *)(base + desc->original_first_thunk);
        slots = (ULONGLONG *)(base + desc->first_thunk);
        for (; *names; names++, slots++) {
            const char *name;
            if (*names & IMAGE_ORDINAL_FLAG64) continue;
            name = (const char *)(base + (DWORD)*names + 2);
            for (unsigned i = 0; i < count; i++) {
                if (!streq(name, hooks[i].name) || *slots == (ULONGLONG)hooks[i].hook) continue;
                *hooks[i].real = (void *)*slots;
                set_slot(slots, hooks[i].hook);
            }
        }
    }
}

typedef struct {
    WORD length, maximum_length;
    const WCHAR *buffer;
} COUNTED_STRING; /* UNICODE_STRING */

typedef struct {
    DWORD flags;
    const COUNTED_STRING *full_name, *base_name;
    void *dll_base;
    DWORD size_of_image;
} DLL_NOTIFICATION_DATA;

static void dll_loaded(DWORD reason, const DLL_NOTIFICATION_DATA *data, void *context)
{
    (void)context;
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED) return;
    patch_user32_delay_imports(data->dll_base);
    if (!data->base_name) return;
    if (wname_is(data->base_name->buffer, data->base_name->length / 2, "simbeor_sdk.dll"))
        patch_imports(data->dll_base, "bcrypt.dll", bcrypt_hooks, sizeof(bcrypt_hooks) / sizeof(bcrypt_hooks[0]));
    else if (wname_is(data->base_name->buffer, data->base_name->length / 2, "dwrite.dll"))
        dwrite_loaded(data->dll_base);
    else if (wname_is(data->base_name->buffer, data->base_name->length / 2, "d2d1.dll"))
        d2d1_loaded(data->dll_base);
}

BOOL DllMain(HMODULE self, DWORD reason, void *reserved)
{
    static void *cookie;
    (void)self; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        patch_user32_delay_imports(GetModuleHandleW(0));
        patch_nested_activation(GetModuleHandleW(0));
        if (is_wine_builtin(GetModuleHandleW(L"user32.dll")))
            patch_imports(GetModuleHandleW(L"user32.dll"), "win32u.dll", win32u_hooks,
                          sizeof(win32u_hooks) / sizeof(win32u_hooks[0]));
        LdrRegisterDllNotification(0, (void *)dll_loaded, 0, &cookie);
        if (GetModuleHandleW(L"dwrite.dll")) dwrite_loaded(GetModuleHandleW(L"dwrite.dll"));
        if (GetModuleHandleW(L"d2d1.dll")) d2d1_loaded(GetModuleHandleW(L"d2d1.dll"));
    }
    return 1;
}
