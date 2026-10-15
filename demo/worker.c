#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Static_assert(SYS_read == 0 && SYS_getpid == 39 && SYS_getppid == 110 &&
               SYS_exit == 60 && SYS_exit_group == 231 && SYS_delete_module == 176,
               "La demo richiede Linux x86-64 nativo");

static double milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) { perror("clock_gettime"); exit(1); }
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

/* Lo script abilita il monitor solo dopo l'avvio del programma:
 * in questo modo le read del loader non consumano il budget. */
static int pipe_demo(const char *ready, const char *start)
{
    int pipefd[2];
    if (pipe(pipefd)) { perror("pipe"); return 1; }
    FILE *file = fopen(ready, "w");
    if (!file) { perror("ready"); return 1; }
    fclose(file);
    while (access(start, F_OK)) usleep(10000);

    pid_t writer = fork();
    if (writer < 0) { perror("fork"); return 1; }
    if (!writer) {
        close(pipefd[0]);
        usleep(200000);
        _exit(write(pipefd[1], "AB", 2) == 2 ? 0 : 1);
    }
    close(pipefd[1]);
    double durations[2];
    char bytes[2];
    for (int i = 1; i <= 2; i++) {
        char byte;
        double start_ms = milliseconds();
        long result = syscall(SYS_read, pipefd[0], &byte, 1);
        if (result != 1) { perror("read"); return 1; }
        durations[i-1] = milliseconds()-start_ms;
        bytes[i-1] = byte;
    }
    printf("  read 1: %c, %.1f ms; read 2: %c, %.1f ms\n",
           bytes[0], durations[0], bytes[1], durations[1]);
    close(pipefd[0]);
    int status;
    if (waitpid(writer, &status, 0) < 0) { perror("waitpid"); return 1; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *program = strrchr(argv[0], '/');
    program = program ? program + 1 : argv[0];
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "pipe") && argc == 4)
        return pipe_demo(argv[2], argv[3]);
    if (!strcmp(argv[1], "exit") || !strcmp(argv[1], "exit_group")) {
        syscall(!strcmp(argv[1], "exit") ? SYS_exit : SYS_exit_group, 42);
        perror("syscall senza ritorno");
        return 1;
    }
    if (!strcmp(argv[1], "delete") || !strcmp(argv[1], "force")) {
        int force = !strcmp(argv[1], "force");
        errno = 0;
        long result = syscall(SYS_delete_module, "syscall_throttle",
                              O_NONBLOCK | (force ? O_TRUNC : 0));
        int error = errno;
        printf("%s: risultato=%ld, errno=%d (%s)",
               force ? "O_NONBLOCK | O_TRUNC" : "O_NONBLOCK",
               result, error, strerror(error));
        /* Sono richieste di rimozione: il risultato viene mostrato allo spettatore. */
        return 0;
    }
    if (strcmp(argv[1], "getpid") && strcmp(argv[1], "getppid")) return 2;
    int count = argc == 3 ? atoi(argv[2]) : 1;
    if (count < 1 || count > 100) return 2;
    long number = !strcmp(argv[1], "getpid") ? SYS_getpid : SYS_getppid;
    double durations[100];
    for (int i = 1; i <= count; i++) {
        double start_ms = milliseconds();
        long result = syscall(number);
        if (result < 0) { perror("syscall"); return 1; }
        durations[i-1] = milliseconds()-start_ms;
    }
    /* Un'unica riga per le chiamate sequenziali, dopo averle eseguite tutte. */
    if (count == 1) {
        printf("  %s EUID=%u, %s: %.1f ms\n", program,
               (unsigned)geteuid(), argv[1], durations[0]);
    } else {
        printf("  %s EUID=%u, %s: ", program, (unsigned)geteuid(), argv[1]);
        for (int i = 0; i < count; i++)
            printf("%s%.1f ms", i ? " → " : "", durations[i]);
        putchar('\n');
    }
    return 0;
}
