#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <syscall_throttle.h>

static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso: %s ping\n", program_name);
}

int main(int argc, char *argv[])
{
    int fd;
    int ret;

    if (argc != 2 || strcmp(argv[1], "ping") != 0) {
        print_usage(argv[0]);
        return 1;
    }

    fd = open(ST_DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr,
                "Impossibile aprire %s: %s\n",
                ST_DEVICE_PATH,
                strerror(errno));
        return 1;
    }

    ret = ioctl(fd, ST_IOCTL_PING);
    if (ret == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_PING fallita: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }

    printf("PING completato correttamente.\n");

    if (close(fd) == -1) {
        fprintf(stderr,
                "Chiusura di %s fallita: %s\n",
                ST_DEVICE_PATH,
                strerror(errno));
        return 1;
    }

    return 0;
}
