#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "protocol.h"

_Static_assert(SYS_read == 0 && SYS_getpid == 39 && SYS_getppid == 110,
               "La demo richiede l'ABI nativa x86-64");
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(90);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void send_result(int fd, const struct demo_result *r)
{
    const char *p = (const char *)r;
    size_t left = sizeof(*r);
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(91);
        p += n;
        left -= (size_t)n;
    }
}
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    int gate = atoi(argv[1]), output = atoi(argv[2]);
    int number = atoi(argv[3]), data = atoi(argv[4]);
    if (number != SYS_getpid && number != SYS_getppid && number != SYS_read)
        return 2;
    struct demo_result r = {.kind = DEMO_READY, .euid = (uint32_t)geteuid()};
    send_result(output, &r);
    char byte;
    /* recv non usa SYS_read: il gate resta fuori dalla syscall dimostrata. */
    if (recv(gate, &byte, 1, MSG_WAITALL) != 1 || byte != 'G') return 3;
    r.kind = DEMO_RESULT;
    r.start_ns = now_ns();
    errno = 0;
    r.value = number == SYS_read ? syscall(SYS_read, data, &byte, 1)
                                 : syscall(number);
    r.error = r.value < 0 ? errno : 0;
    r.end_ns = now_ns();
    send_result(output, &r);
    if (r.error || (number == SYS_read && (r.value != 1 || byte != 'X')))
        return 4;
    return 0;
}
