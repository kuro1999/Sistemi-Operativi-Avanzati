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
            "  %s uid-list\n"
            "  %s program-add <nome>\n"
            "  %s program-remove <nome>\n"
            "  %s program-count\n"
            "  %s program-list\n"
            "  %s syscall-add <numero>\n"
            "  %s syscall-remove <numero>\n"
            "  %s syscall-count\n"
            "  %s syscall-list\n",
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
            program_name,
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


static int parse_syscall_number(const char *text, __u32 *number)
{
    const char *cursor;
    char *end;
    unsigned long long value;

    if (text == NULL || number == NULL || text[0] == '\0')
        return -1;

    /*
     * Accettiamo esclusivamente cifre decimali.
     *
     * Vengono quindi rifiutati:
     *
     *     numeri negativi
     *     segno positivo
     *     spazi
     *     suffissi
     *     notazioni non decimali
     */
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9')
            return -1;
    }

    errno = 0;
    end = NULL;
    value = strtoull(text, &end, 10);

    if (errno == ERANGE ||
        end == text ||
        *end != '\0' ||
        value > UINT32_MAX) {
        return -1;
    }

    /*
     * Il limite architetturale NR_syscalls viene controllato
     * dal kernel, perché dipende dal kernel in esecuzione.
     */
    *number = (__u32)value;

    return 0;
}

static int validate_program_name(const char *name)
{
    size_t length;

    if (name == NULL || name[0] == '\0')
        return -1;

    /*
     * argv contiene già una stringa terminata da NUL, quindi
     * strlen() è sicura in questo contesto user-space.
     */
    length = strlen(name);

    if (length > ST_PROGRAM_NAME_MAX)
        return -1;

    /*
     * Accettiamo esclusivamente il basename, non un percorso.
     */
    if (strchr(name, '/') != NULL)
        return -1;

    return 0;
}

static int execute_program_add(int fd, const char *name)
{
    struct st_program_request request = {0};

    /*
     * validate_program_name() garantisce che il nome e il
     * terminatore NUL entrino nell'array della richiesta.
     */
    memcpy(request.name, name, strlen(name) + 1U);

    if (ioctl(fd, ST_IOCTL_PROGRAM_ADD, &request) == -1) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "Programma '%s' già registrato.\n",
                    name);
        } else if (errno == EPERM) {
            fprintf(stderr,
                    "Registrazione programma non consentita: "
                    "sono richiesti privilegi root.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr,
                    "Nome programma non valido: %s\n",
                    name);
        } else {
            fprintf(stderr,
                    "ioctl ST_IOCTL_PROGRAM_ADD fallita: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    printf("Programma '%s' registrato.\n", name);
    return 0;
}


static int execute_program_remove(int fd, const char *name)
{
    struct st_program_request request = {0};

    memcpy(request.name, name, strlen(name) + 1U);

    if (ioctl(fd, ST_IOCTL_PROGRAM_REMOVE, &request) == -1) {
        if (errno == ENOENT) {
            fprintf(stderr,
                    "Programma '%s' non registrato.\n",
                    name);
        } else if (errno == EPERM) {
            fprintf(stderr,
                    "Rimozione programma non consentita: "
                    "sono richiesti privilegi root.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr,
                    "Nome programma non valido: %s\n",
                    name);
        } else {
            fprintf(stderr,
                    "ioctl ST_IOCTL_PROGRAM_REMOVE fallita: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    printf("Programma '%s' rimosso.\n", name);
    return 0;
}


static int execute_program_count(int fd)
{
    struct st_program_count response = {0};

    if (ioctl(fd, ST_IOCTL_PROGRAM_GET_COUNT, &response) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_PROGRAM_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    if (response.reserved != 0U) {
        fprintf(stderr,
                "Risposta PROGRAM_GET_COUNT non valida.\n");
        return 1;
    }

    printf("Programmi registrati: %u\n", response.count);
    return 0;
}


static int execute_program_list(int fd)
{
    struct st_program_count count_response = {0};
    struct st_program_list_request request;
    struct st_program_name *programs = NULL;
    __u32 capacity;
    __u32 index;
    unsigned int attempt;

    /*
     * Prima richiesta: determina il numero iniziale di elementi
     * da allocare nello user-space.
     */
    if (ioctl(fd,
              ST_IOCTL_PROGRAM_GET_COUNT,
              &count_response) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_PROGRAM_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    if (count_response.reserved != 0U) {
        fprintf(stderr,
                "Risposta PROGRAM_GET_COUNT non valida.\n");
        return 1;
    }

    capacity = count_response.count;

    if (capacity == 0U) {
        printf("Programmi registrati: 0\n");
        printf("  nessuno\n");
        return 0;
    }

    /*
     * Il numero di elementi può cambiare tra GET_COUNT e LIST.
     * Sono consentiti al massimo quattro tentativi.
     */
    for (attempt = 0U; attempt < 4U; attempt++) {
        programs = calloc(capacity, sizeof(*programs));
        if (programs == NULL) {
            fprintf(stderr,
                    "Memoria insufficiente per la lista "
                    "dei programmi.\n");
            return 1;
        }

        memset(&request, 0, sizeof(request));

        request.programs_ptr =
            (__u64)(uintptr_t)programs;
        request.capacity = capacity;

        if (ioctl(fd,
                  ST_IOCTL_PROGRAM_LIST,
                  &request) == 0) {
            /*
             * Il kernel non può dichiarare di aver scritto più
             * elementi della capacità fornita.
             */
            if (request.count > capacity) {
                fprintf(stderr,
                        "Risposta PROGRAM_LIST non valida: "
                        "conteggio superiore alla capacità.\n");
                free(programs);
                return 1;
            }

            printf("Programmi registrati: %u\n",
                   request.count);

            if (request.count == 0U) {
                printf("  nessuno\n");
                free(programs);
                return 0;
            }

            for (index = 0U;
                 index < request.count;
                 index++) {
                /*
                 * Validazione difensiva: ogni nome restituito
                 * dal kernel deve contenere un terminatore NUL
                 * entro il record a dimensione fissa.
                 */
                if (memchr(programs[index].name,
                           '\0',
                           sizeof(programs[index].name)) == NULL) {
                    fprintf(stderr,
                            "Risposta PROGRAM_LIST non valida: "
                            "nome non terminato.\n");
                    free(programs);
                    return 1;
                }

                printf("  %s\n", programs[index].name);
            }

            free(programs);
            return 0;
        }

        if (errno != ENOSPC) {
            fprintf(stderr,
                    "ioctl ST_IOCTL_PROGRAM_LIST fallita: %s\n",
                    strerror(errno));
            free(programs);
            return 1;
        }

        /*
         * ENOSPC indica che il registro è cresciuto.
         * request.count contiene la nuova dimensione richiesta.
         */
        if (request.count <= capacity) {
            fprintf(stderr,
                    "Risposta PROGRAM_LIST non valida dopo "
                    "ENOSPC.\n");
            free(programs);
            return 1;
        }

        capacity = request.count;
        free(programs);
        programs = NULL;
    }

    fprintf(stderr,
            "Il registro programmi è cambiato troppe volte "
            "durante la consultazione.\n");

    return 1;
}

static int execute_syscall_count(int fd)
{
    struct st_syscall_count response = {
        .count = 0U,
        .reserved = 0U,
    };

    if (ioctl(fd,
              ST_IOCTL_SYSCALL_GET_COUNT,
              &response) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_SYSCALL_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    /*
     * Il kernel corrente restituisce reserved sempre a zero.
     * Un valore diverso indicherebbe una risposta incompatibile
     * o non conforme alla versione corrente dell'UAPI.
     */
    if (response.reserved != 0U) {
        fprintf(stderr,
                "Risposta SYSCALL_GET_COUNT non valida.\n");
        return 1;
    }

    printf("System call registrate: %u\n",
           (unsigned int)response.count);

    return 0;
}

static int execute_syscall_list(int fd)
{
    struct st_syscall_count count_response = {
        .count = 0U,
        .reserved = 0U,
    };
    __u32 capacity;
    unsigned int attempt;

    /*
     * Il primo conteggio serve soltanto a ottenere una capacità
     * iniziale. Il registro può cambiare prima di SYSCALL_LIST,
     * quindi il risultato non viene considerato definitivo.
     */
    if (ioctl(fd,
              ST_IOCTL_SYSCALL_GET_COUNT,
              &count_response) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_SYSCALL_GET_COUNT fallita: %s\n",
                strerror(errno));
        return 1;
    }

    if (count_response.reserved != 0U) {
        fprintf(stderr,
                "Risposta SYSCALL_GET_COUNT non valida.\n");
        return 1;
    }

    capacity = count_response.count;

    /*
     * Un numero limitato di tentativi evita un ciclo infinito
     * nel caso in cui il registro continui a crescere durante
     * la consultazione.
     */
    for (attempt = 0U; attempt < 4U; attempt++) {
        struct st_syscall_list_request request = {0};
        __u32 *numbers = NULL;
        __u32 index;
        int saved_errno;

        if (capacity > 0U) {
            /*
             * capacity proviene dal registro kernel, che può
             * contenere al massimo NR_syscalls elementi.
             *
             * calloc() restituisce NULL se l'allocazione non
             * può essere soddisfatta.
             */
            numbers = calloc((size_t)capacity,
                             sizeof(*numbers));
            if (numbers == NULL) {
                fprintf(stderr,
                        "Memoria insufficiente per la lista "
                        "delle system call.\n");
                return 1;
            }
        }

        request.numbers_ptr =
            (__u64)(uintptr_t)numbers;
        request.capacity = capacity;
        request.count = 0U;
        request.reserved[0] = 0U;
        request.reserved[1] = 0U;

        if (ioctl(fd,
                  ST_IOCTL_SYSCALL_LIST,
                  &request) == 0) {
            if (request.reserved[0] != 0U ||
                request.reserved[1] != 0U) {
                fprintf(stderr,
                        "Risposta SYSCALL_LIST non valida: "
                        "campi reserved modificati.\n");
                free(numbers);
                return 1;
            }

            if (request.count > capacity) {
                fprintf(stderr,
                        "Risposta SYSCALL_LIST non valida: "
                        "count supera la capacità.\n");
                free(numbers);
                return 1;
            }

            if (request.count > 0U && numbers == NULL) {
                fprintf(stderr,
                        "Risposta SYSCALL_LIST non valida: "
                        "array assente.\n");
                return 1;
            }

            /*
             * Il kernel deve restituire i numeri in ordine
             * strettamente crescente, poiché attraversa la
             * bitmap con for_each_set_bit().
             */
            for (index = 1U;
                 index < request.count;
                 index++) {
                if (numbers[index] <= numbers[index - 1U]) {
                    fprintf(stderr,
                            "Risposta SYSCALL_LIST non valida: "
                            "ordine dei numeri incoerente.\n");
                    free(numbers);
                    return 1;
                }
            }

            printf("System call registrate: %u\n",
                   (unsigned int)request.count);

            if (request.count == 0U) {
                printf("  nessuna\n");
            } else {
                for (index = 0U;
                     index < request.count;
                     index++) {
                    printf("  %u\n",
                           (unsigned int)numbers[index]);
                }
            }

            free(numbers);
            return 0;
        }

        saved_errno = errno;

        if (request.reserved[0] != 0U ||
            request.reserved[1] != 0U) {
            fprintf(stderr,
                    "Risposta SYSCALL_LIST non valida: "
                    "campi reserved modificati.\n");
            free(numbers);
            return 1;
        }

        if (saved_errno != ENOSPC) {
            fprintf(stderr,
                    "ioctl ST_IOCTL_SYSCALL_LIST fallita: %s\n",
                    strerror(saved_errno));
            free(numbers);
            return 1;
        }

        /*
         * In caso di ENOSPC, il kernel deve comunicare una
         * capacità strettamente maggiore di quella utilizzata.
         * Altrimenti un nuovo tentativo non potrebbe progredire.
         */
        if (request.count <= capacity) {
            fprintf(stderr,
                    "Risposta SYSCALL_LIST non valida: "
                    "capacità richiesta non crescente.\n");
            free(numbers);
            return 1;
        }

        free(numbers);
        capacity = request.count;
    }

    fprintf(stderr,
            "Impossibile ottenere una lista stabile delle "
            "system call dopo più tentativi.\n");

    return 1;
}

static int is_simple_command(const char *command)
{
    return strcmp(command, "ping") == 0 ||
           strcmp(command, "status") == 0 ||
           strcmp(command, "enable") == 0 ||
           strcmp(command, "disable") == 0 ||
           strcmp(command, "uid-count") == 0 ||
           strcmp(command, "uid-list") == 0 ||
           strcmp(command, "program-count") == 0 ||
           strcmp(command, "program-list") == 0 ||
           strcmp(command, "syscall-count") == 0 ||
           strcmp(command, "syscall-list") == 0;
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

    if (strcmp(command, "uid-list") == 0)
        return execute_uid_list(fd);

    if (strcmp(command, "program-count") == 0)
        return execute_program_count(fd);

    if (strcmp(command, "program-list") == 0)
        return execute_program_list(fd);

    if (strcmp(command, "syscall-count") == 0)
        return execute_syscall_count(fd);

    if (strcmp(command, "syscall-list") == 0)
        return execute_syscall_list(fd);

    return 1;
}

static int execute_syscall_add(int fd, __u32 number)
{
    struct st_syscall_request request = {
        .number = number,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_SYSCALL_ADD, &request) == -1) {
        switch (errno) {
        case EPERM:
            fprintf(stderr,
                    "Registrazione system call non consentita: "
                    "sono richiesti privilegi root.\n");
            break;

        case EEXIST:
            fprintf(stderr,
                    "System call %u già registrata.\n",
                    number);
            break;

        case EINVAL:
            fprintf(stderr,
                    "Numero di system call non valido per "
                    "l'ABI x86-64 corrente: %u.\n",
                    number);
            break;

        case EFAULT:
            fprintf(stderr,
                    "Richiesta SYSCALL_ADD non accessibile "
                    "dal kernel.\n");
            break;

        default:
            fprintf(stderr,
                    "ioctl ST_IOCTL_SYSCALL_ADD fallita: %s\n",
                    strerror(errno));
            break;
        }

        return 1;
    }

    printf("System call %u registrata.\n", number);

    return 0;
}

static int execute_syscall_remove(int fd, __u32 number)
{
    struct st_syscall_request request = {
        .number = number,
        .reserved = 0U,
    };

    if (ioctl(fd, ST_IOCTL_SYSCALL_REMOVE, &request) == -1) {
        switch (errno) {
        case EPERM:
            fprintf(stderr,
                    "Rimozione system call non consentita: "
                    "sono richiesti privilegi root.\n");
            break;

        case ENOENT:
            fprintf(stderr,
                    "System call %u non registrata.\n",
                    number);
            break;

        case EINVAL:
            fprintf(stderr,
                    "Numero di system call non valido per "
                    "l'ABI x86-64 corrente: %u.\n",
                    number);
            break;

        case EFAULT:
            fprintf(stderr,
                    "Richiesta SYSCALL_REMOVE non accessibile "
                    "dal kernel.\n");
            break;

        default:
            fprintf(stderr,
                    "ioctl ST_IOCTL_SYSCALL_REMOVE fallita: %s\n",
                    strerror(errno));
            break;
        }

        return 1;
    }

    printf("System call %u rimossa.\n", number);

    return 0;
}

int main(int argc, char *argv[])
{
    __u32 uid = 0U;
    __u32 syscall_number = 0U;
    const char *program_name = NULL;
    int uid_operation = 0;
    int program_operation = 0;
    int syscall_operation = 0;
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
    } else if (argc == 3 &&
               (strcmp(argv[1], "program-add") == 0 ||
                strcmp(argv[1], "program-remove") == 0)) {
        if (validate_program_name(argv[2]) != 0) {
            fprintf(stderr,
                    "Nome programma non valido: %s\n",
                    argv[2]);
            return 1;
        }

        program_name = argv[2];

        if (strcmp(argv[1], "program-add") == 0)
            program_operation = 1;
        else
            program_operation = 2;
    } else if (argc == 3 &&
               (strcmp(argv[1], "syscall-add") == 0 ||
                strcmp(argv[1], "syscall-remove") == 0)) {
        if (parse_syscall_number(argv[2],
                                 &syscall_number) != 0) {
            fprintf(stderr,
                    "Numero di system call non valido: %s\n",
                    argv[2]);
            return 1;
        }

        if (strcmp(argv[1], "syscall-add") == 0)
            syscall_operation = 1;
        else
            syscall_operation = 2;
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
    else if (program_operation == 1)
        result = execute_program_add(fd, program_name);
    else if (program_operation == 2)
        result = execute_program_remove(fd, program_name);
    else if (syscall_operation == 1)
        result = execute_syscall_add(fd, syscall_number);
    else if (syscall_operation == 2)
        result = execute_syscall_remove(fd, syscall_number);
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
