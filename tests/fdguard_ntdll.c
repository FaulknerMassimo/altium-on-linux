/* Stand-in for Wine's ntdll.so in tests/test_fdguard.py: built as a library named
 * ntdll.so, so the fd guard takes its pipes and closes for Wine's own. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int fake_server_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC); }
int fake_close(int fd) { return close(fd); }

/* receive_fd: one fd sent over a socket, as wineserver sends the first request pipe. */
int fake_receive_fd(int sock)
{
    char byte, buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { &byte, 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf) };
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&msg)), sizeof(fd));
    return fd;
}
