/*
 * Native side of tests/test_fdguard_wine.py: preloaded after the fd guard, in the test
 * program only (its command line names fdguard_wine.exe). A thread waits for the trigger
 * file, then calls close() on every pipe in the process, the way a native double close
 * hits Wine's server pipes, and writes "closed N, survived M" to the done file.
 *
 * Environment: FDPOKE_TRIGGER, FDPOKE_DONE (host paths of the same files).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    pthread_t thread;
    pthread_create(&thread, NULL, poke, NULL);
    pthread_detach(thread);
}
