#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DURATION_MS 200ULL

static int parse_duration_ms(const char *text,
                             uint64_t *duration_ms)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || duration_ms == NULL ||
        text[0] == '\0' || text[0] == '-') {
        return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 10);

    if (errno == ERANGE ||
        end == text ||
        *end != '\0') {
        return -1;
    }

    *duration_ms = (uint64_t)value;
    return 0;
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

int main(int argc, char **argv)
{
    struct timespec request;
    struct timespec remaining = {0};
    struct timespec start;
    struct timespec end;
    uint64_t duration_ms = DEFAULT_DURATION_MS;
    long syscall_result;

    if (argc > 2) {
        fprintf(stderr,
                "Uso: %s [durata_millisecondi]\n",
                argv[0]);
        return 1;
    }

    if (argc == 2 &&
        parse_duration_ms(argv[1], &duration_ms) != 0) {
        fprintf(stderr,
                "Durata non valida: %s\n",
                argv[1]);
        return 1;
    }

    if (duration_ms >
        ((uint64_t)LLONG_MAX / 1000ULL)) {
        fprintf(stderr,
                "Durata troppo grande.\n");
        return 1;
    }

    request.tv_sec =
        (time_t)(duration_ms / 1000ULL);

    request.tv_nsec =
        (long)((duration_ms % 1000ULL) * 1000000ULL);

    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        fprintf(stderr,
                "clock_gettime iniziale fallita: %s\n",
                strerror(errno));
        return 1;
    }

    errno = 0;

    /*
     * Chiamata diretta alla syscall x86-64 nanosleep.
     * Non passa attraverso la scelta effettuata dalla libc.
     */
    syscall_result = syscall(SYS_nanosleep,
                             &request,
                             &remaining);

    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
        fprintf(stderr,
                "clock_gettime finale fallita: %s\n",
                strerror(errno));
        return 1;
    }

    if (syscall_result == -1) {
        fprintf(stderr,
                "SYS_nanosleep fallita: %s; "
                "tempo residuo=%ld.%09ld secondi\n",
                strerror(errno),
                (long)remaining.tv_sec,
                remaining.tv_nsec);
        return 1;
    }

    printf("SYS_nanosleep completata.\n");
    printf("Durata richiesta: %llu ms\n",
           (unsigned long long)duration_ms);
    printf("Durata osservata: %.6f secondi\n",
           elapsed_seconds(&start, &end));

    return 0;
}
