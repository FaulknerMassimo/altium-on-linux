/*
 * LD_PRELOAD library for the Wine processes in the bottle: keeps other code from closing
 * the pipes Wine's threads use to talk to wineserver.
 *
 * Every Wine thread has three pipes to wineserver (requests, replies, waits), which
 * ntdll.so creates with pipe2() when the thread starts and closes when it ends (the first
 * thread's request pipe comes from wineserver instead, over its socket). Some
 * native code in Altium's process sometimes closes a file descriptor it does not own
 * (a double close: the number had been freed and handed to a new thread's pipe). The
 * thread that owned the pipe then fails (`wine client error: read: Bad file descriptor`),
 * or, worse, the number is reused once more for another thread's pipe and the first
 * thread waits for a reply that goes to the other one. wineserver gives up on such a
 * thread, but if it held a lock, everything that needs the lock stops: a frozen Altium
 * (docs/how-it-works.md §20).
 *
 * The library remembers the pipes ntdll.so creates or, before it creates any, receives
 * (by inode), and refuses close(),
 * dup2(), dup3() and close_range() on them from any other code while they are open:
 * close() returns 0 without closing, dup2() and dup3() fail with EBUSY, close_range()
 * skips them. Each refusal is logged with a backtrace, which names the code that tried.
 * ntdll.so's own closes always go through; one that is not the thread closing its own
 * pipes (they are in its TEB, which ntdll's NtCurrentTeb finds) or a pipe that is only
 * seconds old is logged, so that a close through Wine itself (its handle-to-fd cache)
 * shows up too.
 *
 * A close() that fails with EBADF is logged too: that is the double close that did no
 * harm this time, because nothing had the number yet. One backtrace per call chain, then
 * a line at its 2nd, 4th, 8th... time. It names the culprit long before it hits a pipe.
 *
 * Two such closes are Wine's own (soda-11.0-7, ValveSoftware/wine), and the library skips
 * them, logging each one that would have closed an open fd:
 * - Under fsync, a thread's alert_fd (right after its server fds in the TEB) holds an
 *   fsync shared-memory index, not a fd, and the thread's exit closes it anyway:
 *   whatever fd has that number goes, another thread's server pipe included. Skipped
 *   unless the fd is an ntsync object (alert_fd's real meaning under ntsync); once a
 *   thread exit has been seen, only from that call site.
 * - crypt32.so fclose()s the certificate bundle, then close()s its fd again.
 *
 * Only closes through the libc functions are seen: glibc's internal ones (fclose and
 * the like) and raw syscalls are not. A forked child is left alone.
 *
 * Environment:
 *   ALTIUM_FD_GUARD=log       log stray closes but let them through; =off: do nothing
 *   ALTIUM_FD_GUARD_LOG=FILE  log file (default $WINEPREFIX/altium-on-linux/logs/fd-guard.log);
 *                             every line also goes to stderr
 *   ALTIUM_FD_GUARD_FRESH_MS  age below which ntdll.so closing a pipe is not logged (5000)
 *
 * Build: scripts/build.sh. Loaded inside the Bottles Flatpak, so it uses only old glibc
 * symbols, like the Vulkan layer; system calls go straight to the kernel.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

#define MAX_FDS 65536
#define CLOSE_RANGE_CLOEXEC (1U << 2)
/* x86-64 TEB: ntdll.so keeps its per-thread data (the server fds among them) in
 * GdiTebBatch. */
#define TEB_SELF 0x30
#define TEB_TID 0x48
#define TEB_BATCH 0x2f0
#define TEB_BATCH_SIZE 0x4e8
#define DETAILED_EVENTS 100 /* with a backtrace; after that one line each, up to MAX_EVENTS */
#define MAX_EVENTS 10000
#define MAX_NTDLL_EVENTS 200 /* ntdll.so's own odd closes, counted apart so they cannot crowd out refusals */
#define MAX_CHAINS 256       /* call chains of closes that failed with EBADF, each logged with a backtrace once */
#define DETAILED_SKIPS 3     /* Wine's own bad closes are always the same code: few backtraces */
#define CHAIN_FRAMES 8

/* Inode of the pipe ntdll.so created behind each fd (0: none), and when (ns). */
static uint64_t pipe_ino[MAX_FDS];
static uint64_t pipe_born[MAX_FDS];
static uintptr_t ntdll_lo, ntdll_hi; /* ntdll.so's code, once it has created a pipe */
static void *(*nt_current_teb)(void); /* ntdll.so's: the calling thread's TEB, from a pthread key */
static pid_t guard_pid;              /* process the marks belong to */
static int made_pipes;               /* ntdll.so has created a pipe: start-up is over */
static int mode;                     /* 0 block, 1 log only, 2 off */
static uint64_t fresh_ns = 5000000000ull;
static char log_path[512];
static int log_fd = -1;
static uint64_t log_ino; /* the log file's inode: its fd may be closed under us too */
static volatile int log_lock;
static unsigned events, ntdll_events, skip_events, blocked, skipped;
static size_t server_off; /* TEB offset of a thread's four server fds, once found (0: not yet) */
static __thread int crypt32_fclosed; /* fd + 1 that crypt32.so just closed with fclose() */
static const void *alert_site;                 /* where ntdll.so's thread exit closes alert_fd, once seen */
static __thread const void *alert_candidate;   /* this thread's skipped alert_fd close, until ntdll.so's next */
static uint64_t chain_key[MAX_CHAINS]; /* under log_lock */
static unsigned chain_seen[MAX_CHAINS];

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

__attribute__((constructor)) static void init(void)
{
    const char *m = getenv("ALTIUM_FD_GUARD");
    if (m && !strcmp(m, "log")) mode = 1;
    else if (m && (!strcmp(m, "off") || !strcmp(m, "0"))) mode = 2;
    const char *fresh = getenv("ALTIUM_FD_GUARD_FRESH_MS");
    if (fresh) { /* not strtoull: C23 headers turn it into a glibc 2.38 symbol */
        fresh_ns = 0;
        while (*fresh >= '0' && *fresh <= '9') fresh_ns = fresh_ns * 10 + (uint64_t)(*fresh++ - '0') * 1000000ull;
    }
    const char *path = getenv("ALTIUM_FD_GUARD_LOG"), *prefix = getenv("WINEPREFIX");
    if (path) snprintf(log_path, sizeof(log_path), "%s", path);
    else if (prefix) snprintf(log_path, sizeof(log_path), "%s/altium-on-linux/logs/fd-guard.log", prefix);
}

/* ---- Who is calling ------------------------------------------------------- */

static int find_code(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    if (info->dlpi_addr != *(uintptr_t *)data) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            ntdll_hi = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
            __atomic_store_n(&ntdll_lo, info->dlpi_addr + ph->p_vaddr, __ATOMIC_RELEASE);
        }
    }
    return 1;
}

static int in_ntdll(const void *addr)
{
    uintptr_t lo = __atomic_load_n(&ntdll_lo, __ATOMIC_ACQUIRE);
    if (lo) return lo <= (uintptr_t)addr && (uintptr_t)addr < ntdll_hi;
    Dl_info info;
    if (!dladdr(addr, &info) || !info.dli_fname) return 0;
    const char *base = strrchr(info.dli_fname, '/');
    if (strcmp(base ? base + 1 : info.dli_fname, "ntdll.so")) return 0;
    uintptr_t fbase = (uintptr_t)info.dli_fbase;
    dl_iterate_phdr(find_code, &fbase);
    void *lib = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (lib) {
        nt_current_teb = (void *(*)(void))dlsym(lib, "NtCurrentTeb");
        dlclose(lib);
    }
    return 1;
}

/* Read the current thread's TEB, if it has one. On the Unix side, ntdll.so finds it in a
 * pthread key, not in the GS base, which is already 0 while a thread exits; native
 * threads have none. Before ntdll.so has set the key, it may return another library's
 * value, so the TEB must point at itself, and the reads are checked by the kernel. */
/* Copy from our own memory, failing instead of faulting. syscall() takes its arguments
 * as longs: an int in the stack slot of the last one (flags) leaves garbage above it. */
static int read_self(void *dst, uintptr_t src, size_t size)
{
    struct iovec local = { dst, size }, remote = { (void *)src, size };
    return syscall(SYS_process_vm_readv, (long)getpid(), &local, 1ul, &remote, 1ul, 0ul) == (long)size;
}

static int read_teb(size_t offset, void *buf, size_t size)
{
    uintptr_t teb = nt_current_teb ? (uintptr_t)nt_current_teb() : 0, self = 0;
    return teb && read_self(&self, teb + TEB_SELF, sizeof(self)) && self == teb && read_self(buf, teb + offset, size);
}

static int marked(int fd);

/* TEB offset of the calling thread's four server fds (request, reply, wait[0], wait[1];
 * TEB+0x384 in soda-11.0-7): four ints in a row that are pipes ntdll.so made. Found once,
 * from a thread whose pipes are all open; 0 until then. */
static size_t server_fds_offset(void)
{
    size_t off = __atomic_load_n(&server_off, __ATOMIC_RELAXED);
    unsigned char batch[TEB_BATCH_SIZE];
    if (off || !read_teb(TEB_BATCH, batch, sizeof(batch))) return off;
    for (size_t i = 0; i + 5 * sizeof(int) <= sizeof(batch); i += sizeof(int)) {
        int v[4];
        memcpy(v, batch + i, sizeof(v));
        if (v[0] != v[1] && v[1] != v[2] && v[2] != v[3] && marked(v[0]) && marked(v[1]) && marked(v[2]) &&
            marked(v[3])) {
            __atomic_store_n(&server_off, TEB_BATCH + i, __ATOMIC_RELAXED);
            return TEB_BATCH + i;
        }
    }
    return 0;
}

/* Is fd one of the current thread's own server fds? Returns 1 if so, 0 if its TEB does
 * not hold it, -1 if the thread has no TEB. Until the slots are known, any int in the
 * thread's data counts. */
static int own_server_fd(int fd)
{
    unsigned char batch[TEB_BATCH_SIZE];
    size_t off = server_fds_offset();
    if (off) {
        int v[4];
        if (!read_teb(off, v, sizeof(v))) return -1;
        return v[0] == fd || v[1] == fd || v[2] == fd || v[3] == fd;
    }
    if (!read_teb(TEB_BATCH, batch, sizeof(batch))) return -1;
    for (size_t i = 0; i + sizeof(int) <= sizeof(batch); i += sizeof(int)) {
        int v;
        memcpy(&v, batch + i, sizeof(v));
        if (v == fd) return 1;
    }
    return 0;
}

/* ---- Log ------------------------------------------------------------------- */

/* Open the log file, or reopen it if its fd no longer refers to it. Under log_lock. */
static void open_log(void)
{
    struct stat st;
    if (log_fd >= 0 && !syscall(SYS_fstat, log_fd, &st) && st.st_ino == log_ino) return;
    log_fd = -1;
    if (!log_path[0]) return;
    int fd = syscall(SYS_openat, AT_FDCWD, log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (syscall(SYS_fstat, fd, &st)) { syscall(SYS_close, fd); return; }
    log_fd = fd;
    log_ino = st.st_ino;
}

static void out(const char *s, size_t n)
{
    if (log_fd >= 0) syscall(SYS_write, log_fd, s, n);
    syscall(SYS_write, 2, s, n);
}

/* How many times this call chain has closed a closed fd, this time included (0: the
 * table is full). Under log_lock. */
static unsigned chain_seen_count(void *const *frames, int count)
{
    uint64_t key = 14695981039346656037ull; /* FNV-1a */
    for (int i = 0; i < count && i < CHAIN_FRAMES; i++) key = (key ^ (uintptr_t)frames[i]) * 1099511628211ull;
    for (unsigned i = 0; i < MAX_CHAINS; i++) {
        if (!chain_key[i]) chain_key[i] = key;
        if (chain_key[i] == key) return ++chain_seen[i];
    }
    return 0;
}

/* What fd is now: "pipe inode N" (with its age if ntdll.so made it), or where it points. */
static void describe_fd(int fd, char *buf, size_t size)
{
    struct stat st;
    char path[32];
    if (syscall(SYS_fstat, fd, &st)) { snprintf(buf, size, "closed"); return; }
    if (S_ISFIFO(st.st_mode) && (unsigned)fd < MAX_FDS && pipe_ino[fd] == st.st_ino)
        snprintf(buf, size, "pipe inode %llu, %llu ms old", (unsigned long long)st.st_ino,
                 (unsigned long long)((now_ns() - pipe_born[fd]) / 1000000));
    else if (S_ISFIFO(st.st_mode)) snprintf(buf, size, "pipe inode %llu", (unsigned long long)st.st_ino);
    else {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        long n = syscall(SYS_readlinkat, AT_FDCWD, path, buf, size - 1);
        buf[n > 0 ? n : 0] = 0;
        if (n <= 0) snprintf(buf, size, "not a pipe inode %llu", (unsigned long long)st.st_ino);
    }
}

enum kind {
    STRAY,         /* other code closing a server pipe */
    NTDLL_NOT_OWN, /* ntdll.so closing a server pipe that is not the thread's own */
    NTDLL_NO_TEB,  /* the same on a thread without a TEB */
    DOUBLE_CLOSE,  /* a close that failed with EBADF */
    SKIPPED,       /* one of Wine's own bad closes, skipped */
};

static void report(const char *what, const char *op, int fd, const void *caller, enum kind kind)
{
    int saved = errno;
    unsigned seq = 0, max = 0, detail = 0;
    switch (kind) {
    case STRAY: seq = __atomic_fetch_add(&events, 1, __ATOMIC_RELAXED); max = MAX_EVENTS; detail = DETAILED_EVENTS; break;
    case NTDLL_NOT_OWN:
    case NTDLL_NO_TEB:
        seq = __atomic_fetch_add(&ntdll_events, 1, __ATOMIC_RELAXED);
        max = MAX_NTDLL_EVENTS;
        detail = DETAILED_EVENTS / 10;
        break;
    case SKIPPED: seq = __atomic_fetch_add(&skip_events, 1, __ATOMIC_RELAXED); max = MAX_EVENTS; detail = DETAILED_SKIPS; break;
    case DOUBLE_CLOSE: max = 1; break;
    }
    if (seq >= max) { errno = saved; return; }
    void *frames[32];
    int count = kind == DOUBLE_CLOSE ? backtrace(frames, 32) : 0; /* outside the lock: it may load libgcc_s */
    int detailed = seq < detail;
    unsigned seen = 0;
    while (__sync_lock_test_and_set(&log_lock, 1)) ;
    if (kind == DOUBLE_CLOSE) {
        seen = chain_seen_count(frames + 1, count - 1);
        if (!seen || (seen & (seen - 1))) { __sync_lock_release(&log_lock); errno = saved; return; } /* 1, 2, 4, 8... */
        detailed = seen == 1;
    }
    open_log();

    char proc[17] = "", thread[17] = "", line[640];
    int pfd = syscall(SYS_openat, AT_FDCWD, "/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (pfd >= 0) {
        long n = syscall(SYS_read, pfd, proc, sizeof(proc) - 1);
        proc[n > 0 ? n - 1 : 0] = 0; /* drop the newline */
        syscall(SYS_close, pfd);
    }
    prctl(PR_GET_NAME, thread);
    uint64_t wine_tid = 0;
    if (!read_teb(TEB_TID, &wine_tid, sizeof(wine_tid))) wine_tid = 0;
    char target[80];
    if (kind == DOUBLE_CLOSE && seen == 1) snprintf(target, sizeof(target), "first time from this call chain");
    else if (kind == DOUBLE_CLOSE) snprintf(target, sizeof(target), "%u times from this call chain", seen);
    else describe_fd(fd, target, sizeof(target));
    Dl_info info = {0};
    dladdr(caller, &info);
    const char *mod = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    struct tm tm;
    localtime_r(&rt.tv_sec, &tm);
    int n = snprintf(line, sizeof(line),
                     "fd-guard %02d:%02d:%02d %.3f: %s %s fd %d (%s) in %s pid %d, thread %ld \"%s\" (Wine %04llx%s), "
                     "called from %s+%#lx\n",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, now_ns() / 1e9, what, op, fd, target, proc,
                     getpid(), (long)syscall(SYS_gettid), thread, (unsigned long long)wine_tid,
                     kind == NTDLL_NO_TEB ? ", no TEB" : kind == NTDLL_NOT_OWN ? ", fd not in its TEB" : "",
                     mod ? mod + 1 : info.dli_fname ? info.dli_fname : "?",
                     (unsigned long)((uintptr_t)caller - (uintptr_t)info.dli_fbase));
    out(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);

    if (detailed) {
        if (!count) count = backtrace(frames, 32);
        for (int i = 1; i < count; i++) { /* frame 0 is report() */
            Dl_info fi = {0};
            dladdr(frames[i], &fi);
            const char *fm = fi.dli_fname ? strrchr(fi.dli_fname, '/') : NULL;
            n = snprintf(line, sizeof(line), "fd-guard     #%d %s+%#lx%s%s%s\n", i,
                         fm ? fm + 1 : fi.dli_fname ? fi.dli_fname : "?",
                         (unsigned long)((uintptr_t)frames[i] - (uintptr_t)fi.dli_fbase),
                         fi.dli_sname ? " (" : "", fi.dli_sname ? fi.dli_sname : "", fi.dli_sname ? ")" : "");
            out(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
        }
    }
    __sync_lock_release(&log_lock);
    errno = saved;
}

__attribute__((destructor)) static void fini(void)
{
    if (!blocked && !skipped) return;
    char line[200];
    int n = snprintf(line, sizeof(line),
                     "fd-guard: pid %d refused %u stray closes of Wine's server pipes, skipped %u bad closes by Wine\n",
                     getpid(), blocked, skipped);
    while (__sync_lock_test_and_set(&log_lock, 1)) ;
    open_log();
    out(line, n);
    __sync_lock_release(&log_lock);
}

/* ---- Marks ----------------------------------------------------------------- */

static void mark(int fd)
{
    struct stat st;
    if ((unsigned)fd >= MAX_FDS || syscall(SYS_fstat, fd, &st) || !S_ISFIFO(st.st_mode)) return;
    if (!guard_pid) guard_pid = getpid();
    pipe_born[fd] = now_ns();
    __atomic_store_n(&pipe_ino[fd], (uint64_t)st.st_ino, __ATOMIC_RELEASE);
}

/* Is fd still the pipe ntdll.so created behind it? A mark outlives a pipe closed some way
 * this library does not see; the inode tells. */
static int marked(int fd)
{
    if ((unsigned)fd >= MAX_FDS) return 0;
    uint64_t ino = __atomic_load_n(&pipe_ino[fd], __ATOMIC_ACQUIRE);
    if (!ino || getpid() != guard_pid) return 0;
    struct stat st;
    if (!syscall(SYS_fstat, fd, &st) && S_ISFIFO(st.st_mode) && st.st_ino == ino) return 1;
    __atomic_store_n(&pipe_ino[fd], 0, __ATOMIC_RELEASE);
    return 0;
}

static void unmark(int fd)
{
    __atomic_store_n(&pipe_ino[fd], 0, __ATOMIC_RELEASE);
}

/* Should the caller be kept from closing (or replacing) fd? */
static int refuse(int fd, const void *caller, const char *op)
{
    if (mode == 2 || !marked(fd)) return 0;
    if (in_ntdll(caller)) {
        int own;
        if (now_ns() - pipe_born[fd] >= fresh_ns && (own = own_server_fd(fd)) != 1)
            report("ntdll.so, not its own thread's pipe:", op, fd, caller, own < 0 ? NTDLL_NO_TEB : NTDLL_NOT_OWN);
        unmark(fd);
        return 0;
    }
    if (mode == 1) {
        report("let through stray", op, fd, caller, STRAY);
        unmark(fd);
        return 0;
    }
    __atomic_fetch_add(&blocked, 1, __ATOMIC_RELAXED);
    report("refused stray", op, fd, caller, STRAY);
    return 1;
}

/* ---- Wine's own bad closes ---------------------------------------------------- */

static int in_module(const void *addr, const char *name)
{
    Dl_info info;
    if (!dladdr(addr, &info) || !info.dli_fname) return 0;
    const char *base = strrchr(info.dli_fname, '/');
    return !strcmp(base ? base + 1 : info.dli_fname, name);
}

/* Is this ntdll.so's thread exit closing the thread's alert_fd while it holds an fsync
 * index (get_inproc_alert_fd() stores reply->fsync_shm_idx in it)? alert_fd follows the
 * four server fds. Under ntsync it is an ntsync event, and closing it is right.
 * Once a thread exit has been seen (the alert_fd close, then the thread's own wait_fd[0]),
 * only closes from that call site count: other closes by ntdll.so of the same number are
 * Wine's fd cache at work. */
static int closes_fsync_index(int fd, const void *caller)
{
    size_t off;
    int alert;
    char link[64], path[32];
    const void *site = __atomic_load_n(&alert_site, __ATOMIC_RELAXED);
    if ((site && caller != site) || !in_ntdll(caller) || !(off = server_fds_offset()) ||
        !read_teb(off + 4 * sizeof(int), &alert, sizeof(alert)) || alert != fd)
        return 0;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    long n = syscall(SYS_readlinkat, AT_FDCWD, path, link, sizeof(link) - 1);
    link[n > 0 ? n : 0] = 0;
    if (strstr(link, "ntsync")) return 0;
    if (!site) alert_candidate = caller;
    return 1;
}

/* After a skipped alert_fd close, ntdll.so's next close on the thread: if it is the
 * thread's own wait_fd[0], that was the thread exit, and its call site is the one. */
static void confirm_alert_site(int fd, const void *caller)
{
    int wait0;
    size_t off = server_fds_offset();
    if (caller != alert_candidate && in_ntdll(caller) && off && read_teb(off + 2 * sizeof(int), &wait0, sizeof(wait0)) &&
        wait0 == fd)
        __atomic_store_n(&alert_site, alert_candidate, __ATOMIC_RELAXED);
    alert_candidate = NULL;
}

/* Skip one of Wine's bad closes (in log mode, let it through). Logged when the fd is open:
 * then it was some other code's. */
static int skip_close(int fd, const void *caller, const char *what)
{
    char line[160];
    snprintf(line, sizeof(line), "%s %s:", mode == 1 ? "let through" : "skipped", what);
    if (syscall(SYS_fcntl, fd, F_GETFD) != -1) report(line, "close of", fd, caller, SKIPPED);
    if (mode == 1) return 0;
    __atomic_fetch_add(&skipped, 1, __ATOMIC_RELAXED);
    return 1;
}

/* ---- Interposed functions ---------------------------------------------------- */

static int made_pipe(int ret, int fds[2], const void *caller)
{
    if (!ret && mode != 2 && in_ntdll(caller)) { mark(fds[0]); mark(fds[1]); made_pipes = 1; }
    return ret;
}

EXPORT int pipe2(int fds[2], int flags)
{
    return made_pipe(syscall(SYS_pipe2, fds, flags), fds, __builtin_return_address(0));
}

EXPORT int pipe(int fds[2])
{
    return made_pipe(syscall(SYS_pipe2, fds, 0), fds, __builtin_return_address(0));
}

/* The first thread's request pipe: the first fd ntdll.so receives from wineserver. */
EXPORT ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    ssize_t ret = syscall(SYS_recvmsg, fd, msg, flags);
    if (ret < 0 || made_pipes || mode == 2) return ret;
    /* Who called only when fds came: other processes (wineserver, X clients) never stop
     * asking, and dladdr is slow. */
    int checked = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        if (!checked++ && !in_ntdll(__builtin_return_address(0))) return ret;
        for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
            int received;
            memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            mark(received);
        }
    }
    return ret;
}

EXPORT int close(int fd)
{
    const void *caller = __builtin_return_address(0);
    if (fd >= 0 && mode != 2 && guard_pid && getpid() == guard_pid) {
        if (crypt32_fclosed && crypt32_fclosed == fd + 1) {
            crypt32_fclosed = 0;
            if (in_module(caller, "crypt32.so") && skip_close(fd, caller, "crypt32.so's second close of a fclose()d fd"))
                return 0;
        }
        if (alert_candidate) confirm_alert_site(fd, caller);
        if (closes_fsync_index(fd, caller) && skip_close(fd, caller, "ntdll.so's thread-exit close of an fsync index (alert_fd)"))
            return 0;
    }
    if (refuse(fd, caller, "close of")) return 0;
    long ret = syscall(SYS_close, fd);
    /* Only in a Wine process (it has made server pipes), not in a forked child that closes
     * every number before exec. */
    if (ret && errno == EBADF && fd >= 0 && mode != 2 && guard_pid && getpid() == guard_pid)
        report("double close:", "close of", fd, caller, DOUBLE_CLOSE);
    return ret;
}

/* crypt32.so's import_certs_from_file() fclose()s the fd that import_certs_from_path()
 * then closes: note the fd for close(). */
EXPORT int fclose(FILE *fp)
{
    static int (*real_fclose)(FILE *);
    if (!real_fclose) real_fclose = (int (*)(FILE *))dlsym(RTLD_NEXT, "fclose");
    int fd = mode != 2 ? fileno(fp) : -1;
    int ret = real_fclose(fp);
    if (fd >= 0 && guard_pid && in_module(__builtin_return_address(0), "crypt32.so")) crypt32_fclosed = fd + 1;
    return ret;
}

EXPORT int dup2(int old, int new)
{
    if (old != new && refuse(new, __builtin_return_address(0), "dup2 onto")) { errno = EBUSY; return -1; }
    return syscall(SYS_dup2, old, new);
}

EXPORT int dup3(int old, int new, int flags)
{
    if (old != new && refuse(new, __builtin_return_address(0), "dup3 onto")) { errno = EBUSY; return -1; }
    return syscall(SYS_dup3, old, new, flags);
}

static int close_range_from(unsigned first, unsigned last, int flags, const void *caller)
{
    if (mode == 2 || (flags & CLOSE_RANGE_CLOEXEC) || first > last) return syscall(SYS_close_range, first, last, flags);
    unsigned start = first, end = last < MAX_FDS - 1 ? last : MAX_FDS - 1;
    for (unsigned fd = first; fd <= end; fd++) {
        if (!pipe_ino[fd] || !refuse(fd, caller, "close_range of")) continue;
        if (fd > start && syscall(SYS_close_range, start, fd - 1, flags)) return -1;
        start = fd + 1;
    }
    return start > last ? 0 : syscall(SYS_close_range, start, last, flags);
}

EXPORT int close_range(unsigned first, unsigned last, int flags)
{
    return close_range_from(first, last, flags, __builtin_return_address(0));
}

EXPORT void closefrom(int lowfd)
{
    if (lowfd < 0) lowfd = 0;
    close_range_from(lowfd, ~0u, 0, __builtin_return_address(0));
}
