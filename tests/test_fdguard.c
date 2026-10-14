/* Host test driver for src/fdguard/fdguard.c (run by tests/test_fdguard.py with the guard
 * preloaded): runs the scenario named on the command line and prints what happened to the
 * file descriptors. "Wine" pipes come from fake_server_pipe() in a library named
 * ntdll.so; threads get a fake TEB holding their pipes, which that library's NtCurrentTeb
 * returns, as Wine's do. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int fake_server_pipe(int fds[2]);
int fake_close(int fd);
int fake_receive_fd(int sock);
void fake_set_teb(void *teb);

#define TEB_SELF 0x30
#define TEB_TID 0x48
#define TEB_BATCH 0x2f0

static int is_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

/* Still the file it was? (Its number may now hold the guard's log, reopened.) */
static int same_file(int fd, const struct stat *was)
{
    struct stat st;
    return !fstat(fd, &st) && st.st_ino == was->st_ino && st.st_dev == was->st_dev;
}

static int is_pipe(int fd)
{
    struct stat st;
    return !fstat(fd, &st) && S_ISFIFO(st.st_mode);
}

/* A TEB as Wine's ntdll.so lays out its thread data: the server fds somewhere in
 * GdiTebBatch. */
static unsigned char *make_teb(unsigned tid, const int *fds, int count)
{
    unsigned char *teb = calloc(1, 0x1000);
    memcpy(teb + TEB_SELF, &teb, sizeof(teb));
    memcpy(teb + TEB_TID, &(uint64_t){ tid }, 8);
    memcpy(teb + TEB_BATCH + 0x94, fds, count * sizeof(int));
    return teb;
}

/* Send a fd over a socket, as wineserver does. */
static void send_fd(int sock, int fd)
{
    char buf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { "x", 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    sendmsg(sock, &msg, 0);
}

static void set_teb(unsigned char *teb) { fake_set_teb(teb); }

/* own-exit: a thread closes its own pipes, as Wine's exit_thread does. */
static void *own_exit(void *arg)
{
    (void)arg;
    int p[2];
    fake_server_pipe(p);
    set_teb(make_teb(0x111, p, 2));
    int r0 = fake_close(p[0]), r1 = fake_close(p[1]);
    printf("own-exit: close %d %d, open %d %d\n", r0, r1, is_open(p[0]), is_open(p[1]));
    return NULL;
}

/* A thread that keeps its pipes in its TEB until told to finish. */
static int other_pipe[2];
static pthread_t other_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int stage;

static void *other(void *arg)
{
    (void)arg;
    fake_server_pipe(other_pipe);
    set_teb(make_teb(0x222, other_pipe, 2));
    pthread_mutex_lock(&lock);
    stage = 1;
    pthread_cond_broadcast(&cond);
    while (stage < 2) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void start_other(void)
{
    pthread_create(&other_thread, NULL, other, NULL);
    pthread_mutex_lock(&lock);
    while (stage < 1) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
}

static void stop_other(void)
{
    pthread_mutex_lock(&lock);
    stage = 2;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(other_thread, NULL);
}

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "";
    int p[2];
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!strcmp(s, "stray")) { /* also "log" and "off", with ALTIUM_FD_GUARD set */
        fake_server_pipe(p);
        int r = close(p[0]);
        printf("stray: close %d, open %d, pipe %d\n", r, is_open(p[0]), is_pipe(p[0]));
    } else if (!strcmp(s, "fresh")) {
        fake_server_pipe(p);
        printf("fresh: close %d, open %d\n", fake_close(p[0]), is_open(p[0]));
    } else if (!strcmp(s, "own-exit")) {
        pthread_t t;
        pthread_create(&t, NULL, own_exit, NULL);
        pthread_join(t, NULL);
    } else if (!strcmp(s, "other-thread")) {
        /* ntdll.so code on another Wine thread closes the first thread's pipe. */
        start_other();
        int mine[2];
        fake_server_pipe(mine);
        set_teb(make_teb(0x333, mine, 2));
        int r = fake_close(other_pipe[1]);
        printf("other-thread: close %d, open %d\n", r, is_open(other_pipe[1]));
        stop_other();
    } else if (!strcmp(s, "no-teb")) {
        /* ntdll.so code on a thread without a TEB (a native one) closes a Wine thread's pipe. */
        start_other();
        int r = fake_close(other_pipe[0]);
        printf("no-teb: close %d, open %d\n", r, is_open(other_pipe[0]));
        stop_other();
    } else if (!strcmp(s, "bad-teb")) {
        /* Before ntdll.so sets its key, NtCurrentTeb can return some other library's value. */
        start_other();
        unsigned char *junk = make_teb(0x444, other_pipe, 2);
        memset(junk + TEB_SELF, 0, 8);
        set_teb(junk);
        int r = fake_close(other_pipe[0]);
        printf("bad-teb: close %d, open %d\n", r, is_open(other_pipe[0]));
        stop_other();
    } else if (!strcmp(s, "stale")) {
        /* The pipe went away unseen; its number now holds another file. */
        fake_server_pipe(p);
        syscall(SYS_close, p[0]);
        int fd = open("/dev/null", O_RDONLY);
        int r = close(fd);
        printf("stale: same number %d, close %d, open %d\n", fd == p[0], r, is_open(fd));
    } else if (!strcmp(s, "dup")) {
        fake_server_pipe(p);
        int fd = open("/dev/null", O_RDONLY);
        int r2 = dup2(fd, p[0]), e2 = errno;
        int r3 = dup3(fd, p[1], O_CLOEXEC), e3 = errno;
        printf("dup: dup2 %d %s, dup3 %d %s, pipes %d %d\n", r2, e2 == EBUSY ? "EBUSY" : "?", r3,
               e3 == EBUSY ? "EBUSY" : "?", is_pipe(p[0]), is_pipe(p[1]));
        int other = open("/dev/null", O_RDONLY);
        printf("dup: dup2 onto a plain fd %d\n", dup2(fd, other) == other);
    } else if (!strcmp(s, "close-range")) {
        int a[2], b[2], plain[3];
        struct stat null;
        stat("/dev/null", &null);
        fake_server_pipe(a);
        for (int i = 0; i < 3; i++) plain[i] = open("/dev/null", O_RDONLY);
        fake_server_pipe(b);
        int r = close_range(3, ~0u, 0);
        printf("close-range: %d, pipes %d %d %d %d, plain %d %d %d\n", r, is_pipe(a[0]), is_pipe(a[1]), is_pipe(b[0]),
               is_pipe(b[1]), same_file(plain[0], &null), same_file(plain[1], &null), same_file(plain[2], &null));
        int c = open("/dev/null", O_RDONLY);
        closefrom(3);
        printf("closefrom: pipes %d %d %d %d, plain %d\n", is_pipe(a[0]), is_pipe(a[1]), is_pipe(b[0]), is_pipe(b[1]),
               same_file(c, &null));
    } else if (!strcmp(s, "received")) {
        /* The first thread's request pipe arrives over the socket before ntdll.so makes
         * a pipe; fds received after that are files for handles, not protected. */
        int sock[2], q[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sock);
        pipe(q);
        send_fd(sock[0], q[1]);
        int first = fake_receive_fd(sock[1]);
        int r1 = close(first);
        fake_server_pipe(p);
        send_fd(sock[0], q[0]);
        int later = fake_receive_fd(sock[1]);
        int r2 = close(later);
        printf("received: first close %d open %d, later close %d open %d\n", r1, is_open(first), r2, is_open(later));
    } else if (!strcmp(s, "fork")) {
        fake_server_pipe(p);
        pid_t child = fork();
        if (!child) _exit(close(p[0]) == 0 && !is_open(p[0]) ? 0 : 1);
        int status;
        waitpid(child, &status, 0);
        printf("fork: child closed %d, parent open %d\n", WIFEXITED(status) && !WEXITSTATUS(status), is_open(p[0]));
    } else {
        fprintf(stderr, "unknown scenario %s\n", s);
        return 2;
    }
    return 0;
}
