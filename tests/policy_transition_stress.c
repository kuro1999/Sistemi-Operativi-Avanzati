#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <syscall_throttle.h>

#define TEST_THREADS 6U
#define TEST_ITERATIONS 100U
#define FINAL_MAX 7U

struct worker_result {
    unsigned int errors;
    int first_errno;
    const char *first_operation;
};

static int device_fd = -1;
static pthread_barrier_t start_barrier;
static struct worker_result worker_results[TEST_THREADS];

static void record_error(unsigned int index,
                         const char *operation,
                         int error_number)
{
    struct worker_result *result = &worker_results[index];

    result->errors++;

    if (result->first_operation == NULL) {
        result->first_operation = operation;
        result->first_errno = error_number;
    }
}

static void execute_monitor_transition(unsigned int index,
                                       unsigned int iteration)
{
    unsigned long command;
    const char *operation;

    if (((iteration + index) & 1U) == 0U) {
        command = ST_IOCTL_ENABLE;
        operation = "ENABLE";
    } else {
        command = ST_IOCTL_DISABLE;
        operation = "DISABLE";
    }

    errno = 0;

    if (ioctl(device_fd, command) == -1)
        record_error(index, operation, errno);
}

static void execute_max_set(unsigned int index,
                            unsigned int iteration)
{
    struct st_max_config configuration = {
        .max_invocations =
            ((__u64)index << 32) | (__u64)iteration,
        .reserved = {0U, 0U},
    };

    errno = 0;

    if (ioctl(device_fd,
              ST_IOCTL_MAX_SET,
              &configuration) == -1) {
        record_error(index, "MAX_SET", errno);
    }
}

static void execute_max_get(unsigned int index)
{
    struct st_max_config response = {
        .max_invocations = 0U,
        .reserved = {0U, 0U},
    };

    errno = 0;

    if (ioctl(device_fd,
              ST_IOCTL_MAX_GET,
              &response) == -1) {
        record_error(index, "MAX_GET", errno);
        return;
    }

    if (response.reserved[0] != 0U ||
        response.reserved[1] != 0U) {
        record_error(index, "MAX_GET reserved", EPROTO);
    }
}

static void execute_status_get(unsigned int index)
{
    struct st_monitor_status response = {
        .enabled = 0U,
        .reserved = 0U,
    };

    errno = 0;

    if (ioctl(device_fd,
              ST_IOCTL_GET_STATUS,
              &response) == -1) {
        record_error(index, "GET_STATUS", errno);
        return;
    }

    if (response.reserved != 0U ||
        response.enabled > 1U) {
        record_error(index, "GET_STATUS response", EPROTO);
    }
}

static void *worker(void *argument)
{
    uintptr_t index = (uintptr_t)argument;
    unsigned int iteration;
    int barrier_result;

    barrier_result = pthread_barrier_wait(&start_barrier);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        record_error((unsigned int)index,
                     "pthread_barrier_wait",
                     barrier_result);
        return NULL;
    }

    for (iteration = 0U;
         iteration < TEST_ITERATIONS;
         iteration++) {
        switch (index) {
        case 0U:
        case 1U:
            execute_monitor_transition((unsigned int)index,
                                       iteration);
            break;

        case 2U:
        case 3U:
            execute_max_set((unsigned int)index,
                            iteration);
            break;

        case 4U:
            execute_max_get((unsigned int)index);
            break;

        case 5U:
            execute_status_get((unsigned int)index);
            break;

        default:
            record_error((unsigned int)index,
                         "ruolo worker",
                         EINVAL);
            return NULL;
        }
    }

    return NULL;
}

static int verify_final_state(void)
{
    struct st_max_config configuration = {
        .max_invocations = FINAL_MAX,
        .reserved = {0U, 0U},
    };
    struct st_max_config max_response = {
        .max_invocations = 0U,
        .reserved = {0U, 0U},
    };
    struct st_monitor_status status = {
        .enabled = 0U,
        .reserved = 0U,
    };

    if (ioctl(device_fd,
              ST_IOCTL_MAX_SET,
              &configuration) == -1) {
        fprintf(stderr,
                "MAX_SET finale fallito: %s\n",
                strerror(errno));
        return -1;
    }

    if (ioctl(device_fd, ST_IOCTL_ENABLE) == -1) {
        fprintf(stderr,
                "ENABLE finale fallito: %s\n",
                strerror(errno));
        return -1;
    }

    if (ioctl(device_fd,
              ST_IOCTL_MAX_GET,
              &max_response) == -1) {
        fprintf(stderr,
                "MAX_GET finale fallito: %s\n",
                strerror(errno));
        return -1;
    }

    if (ioctl(device_fd,
              ST_IOCTL_GET_STATUS,
              &status) == -1) {
        fprintf(stderr,
                "GET_STATUS finale fallito: %s\n",
                strerror(errno));
        return -1;
    }

    if (max_response.max_invocations != FINAL_MAX ||
        max_response.reserved[0] != 0U ||
        max_response.reserved[1] != 0U ||
        status.enabled != 1U ||
        status.reserved != 0U) {
        fprintf(stderr,
                "Stato finale attivo incoerente: "
                "MAX=%llu enabled=%u.\n",
                (unsigned long long)
                    max_response.max_invocations,
                status.enabled);
        return -1;
    }

    if (ioctl(device_fd, ST_IOCTL_DISABLE) == -1) {
        fprintf(stderr,
                "DISABLE finale fallito: %s\n",
                strerror(errno));
        return -1;
    }

    memset(&status, 0, sizeof(status));
    memset(&max_response, 0, sizeof(max_response));

    if (ioctl(device_fd,
              ST_IOCTL_GET_STATUS,
              &status) == -1 ||
        ioctl(device_fd,
              ST_IOCTL_MAX_GET,
              &max_response) == -1) {
        fprintf(stderr,
                "Lettura dello stato disattivato fallita: %s\n",
                strerror(errno));
        return -1;
    }

    if (status.enabled != 0U ||
        status.reserved != 0U ||
        max_response.max_invocations != FINAL_MAX ||
        max_response.reserved[0] != 0U ||
        max_response.reserved[1] != 0U) {
        fprintf(stderr,
                "Stato finale disattivato incoerente: "
                "MAX=%llu enabled=%u.\n",
                (unsigned long long)
                    max_response.max_invocations,
                status.enabled);
        return -1;
    }

    return 0;
}

int main(void)
{
    pthread_t threads[TEST_THREADS];
    unsigned int total_errors = 0U;
    unsigned int index;
    int barrier_result;
    int result = 1;

    device_fd = open(ST_DEVICE_PATH, O_RDWR);
    if (device_fd == -1) {
        fprintf(stderr,
                "Impossibile aprire %s: %s\n",
                ST_DEVICE_PATH,
                strerror(errno));
        return 1;
    }

    if (pthread_barrier_init(&start_barrier,
                             NULL,
                             TEST_THREADS + 1U) != 0) {
        fprintf(stderr,
                "Inizializzazione della barriera fallita.\n");
        close(device_fd);
        return 1;
    }

    for (index = 0U; index < TEST_THREADS; index++) {
        int ret;

        ret = pthread_create(&threads[index],
                             NULL,
                             worker,
                             (void *)(uintptr_t)index);
        if (ret != 0) {
            fprintf(stderr,
                    "Creazione del thread %u fallita: %s\n",
                    index,
                    strerror(ret));
            return 1;
        }
    }

    barrier_result = pthread_barrier_wait(&start_barrier);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr,
                "Rilascio della barriera fallito.\n");
        return 1;
    }

    for (index = 0U; index < TEST_THREADS; index++) {
        int ret;

        ret = pthread_join(threads[index], NULL);
        if (ret != 0) {
            fprintf(stderr,
                    "Join del thread %u fallita: %s\n",
                    index,
                    strerror(ret));
            return 1;
        }

        total_errors += worker_results[index].errors;

        if (worker_results[index].errors != 0U) {
            fprintf(stderr,
                    "Worker %u: %u errori; primo in %s: "
                    "%d (%s).\n",
                    index,
                    worker_results[index].errors,
                    worker_results[index].first_operation,
                    worker_results[index].first_errno,
                    strerror(worker_results[index].first_errno));
        }
    }

    printf("Thread concorrenti: %u\n", TEST_THREADS);
    printf("Operazioni per thread: %u\n", TEST_ITERATIONS);
    printf("Operazioni concorrenti totali: %u\n",
           TEST_THREADS * TEST_ITERATIONS);
    printf("Errori durante lo stress: %u\n",
           total_errors);

    if (total_errors != 0U)
        goto cleanup;

    if (verify_final_state() != 0)
        goto cleanup;

    printf("Stato finale verificato: "
           "monitor disattivato, MAX=%u.\n",
           FINAL_MAX);
    printf("TEST SUPERATO: transizioni amministrative "
           "serializzate correttamente.\n");

    result = 0;

cleanup:
    pthread_barrier_destroy(&start_barrier);

    if (close(device_fd) == -1) {
        fprintf(stderr,
                "Chiusura del device fallita: %s\n",
                strerror(errno));
        result = 1;
    }

    return result;
}
