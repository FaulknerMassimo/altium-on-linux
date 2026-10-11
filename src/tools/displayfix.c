/*
 * displayfix: make Wine rebuild its monitor list after monitors are plugged, unplugged or
 * reordered while Altium runs. win32u keeps one registry link per display slot
 * (...\Hardware Profiles\Current\...\Control\Video\{gpu}\0000 -> ...\Sources\<XRandR output>)
 * and cannot point an existing link at another output: it writes into the old target instead.
 * Every process but explorer then reads the outputs that first held each slot (a lost monitor,
 * two primaries). Deleting the links makes the next process that reads the display list
 * rebuild it from X11 (docs/how-it-works.md §19). Built by scripts/build.sh, run by
 * scripts/refresh-displays.sh.
 */

typedef unsigned int DWORD;
typedef int BOOL;
typedef long LONG;
typedef void *HANDLE;
typedef void *HKEY;
typedef long long LONG_PTR;

#define IMPORT __declspec(dllimport)
IMPORT LONG RegOpenKeyExA(HKEY key, const char *name, DWORD options, DWORD access, HKEY *result);
IMPORT LONG RegEnumKeyExA(HKEY key, DWORD index, char *name, DWORD *name_len, DWORD *reserved, char *cls,
                          DWORD *cls_len, void *last_write);
IMPORT LONG RegQueryValueExA(HKEY key, const char *name, DWORD *reserved, DWORD *type, void *data, DWORD *len);
IMPORT LONG RegCloseKey(HKEY key);
IMPORT LONG NtDeleteKey(HANDLE key);
IMPORT DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD len);
IMPORT int wsprintfA(char *buf, const char *fmt, ...);
IMPORT HANDLE GetStdHandle(DWORD id);
IMPORT BOOL WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *written, void *overlapped);
IMPORT __declspec(noreturn) void ExitProcess(unsigned code);

#define HKEY_LOCAL_MACHINE ((HKEY)(LONG_PTR)(LONG)0x80000002)
#define REG_OPTION_OPEN_LINK 8
#define REG_LINK 6
#define KEY_READ 0x20019
#define KEY_QUERY_VALUE 1
#define DELETE 0x10000
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define MAX_SLOTS 64

static const char video_path[] =
    "System\\CurrentControlSet\\Hardware Profiles\\Current\\System\\CurrentControlSet\\Control\\Video";

static void out(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

static BOOL is_slot_name(const char *name)
{
    for (int i = 0; i < 4; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f') || (name[i] >= 'A' && name[i] <= 'F')))
            return 0;
    return !name[4];
}

/* Delete the slot links (0000, 0001, ...) under one GPU key; real keys are left alone. */
static int delete_slot_links(HKEY gpu, const char *gpu_name)
{
    char names[MAX_SLOTS][8], line[256];
    int count = 0, deleted = 0;
    for (DWORD i = 0; count < MAX_SLOTS; i++) {
        char name[64];
        DWORD len = sizeof(name);
        if (RegEnumKeyExA(gpu, i, name, &len, 0, 0, 0, 0)) break;
        if (is_slot_name(name)) {
            for (int j = 0; j < 5; j++) names[count][j] = name[j];
            count++;
        }
    }
    for (int i = 0; i < count; i++) {
        HKEY link;
        DWORD type = 0, len = 0;
        if (RegOpenKeyExA(gpu, names[i], REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE | DELETE, &link)) continue;
        /* Only a link key holds a REG_LINK SymbolicLinkValue; opened through the link it would be the target. */
        if (!RegQueryValueExA(link, "SymbolicLinkValue", 0, &type, 0, &len) && type == REG_LINK && !NtDeleteKey(link)) {
            wsprintfA(line, "deleted %s\\%s\r\n", gpu_name, names[i]);
            out(line);
            deleted++;
        }
        RegCloseKey(link);
    }
    return deleted;
}

void start_entry(void)
{
    char display[256], gpu_name[64], line[128];
    HKEY video, gpu;
    int deleted = 0;

    /* Without DISPLAY this process would rebuild the list from Wine's fake 1024x768 display. */
    if (!GetEnvironmentVariableA("DISPLAY", display, sizeof(display))) {
        out("displayfix: DISPLAY is not set, refusing to touch the display registry\r\n");
        ExitProcess(2);
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, video_path, 0, KEY_READ, &video)) {
        out("displayfix: no display registry yet\r\n");
        ExitProcess(0);
    }
    for (DWORD i = 0;; i++) {
        DWORD len = sizeof(gpu_name);
        if (RegEnumKeyExA(video, i, gpu_name, &len, 0, 0, 0, 0)) break;
        if (RegOpenKeyExA(video, gpu_name, 0, KEY_READ, &gpu)) continue;
        deleted += delete_slot_links(gpu, gpu_name);
        RegCloseKey(gpu);
    }
    RegCloseKey(video);
    wsprintfA(line, "displayfix: deleted %d display slot links\r\n", deleted);
    out(line);
    ExitProcess(0);
}
