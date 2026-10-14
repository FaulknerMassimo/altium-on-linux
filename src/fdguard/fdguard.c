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
static unsigned events, ntdll_events, blocked;

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

/* Is fd one of the current thread's own server fds? Returns 1 if so, 0 if its TEB does
 * not hold it, -1 if the thread has no TEB. */
static int own_server_fd(int fd)
{
    unsigned char batch[TEB_BATCH_SIZE];
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

/* own: for ntdll.so's own closes, what own_server_fd() said; 2 for the other reports. */
static void report(const char *what, const char *op, int fd, const void *caller, int own)
{
    int saved = errno;
    unsigned seq = own == 2 ? __atomic_fetch_add(&events, 1, __ATOMIC_RELAXED)
                            : __atomic_fetch_add(&ntdll_events, 1, __ATOMIC_RELAXED);
    if (seq >= (own == 2 ? MAX_EVENTS : MAX_NTDLL_EVENTS)) { errno = saved; return; }
    while (__sync_lock_test_and_set(&log_lock, 1)) ;
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
    struct stat st;
    char target[64] = "closed";
    if (!syscall(SYS_fstat, fd, &st)) snprintf(target, sizeof(target), "%s inode %llu",
                                               S_ISFIFO(st.st_mode) ? "pipe" : "not a pipe", (unsigned long long)st.st_ino);
    Dl_info info = {0};
    dladdr(caller, &info);
    const char *mod = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
    uint64_t age = pipe_born[fd] ? (now_ns() - pipe_born[fd]) / 1000000 : 0;
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    struct tm tm;
    localtime_r(&rt.tv_sec, &tm);
    int n = snprintf(line, sizeof(line),
                     "fd-guard %02d:%02d:%02d %.3f: %s %s fd %d (%s, %llu ms old) in %s pid %d, thread %ld \"%s\" (Wine %04llx%s), "
                     "called from %s+%#lx\n",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, now_ns() / 1e9, what, op, fd, target, (unsigned long long)age, proc,
                     getpid(), (long)syscall(SYS_gettid), thread, (unsigned long long)wine_tid,
                     own == 2 ? "" : own < 0 ? ", no TEB" : ", fd not in its TEB",
                     mod ? mod + 1 : info.dli_fname ? info.dli_fname : "?",
                     (unsigned long)((uintptr_t)caller - (uintptr_t)info.dli_fbase));
    out(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);

    if (seq < (own == 2 ? DETAILED_EVENTS : DETAILED_EVENTS / 10)) {
        void *frames[32];
        int count = backtrace(frames, 32);
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
    if (!blocked) return;
    char line[160];
    int n = snprintf(line, sizeof(line), "fd-guard: pid %d refused %u stray closes of Wine's server pipes\n", getpid(),
                     blocked);
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
            report("ntdll.so, not its own thread's pipe:", op, fd, caller, own);
        unmark(fd);
        return 0;
    }
    if (mode == 1) {
        report("let through stray", op, fd, caller, 2);
        unmark(fd);
        return 0;
    }
    __atomic_fetch_add(&blocked, 1, __ATOMIC_RELAXED);
    report("refused stray", op, fd, caller, 2);
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
    if (refuse(fd, __builtin_return_address(0), "close of")) return 0;
    return syscall(SYS_close, fd);
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
