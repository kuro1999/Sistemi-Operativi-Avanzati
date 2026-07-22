#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
            "  %s disable\n"
            "  %s uid-add <UID>\n"
            "  %s uid-remove <UID>\n"
            "  %s uid-count\n"
            "  %s uid-list\n",
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name);
}

static int parse_uid(const char *text, __u32 *uid)
{
    const char *cursor;
    char *end;
    unsigned long long value;

    if (text == NULL || text[0] == '\0')
        return -1;

    /*
     * Accettiamo esclusivamente cifre decimali.
     *
     * In questo modo rifiutiamo valori negativi, spazi,
     * suffissi e rappresentazioni non decimali.
     */
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9')
            return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 10);

    if (errno == ERANGE ||
        end == text ||
        *end != '\0' ||
        value > UINT32_MAX) {
        return -1;
    }

    *uid = (__u32)value;
    return 0;
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

static int execute_uid_add(int fd, __u32 uid)
{
    struct st_uid_request request = {
        .uid = uid,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_UID_ADD, &request) == -1) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "UID %u già registrato.\n",
                    (unsigned int)uid);
        } else if (errno == EPERM) {
            fprintf(stderr,
                    "Registrazione UID non consentita: "
                    "sono richiesti privilegi root.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr,
                    "UID %u non valido.\n",
                    (unsigned int)uid);
        } else {
            fprintf(stderr,
                    "ioctl ST_IOCTL_UID_ADD fallita: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    printf("UID %u registrato.\n", (unsigned int)uid);
    return 0;
}


static int execute_uid_remove(int fd, __u32 uid)
{
    struct st_uid_request request = {
        .uid = uid,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_UID_REMOVE, &request) == -1) {
        if (errno == ENOENT) {
            fprintf(stderr,
                    "UID %u non registrato.\n",
                    (unsigned int)uid);
        } else if (errno == EPERM) {
            fprintf(stderr,
                    "Rimozione UID non consentita: "
                    "sono richiesti privilegi root.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr,
                    "UID %u non valido.\n",
                    (unsigned int)uid);
        } else {
            fprintf(stderr,
                    "ioctl ST_IOCTL_UID_REMOVE fallita: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    printf("UID %u rimosso.\n", (unsigned int)uid);
    return 0;
}


static int execute_uid_count(int fd)
{
    struct st_uid_count result = {
        .count = 0U,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_UID_GET_COUNT, &result) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_UID_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    printf("UID registrati: %u\n",
           (unsigned int)result.count);

    return 0;
}


static int execute_uid_list(int fd)
{
    struct st_uid_count count_result = {
        .count = 0U,
        .reserved = 0U,
    };
    __u32 capacity;
    unsigned int attempt;

    /*
     * Prima scopriamo quanti elementi sono attualmente presenti,
     * così possiamo dimensionare correttamente l'array user-space.
     */
    if (ioctl(fd, ST_IOCTL_UID_GET_COUNT, &count_result) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_UID_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    capacity = count_result.count;

    if (capacity == 0U) {
        printf("UID registrati: 0\n");
        printf("  nessuno\n");
        return 0;
    }

    /*
     * Il registro può crescere tra GET_COUNT e UID_LIST.
     * In caso di ENOSPC, il kernel restituisce nel campo count
     * la nuova capacità necessaria e ripetiamo l'operazione.
     */
    for (attempt = 0U; attempt < 4U; attempt++) {
        struct st_uid_list_request request;
        __u32 *uids;
        __u32 index;
        int ioctl_error;

        uids = calloc(capacity, sizeof(*uids));
        if (uids == NULL) {
            fprintf(stderr,
                    "Impossibile allocare memoria per %u UID.\n",
                    (unsigned int)capacity);
            return 1;
        }

        request.uids_ptr = (__u64)(uintptr_t)uids;
        request.capacity = capacity;
        request.count = 0U;
        request.reserved[0] = 0U;
        request.reserved[1] = 0U;

        if (ioctl(fd, ST_IOCTL_UID_LIST, &request) == 0) {
            /*
             * Il kernel non deve mai dichiarare di aver copiato
             * più elementi della capacità fornita.
             */
            if (request.count > capacity) {
                fprintf(stderr,
                        "Risposta UID_LIST non valida ricevuta "
                        "dal driver.\n");
                free(uids);
                return 1;
            }

            printf("UID registrati: %u\n",
                   (unsigned int)request.count);

            if (request.count == 0U) {
                printf("  nessuno\n");
            } else {
                for (index = 0U; index < request.count; index++) {
                    printf("  %u\n",
                           (unsigned int)uids[index]);
                }
            }

            free(uids);
            return 0;
        }

        ioctl_error = errno;
        free(uids);

        if (ioctl_error != ENOSPC) {
            fprintf(stderr,
                    "ioctl ST_IOCTL_UID_LIST fallita: %s\n",
                    strerror(ioctl_error));
            return 1;
        }

        /*
         * In caso di ENOSPC il kernel deve restituire una capacità
         * strettamente maggiore di quella appena utilizzata.
         */
        if (request.count <= capacity) {
            fprintf(stderr,
                    "Il driver ha restituito una capacità UID_LIST "
                    "non valida.\n");
            return 1;
        }

        capacity = request.count;
    }

    fprintf(stderr,
            "Impossibile ottenere la lista: il registro UID "
            "è stato modificato ripetutamente.\n");

    return 1;
}

static int is_simple_command(const char *command)
{
    return strcmp(command, "ping") == 0 ||
           strcmp(command, "status") == 0 ||
           strcmp(command, "enable") == 0 ||
           strcmp(command, "disable") == 0 ||
           strcmp(command, "uid-count") == 0 ||
           strcmp(command, "uid-list") == 0;
}

static int execute_simple_command(int fd, const char *command)
{
    if (strcmp(command, "ping") == 0)
        return execute_ping(fd);

    if (strcmp(command, "status") == 0)
        return execute_status(fd);

    if (strcmp(command, "enable") == 0)
        return execute_enable(fd);

    if (strcmp(command, "disable") == 0)
        return execute_disable(fd);

    if (strcmp(command, "uid-count") == 0)
        return execute_uid_count(fd);

    return execute_uid_list(fd);
}

int main(int argc, char *argv[])
{
    __u32 uid = 0U;
    int uid_operation = 0;
    int fd;
    int result;

    if (argc == 2 && is_simple_command(argv[1])) {
        uid_operation = 0;
    } else if (argc == 3 &&
               (strcmp(argv[1], "uid-add") == 0 ||
                strcmp(argv[1], "uid-remove") == 0)) {
        if (parse_uid(argv[2], &uid) != 0) {
            fprintf(stderr, "UID non valido: %s\n", argv[2]);
            return 1;
        }

        if (strcmp(argv[1], "uid-add") == 0)
            uid_operation = 1;
        else
            uid_operation = 2;
    } else {
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

    if (uid_operation == 1)
        result = execute_uid_add(fd, uid);
    else if (uid_operation == 2)
        result = execute_uid_remove(fd, uid);
    else
        result = execute_simple_command(fd, argv[1]);

    if (close(fd) == -1) {
        fprintf(stderr,
                "Chiusura di %s fallita: %s\n",
                ST_DEVICE_PATH,
                strerror(errno));
        return 1;
    }

    return result;
}
