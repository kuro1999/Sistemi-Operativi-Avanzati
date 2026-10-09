#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <syscall_throttle.h>

/* Mantiene testo, stream e codice di uscita dei messaggi originali. */
static int fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    return 1;
}

static int ioctl_failed(const char *name)
{
    return fail("ioctl %s fallita: %s\n", name, strerror(errno));
}

static void print_usage(const char *program_name)
{
    static const char *const syntax[] = {
        "ping", "status", "enable", "disable",
        "uid-add <UID>", "uid-remove <UID>", "uid-count", "uid-list",
        "program-add <nome>", "program-remove <nome>", "program-count", "program-list",
        "syscall-add <numero>", "syscall-remove <numero>", "syscall-count", "syscall-list",
        "max-set <valore>", "max-get", "stats", "stats-reset",
    };

    fprintf(stderr, "Uso:\n");
    for (size_t i = 0; i < sizeof(syntax) / sizeof(syntax[0]); i++)
        fprintf(stderr, "  %s %s\n", program_name, syntax[i]);
}

static int parse_decimal(const char *text, uint64_t limit, uint64_t *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || out == NULL || text[0] == '\0')
        return -1;

    /* Solo cifre decimali: niente segni, spazi o prefissi. */
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

static int parse_u32(const char *text, __u32 *out)
{
    uint64_t value;

    if (out == NULL || parse_decimal(text, UINT32_MAX, &value) != 0)
        return -1;

    *out = (__u32)value;
    return 0;
}

static int execute_ping(int fd)
{
    if (ioctl(fd, ST_IOCTL_PING) == -1)
        return ioctl_failed("ST_IOCTL_PING");

    printf("PING completato correttamente.\n");
    return 0;
}

static int execute_status(int fd)
{
    struct st_monitor_status status = {0};

    if (ioctl(fd, ST_IOCTL_GET_STATUS, &status) == -1)
        return ioctl_failed("ST_IOCTL_GET_STATUS");

    printf("Monitor: %s\n", status.enabled != 0U ? "attivo" : "disattivato");

    return 0;
}

static int execute_enable(int fd)
{
    if (ioctl(fd, ST_IOCTL_ENABLE) == -1)
        return ioctl_failed("ST_IOCTL_ENABLE");

    printf("Monitor attivato.\n");
    return 0;
}

static int execute_disable(int fd)
{
    if (ioctl(fd, ST_IOCTL_DISABLE) == -1)
        return ioctl_failed("ST_IOCTL_DISABLE");

    printf("Monitor disattivato.\n");
    return 0;
}

static int execute_uid_update(int fd, __u32 uid, int add)
{
    struct st_uid_request request = {.uid = uid, .reserved = 0U};
    unsigned long command = add ? ST_IOCTL_UID_ADD : ST_IOCTL_UID_REMOVE;

    if (ioctl(fd, command, &request) == -1) {
        if (errno == (add ? EEXIST : ENOENT))
            return fail("UID %u %s registrato.\n", (unsigned int)uid, add ? "già" : "non");
        else if (errno == EPERM)
            return fail("%s UID non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == EINVAL)
            return fail("UID %u non valido.\n", (unsigned int)uid);
        else
            return fail("ioctl ST_IOCTL_UID_%s fallita: %s\n", add ? "ADD" : "REMOVE", strerror(errno));
    }

    printf("UID %u %s.\n", (unsigned int)uid, add ? "registrato" : "rimosso");
    return 0;
}

static int validate_program_name(const char *name)
{
    /* argv contiene stringhe terminate da NUL; accettiamo solo basename. */
    return name && name[0] && strlen(name) <= ST_PROGRAM_NAME_MAX && !strchr(name, '/') ? 0 : -1;
}

static int execute_program_update(int fd, const char *name, int add)
{
    struct st_program_request request = {0};
    unsigned long command = add ? ST_IOCTL_PROGRAM_ADD : ST_IOCTL_PROGRAM_REMOVE;

    memcpy(request.name, name, strlen(name) + 1U);
    if (ioctl(fd, command, &request) == -1) {
        if (errno == (add ? EEXIST : ENOENT))
            return fail("Programma '%s' %s registrato.\n", name, add ? "già" : "non");
        else if (errno == EPERM)
            return fail("%s programma non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == EINVAL)
            return fail("Nome programma non valido: %s\n", name);
        else
            return fail("ioctl ST_IOCTL_PROGRAM_%s fallita: %s\n", add ? "ADD" : "REMOVE", strerror(errno));
    }

    printf("Programma '%s' %s.\n", name, add ? "registrato" : "rimosso");
    return 0;
}

static int execute_max_set(int fd, uint64_t max_invocations)
{
    struct st_max_config request = {.max_invocations = (__u64)max_invocations};

    if (ioctl(fd, ST_IOCTL_MAX_SET, &request) == -1) {
        switch (errno) {
        case EPERM:
            return fail("Modifica di MAX non consentita: sono richiesti privilegi root.\n");
        case EINVAL:
            return fail("Richiesta MAX_SET non valida.\n");
        case EFAULT:
            return fail("Richiesta MAX_SET non accessibile dal kernel.\n");
        default:
            return ioctl_failed("ST_IOCTL_MAX_SET");
        }
    }

    printf("MAX impostato a %llu invocazioni per finestra globale di un secondo.\n",
           (unsigned long long)max_invocations);

    return 0;
}

static int execute_max_get(int fd)
{
    struct st_max_config response = {0};

    if (ioctl(fd, ST_IOCTL_MAX_GET, &response) == -1)
        return ioctl_failed("ST_IOCTL_MAX_GET");

    if (response.reserved[0] != 0U || response.reserved[1] != 0U)
        return fail("Il driver ha restituito una risposta MAX_GET non valida.\n");

    printf("MAX: %llu invocazioni per finestra globale di un secondo.\n",
           (unsigned long long)response.max_invocations);

    return 0;
}

static int execute_statistics_get(int fd)
{
    struct st_statistics_snapshot snapshot = {0};
    double average_blocked;
    double average_delay_ms;
    unsigned int index;

    if (ioctl(fd, ST_IOCTL_STATS_GET, &snapshot) == -1)
        return ioctl_failed("ST_IOCTL_STATS_GET");

    for (index = 0; index < sizeof(snapshot.reserved) / sizeof(snapshot.reserved[0]); index++)
        if (snapshot.reserved[index] != 0U)
            return fail("Snapshot statistiche non valido: reserved[%u]=%u.\n",
                        index, snapshot.reserved[index]);

    if (snapshot.peak_valid > 1U || snapshot.session_active > 1U)
        return fail("Snapshot statistiche non valido: flag fuori dominio.\n");

    if (snapshot.current_blocked > snapshot.peak_blocked)
        return fail("Snapshot statistiche incoerente: current_blocked > peak_blocked.\n");

    /* La sottrazione evita overflow anche con contatori saturi a U64_MAX. */
    if (snapshot.completed_blocked_invocations > snapshot.blocked_invocations ||
        snapshot.interrupted_blocked_invocations >
            snapshot.blocked_invocations - snapshot.completed_blocked_invocations)
        return fail("Snapshot statistiche incoerente: eventi conclusi superiori agli eventi di blocco.\n");

    if (snapshot.peak_valid != 0U &&
        memchr(snapshot.peak_program, '\0', sizeof(snapshot.peak_program)) == NULL)
        return fail("Snapshot statistiche non valido: nome del programma non terminato da NUL.\n");

    average_blocked = snapshot.observation_ns == 0U ? 0.0 :
        (double)snapshot.blocked_thread_time_ns / (double)snapshot.observation_ns;
    average_delay_ms = snapshot.completed_blocked_invocations == 0U ? 0.0 :
        ((double)snapshot.total_delay_ns / (double)snapshot.completed_blocked_invocations) / 1000000.0;

    printf("Sessione statistiche: %s\n", snapshot.session_active != 0U ? "attiva" : "inattiva");
    printf("Durata osservazione: %.6f secondi\n", (double)snapshot.observation_ns / 1000000000.0);
    printf("Invocazioni rilevanti: %llu\n", (unsigned long long) snapshot.relevant_invocations);
    printf("Invocazioni bloccate: %llu\n", (unsigned long long) snapshot.blocked_invocations);
    printf("Invocazioni bloccate completate: %llu\n",
           (unsigned long long)snapshot.completed_blocked_invocations);
    printf("Attese interrotte da segnale: %llu\n",
           (unsigned long long)snapshot.interrupted_blocked_invocations);
    printf("Thread attualmente bloccati: %u\n", snapshot.current_blocked);
    printf("Picco thread bloccati: %u\n", snapshot.peak_blocked);
    printf("Media temporale thread bloccati: %.6f\n", average_blocked);

    if (snapshot.completed_blocked_invocations == 0U)
        printf("Ritardo medio delle chiamate bloccate: non disponibile\n");
    else
        printf("Ritardo medio delle chiamate bloccate: %.6f ms\n", average_delay_ms);

    if (snapshot.peak_valid == 0U) {
        printf("Peak delay: non disponibile\n");
        printf("System call del peak: non disponibile\n");
        printf("Effective UID del peak: non disponibile\n");
        printf("Programma del peak: non disponibile\n");
    } else {
        printf("Peak delay: %llu ns (%.6f ms)\n",
               (unsigned long long)snapshot.peak_delay_ns,
               (double)snapshot.peak_delay_ns / 1000000.0);
        printf("System call del peak: %u\n", snapshot.peak_syscall_nr);
        printf("Effective UID del peak: %u\n", snapshot.peak_euid);
        printf("Programma del peak: %s\n", snapshot.peak_program);
    }

    return 0;
}

static int execute_statistics_reset(int fd)
{
    if (ioctl(fd, ST_IOCTL_STATS_RESET) == -1) {
        if (errno == EPERM)
            return fail("Reset statistiche non consentito: sono richiesti privilegi root.\n");
        if (errno == EBUSY)
            return fail("Reset statistiche non eseguibile: "
                        "contabilizzazione o attese in corso, oppure sessione in chiusura.\n");
        return ioctl_failed("ST_IOCTL_STATS_RESET");
    }

    printf("Statistiche azzerate.\n");
    return 0;
}

enum registry { REG_UID, REG_PROGRAM, REG_SYSCALL };

static const struct registry_info {
    const char *ioctl_name, *title, *empty, *allocation_error;
    const char *count_error, *growth_error, *unstable_error;
    size_t item_size;
} registries[] = {
    { "UID", "UID registrati", "nessuno",
      "Impossibile allocare memoria per %u UID.\n",
      "Risposta UID_LIST non valida ricevuta dal driver.\n",
      "Il driver ha restituito una capacità UID_LIST non valida.\n",
      "Impossibile ottenere la lista: il registro UID è stato modificato ripetutamente.\n",
      sizeof(__u32) },
    { "PROGRAM", "Programmi registrati", "nessuno",
      "Memoria insufficiente per la lista dei programmi.\n",
      "Risposta PROGRAM_LIST non valida: conteggio superiore alla capacità.\n",
      "Risposta PROGRAM_LIST non valida dopo ENOSPC.\n",
      "Il registro programmi è cambiato troppe volte durante la consultazione.\n",
      sizeof(struct st_program_name) },
    { "SYSCALL", "System call registrate", "nessuna",
      "Memoria insufficiente per la lista delle system call.\n",
      "Risposta SYSCALL_LIST non valida: count supera la capacità.\n",
      "Risposta SYSCALL_LIST non valida: capacità richiesta non crescente.\n",
      "Impossibile ottenere una lista stabile delle system call dopo più tentativi.\n",
      sizeof(__u32) },
};

/* Ogni ioctl usa il proprio tipo UAPI: nessun cast tra strutture diverse. */
static int read_count(int fd, enum registry kind, __u32 *count)
{
    int result;
    __u32 reserved;

    if (kind == REG_UID) {
        struct st_uid_count response = {0};
        result = ioctl(fd, ST_IOCTL_UID_GET_COUNT, &response);
        *count = response.count;
        reserved = 0; /* UID_GET_COUNT non validava reserved. */
    } else if (kind == REG_PROGRAM) {
        struct st_program_count response = {0};
        result = ioctl(fd, ST_IOCTL_PROGRAM_GET_COUNT, &response);
        *count = response.count;
        reserved = response.reserved;
    } else {
        struct st_syscall_count response = {0};
        result = ioctl(fd, ST_IOCTL_SYSCALL_GET_COUNT, &response);
        *count = response.count;
        reserved = response.reserved;
    }
    if (result == -1)
        return fail("ioctl ST_IOCTL_%s_GET_COUNT fallita: %s\n",
                    registries[kind].ioctl_name, strerror(errno));
    if (reserved)
        return fail("Risposta %s_GET_COUNT non valida.\n", registries[kind].ioctl_name);
    return 0;
}

static int read_list(int fd, enum registry kind, void *items, __u32 capacity,
                     __u32 *count, int *bad_reserved)
{
    __u64 pointer = (__u64)(uintptr_t)items;
    int result;

    *bad_reserved = 0;
    if (kind == REG_UID) {
        struct st_uid_list_request request = {.uids_ptr = pointer, .capacity = capacity};
        result = ioctl(fd, ST_IOCTL_UID_LIST, &request);
        *count = request.count;
    } else if (kind == REG_PROGRAM) {
        struct st_program_list_request request = {.programs_ptr = pointer, .capacity = capacity};
        result = ioctl(fd, ST_IOCTL_PROGRAM_LIST, &request);
        *count = request.count;
    } else {
        struct st_syscall_list_request request = {.numbers_ptr = pointer, .capacity = capacity};
        result = ioctl(fd, ST_IOCTL_SYSCALL_LIST, &request);
        *count = request.count;
        *bad_reserved = request.reserved[0] || request.reserved[1];
    }
    return result;
}

static int print_list(enum registry kind, const void *items, __u32 count)
{
    const struct st_program_name *programs = items;
    const __u32 *numbers = items;

    if (kind == REG_SYSCALL) {
        for (__u32 i = 1; i < count; i++)
            if (numbers[i] <= numbers[i - 1])
                return fail("Risposta SYSCALL_LIST non valida: ordine dei numeri incoerente.\n");
    }
    printf("%s: %u\n", registries[kind].title, count);
    if (!count)
        printf("  %s\n", registries[kind].empty);
    for (__u32 i = 0; i < count; i++) {
        if (kind == REG_PROGRAM) {
            if (!memchr(programs[i].name, '\0', sizeof(programs[i].name)))
                return fail("Risposta PROGRAM_LIST non valida: nome non terminato.\n");
            printf("  %s\n", programs[i].name);
        } else {
            printf("  %u\n", numbers[i]);
        }
    }
    return 0;
}

static int execute_registry(int fd, enum registry kind, int list)
{
    const struct registry_info *info = &registries[kind];
    __u32 capacity;

    if (read_count(fd, kind, &capacity))
        return 1;
    if (!list) {
        printf("%s: %u\n", info->title, capacity);
        return 0;
    }
    /* Solo SYSCALL_LIST interroga il driver anche con conteggio iniziale zero. */
    if (!capacity && kind != REG_SYSCALL)
        return print_list(kind, NULL, 0);

    /* ENOSPC aggiorna la capacità; restano al massimo quattro tentativi. */
    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        void *items = capacity ? calloc(capacity, info->item_size) : NULL;
        __u32 count;
        int bad_reserved, result, saved_errno;

        if (capacity && !items)
            return fail(info->allocation_error, capacity);
        result = read_list(fd, kind, items, capacity, &count, &bad_reserved);
        saved_errno = errno;
        if (bad_reserved) {
            free(items);
            return fail("Risposta SYSCALL_LIST non valida: campi reserved modificati.\n");
        }
        if (result == 0) {
            result = count > capacity ? fail("%s", info->count_error)
                                      : print_list(kind, items, count);
            free(items);
            return result;
        }
        free(items);
        if (saved_errno != ENOSPC)
            return fail("ioctl ST_IOCTL_%s_LIST fallita: %s\n",
                        info->ioctl_name, strerror(saved_errno));
        if (count <= capacity)
            return fail("%s", info->growth_error);
        capacity = count;
    }
    return fail("%s", info->unstable_error);
}

struct simple_command {
    const char *name;
    int (*execute)(int fd);
    enum registry registry;
    int list;
};

static const struct simple_command simple_commands[] = {
    {"ping", execute_ping, 0, 0},
    {"status", execute_status, 0, 0},
    {"enable", execute_enable, 0, 0},
    {"disable", execute_disable, 0, 0},
    {"uid-count", NULL, REG_UID, 0},
    {"uid-list", NULL, REG_UID, 1},
    {"program-count", NULL, REG_PROGRAM, 0},
    {"program-list", NULL, REG_PROGRAM, 1},
    {"syscall-count", NULL, REG_SYSCALL, 0},
    {"syscall-list", NULL, REG_SYSCALL, 1},
    {"max-get", execute_max_get, 0, 0},
    {"stats", execute_statistics_get, 0, 0},
    {"stats-reset", execute_statistics_reset, 0, 0},
};

static const struct simple_command *find_simple_command(const char *name)
{
    for (size_t i = 0; i < sizeof(simple_commands) / sizeof(simple_commands[0]); i++)
        if (strcmp(name, simple_commands[i].name) == 0)
            return &simple_commands[i];
    return NULL;
}

static int execute_syscall_update(int fd, __u32 number, int add)
{
    struct st_syscall_request request = {.number = number, .reserved = 0U};
    unsigned long command = add ? ST_IOCTL_SYSCALL_ADD : ST_IOCTL_SYSCALL_REMOVE;

    if (ioctl(fd, command, &request) == -1) {
        if (errno == EPERM)
            return fail("%s system call non consentita: sono richiesti privilegi root.\n",
                    add ? "Registrazione" : "Rimozione");
        else if (errno == (add ? EEXIST : ENOENT))
            return fail("System call %u %s registrata.\n", number, add ? "già" : "non");
        else if (add && errno == EOPNOTSUPP)
            return fail("System call %u non supportata dal driver.\n", number);
        else if (errno == EINVAL)
            return fail("Numero di system call non valido per l'ABI x86-64 corrente: %u.\n", number);
        else if (errno == EFAULT)
            return fail("Richiesta SYSCALL_%s non accessibile dal kernel.\n", add ? "ADD" : "REMOVE");
        else
            return fail("ioctl ST_IOCTL_SYSCALL_%s fallita: %s\n", add ? "ADD" : "REMOVE", strerror(errno));
    }

    printf("System call %u %s.\n", number, add ? "registrata" : "rimossa");
    return 0;
}

enum operation {
    OP_SIMPLE,
    OP_UID_ADD, OP_UID_REMOVE,
    OP_PROGRAM_ADD, OP_PROGRAM_REMOVE,
    OP_SYSCALL_ADD, OP_SYSCALL_REMOVE,
    OP_MAX_SET
};

static const struct {
    const char *name;
    enum operation operation;
} argument_commands[] = {
    {"uid-add", OP_UID_ADD},
    {"uid-remove", OP_UID_REMOVE},
    {"program-add", OP_PROGRAM_ADD},
    {"program-remove", OP_PROGRAM_REMOVE},
    {"syscall-add", OP_SYSCALL_ADD},
    {"syscall-remove", OP_SYSCALL_REMOVE},
    {"max-set", OP_MAX_SET},
};

int main(int argc, char *argv[])
{
    const struct simple_command *simple = NULL;
    enum operation operation = OP_SIMPLE;
    __u32 number = 0U;
    uint64_t max_invocations = 0U;
    const char *invalid = NULL;
    int fd, result;

    if (argc == 2) {
        simple = find_simple_command(argv[1]);
        if (simple == NULL)
            goto usage;
    } else if (argc == 3) {
        size_t i;
        for (i = 0; i < sizeof(argument_commands) / sizeof(argument_commands[0]); i++) {
            if (strcmp(argv[1], argument_commands[i].name) == 0) {
                operation = argument_commands[i].operation;
                break;
            }
        }
        if (operation == OP_SIMPLE)
            goto usage;

        switch (operation) {
        case OP_UID_ADD:
        case OP_UID_REMOVE:
            if (parse_u32(argv[2], &number) != 0)
                invalid = "UID non valido";
            break;
        case OP_SYSCALL_ADD:
        case OP_SYSCALL_REMOVE:
            if (parse_u32(argv[2], &number) != 0)
                invalid = "Numero di system call non valido";
            break;
        case OP_PROGRAM_ADD:
        case OP_PROGRAM_REMOVE:
            if (validate_program_name(argv[2]) != 0)
                invalid = "Nome programma non valido";
            break;
        case OP_MAX_SET:
            if (parse_decimal(argv[2], UINT64_MAX, &max_invocations) != 0)
                invalid = "Valore MAX non valido";
            break;
        case OP_SIMPLE:
            goto usage;
        }
        if (invalid != NULL)
            return fail("%s: %s\n", invalid, argv[2]);
    } else {
        goto usage;
    }

    /* Nessun accesso al device prima della validazione completa degli argomenti. */
    fd = open(ST_DEVICE_PATH, O_RDWR);
    if (fd == -1)
        return fail("Impossibile aprire %s: %s\n", ST_DEVICE_PATH, strerror(errno));

    switch (operation) {
    case OP_UID_ADD:
    case OP_UID_REMOVE:
        result = execute_uid_update(fd, number, operation == OP_UID_ADD);
        break;
    case OP_PROGRAM_ADD:
    case OP_PROGRAM_REMOVE:
        result = execute_program_update(fd, argv[2], operation == OP_PROGRAM_ADD);
        break;
    case OP_SYSCALL_ADD:
    case OP_SYSCALL_REMOVE:
        result = execute_syscall_update(fd, number, operation == OP_SYSCALL_ADD);
        break;
    case OP_MAX_SET:
        result = execute_max_set(fd, max_invocations);
        break;
    case OP_SIMPLE:
        result = simple->execute ? simple->execute(fd)
                                 : execute_registry(fd, simple->registry, simple->list);
        break;
    default:
        result = 1;
        break;
    }

    if (close(fd) == -1)
        return fail("Chiusura di %s fallita: %s\n", ST_DEVICE_PATH, strerror(errno));
    return result;

usage:
    print_usage(argv[0]);
    return 1;
}
