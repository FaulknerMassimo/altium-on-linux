/*
 * Native side of tests/test_fdguard_wine.py: preloaded after the fd guard, in the test
 * program only (its command line names fdguard_wine.exe). A thread waits for the trigger
 * file, then calls close() on every pipe in the process, the way a native double close
 * hits Wine's server pipes, and writes "closed N, survived M" to the done file.
 *
 * With FDPOKE_MODE=alert, the thread instead puts a /dev/null "canary" on every free fd
 * number below 32768 and creates the ready file; the program then runs threads that make
 * an alertable wait and exit, and creates the check file. The thread counts the canaries
 * that were closed under it and writes "canaries: closed N of M"; the rest stay open.
 *
 * Environment: FDPOKE_TRIGGER, FDPOKE_DONE, FDPOKE_READY, FDPOKE_CHECK (host paths of the
 * same files), FDPOKE_MODE.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define CANARY_FDS 32768

static void wait_for(const char *path)
{
    for (int i = 0; i < 60000 && access(path, F_OK); i += 10) usleep(10000);
}

static void write_file(const char *path, const char *text, int n)
{
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out >= 0) { write(out, text, n); close(out); }
}

static void *canaries(void *arg)
{
    (void)arg;
    static unsigned char laid[CANARY_FDS];
    struct rlimit lim;
    if (!getrlimit(RLIMIT_NOFILE, &lim) && lim.rlim_cur < CANARY_FDS + 1024) {
        lim.rlim_cur = lim.rlim_max < CANARY_FDS + 1024 ? lim.rlim_max : CANARY_FDS + 1024;
        setrlimit(RLIMIT_NOFILE, &lim);
    }
    wait_for(getenv("FDPOKE_TRIGGER"));
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC), count = 0, closed = 0;
    struct stat null_st, st;
    fstat(null, &null_st);
    for (int fd = 3; fd < CANARY_FDS; fd++)
        if (fcntl(fd, F_GETFD) == -1 && dup2(null, fd) == fd) { laid[fd] = 1; count++; }
    write_file(getenv("FDPOKE_READY"), "", 0);
    wait_for(getenv("FDPOKE_CHECK"));
    for (int fd = 3; fd < CANARY_FDS; fd++) {
        if (!laid[fd]) continue;
        if (fstat(fd, &st) || st.st_ino != null_st.st_ino || st.st_dev != null_st.st_dev) closed++;
    } /* the rest stay, for the closes when the program exits */
    close(null);
    char text[64];
    write_file(getenv("FDPOKE_DONE"), text, snprintf(text, sizeof(text), "canaries: closed %d of %d\n", closed, count));
    return NULL;
}

static void *poke(void *arg)
{
    (void)arg;
    const char *trigger = getenv("FDPOKE_TRIGGER"), *done = getenv("FDPOKE_DONE");
    for (int i = 0; i < 60000 && access(trigger, F_OK); i += 10) usleep(10000);
    int closed = 0, survived = 0;
    for (int fd = 3; fd < 4096; fd++) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISFIFO(st.st_mode)) continue;
        close(fd);
        if (fcntl(fd, F_GETFD) == -1) closed++;
        else survived++;
    }
    char text[64];
    int n = snprintf(text, sizeof(text), "closed %d, survived %d\n", closed, survived);
    int out = open(done, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out >= 0) { write(out, text, n); close(out); }
    return NULL;
}

__attribute__((constructor)) static void init(void)
{
    char cmdline[4096] = "";
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    for (ssize_t i = 0; i < n; i++) if (!cmdline[i]) cmdline[i] = ' ';
    if (!getenv("FDPOKE_TRIGGER") || !getenv("FDPOKE_DONE") || !strstr(cmdline, "fdguard_wine.exe")) return;
    const char *mode = getenv("FDPOKE_MODE");
    int alert = mode && !strcmp(mode, "alert");
    if (alert && (!getenv("FDPOKE_READY") || !getenv("FDPOKE_CHECK"))) return;
    pthread_t thread;
    pthread_create(&thread, NULL, alert ? canaries : poke, NULL);
    pthread_detach(thread);
}
