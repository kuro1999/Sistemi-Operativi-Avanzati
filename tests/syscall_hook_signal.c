#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signal_received;

static void signal_handler(int signal_number)
{
    (void)signal_number;
    signal_received = 1;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds;
    long nanoseconds;

    seconds = end->tv_sec - start->tv_sec;
    nanoseconds = end->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }

    return (double)seconds +
           (double)nanoseconds / 1000000000.0;
}

int main(void)
{
    struct sigaction action;
    struct timespec request = {
        .tv_sec = 0,
        .tv_nsec = 20000000L,
    };
    struct timespec remaining = {0};
    struct timespec start;
    struct timespec end;
    long syscall_result;
    int saved_errno;

    memset(&action, 0, sizeof(action));

    action.sa_handler = signal_handler;
    action.sa_flags = 0;

    if (sigemptyset(&action.sa_mask) == -1) {
        fprintf(stderr,
                "sigemptyset fallita: %s\n",
                strerror(errno));
        return 1;
    }

    /*
     * Non usiamo SA_RESTART: il risultato atteso dopo SIGUSR1
     * è il ritorno in user space con errno == EINTR.
     */
    if (sigaction(SIGUSR1, &action, NULL) == -1) {
        fprintf(stderr,
                "sigaction fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("pid=%ld\n", (long)getpid());
    fflush(stdout);

    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        fprintf(stderr,
                "clock_gettime iniziale fallita: %s\n",
                strerror(errno));
        return 1;
    }

    errno = 0;

    syscall_result = syscall(SYS_nanosleep,
                             &request,
                             &remaining);

    saved_errno = errno;

    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
        fprintf(stderr,
                "clock_gettime finale fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("syscall_result=%ld\n", syscall_result);
    printf("errno=%d (%s)\n",
           saved_errno,
           strerror(saved_errno));
    printf("signal_received=%d\n",
           signal_received != 0);
    printf("durata_osservata=%.6f secondi\n",
           elapsed_seconds(&start, &end));
    printf("remaining=%ld.%09ld secondi\n",
           (long)remaining.tv_sec,
           remaining.tv_nsec);

    if (syscall_result != -1) {
        fprintf(stderr,
                "ERRORE: la syscall non è stata interrotta.\n");
        return 1;
    }

    if (saved_errno != EINTR) {
        fprintf(stderr,
                "ERRORE: errno atteso=%d, osservato=%d.\n",
                EINTR,
                saved_errno);
        return 1;
    }

    if (signal_received == 0) {
        fprintf(stderr,
                "ERRORE: il signal handler non è stato eseguito.\n");
        return 1;
    }

    printf("Attesa del rate limiter interrotta correttamente.\n");
    return 0;
}
