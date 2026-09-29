#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <syscall_throttle.h>

static int control_fd;
static uid_t worker_uid;
static gid_t worker_gid;

static void message(const char *s)
{
    size_t left = strlen(s);
    while (left != 0) {
        ssize_t n = write(STDOUT_FILENO, s, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        s += n;
        left -= (size_t)n;
    }
}

static void require(bool ok, const char *s)
{
    if (!ok) {
        message("FAIL: ");
        message(s);
        message("\n");
        _exit(1);
    }
}

static long call_ioctl(int fd, unsigned long command, void *arg)
{
    return syscall(SYS_ioctl, fd, command, arg);
}

static void command(unsigned long cmd, void *arg)
{
    require(call_ioctl(control_fd, cmd, arg) == 0, "ioctl di controllo");
}

static void pause_ms(long ms)
{
    struct timespec t = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static double seconds(void)
{
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock_gettime");
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void set_max(unsigned long long n)
{
    struct st_max_config c = { .max_invocations = n };
    command(ST_IOCTL_MAX_SET, &c);
}

static void wait_ok(pid_t pid)
{
    int status;
    require(waitpid(pid, &status, 0) == pid, "waitpid");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "esito del processo figlio");
}

static void probe_blocked(int fd, unsigned long cmd)
{
    struct st_statistics_snapshot s;
    pid_t pid = fork();
    require(pid >= 0, "fork probe");
    if (pid == 0) {
        long ret = call_ioctl(fd, cmd, NULL);
        _exit(ret == -1 && errno == ENOTTY ? 0 : 2);
    }

    bool blocked = false;
    double deadline = seconds() + 2.0;
    while (seconds() < deadline) {
        int status;
        require(waitpid(pid, &status, WNOHANG) == 0,
                "ioctl estranea terminata prima di ottenere budget");
        command(ST_IOCTL_STATS_GET, &s);
        if (s.current_blocked == 1) {
            blocked = true;
            break;
        }
        pause_ms(10);
    }
    require(blocked, "ioctl estranea non osservata nel limiter");
    set_max(1);
    wait_ok(pid);
    set_max(0);
}

static void run_test(void)
{
    struct st_uid_request root = { .uid = 0 };
    struct st_uid_request user = { .uid = worker_uid };
    struct st_syscall_request number = { .number = SYS_ioctl };
    struct st_monitor_status status;
    struct st_statistics_snapshot stats;

    control_fd = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
    int other = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int alias = dup(control_fd);
    require(control_fd >= 0 && other >= 0 && alias >= 0,
            "apertura descriptor");

    command(ST_IOCTL_UID_ADD, &root);
    command(ST_IOCTL_UID_ADD, &user);
    command(ST_IOCTL_SYSCALL_ADD, &number);
    set_max(0);
    command(ST_IOCTL_ENABLE, NULL);

    command(ST_IOCTL_PING, NULL);
    require(call_ioctl(alias, ST_IOCTL_PING, NULL) == 0,
            "descriptor duplicato");
    command(ST_IOCTL_GET_STATUS, &status);
    require(status.enabled == 1, "monitor attivo");
    require(call_ioctl(control_fd, ST_IOCTL_MAX_SET, NULL) == -1 &&
            errno == EFAULT, "puntatore invalido non rifiutato");
    message("PASS: controllo e descriptor duplicato con MAX=0; EFAULT conservato\n");

    pid_t child = fork();
    require(child >= 0, "fork privilegi");
    if (child == 0) {
        require(setgroups(0, NULL) == 0 && setgid(worker_gid) == 0 &&
                setuid(worker_uid) == 0, "cambio credenziali");
        command(ST_IOCTL_GET_STATUS, &status);
        require(call_ioctl(control_fd, ST_IOCTL_DISABLE, NULL) == -1 &&
                errno == EPERM, "modifica non-root non rifiutata");
        _exit(0);
    }
    wait_ok(child);
    message("PASS: lettura non-root raggiungibile, modifica negata con EPERM\n");

    /*
     * Chiude la prova globale sugli UID e libera il traffico estraneo.
     * La fase successiva seleziona solo questo eseguibile.
     */
    command(ST_IOCTL_DISABLE, NULL);
    command(ST_IOCTL_GET_STATUS, &status);
    require(status.enabled == 0, "DISABLE con UID root registrato e MAX=0");
    command(ST_IOCTL_UID_REMOVE, &root);
    command(ST_IOCTL_UID_REMOVE, &user);
    message("PASS: DISABLE con UID root registrato e MAX=0\n");

    struct st_program_request program = {
        .name = "control_ioctl_regression"
    };
    command(ST_IOCTL_PROGRAM_ADD, &program);
    command(ST_IOCTL_ENABLE, NULL);
    command(ST_IOCTL_STATS_GET, &stats);
    require(stats.relevant_invocations == 0 &&
            stats.current_blocked == 0,
            "sessione isolata non vuota");
    message("FASE ISOLATA: selezione per programma, nessun UID registrato\n");

    probe_blocked(other, ST_IOCTL_PING);
    message("PASS: comando noto su altro oggetto sottoposto al limite\n");

    probe_blocked(control_fd, _IO(ST_IOCTL_MAGIC, 0x7f));
    message("PASS: comando sconosciuto sul monitor sottoposto al limite\n");

    command(ST_IOCTL_STATS_GET, &stats);
    require(stats.relevant_invocations == 2 &&
            stats.blocked_invocations == 2 &&
            stats.completed_blocked_invocations == 2 &&
            stats.interrupted_blocked_invocations == 0 &&
            stats.current_blocked == 0,
            "statistiche delle ioctl estranee");

    command(ST_IOCTL_DISABLE, NULL);
    command(ST_IOCTL_GET_STATUS, &status);
    require(status.enabled == 0, "DISABLE con MAX=0");

    command(ST_IOCTL_SYSCALL_REMOVE, &number);
    command(ST_IOCTL_PROGRAM_REMOVE, &program);
    close(alias);
    close(other);
    close(control_fd);
    message("PASS COMPLESSIVO: monitor OFF, registri vuoti, MAX=0\n");
}

int main(void)
{
    const char *uid_text = getenv("SUDO_UID");
    require(geteuid() == 0 && uid_text != NULL, "eseguire con sudo");
    worker_uid = (uid_t)strtoul(uid_text, NULL, 10);
    require(worker_uid != 0, "avviare sudo da un utente non-root");
    struct passwd *pw = getpwuid(worker_uid);
    require(pw != NULL, "utente di test assente");
    worker_gid = pw->pw_gid;

    control_fd = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
    require(control_fd >= 0, "modulo non caricato o device non accessibile");
    struct st_monitor_status status;
    struct st_uid_count u;
    struct st_program_count p;
    struct st_syscall_count s;

    command(ST_IOCTL_GET_STATUS, &status);
    command(ST_IOCTL_UID_GET_COUNT, &u);
    command(ST_IOCTL_PROGRAM_GET_COUNT, &p);
    command(ST_IOCTL_SYSCALL_GET_COUNT, &s);
    require(status.enabled == 0 &&
            u.count == 0 && p.count == 0 && s.count == 0,
            "richiesti monitor OFF e registri vuoti");
    close(control_fd);

    pid_t child = fork();
    require(child >= 0, "fork supervisore");
    if (child == 0) {
        require(setpgid(0, 0) == 0, "gruppo dei processi di test");
        run_test();
        _exit(0);
    }

    (void)setpgid(child, child);
    bool reaped = false;
    double deadline = seconds() + 15.0;

    while (seconds() < deadline) {
        int result;
        pid_t ret = waitpid(child, &result, WNOHANG);
        if (ret == child) {
            reaped = true;
            if (WIFEXITED(result) && WEXITSTATUS(result) == 0)
                return 0;
            break;
        }
        require(ret == 0 || (ret < 0 && errno == EINTR),
                "wait supervisore");
        pause_ms(20);
    }

    message("FAIL o timeout: termino i processi di test e tento il recupero\n");
    (void)kill(-child, SIGKILL);
    if (!reaped)
        (void)waitpid(child, NULL, 0);

    for (int attempt = 0; attempt < 30; attempt++) {
        if (syscall(SYS_delete_module, "syscall_throttle", O_NONBLOCK) == 0 ||
            errno == ENOENT) {
            message("Recupero: modulo scaricato; test NON superato\n");
            return 1;
        }
        pause_ms(100);
    }

    message("Recupero non completato: non proseguire, inviare questo output\n");
    return 1;
}
