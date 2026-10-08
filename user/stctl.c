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
            "  %s syscall-list\n"
            "  %s max-set <valore>\n"
            "  %s max-get\n"
            "  %s stats\n"
            "  %s stats-reset\n",
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
            program_name,
            program_name,
            program_name,
            program_name,
            program_name);
}

/* Solo cifre decimali; il limite dipende dal tipo richiesto. */
static int parse_decimal(const char *text, uint64_t limit, uint64_t *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || out == NULL || text[0] == '\0')
        return -1;

    for (const char *p = text; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return -1;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > limit)
        return -1;

    *out = (uint64_t)value;
    return 0;
}

static int parse_uid(const char *text, __u32 *out)
{
    uint64_t value;

    if (out == NULL || parse_decimal(text, UINT32_MAX, &value) != 0)
        return -1;

    *out = (__u32)value;
    return 0;
}

static int parse_syscall_number(const char *text, __u32 *out)
{
    return parse_uid(text, out);
}

static int parse_max_invocations(const char *text, uint64_t *out)
{
    return parse_decimal(text, UINT64_MAX, out);
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

static int execute_uid_update(int fd, __u32 uid, int add)
{
    struct st_uid_request request = {.uid = uid, .reserved = 0U};
    unsigned long command = add ? ST_IOCTL_UID_ADD : ST_IOCTL_UID_REMOVE;

    if (ioctl(fd, command, &request) == -1) {
        if (errno == (add ? EEXIST : ENOENT))
            fprintf(stderr, "UID %u %s registrato.\n",
                    (unsigned int)uid, add ? "già" : "non");
        else if (errno == EPERM)
            fprintf(stderr, "%s UID non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == EINVAL)
            fprintf(stderr, "UID %u non valido.\n", (unsigned int)uid);
        else
            fprintf(stderr, "ioctl ST_IOCTL_UID_%s fallita: %s\n",
                    add ? "ADD" : "REMOVE", strerror(errno));
        return 1;
    }

    printf("UID %u %s.\n", (unsigned int)uid, add ? "registrato" : "rimosso");
    return 0;
}

static int execute_uid_add(int fd, __u32 uid)
{
    return execute_uid_update(fd, uid, 1);
}

static int execute_uid_remove(int fd, __u32 uid)
{
    return execute_uid_update(fd, uid, 0);
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

static int execute_program_update(int fd, const char *name, int add)
{
    struct st_program_request request = {0};
    unsigned long command = add ? ST_IOCTL_PROGRAM_ADD : ST_IOCTL_PROGRAM_REMOVE;

    /* Il chiamante ha gia validato basename e lunghezza. */
    memcpy(request.name, name, strlen(name) + 1U);
    if (ioctl(fd, command, &request) == -1) {
        if (errno == (add ? EEXIST : ENOENT))
            fprintf(stderr, "Programma '%s' %s registrato.\n",
                    name, add ? "già" : "non");
        else if (errno == EPERM)
            fprintf(stderr, "%s programma non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == EINVAL)
            fprintf(stderr, "Nome programma non valido: %s\n", name);
        else
            fprintf(stderr, "ioctl ST_IOCTL_PROGRAM_%s fallita: %s\n",
                    add ? "ADD" : "REMOVE", strerror(errno));
        return 1;
    }

    printf("Programma '%s' %s.\n", name, add ? "registrato" : "rimosso");
    return 0;
}

static int execute_program_add(int fd, const char *name)
{
    return execute_program_update(fd, name, 1);
}

static int execute_program_remove(int fd, const char *name)
{
    return execute_program_update(fd, name, 0);
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

static int execute_max_set(int fd, uint64_t max_invocations)
{
    struct st_max_config request = {
        .max_invocations = (__u64)max_invocations,
        .reserved = {0U, 0U},
    };

    if (ioctl(fd, ST_IOCTL_MAX_SET, &request) == -1) {
        switch (errno) {
        case EPERM:
            fprintf(stderr,
                    "Modifica di MAX non consentita: "
                    "sono richiesti privilegi root.\n");
            break;

        case EINVAL:
            fprintf(stderr,
                    "Richiesta MAX_SET non valida.\n");
            break;

        case EFAULT:
            fprintf(stderr,
                    "Richiesta MAX_SET non accessibile "
                    "dal kernel.\n");
            break;

        default:
            fprintf(stderr,
                    "ioctl ST_IOCTL_MAX_SET fallita: %s\n",
                    strerror(errno));
            break;
        }

        return 1;
    }

    printf("MAX impostato a %llu invocazioni "
           "per finestra globale di un secondo.\n",
           (unsigned long long)max_invocations);

    return 0;
}

static int execute_max_get(int fd)
{
    struct st_max_config response = {
        .max_invocations = 0U,
        .reserved = {0U, 0U},
    };

    if (ioctl(fd, ST_IOCTL_MAX_GET, &response) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_MAX_GET fallita: %s\n",
                strerror(errno));
        return 1;
    }

    /*
     * Il kernel deve restituire i campi riservati a zero.
     * Un valore diverso indicherebbe una risposta incompatibile
     * con la versione corrente dell'UAPI.
     */
    if (response.reserved[0] != 0U ||
        response.reserved[1] != 0U) {
        fprintf(stderr,
                "Il driver ha restituito una risposta "
                "MAX_GET non valida.\n");
        return 1;
    }

    printf("MAX: %llu invocazioni per finestra "
           "globale di un secondo.\n",
           (unsigned long long)response.max_invocations);

    return 0;
}


static int execute_statistics_get(int fd)
{
    struct st_statistics_snapshot snapshot = {0};
    double average_blocked;
    double average_delay_ms;
    unsigned int index;

    if (ioctl(
            fd,
            ST_IOCTL_STATS_GET,
            &snapshot) == -1) {
        fprintf(stderr,
                "ioctl ST_IOCTL_STATS_GET fallita: %s\n",
                strerror(errno));
        return 1;
    }

    for (index = 0U;
         index <
             sizeof(snapshot.reserved) /
             sizeof(snapshot.reserved[0]);
         index++) {
        if (snapshot.reserved[index] != 0U) {
            fprintf(stderr,
                    "Snapshot statistiche non valido: "
                    "reserved[%u]=%u.\n",
                    index,
                    snapshot.reserved[index]);
            return 1;
        }
    }

    if (snapshot.peak_valid > 1U ||
        snapshot.session_active > 1U) {
        fprintf(stderr,
                "Snapshot statistiche non valido: "
                "flag fuori dominio.\n");
        return 1;
    }

    if (snapshot.current_blocked >
        snapshot.peak_blocked) {
        fprintf(stderr,
                "Snapshot statistiche incoerente: "
                "current_blocked > peak_blocked.\n");
        return 1;
    }

    /*
     * Controllo scritto senza sommare prima i contatori,
     * così resta corretto anche in caso di saturazione a U64_MAX.
     */
    if (snapshot.completed_blocked_invocations >
            snapshot.blocked_invocations ||
        snapshot.interrupted_blocked_invocations >
            snapshot.blocked_invocations -
                snapshot.completed_blocked_invocations) {
        fprintf(stderr,
                "Snapshot statistiche incoerente: "
                "eventi conclusi superiori agli eventi "
                "di blocco.\n");
        return 1;
    }

    if (snapshot.peak_valid != 0U &&
        memchr(
            snapshot.peak_program,
            '\0',
            sizeof(snapshot.peak_program)) == NULL) {
        fprintf(stderr,
                "Snapshot statistiche non valido: "
                "nome del programma non terminato da NUL.\n");
        return 1;
    }

    if (snapshot.observation_ns == 0U) {
        average_blocked = 0.0;
    } else {
        average_blocked =
            (double)snapshot.blocked_thread_time_ns /
            (double)snapshot.observation_ns;
    }

    if (snapshot.completed_blocked_invocations == 0U) {
        average_delay_ms = 0.0;
    } else {
        average_delay_ms =
            ((double)snapshot.total_delay_ns /
             (double)snapshot.completed_blocked_invocations) /
            1000000.0;
    }

    printf("Sessione statistiche: %s\n",
           snapshot.session_active != 0U
               ? "attiva"
               : "inattiva");

    printf("Durata osservazione: %.6f secondi\n",
           (double)snapshot.observation_ns /
               1000000000.0);

    printf("Invocazioni rilevanti: %llu\n",
           (unsigned long long)
               snapshot.relevant_invocations);

    printf("Invocazioni bloccate: %llu\n",
           (unsigned long long)
               snapshot.blocked_invocations);

    printf("Invocazioni bloccate completate: %llu\n",
           (unsigned long long)
               snapshot.completed_blocked_invocations);

    printf("Attese interrotte da segnale: %llu\n",
           (unsigned long long)
               snapshot.interrupted_blocked_invocations);

    printf("Thread attualmente bloccati: %u\n",
           snapshot.current_blocked);

    printf("Picco thread bloccati: %u\n",
           snapshot.peak_blocked);

    printf("Media temporale thread bloccati: %.6f\n",
           average_blocked);

    if (snapshot.completed_blocked_invocations == 0U) {
        printf("Ritardo medio delle chiamate bloccate: "
               "non disponibile\n");
    } else {
        printf("Ritardo medio delle chiamate bloccate: "
               "%.6f ms\n",
               average_delay_ms);
    }

    if (snapshot.peak_valid == 0U) {
        printf("Peak delay: non disponibile\n");
        printf("System call del peak: non disponibile\n");
        printf("Effective UID del peak: non disponibile\n");
        printf("Programma del peak: non disponibile\n");
    } else {
        printf("Peak delay: %llu ns (%.6f ms)\n",
               (unsigned long long)
                   snapshot.peak_delay_ns,
               (double)snapshot.peak_delay_ns /
                   1000000.0);

        printf("System call del peak: %u\n",
               snapshot.peak_syscall_nr);

        printf("Effective UID del peak: %u\n",
               snapshot.peak_euid);

        printf("Programma del peak: %s\n",
               snapshot.peak_program);
    }

    return 0;
}


static int execute_statistics_reset(int fd)
{
    if (ioctl(fd, ST_IOCTL_STATS_RESET) == -1) {
        if (errno == EPERM) {
            fprintf(stderr,
                    "Reset statistiche non consentito: "
                    "sono richiesti privilegi root.\n");
        } else if (errno == EBUSY) {
            fprintf(stderr,
                    "Reset statistiche non eseguibile: "
                    "esistono thread attualmente bloccati.\n");
        } else {
            fprintf(stderr,
                    "ioctl ST_IOCTL_STATS_RESET fallita: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    printf("Statistiche azzerate.\n");
    return 0;
}


struct simple_command {
    const char *name;
    int (*execute)(int fd);
};

static const struct simple_command simple_commands[] = {
    {"ping", execute_ping},
    {"status", execute_status},
    {"enable", execute_enable},
    {"disable", execute_disable},
    {"uid-count", execute_uid_count},
    {"uid-list", execute_uid_list},
    {"program-count", execute_program_count},
    {"program-list", execute_program_list},
    {"syscall-count", execute_syscall_count},
    {"syscall-list", execute_syscall_list},
    {"max-get", execute_max_get},
    {"stats", execute_statistics_get},
    {"stats-reset", execute_statistics_reset},
};

static const struct simple_command *find_simple_command(const char *name)
{
    for (size_t i = 0; i < sizeof(simple_commands) / sizeof(simple_commands[0]); i++)
        if (strcmp(name, simple_commands[i].name) == 0)
            return &simple_commands[i];
    return NULL;
}

static int is_simple_command(const char *name)
{
    return find_simple_command(name) != NULL;
}

static int execute_simple_command(int fd, const char *name)
{
    const struct simple_command *command = find_simple_command(name);
    return command != NULL ? command->execute(fd) : 1;
}

static int execute_syscall_update(int fd, __u32 number, int add)
{
    struct st_syscall_request request = {.number = number, .reserved = 0U};
    unsigned long command = add ? ST_IOCTL_SYSCALL_ADD : ST_IOCTL_SYSCALL_REMOVE;

    if (ioctl(fd, command, &request) == -1) {
        if (errno == EPERM)
            fprintf(stderr, "%s system call non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == (add ? EEXIST : ENOENT))
            fprintf(stderr, "System call %u %s registrata.\n",
                    number, add ? "già" : "non");
        else if (add && errno == EOPNOTSUPP)
            fprintf(stderr,
                    "System call %u non supportata: "
                    "delete_module non può essere sottoposta "
                    "a throttling perché è necessaria alla "
                    "rimozione sicura del modulo.\n", number);
        else if (errno == EINVAL)
            fprintf(stderr, "Numero di system call non valido per l'ABI x86-64 corrente: %u.\n",
                    number);
        else if (errno == EFAULT)
            fprintf(stderr, "Richiesta SYSCALL_%s non accessibile dal kernel.\n",
                    add ? "ADD" : "REMOVE");
        else
            fprintf(stderr, "ioctl ST_IOCTL_SYSCALL_%s fallita: %s\n",
                    add ? "ADD" : "REMOVE", strerror(errno));
        return 1;
    }

    printf("System call %u %s.\n", number, add ? "registrata" : "rimossa");
    return 0;
}

static int execute_syscall_add(int fd, __u32 number)
{
    return execute_syscall_update(fd, number, 1);
}

static int execute_syscall_remove(int fd, __u32 number)
{
    return execute_syscall_update(fd, number, 0);
}

int main(int argc, char *argv[])
{
    __u32 uid = 0U;
    __u32 syscall_number = 0U;
    uint64_t max_invocations = 0U;
    const char *program_name = NULL;
    int uid_operation = 0;
    int program_operation = 0;
    int syscall_operation = 0;
    int max_operation = 0;
    int fd;
    int result;

    if (argc == 2 && is_simple_command(argv[1])) {
        /*
         * I comandi semplici non richiedono ulteriori
         * operazioni di parsing.
         */
    } else if (argc == 3 &&
               (strcmp(argv[1], "uid-add") == 0 ||
                strcmp(argv[1], "uid-remove") == 0)) {
        if (parse_uid(argv[2], &uid) != 0) {
            fprintf(stderr,
                    "UID non valido: %s\n",
                    argv[2]);
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

    } else if (argc == 3 &&
               strcmp(argv[1], "max-set") == 0) {
        if (parse_max_invocations(argv[2],
                                  &max_invocations) != 0) {
            fprintf(stderr,
                    "Valore MAX non valido: %s\n",
                    argv[2]);
            return 1;
        }

        max_operation = 1;

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
    else if (max_operation == 1)
        result = execute_max_set(fd, max_invocations);
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
