/*
 * SHFolder.dll proxy for Altium Designer under Wine.
 *
 * X2.EXE imports SHFolder.dll at startup, so a copy of this DLL placed next to
 * X2.EXE is loaded early. Its exports forward to shell32 like Wine's own
 * shfolder. On load it works around two Wine gaps, in X2.EXE and in every DLL
 * loaded later (via LdrRegisterDllNotification):
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
IMPORT NTSTATUS LdrRegisterDllNotification(DWORD flags, void *callback, void *context, void **cookie);

#define PAGE_READWRITE 0x04
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

static const struct { const char *name; void *hook; void **real; } bcrypt_hooks[] = {
    { "BCryptImportKeyPair", (void *)import_key_pair_hook, (void **)&real_import_key_pair },
    { "BCryptDecrypt", (void *)decrypt_hook, (void **)&real_decrypt },
    { "BCryptDestroyKey", (void *)destroy_key_hook, (void **)&real_destroy_key },
};

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

/* Called after Wine has resolved the module's imports (see process_attach in ntdll/loader.c). */
static void patch_bcrypt_imports(BYTE *base)
{
    DWORD size;
    IMPORT_DESCRIPTOR *desc = (IMPORT_DESCRIPTOR *)data_directory(base, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);

    for (; desc && desc->name; desc++) {
        ULONGLONG *names, *slots;
        if (!desc->original_first_thunk || !streq_ci((const char *)(base + desc->name), "bcrypt.dll")) continue;
        names = (ULONGLONG *)(base + desc->original_first_thunk);
        slots = (ULONGLONG *)(base + desc->first_thunk);
        for (; *names; names++, slots++) {
            const char *name;
            if (*names & IMAGE_ORDINAL_FLAG64) continue;
            name = (const char *)(base + (DWORD)*names + 2);
            for (unsigned i = 0; i < sizeof(bcrypt_hooks) / sizeof(bcrypt_hooks[0]); i++) {
                if (!streq(name, bcrypt_hooks[i].name) || *slots == (ULONGLONG)bcrypt_hooks[i].hook) continue;
                *bcrypt_hooks[i].real = (void *)*slots;
                set_slot(slots, bcrypt_hooks[i].hook);
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
    if (data->base_name && wname_is(data->base_name->buffer, data->base_name->length / 2, "simbeor_sdk.dll"))
        patch_bcrypt_imports(data->dll_base);
}

BOOL DllMain(HMODULE self, DWORD reason, void *reserved)
{
    static void *cookie;
    (void)self; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        patch_user32_delay_imports(GetModuleHandleW(0));
        LdrRegisterDllNotification(0, (void *)dll_loaded, 0, &cookie);
    }
    return 1;
}
