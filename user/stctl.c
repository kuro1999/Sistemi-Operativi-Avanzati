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
    struct st_uid_request request = {.uid = uid};
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

    printf("MAX: %llu invocazioni per finestra globale di un secondo.\n",
           (unsigned long long)response.max_invocations);

    return 0;
}

static int execute_statistics_get(int fd)
{
    struct st_statistics_snapshot snapshot = {0};
    double average_blocked;

    if (ioctl(fd, ST_IOCTL_STATS_GET, &snapshot) == -1)
        return ioctl_failed("ST_IOCTL_STATS_GET");

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

    if (snapshot.peak_valid == 0U) {
        printf("Peak delay: non disponibile\n");
        printf("Effective UID del peak: non disponibile\n");
        printf("Programma del peak: non disponibile\n");
    } else {
        printf("Peak delay: %llu ns (%.6f ms)\n",
               (unsigned long long)snapshot.peak_delay_ns,
               (double)snapshot.peak_delay_ns / 1000000.0);
        printf("Effective UID del peak: %u\n", snapshot.peak_euid);
        printf("Programma del peak: %s\n", snapshot.peak_program);
    }

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

    if (kind == REG_UID) {
        struct st_uid_count response = {0};
        result = ioctl(fd, ST_IOCTL_UID_GET_COUNT, &response);
        *count = response.count;
    } else if (kind == REG_PROGRAM) {
        struct st_program_count response = {0};
        result = ioctl(fd, ST_IOCTL_PROGRAM_GET_COUNT, &response);
        *count = response.count;
    } else {
        struct st_syscall_count response = {0};
        result = ioctl(fd, ST_IOCTL_SYSCALL_GET_COUNT, &response);
        *count = response.count;
    }
    if (result == -1)
        return fail("ioctl ST_IOCTL_%s_GET_COUNT fallita: %s\n",
                    registries[kind].ioctl_name, strerror(errno));
    return 0;
}

static int read_list(int fd, enum registry kind, void *items, __u32 capacity,
                     __u32 *count)
{
    __u64 pointer = (__u64)(uintptr_t)items;
    int result;

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
        int result, saved_errno;

        if (capacity && !items)
            return fail(info->allocation_error, capacity);
        result = read_list(fd, kind, items, capacity, &count);
        saved_errno = errno;
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

static int execute_syscall_update(int fd, __u32 number, int add)
{
    struct st_syscall_request request = {.number = number};
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

enum operation { OP_SIMPLE, OP_COUNT, OP_LIST, OP_UID, OP_PROGRAM, OP_SYSCALL, OP_MAX_SET };

/* Una sola tabella definisce sintassi, argomenti e operazione di ogni comando.
 * option indica il registro per COUNT/LIST, oppure add=1/remove=0 per gli update. */
static const struct command {
    const char *name, *argument;
    enum operation operation;
    int option;
    int (*execute)(int fd);
} commands[] = {
    {"ping",           NULL,       OP_SIMPLE,  0,           execute_ping},
    {"status",         NULL,       OP_SIMPLE,  0,           execute_status},
    {"enable",         NULL,       OP_SIMPLE,  0,           execute_enable},
    {"disable",        NULL,       OP_SIMPLE,  0,           execute_disable},
    {"uid-add",        "<UID>",    OP_UID,     1,           NULL},
    {"uid-remove",     "<UID>",    OP_UID,     0,           NULL},
    {"uid-count",      NULL,       OP_COUNT,   REG_UID,     NULL},
    {"uid-list",       NULL,       OP_LIST,    REG_UID,     NULL},
    {"program-add",    "<nome>",   OP_PROGRAM, 1,           NULL},
    {"program-remove", "<nome>",   OP_PROGRAM, 0,           NULL},
    {"program-count",  NULL,       OP_COUNT,   REG_PROGRAM, NULL},
    {"program-list",   NULL,       OP_LIST,    REG_PROGRAM, NULL},
    {"syscall-add",    "<numero>", OP_SYSCALL, 1,           NULL},
    {"syscall-remove", "<numero>", OP_SYSCALL, 0,           NULL},
    {"syscall-count",  NULL,       OP_COUNT,   REG_SYSCALL, NULL},
    {"syscall-list",   NULL,       OP_LIST,    REG_SYSCALL, NULL},
    {"max-set",        "<valore>", OP_MAX_SET, 0,           NULL},
    {"max-get",        NULL,       OP_SIMPLE,  0,           execute_max_get},
    {"stats",          NULL,       OP_SIMPLE,  0,           execute_statistics_get},
};

static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso:\n");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        fprintf(stderr, "  %s %s%s%s\n", program_name, commands[i].name,
                commands[i].argument ? " " : "", commands[i].argument ? commands[i].argument : "");
}

static const struct command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strcmp(name, commands[i].name) == 0)
            return &commands[i];
    return NULL;
}

static int execute_command(int fd, const struct command *command, const char *text, uint64_t value)
{
    switch (command->operation) {
    case OP_SIMPLE:
        return command->execute(fd);
    case OP_COUNT:
    case OP_LIST:
        return execute_registry(fd, command->option, command->operation == OP_LIST);
    case OP_UID:
        return execute_uid_update(fd, (__u32)value, command->option);
    case OP_PROGRAM:
        return execute_program_update(fd, text, command->option);
    case OP_SYSCALL:
        return execute_syscall_update(fd, (__u32)value, command->option);
    case OP_MAX_SET:
        return execute_max_set(fd, value);
    }
    return 1;
}

int main(int argc, char *argv[])
{
    const struct command *command;
    const char *text = argc == 3 ? argv[2] : NULL;
    uint64_t value = 0;
    int fd, result;

    if (argc < 2 || argc > 3 || !(command = find_command(argv[1])) ||
        argc != (command->argument ? 3 : 2)) {
        print_usage(argv[0]);
        return 1;
    }
    if (command->argument) {
        if (command->operation == OP_PROGRAM) {
            if (validate_program_name(text) != 0)
                return fail("Nome programma non valido: %s\n", text);
        } else if (parse_decimal(text, command->operation == OP_MAX_SET ? UINT64_MAX : UINT32_MAX,
                                 &value) != 0) {
            const char *invalid = command->operation == OP_UID ? "UID non valido" :
                command->operation == OP_SYSCALL ? "Numero di system call non valido" : "Valore MAX non valido";
            return fail("%s: %s\n", invalid, text);
        }
    }

    /* Nessun accesso al device prima della validazione completa degli argomenti. */
    fd = open(ST_DEVICE_PATH, O_RDWR);
    if (fd == -1)
        return fail("Impossibile aprire %s: %s\n", ST_DEVICE_PATH, strerror(errno));
    result = execute_command(fd, command, text, value);
    if (close(fd) == -1)
        return fail("Chiusura di %s fallita: %s\n", ST_DEVICE_PATH, strerror(errno));
    return result;
}
