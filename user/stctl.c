#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <syscall_throttle.h>

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Uso:\n"
            "  %s ping\n"
            "  %s status\n"
            "  %s enable\n"
            "  %s disable\n",
            program_name,
            program_name,
            program_name,
            program_name);
}

static int execute_ping(int fd)
{
    if (ioctl(fd, ST_IOCTL_PING) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_PING fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("PING completato correttamente.\n");
    return 0;
}

static int execute_status(int fd)
{
    struct st_monitor_status status = {
        .enabled = 0U,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_GET_STATUS, &status) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_GET_STATUS fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("Monitor: %s\n",
           status.enabled != 0U ? "attivo" : "disattivato");

    return 0;
}

static int execute_enable(int fd)
{
    if (ioctl(fd, ST_IOCTL_ENABLE) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_ENABLE fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("Monitor attivato.\n");
    return 0;
}

static int execute_disable(int fd)
{
    if (ioctl(fd, ST_IOCTL_DISABLE) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_DISABLE fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("Monitor disattivato.\n");
    return 0;
}

static int is_valid_command(const char *command)
{
    return strcmp(command, "ping") == 0 ||
           strcmp(command, "status") == 0 ||
           strcmp(command, "enable") == 0 ||
           strcmp(command, "disable") == 0;
}

static int execute_command(int fd, const char *command)
{
    if (strcmp(command, "ping") == 0)
        return execute_ping(fd);

    if (strcmp(command, "status") == 0)
        return execute_status(fd);

    if (strcmp(command, "enable") == 0)
        return execute_enable(fd);

    return execute_disable(fd);
}

int main(int argc, char *argv[])
{
    int fd;
    int result;

    if (argc != 2 || !is_valid_command(argv[1])) {
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

    result = execute_command(fd, argv[1]);

    if (close(fd) == -1) {
        fprintf(stderr,
                "Chiusura di %s fallita: %s\n",
                ST_DEVICE_PATH,
                strerror(errno));
        return 1;
    }

    return result;
}
