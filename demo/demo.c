#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <syscall_throttle.h>
#include "protocol.h"

#define NS 1000000000ULL
#define MAX_WORKERS 8
static int device = -1;
static uid_t demo_uid;
static gid_t demo_gid;
enum syscall_index { CALL_GETPID, CALL_GETPPID, CALL_READ, CALL_EXIT,
                     CALL_EXIT_GROUP, CALL_DELETE_MODULE, CALL_COUNT };
static bool owned, uid_added, program_added[2], syscall_added[CALL_COUNT];
static const char *programs[] = {"demo_worker", "demo_alt"};
static const unsigned int numbers[] = {SYS_getpid, SYS_getppid, SYS_read,
                                       SYS_exit, SYS_exit_group, SYS_delete_module};
static char directory[PATH_MAX];
static volatile sig_atomic_t interrupted;
struct worker {
    pid_t pid;
    int gate, output, data;
    bool done;
    unsigned int number;
    int program;
    int expected_error; /* Solo per delete_module; le altre chiamate devono riuscire. */
    struct demo_result result;
};
static struct worker workers[MAX_WORKERS];
static size_t worker_count;
static bool cleanup(void);
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * NS + (uint64_t)t.tv_nsec;
}
static void stop_signal(int sig) { interrupted = sig; }
static void fail(const char *message, int code)
{
    fprintf(stderr, "\n%s: %s\n", code == 3 ? "INCONCLUSIVO" : "FAIL", message);
    if (!cleanup()) fprintf(stderr, "Cleanup incompleto: controllare device e log kernel.\n");
    exit(code);
}
static void check(bool ok, const char *message)
{
    if (interrupted) fail("esecuzione interrotta da segnale", 1);
    if (!ok) fail(message, 1);
}
static void pause_ms(unsigned int ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR)
        check(!interrupted, "segnale durante attesa");
}
static void command(unsigned long op, void *arg)
{
    if (ioctl(device, op, arg) < 0) {
        char message[160];
        snprintf(message, sizeof(message), "ioctl %#lx: %s", op, strerror(errno));
        fail(message, 1);
    }
}
static void set_max(uint64_t value)
{
    struct st_max_config c = {.max_invocations = value};
    command(ST_IOCTL_MAX_SET, &c);
}
static void uid_change(bool add)
{
    struct st_uid_request r = {.uid = demo_uid};
    command(add ? ST_IOCTL_UID_ADD : ST_IOCTL_UID_REMOVE, &r);
    uid_added = add;
}
static void program_change(int index, bool add)
{
    struct st_program_request r = {0};
    strcpy(r.name, programs[index]);
    command(add ? ST_IOCTL_PROGRAM_ADD : ST_IOCTL_PROGRAM_REMOVE, &r);
    program_added[index] = add;
}
static void syscall_change(int index, bool add)
{
    struct st_syscall_request r = {.number = numbers[index]};
    command(add ? ST_IOCTL_SYSCALL_ADD : ST_IOCTL_SYSCALL_REMOVE, &r);
    syscall_added[index] = add;
}
static struct st_statistics_snapshot snapshot(void)
{
    struct st_statistics_snapshot s = {0};
    command(ST_IOCTL_STATS_GET, &s);
    check(s.current_blocked <= s.peak_blocked && s.session_active <= 1 &&
          s.peak_valid <= 1, "snapshot incoerente");
    double average = s.observation_ns ? (double)s.blocked_thread_time_ns / s.observation_ns : 0;
    check(average <= s.peak_blocked + 0.000001, "media oltre il picco");
    check(!s.peak_valid || memchr(s.peak_program, 0, sizeof(s.peak_program)),
          "nome peak privo di terminatore");
    return s;
}
static void print_stats(const char *label)
{
    struct st_statistics_snapshot s = snapshot();
    printf("  Statistiche: %s\n"
           "    Thread attualmente sospesi: %u\n"
           "    Massimo numero di thread sospesi: %u\n"
           "    Media temporale dei thread sospesi: %.6f\n"
           "    Durata di osservazione: %.3f s\n",
           label, s.current_blocked, s.peak_blocked,
           s.observation_ns ? (double)s.blocked_thread_time_ns / s.observation_ns : 0,
           (double)s.observation_ns / NS);
    printf("    Integrale dei thread sospesi: %.6f thread*s\n",
           (double)s.blocked_thread_time_ns / NS);
    if (!s.peak_valid)
        puts("    Ritardo massimo: non disponibile (nessuna attesa conclusa nella sessione).");
    if (s.peak_valid)
        printf("    Ritardo massimo nel limiter: %.3f ms\n"
               "    Programma associato: %s; UID effettivo: %u\n",
               (double)s.peak_delay_ns / 1000000, s.peak_program, s.peak_euid);
}
static void wait_current(unsigned int expected)
{
    uint64_t deadline = now_ns() + 5 * NS;
    while (snapshot().current_blocked != expected) {
        if (now_ns() >= deadline) {
            struct st_statistics_snapshot s = snapshot();
            fprintf(stderr, "Diagnostica: attesi=%u, correnti=%u, picco=%u, sessione=%u\n",
                    expected, s.current_blocked, s.peak_blocked, s.session_active);
            for (size_t i = 0; i < worker_count; i++) {
                char path[96], state[160] = "non disponibile";
                snprintf(path, sizeof(path), "/proc/%ld/wchan", (long)workers[i].pid);
                FILE *f = fopen(path, "r");
                if (f) {
                    if (!fgets(state, sizeof(state), f)) strcpy(state, "non leggibile");
                    fclose(f);
                }
                fprintf(stderr, "Worker PID=%ld, syscall=%u, wchan=%s\n",
                        (long)workers[i].pid, workers[i].number, state);
            }
            fail("numero di waiter inatteso (timeout)", 1);
        }
        pause_ms(5);
    }
}
static void reap_status(struct worker *w, int expected_status)
{
    uint64_t deadline = now_ns() + 3 * NS;
    int status;
    for (;;) {
        pid_t result = waitpid(w->pid, &status, WNOHANG);
        if (result == w->pid) break;
        check(result == 0 || (result < 0 && errno == EINTR), "waitpid worker");
        check(now_ns() < deadline, "worker non termina dopo il risultato");
        pause_ms(5);
    }
    if (expected_status == DEMO_EXIT_STATUS)
        printf("    waitpid PID=%ld: uscita normale=%s, stato atteso=%d, osservato=%d.\n",
               (long)w->pid, WIFEXITED(status) ? "si" : "no", expected_status,
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    w->pid = 0;
    check(WIFEXITED(status) && WEXITSTATUS(status) == expected_status, "stato di uscita worker inatteso");
}
static bool receive(int fd, struct demo_result *r, int timeout_ms)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int ret;
    do { ret = poll(&p, 1, timeout_ms); } while (ret < 0 && errno == EINTR && !interrupted);
    check(ret >= 0, "poll risultato");
    if (!ret) return false;
    check(p.revents & POLLIN, "worker chiuso senza messaggio");
    /* Ogni record viene scritto in pipe con dimensione inferiore a PIPE_BUF. */
    check(read(fd, r, sizeof(*r)) == (ssize_t)sizeof(*r), "record worker incompleto");
    return true;
}
static struct worker *spawn_mode(int program, uid_t uid, unsigned int number, bool force)
{
    check(worker_count < MAX_WORKERS, "troppi worker");
    int gate[2], output[2], data[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, gate) == 0, "socketpair");
    check(pipe2(output, O_CLOEXEC) == 0, "pipe risultato");
    check(pipe2(data, O_CLOEXEC) == 0, "pipe dati");
    struct worker *w = &workers[worker_count++];
    *w = (struct worker){.gate=gate[0], .output=output[0], .data=data[1], .number=number,
                         .program=program,
                         .expected_error=number==SYS_delete_module ? (force ? EPERM : EWOULDBLOCK) : 0};
    char path[PATH_MAX], a[24], b[24], c[24], d[24];
    check(snprintf(path, sizeof(path), "%s/%s", directory, programs[program]) < (int)sizeof(path), "percorso worker");
    snprintf(a,sizeof(a),"%d",gate[1]); snprintf(b,sizeof(b),"%d",output[1]);
    snprintf(c,sizeof(c),"%u",number); snprintf(d,sizeof(d),"%d",data[0]);
    pid_t pid = fork();
    check(pid >= 0, "fork worker");
    if (!pid) {
        if (fcntl(gate[1], F_SETFD, 0) || fcntl(output[1], F_SETFD, 0) ||
            fcntl(data[0], F_SETFD, 0)) _exit(120);
        if (uid && (setgroups(0,NULL) || setgid(demo_gid) || setuid(uid))) _exit(121);
        if (force) execl(path, path, a, b, c, d, "force", (char *)NULL);
        else execl(path, path, a, b, c, d, (char *)NULL);
        _exit(122);
    }
    w->pid=pid;
    close(gate[1]); close(output[1]); close(data[0]);
    struct demo_result ready;
    check(receive(w->output,&ready,5000) && ready.kind == DEMO_READY && ready.euid == uid,
          "worker non pronto: verificare permessi dei percorsi e credenziali");
    return w;
}
static struct worker *spawn(int program, uid_t uid, unsigned int number)
{
    return spawn_mode(program, uid, number, false);
}
static void go(struct worker *w)
{
    check(send(w->gate,"G",1,MSG_NOSIGNAL) == 1, "rilascio gate");
}
static bool collect(struct worker *w, int timeout_ms)
{
    if (w->done) return true;
    if (!receive(w->output,&w->result,timeout_ms)) return false;
    if (w->result.kind == DEMO_RESULT && w->result.error != w->expected_error)
        fprintf(stderr,"Syscall %u: errno atteso=%d, ricevuto=%d (%s)\n",
                w->number,w->expected_error,w->result.error,strerror(w->result.error));
    check(w->result.kind == DEMO_RESULT && w->result.error == w->expected_error &&
          w->result.end_ns >= w->result.start_ns, "syscall worker fallita");
    check(w->number == SYS_delete_module ? w->result.value == -1 :
          w->number == SYS_read ? w->result.value == 1 :
          (w->number == SYS_getpid ? w->result.value == w->pid : w->result.value == getpid()),
          "valore restituito dalla syscall inatteso");
    w->done=true;
    return true;
}
static void finish(struct worker *w)
{
    check(collect(w,12000), "timeout completamento syscall");
    reap_status(w, 0);
}
static void wait_worker_blocked(struct worker *w)
{
    char path[96], state[160] = "non disponibile";
    snprintf(path, sizeof(path), "/proc/%ld/wchan", (long)w->pid);
    uint64_t deadline = now_ns() + 5 * NS;
    for (;;) {
        check(!collect(w, 0), "worker completato mentre deve essere sospeso");
        FILE *f = fopen(path, "r");
        check(f != NULL, "apertura wchan del worker");
        bool readable = fgets(state, sizeof(state), f) != NULL;
        fclose(f);
        if (readable && strstr(state, "st_rate_limiter_wait_for_change")) {
            state[strcspn(state,"\n")]=0;
            printf("    /proc/%ld/wchan = %s\n",(long)w->pid,state);
            return;
        }
        if (now_ns() >= deadline) {
            fprintf(stderr, "Worker PID=%ld: wchan=%s\n", (long)w->pid, state);
            fail("attesa individuale non confermata: controllare wchan e simboli del modulo", 3);
        }
        pause_ms(5);
    }
}
static void expect_blocked(struct worker *w)
{
    if (uid_added) {
        /* Il registro EUID puo' selezionare anche processi esterni alla demo. */
        wait_worker_blocked(w);
        check(!collect(w, 150), "worker liberato con MAX=0 e identita' ancora selezionata");
        wait_worker_blocked(w);
        struct st_statistics_snapshot s = snapshot();
        check(s.current_blocked >= 1, "worker in attesa ma contatore globale nullo");
        printf("  Processo PID=%ld sospeso nel limiter; thread attualmente sospesi nel limiter=%u, massimo osservato=%u.\n",
               (long)w->pid, s.current_blocked, s.peak_blocked);
        puts("    Nessun risultato nel controllo di 150 ms; waiter globale atteso >= 1.");
        return;
    }
    wait_current(1);
    check(!collect(w,150), "chiamata passata con MAX=0");
    struct st_statistics_snapshot s=snapshot();
    check(s.current_blocked == 1 && s.peak_blocked == 1 && !s.peak_valid,
          "statistiche attesa non coerenti");
    printf("    PID=%ld: nessun risultato nel controllo di 150 ms; waiter=%u (atteso 1), "
           "picco=%u, peak_valid=%u (atteso 0).\n",
           (long)w->pid,s.current_blocked,s.peak_blocked,s.peak_valid);
}
/* Cleanup solo della configurazione posseduta dalla demo. Nessun unload automatico. */
static bool cleanup(void)
{
    if (!owned) return true;
    bool ok=true;
    /* La prova delete_module chiude il device per non mascherare il pin dell'hook. */
    if (device < 0) device = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (ioctl(device,ST_IOCTL_DISABLE,0)<0) ok=false;
    for (size_t i=0;i<worker_count;i++) {
        struct worker *w=&workers[i];
        if (w->pid > 0) {
            if (kill(w->pid,SIGKILL)<0 && errno!=ESRCH) ok=false;
            int status; pid_t ret;
            do { ret=waitpid(w->pid,&status,0); } while (ret<0 && errno==EINTR);
            if (ret<0 && errno!=ECHILD) ok=false;
        }
        close(w->gate); close(w->output); close(w->data);
    }
    worker_count=0;
    for (int i=0;i<CALL_COUNT;i++) if (syscall_added[i]) {
        struct st_syscall_request r={.number=numbers[i]};
        if (ioctl(device,ST_IOCTL_SYSCALL_REMOVE,&r)<0) ok=false;
        else syscall_added[i]=false;
    }
    for (int i=0;i<2;i++) if (program_added[i]) {
        struct st_program_request r={0}; strcpy(r.name,programs[i]);
        if (ioctl(device,ST_IOCTL_PROGRAM_REMOVE,&r)<0) ok=false;
        else program_added[i]=false;
    }
    if (uid_added) {
        struct st_uid_request r={.uid=demo_uid};
        if (ioctl(device,ST_IOCTL_UID_REMOVE,&r)<0) ok=false;
        else uid_added=false;
    }
    struct st_max_config m={0};
    if (ioctl(device,ST_IOCTL_MAX_SET,&m)<0) ok=false;
    return ok;
}
static void reset_case(void) { check(cleanup(), "cleanup del caso precedente fallito"); }
/* Un rifiuto previsto e' un successo del controllo, non un errore della demo. */
static void expect_error(const char *label, unsigned long op, void *arg, int expected)
{
    errno=0;
    int result=ioctl(device,op,arg);
    int observed=errno;
    if (result!=-1 || observed!=expected) {
        fprintf(stderr,"    %s: ritorno=%d, errno=%d (%s); attesi -1 e errno=%d.\n",
                label,result,observed,strerror(observed),expected);
        fail("risposta ioctl diversa da quella attesa",1);
    }
    const char *reason = expected==EEXIST ? "duplicato rifiutato correttamente (EEXIST)" :
                         expected==ENOENT ? "elemento assente, rimozione rifiutata correttamente (ENOENT)" :
                         "buffer insufficiente segnalato correttamente (ENOSPC)";
    printf("    %s: %s.\n",label,reason);
}
static void read_lists(void)
{
    __u32 ids[2], calls[2]; struct st_program_name names[2];
    struct st_uid_list_request u={.uids_ptr=(uintptr_t)ids,.capacity=2};
    struct st_program_list_request p={.programs_ptr=(uintptr_t)names,.capacity=2};
    struct st_syscall_list_request s={.numbers_ptr=(uintptr_t)calls,.capacity=2};
    command(ST_IOCTL_UID_LIST,&u); command(ST_IOCTL_PROGRAM_LIST,&p); command(ST_IOCTL_SYSCALL_LIST,&s);
    check(u.count==1 && ids[0]==demo_uid && p.count==1 &&
          !strcmp(names[0].name,programs[0]) && s.count==1 && calls[0]==SYS_getpid,
          "elenchi registrati errati");
    printf("    UID: [%u]\n    Programmi: [%s]\n    Syscall: [%u]\n",
           ids[0],names[0].name,calls[0]);
    puts("    PASS: le liste contengono gli elementi appena aggiunti.");
}
static void insufficient_lists(void)
{
    /* Capacita' e' lo spazio offerto dal chiamante, non la dimensione del registro.
     * Anche count parte da zero: verifichiamo che il driver lo aggiorni a uno. */
    struct st_uid_list_request u={0};
    struct st_program_list_request p={0};
    struct st_syscall_list_request s={0};
    expect_error("UID",ST_IOCTL_UID_LIST,&u,ENOSPC);
    expect_error("Programmi",ST_IOCTL_PROGRAM_LIST,&p,ENOSPC);
    expect_error("Syscall",ST_IOCTL_SYSCALL_LIST,&s,ENOSPC);
    check(u.count==1 && p.count==1 && s.count==1,"dimensione richiesta errata");
    printf("    Spazio richiesto dal driver: UID=%u, programmi=%u, syscall=%u elemento ciascuno.\n",
           u.count,p.count,s.count);
    puts("    PASS: ENOSPC e dimensioni necessarie verificati; i registri non sono vuoti.");
}
static void permissions(void)
{
    pid_t pid=fork(); check(pid>=0,"fork privilegi");
    if (!pid) {
        if (setgroups(0,NULL)||setgid(demo_gid)||setuid(demo_uid)) _exit(10);
        int fd=open(ST_DEVICE_PATH,O_RDWR|O_CLOEXEC);
        if (fd<0) _exit(11);
        struct st_monitor_status state;
        struct st_max_config m={.max_invocations=1};
        struct st_uid_request u={.uid=demo_uid};
        struct st_program_request p={0}; strcpy(p.name,programs[0]);
        struct st_syscall_request s={.number=SYS_getpid};
        struct st_uid_count uc; struct st_program_count pc; struct st_syscall_count sc;
        struct st_statistics_snapshot stats;
        __u32 ids[1], nums[1]; struct st_program_name names[1];
        struct st_uid_list_request ul={.uids_ptr=(uintptr_t)ids,.capacity=1};
        struct st_program_list_request pl={.programs_ptr=(uintptr_t)names,.capacity=1};
        struct st_syscall_list_request sl={.numbers_ptr=(uintptr_t)nums,.capacity=1};
        unsigned long reads[]={ST_IOCTL_GET_STATUS,ST_IOCTL_MAX_GET,ST_IOCTL_UID_GET_COUNT,
            ST_IOCTL_PROGRAM_GET_COUNT,ST_IOCTL_SYSCALL_GET_COUNT,ST_IOCTL_STATS_GET,
            ST_IOCTL_UID_LIST,ST_IOCTL_PROGRAM_LIST,ST_IOCTL_SYSCALL_LIST};
        void *outputs[]={&state,&m,&uc,&pc,&sc,&stats,&ul,&pl,&sl};
        for (size_t i=0;i<sizeof(reads)/sizeof(reads[0]);i++)
            if (ioctl(fd,reads[i],outputs[i])) _exit(12);
        unsigned long writes[]={ST_IOCTL_ENABLE,ST_IOCTL_DISABLE,ST_IOCTL_MAX_SET,
            ST_IOCTL_UID_ADD,ST_IOCTL_UID_REMOVE,ST_IOCTL_PROGRAM_ADD,
            ST_IOCTL_PROGRAM_REMOVE,ST_IOCTL_SYSCALL_ADD,ST_IOCTL_SYSCALL_REMOVE};
        void *args[]={NULL,NULL,&m,&u,&u,&p,&p,&s,&s};
        for (size_t i=0;i<sizeof(writes)/sizeof(writes[0]);i++)
            if (ioctl(fd,writes[i],args[i])!=-1 || errno!=EPERM) _exit(13);
        printf("    EUID effettivo del verificatore=%u: %zu letture riuscite su %zu, "
               "%zu modifiche rifiutate su %zu, tutte con EPERM (%d).\n",
               (unsigned int)geteuid(),sizeof(reads)/sizeof(reads[0]),sizeof(reads)/sizeof(reads[0]),
               sizeof(writes)/sizeof(writes[0]),sizeof(writes)/sizeof(writes[0]),EPERM);
        close(fd); _exit(0);
    }
    int status;
    check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "letture pubbliche o protezione EUID fallite");
}
static void registry_demo(void)
{
    puts("\n[1/6] Registri e permessi");
    puts("\n  1. Registrazione di tre elementi");
    /* Ogni conferma viene stampata solo dopo il controllo della ioctl. */
    uid_change(true);
    printf("    UID %u: registrazione riuscita.\n",demo_uid);
    program_change(0,true);
    printf("    Programma %s: registrazione riuscita.\n",programs[0]);
    syscall_change(0,true);
    printf("    Syscall %d: registrazione riuscita.\n",SYS_getpid);

    puts("\n  2. Lettura degli elementi registrati");
    read_lists();

    puts("\n  3. Tentativo di registrazione duplicata");
    puts("    Ripeto le stesse tre ADD: gli elementi sono gia' presenti.");
    struct st_uid_request u={.uid=demo_uid};
    struct st_program_request p={0}; strcpy(p.name,programs[0]);
    struct st_syscall_request s={.number=SYS_getpid};
    expect_error("UID",ST_IOCTL_UID_ADD,&u,EEXIST);
    expect_error("Programma",ST_IOCTL_PROGRAM_ADD,&p,EEXIST);
    expect_error("Syscall",ST_IOCTL_SYSCALL_ADD,&s,EEXIST);

    puts("\n  4. Lettura con buffer insufficiente");
    puts("    Ripeto le LIST offrendo spazio per zero elementi.");
    puts("    Capacita' zero riguarda il buffer: ciascun registro contiene ancora un elemento.");
    insufficient_lists();

    puts("\n  5. Controllo dei permessi");
    puts("    Un processo non-root prova letture e modifiche dei dati.");
    permissions();

    puts("\n  6. Rimozione degli elementi presenti");
    uid_change(false);
    printf("    UID %u: rimozione riuscita.\n",demo_uid);
    program_change(0,false);
    printf("    Programma %s: rimozione riuscita.\n",programs[0]);
    syscall_change(0,false);
    printf("    Syscall %d: rimozione riuscita.\n",SYS_getpid);
    struct st_uid_count uc;
    struct st_program_count pc;
    struct st_syscall_count sc;
    command(ST_IOCTL_UID_GET_COUNT,&uc);
    command(ST_IOCTL_PROGRAM_GET_COUNT,&pc);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&sc);
    check(!uc.count && !pc.count && !sc.count,"registri non vuoti dopo le rimozioni");
    printf("    Conteggi dopo le rimozioni: UID=%u, programmi=%u, syscall=%u (attesi tutti 0).\n",
           uc.count,pc.count,sc.count);

    puts("\n  7. Tentativo di rimozione di elementi assenti");
    puts("    Ripeto le stesse tre REMOVE: gli elementi sono gia' stati rimossi.");
    expect_error("UID",ST_IOCTL_UID_REMOVE,&u,ENOENT);
    expect_error("Programma",ST_IOCTL_PROGRAM_REMOVE,&p,ENOENT);
    expect_error("Syscall",ST_IOCTL_SYSCALL_REMOVE,&s,ENOENT);
    puts("\n  PASS FASE 1: registrazione, lettura, rimozione e controlli verificati.");
    reset_case();
}
static void matching_demo(void)
{
    puts("\n[2/6] Selezione delle syscall e rilascio dei processi");
    puts("  Regola: monitor ON + syscall registrata + (EUID registrato O programma registrato).");
    puts("  Uso MAX=0: se tutti i criteri sono soddisfatti, la chiamata deve restare sospesa.");
    puts("  Ogni caso riparte con monitor OFF e registri vuoti.");
    const char *labels[]={"Selezione tramite EUID",
                          "Selezione tramite programma",
                          "EUID e programma: basta un criterio per mantenere il blocco",
                          "Nessuna identita' registrata",
                          "Syscall invocata non registrata",
                          "Monitor disattivato"};
    const char *reasons[]={"rimossa l'unica identita' registrata (EUID)",
                           "rimossa l'unica identita' registrata (programma)",
                           "rimossi entrambi i criteri di identita'",
                           "ne' EUID ne' programma sono registrati",
                           "getppid non e' tra le syscall registrate",
                           "il monitor e' OFF, anche se identita' e syscall sono registrate"};
    for (int mode=0;mode<6;mode++) {
        printf("\n  Caso 2.%d - %s\n",mode+1,labels[mode]);
        bool u=mode==0||mode==2||mode==4||mode==5;
        bool p=mode==1||mode==2||mode==4||mode==5;
        if (u) uid_change(true);
        if (p) program_change(0,true);
        syscall_change(0,true); set_max(0);
        struct worker *w=spawn(0,demo_uid,mode==4?SYS_getppid:SYS_getpid);
        if (mode!=5) command(ST_IOCTL_ENABLE,NULL);

        /* Descrive la configurazione solo dopo che le ioctl sono riuscite. */
        printf("    Configurazione: monitor=%s, MAX=0, syscall registrata=getpid (%d).\n",
               mode==5?"OFF":"ON",SYS_getpid);
        printf("    EUID %u: %s; programma %s: %s.\n",demo_uid,
               u?"registrato":"non registrato",programs[0],p?"registrato":"non registrato");
        printf("    Worker PID=%ld, EUID=%u, programma=%s: invoca %s (%u).\n",
               (long)w->pid,demo_uid,programs[0],mode==4?"getppid":"getpid",w->number);
        if (mode<3)
            puts("    Atteso: chiamata sospesa, finche' rimane almeno un criterio di identita'.");
        else
            printf("    Atteso: chiamata ammessa, perche' %s.\n",reasons[mode]);
        go(w);

        if (mode<3) {
            if (u) {
                puts("    Verifico il worker in wchan prima e dopo 150 ms senza risultato.");
                puts("    Il contatore globale puo' includere altri processi dello stesso EUID: non deve essere per forza 1.");
            }
            expect_blocked(w);
            puts("    Osservato: il worker non completa la chiamata ed e' sospeso nel limiter.");
            if (p) {
                puts("    Modifica: rimuovo soltanto il programma dal registro.");
                program_change(0,false);
            }
            if (mode==2) {
                puts("    L'EUID resta registrato: atteso ancora BLOCCATO (la regola usa OR).");
                expect_blocked(w);
                puts("    Osservato: ancora sospeso dopo la rimozione del programma.");
            }
            if (u) {
                puts("    Modifica: rimuovo l'EUID dal registro.");
                uid_change(false);
            }
            puts("    Non restano criteri di identita' per il worker: atteso rilascio, pur con ON e MAX=0.");
        }
        finish(w);
        printf("    Osservato: %s completata, valore=%lld, errno=%d, durata=%.3f ms.\n",
               mode==4?"getppid":"getpid",(long long)w->result.value,w->result.error,
               (double)(w->result.end_ns-w->result.start_ns)/1000000);
        if (mode<3)
            puts("    La durata comprende l'attesa provocata dalla demo prima della rimozione dei criteri.");
        if (!uid_added) wait_current(0);
        printf("    PASS: %s; valore restituito dalla syscall verificato.\n",reasons[mode]);
        reset_case();
        puts("    Fine caso: monitor OFF e registri svuotati per il caso successivo.");
    }
    puts("\n  PASS FASE 2: selezione per EUID O programma, rilascio e bypass verificati.");
}
static int compare_completion(const void *a, const void *b)
{
    const struct worker *wa = *(const struct worker *const *)a;
    const struct worker *wb = *(const struct worker *const *)b;
    return (wa->result.end_ns > wb->result.end_ns) -
           (wa->result.end_ns < wb->result.end_ns);
}
static void print_completions(uint64_t origin)
{
    const struct worker *ordered[MAX_WORKERS];
    for (size_t i=0;i<worker_count;i++) ordered[i]=&workers[i];
    qsort(ordered,worker_count,sizeof(ordered[0]),compare_completion);
    puts("  Completamenti dall'avvio del monitor (ordine osservato, non FIFO):");
    puts("    Programma      EUID   Syscall     Completamento     Durata chiamata");
    for (size_t i=0;i<worker_count;i++) {
        const struct worker *w=ordered[i];
        printf("    %-12s %5u   %-7s     +%8.3f ms     %8.3f ms\n",
               programs[w->program],w->result.euid,
               w->number==SYS_getpid ? "getpid" : "getppid",
               (double)(w->result.end_ns-origin)/1000000,
               (double)(w->result.end_ns-w->result.start_ns)/1000000);
    }
    puts("  Le durate user-space includono anche overhead e scheduling.");
}
static void budget_demo(void)
{
    puts("\n[3/6] Un unico budget condiviso tra tutti i processi");
    puts("\n  1. Configurazione e obiettivo");
    program_change(0,true); program_change(1,true);
    syscall_change(0,true); syscall_change(1,true); set_max(2);
    printf("    Programmi registrati: %s e %s.\n",programs[0],programs[1]);
    printf("    Syscall registrate: getpid (%d) e getppid (%d); MAX=2.\n",SYS_getpid,SYS_getppid);
    printf("    Avvio 8 worker: due programmi, EUID 0 e %u, due syscall.\n",demo_uid);
    puts("    I worker sono selezionati tramite il programma; gli EUID non vengono registrati.");
    puts("    Atteso: 2 chiamate complessive per finestra, non 2 per programma, utente o syscall.");
    puts("    Nella prima finestra devono completare 2 worker e gli altri 6 devono attendere.");
    for (int i=0;i<8;i++) spawn((i/4)%2,(i/2)%2?demo_uid:0,i%2?SYS_getppid:SYS_getpid);
    puts("    Avvio il monitor e i worker; raccolgo i dati senza stampare durante il campionamento.");
    uint64_t start=now_ns(); command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    unsigned int before_done=0, before_blocked=0;
    unsigned int after_samples=0, after_done=0, after_blocked=0;
    uint64_t before_time=0, repeat_end=0, first_after=0, last_after=0;
    bool repeated=false;
    /* Prima rileviamo 2 completamenti e 6 waiter, poi ripetiamo MAX=2.
     * Soltanto le letture successive alla ioctl contano come campioni "dopo".
     * Le stampe sono rinviate per non rallentare la finestra osservata. */
    while (now_ns()-start < 750000000ULL) {
        unsigned int done=0;
        for (size_t i=0;i<worker_count;i++) done+=collect(&workers[i],0);
        struct st_statistics_snapshot s=snapshot();
        uint64_t elapsed=now_ns()-start;
        if (elapsed>=750000000ULL) break;
        check(done<=2,"budget globale superato prima del primo rinnovo");
        if (done==2 && s.current_blocked==6) {
            if (!repeated) {
                before_done=done; before_blocked=s.current_blocked; before_time=elapsed;
                set_max(2);
                repeat_end=now_ns()-start;
                repeated=true;
            } else {
                if (!after_samples) first_after=elapsed;
                last_after=elapsed;
                after_done=done; after_blocked=s.current_blocked;
                after_samples++;
            }
        } else if (repeated) fail("budget o waiter cambiati prima del rinnovo",1);
        pause_ms(10);
    }
    if (!repeated || after_samples<3 || last_after<500000000ULL ||
        last_after-first_after<200000000ULL)
        fail("VM troppo lenta: campioni dopo MAX=2 insufficienti nella prima finestra",3);

    puts("\n  2. Osservazione della prima finestra (dati appena raccolti)");
    printf("    Prima di ripetere MAX=2, a +%.3f ms:\n",(double)before_time/1000000);
    printf("    Osservato: %u chiamate completate, %u worker sospesi.\n",before_done,before_blocked);
    puts("    Atteso:   2 chiamate completate, 6 worker sospesi.");

    puts("\n  3. Ripetizione di MAX senza cambiare il valore");
    printf("    Con 6 worker in attesa ho ripetuto MAX=2; ioctl conclusa a +%.3f ms.\n",
           (double)repeat_end/1000000);
    puts("    Atteso: nessun nuovo posto disponibile prima del rinnovo della finestra.");
    printf("    Dopo la ioctl: %u campioni coerenti da +%.3f a +%.3f ms.\n",
           after_samples,(double)first_after/1000000,(double)last_after/1000000);
    printf("    In tutti questi campioni: completamenti=%u (atteso 2), waiter=%u (atteso 6).\n",
           after_done,after_blocked);
    puts("    PASS: ripetere lo stesso MAX non ha rinnovato il budget nel periodo osservato.");

    puts("\n  4. Completamento nelle finestre successive");
    puts("    Lascio proseguire il monitor senza modificare la configurazione.");
    puts("    Atteso: i worker rimanenti avanzano al rinnovo del budget.");
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    struct st_statistics_snapshot s=snapshot();
    check(!s.current_blocked && s.peak_blocked==6 && s.peak_valid,
          "statistiche budget globale inattese");
    check(s.peak_euid==0 || s.peak_euid==demo_uid,"EUID peak inatteso");
    check(!strcmp(s.peak_program,programs[0])||!strcmp(s.peak_program,programs[1]),"programma peak inatteso");
    check(s.peak_delay_ns>=2*NS && s.peak_delay_ns<=s.observation_ns,"peak fuori sessione");
    printf("    Osservato: tutti gli %zu worker hanno completato la syscall con il valore atteso.\n",worker_count);
    print_completions(start);
    puts("    L'ordine puo' cambiare: il limite e' globale, senza garanzia FIFO.");
    puts("    Il budget riguarda l'ammissione; i tempi di completamento includono anche lo scheduling.");

    puts("\n  5. Statistiche e conclusione");
    print_stats("budget globale");
    printf("    Waiter finali=%u (atteso 0): tutti i worker hanno terminato.\n",s.current_blocked);
    printf("    Picco waiter=%u (atteso 6): nella prima finestra due passano e sei attendono.\n",s.peak_blocked);
    puts("    Media temporale = integrale dei thread sospesi / durata di osservazione; non deve essere esattamente 4.");
    puts("    Il ritardo massimo identifica l'attesa piu' lunga nel monitor, con programma ed EUID associati.");
    puts("\n  PASS FASE 3: budget condiviso, MAX invariato e completamento degli otto worker verificati.");
    reset_case();
}
static void statistics_demo(void)
{
    puts("\n[4/6] Media temporale, picchi e conservazione delle statistiche");
    puts("\n  1. Preparazione di quattro attese controllate");
    program_change(0,true); syscall_change(0,true); set_max(0);
    printf("    Registro %s e getpid (%d); imposto MAX=0 e avvio 4 worker con EUID=%u.\n",
           programs[0],SYS_getpid,demo_uid);
    puts("    Atteso con monitor ON: tutti e quattro sospesi, nessuna chiamata completata.");
    for (int i=0;i<4;i++) spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    wait_current(4);

    puts("\n  2. Misurazione della media in un intervallo con quattro waiter");
    puts("    Leggo lo snapshot A, attendo circa 200 ms, poi leggo lo snapshot B.");
    puts("    Atteso: incremento dell'integrale = 4 x tempo effettivamente trascorso.");
    /* Nessuna stampa tra le letture: il confronto usa i timestamp del modulo,
     * non assume che la pausa duri esattamente 200 ms. */
    struct st_statistics_snapshot a=snapshot();
    pause_ms(200);
    struct st_statistics_snapshot b=snapshot();
    check(a.current_blocked==4 && b.current_blocked==4 &&
          a.peak_blocked==4 && b.peak_blocked==4 && !a.peak_valid && !b.peak_valid,
          "numero dei waiter o picchi delle attese pendenti inattesi");
    check(b.observation_ns>a.observation_ns &&
          b.blocked_thread_time_ns>=a.blocked_thread_time_ns,"tempi statistici non crescenti");
    uint64_t elapsed=b.observation_ns-a.observation_ns;
    uint64_t weighted=b.blocked_thread_time_ns-a.blocked_thread_time_ns;
    check(weighted==4*elapsed,"integrale dei quattro waiter errato");
    printf("    Snapshot A: waiter=%u, tempo=%.6f s, integrale=%.6f thread*s.\n",
           a.current_blocked,(double)a.observation_ns/NS,(double)a.blocked_thread_time_ns/NS);
    printf("    Snapshot B: waiter=%u, tempo=%.6f s, integrale=%.6f thread*s.\n",
           b.current_blocked,(double)b.observation_ns/NS,(double)b.blocked_thread_time_ns/NS);
    printf("    Delta tempo=%.6f s.\n",(double)elapsed/NS);
    printf("    Incremento integrale osservato=%.6f thread*s; atteso=%.6f thread*s.\n",
           (double)weighted/NS,(double)(4*elapsed)/NS);
    printf("    Media nel solo intervallo A-B = delta integrale / delta tempo = %.6f (atteso 4).\n",
           (double)weighted/elapsed);
    puts("    PASS: uguaglianza verificata sui valori interi in nanosecondi, prima degli arrotondamenti.");
    puts("    La media dell'intera sessione puo' essere inferiore a 4: include anche l'arrivo progressivo dei worker.");

    puts("\n  3. Distinzione tra picco dei waiter e ritardo massimo");
    for (size_t i=0;i<worker_count;i++) check(!collect(&workers[i],0),"worker passato con MAX=0");
    printf("    Osservato: nessun risultato dai 4 worker; picco waiter=%u (atteso 4).\n",b.peak_blocked);
    printf("    Ritardo massimo disponibile: %s (atteso no).\n",b.peak_valid?"si":"no");
    puts("    Il picco dei waiter e' gia' noto; il ritardo massimo viene aggiornato quando un'attesa termina.");

    puts("\n  4. Disattivazione del monitor e conclusione delle attese");
    puts("    Invio OFF: atteso rilascio dei quattro worker anche se MAX resta zero.");
    command(ST_IOCTL_DISABLE,NULL);
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    /* Aspettiamo i risultati prima di leggere il picco delle attese concluse. */
    a=snapshot();
    check(!a.session_active && !a.current_blocked && a.peak_blocked==4 && a.peak_valid &&
          a.peak_delay_ns>=200000000ULL && a.peak_delay_ns<=a.observation_ns &&
          a.peak_euid==demo_uid && !strcmp(a.peak_program,programs[0]),"peak o chiusura sessione errati");
    puts("    Osservato: tutti e quattro hanno completato getpid con il valore atteso.");
    printf("    Sessione attiva=%u (atteso 0), waiter=%u (atteso 0), picco waiter=%u (atteso 4).\n",
           a.session_active,a.current_blocked,a.peak_blocked);
    printf("    Ritardo massimo=%.3f ms: verificato >= 200 ms e <= durata della sessione (%.3f ms).\n",
           (double)a.peak_delay_ns/1000000,(double)a.observation_ns/1000000);
    printf("    Programma associato=%s (atteso %s), EUID=%u (atteso %u).\n",
           a.peak_program,programs[0],a.peak_euid,demo_uid);

    puts("\n  5. Conservazione delle statistiche con monitor OFF");
    puts("    Salvo lo snapshot dopo il rilascio e avvio un altro worker che esegue getpid.");
    puts("    Atteso: la chiamata passa e lo snapshot rimane identico in tutti i campi.");
    struct worker *w=spawn(0,demo_uid,SYS_getpid); go(w); finish(w);
    b=snapshot();
    bool unchanged=memcmp(&a,&b,sizeof(a))==0;
    printf("    getpid completata: valore=%lld, errno=%d.\n",(long long)w->result.value,w->result.error);
    printf("    Tempo prima/dopo: %.6f / %.6f s.\n",(double)a.observation_ns/NS,(double)b.observation_ns/NS);
    printf("    Integrale prima/dopo: %.6f / %.6f thread*s.\n",
           (double)a.blocked_thread_time_ns/NS,(double)b.blocked_thread_time_ns/NS);
    printf("    Ritardo massimo prima/dopo: %.3f / %.3f ms.\n",
           (double)a.peak_delay_ns/1000000,(double)b.peak_delay_ns/1000000);
    printf("    Snapshot completo identico: %s (atteso si).\n",unchanged?"si":"no");
    check(unchanged,"snapshot modificato a monitor OFF");
    puts("\n  PASS FASE 4: integrale, media nell'intervallo, picchi, rilascio con OFF e conservazione verificati.");
    reset_case();
}
static void blocking_demo(void)
{
    puts("\n[5/6] Attesa nel limiter e attesa dei dati: due blocchi diversi");
    puts("\n  1. Preparazione: read sospesa dal limiter");
    program_change(0,true); syscall_change(2,true); syscall_change(0,true); set_max(0);
    printf("    Registro %s, read (%d) e getpid (%d); imposto MAX=0.\n",
           programs[0],SYS_read,SYS_getpid);
    puts("    Preparo due worker: uno legge un byte da una pipe vuota, l'altro esegue getpid.");
    puts("    Avvio soltanto il reader con monitor ON.");
    puts("    Atteso: read sospesa nel limiter, prima di eseguire la lettura originale.");
    struct worker *reader=spawn(0,demo_uid,SYS_read);
    struct worker *fast=spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL); go(reader); expect_blocked(reader);
    puts("    Osservato: un waiter nel limiter e nessun risultato dal reader.");

    puts("\n  2. Ammissione della read: dalla coda del limiter alla pipe");
    puts("    Imposto MAX=1: read puo' consumare il posto disponibile, ma la pipe resta vuota.");
    puts("    Atteso: waiter del limiter=0, reader ancora sospeso nella lettura della pipe.");
    puts("    Subito dopo avvio getpid: deve attendere il rinnovo del budget mentre read resta in attesa di dati.");
    puts("    Raccolgo prima queste osservazioni; le stampo al termine per non rallentare la finestra.");
    /* Nessuna stampa durante la parte sensibile al tempo: la read deve
     * consumare il budget prima dell'avvio di getpid nella stessa finestra. */
    uint64_t admitted_after=now_ns();
    set_max(1); wait_current(0);
    char path[96],state[128];
    snprintf(path,sizeof(path),"/proc/%ld/wchan",(long)reader->pid);
    uint64_t deadline=now_ns()+3*NS;
    for (;;) {
        FILE *f=fopen(path,"r"); check(f!=NULL,"apertura wchan");
        bool read_ok=fgets(state,sizeof(state),f)!=NULL; fclose(f);
        check(read_ok,"lettura wchan");
        if (strstr(state,"pipe_read")) break;
        if (now_ns()>=deadline) fail("wchan non conferma la read; controllare visibilita' simboli /proc",3);
        pause_ms(5);
    }
    check(!collect(reader,0),"read terminata senza dato");
    struct st_statistics_snapshot admitted=snapshot();
    check(admitted.current_blocked==0,"reader ancora contato nel limiter dopo l'ammissione");
    if (now_ns()-admitted_after>400000000ULL)
        fail("VM troppo lenta per osservare il consumo nella finestra corrente",3);
    go(fast); wait_current(1);
    check(!collect(fast,0),"getpid ammessa nonostante il budget consumato dalla read");
    finish(fast);
    check(!collect(reader,0),"read terminata prima di ricevere il byte");
    state[strcspn(state,"\n")]=0;
    printf("    Osservazione dopo l'ammissione: /proc/%ld/wchan = %s.\n",
           (long)reader->pid,state);
    printf("    Waiter del limiter=%u (atteso 0), nessun risultato da read.\n",admitted.current_blocked);
    puts("    PASS: read ha superato il limiter e ora attende i dati nella pipe.");

    puts("\n  3. Il budget si rinnova anche con read ancora attiva");
    puts("    Dopo l'avvio di getpid ho osservato un waiter nel limiter e nessun suo risultato.");
    printf("    Poi getpid ha restituito %lld, errno=%d, dopo %.3f ms.\n",
           (long long)fast->result.value,fast->result.error,
           (double)(fast->result.end_ns-fast->result.start_ns)/1000000);
    puts("    Read non ha ancora completato: nessun byte e' stato scritto nella pipe.");
    puts("    PASS: la lettura bloccante non impedisce il rinnovo del budget e l'avanzamento di getpid.");
    print_stats("monitor prima di OFF (read ancora in attesa di dati)");
    puts("    La modifica di MAX da 0 a 1 apre una nuova sessione statistica in questa implementazione.");
    puts("    Il tempo totale della read comprende anche attese che non appartengono a queste statistiche.");

    puts("\n  4. Monitor OFF mentre read attende ancora dati");
    puts("    Atteso: OFF riesce, ma non fa comparire dati nella pipe e quindi non completa read.");
    command(ST_IOCTL_DISABLE,NULL);
    check(!collect(reader,0),"DISABLE ha completato una read senza dati");
    struct st_statistics_snapshot frozen=snapshot();
    check(!frozen.session_active && !frozen.current_blocked,"sessione o waiter ancora attivi dopo OFF");
    printf("    Osservato: sessione attiva=%u, waiter=%u; read non ha ancora inviato un risultato.\n",
           frozen.session_active,frozen.current_blocked);
    puts("    PASS: OFF disattiva il limiter, non annulla l'attesa dei dati della syscall originale.");

    puts("\n  5. Invio del dato e confronto delle statistiche congelate");
    puts("    Scrivo il byte X nella pipe. Atteso: read restituisce 1, cioe' un byte letto.");
    check(write(reader->data,"X",1)==1,"scrittura dato"); finish(reader);
    struct st_statistics_snapshot after=snapshot();
    printf("    Osservato: read restituisce %lld, errno=%d, durata totale=%.3f ms.\n",
           (long long)reader->result.value,reader->result.error,
           (double)(reader->result.end_ns-reader->result.start_ns)/1000000);
    bool unchanged=memcmp(&frozen,&after,sizeof(after))==0;
    printf("    Tempo di osservazione prima/dopo: %.6f / %.6f s.\n",
           (double)frozen.observation_ns/NS,(double)after.observation_ns/NS);
    printf("    Integrale prima/dopo: %.6f / %.6f thread*s.\n",
           (double)frozen.blocked_thread_time_ns/NS,(double)after.blocked_thread_time_ns/NS);
    printf("    Ritardo massimo prima/dopo: %.3f / %.3f ms; waiter=%u/%u.\n",
           (double)frozen.peak_delay_ns/1000000,(double)after.peak_delay_ns/1000000,
           frozen.current_blocked,after.current_blocked);
    printf("    Snapshot completo identico: %s (atteso si).\n",unchanged?"si":"no");
    check(unchanged,"durata della read inclusa nello snapshot congelato");
    puts("    PASS: il completamento della read a monitor OFF non modifica le statistiche conservate.");
    puts("\n  PASS FASE 5: attesa dei dati distinta dal limiter, rinnovo del budget e OFF verificati.");
    reset_case();
}
/* Non aspettiamo RESULT: exit/exit_group terminano il processo senza tornare. */
static void nonreturning_case(int call_index, const char *name)
{
    program_change(0,true);
    syscall_change(call_index,true);
    set_max(0);
    struct worker *w=spawn(0,demo_uid,numbers[call_index]);
    command(ST_IOCTL_ENABLE,NULL);
    printf("    Configurazione: monitor ON, MAX=0, programma=%s, syscall=%s (%u).\n",
           programs[0],name,numbers[call_index]);
    printf("    Avvio il worker PID=%ld: vuole terminare tramite %s con stato %d.\n",
           (long)w->pid,name,DEMO_EXIT_STATUS);
    puts("    Atteso prima dell'ammissione: worker ancora vivo e sospeso nel limiter.");
    go(w);
    expect_blocked(w);
    puts("    Osservato: chiamata trattenuta nel limiter, nessun risultato dal worker.");
    puts("    Imposto MAX=1 mantenendo il monitor ON: la syscall viene ammessa.");
    printf("    Atteso dopo l'ammissione: uscita normale con stato %d, nessun ritorno dalla syscall.\n",
           DEMO_EXIT_STATUS);
    /* Monitor ancora ON: esercita il record nonreturning e il tracepoint di uscita. */
    set_max(1);
    reap_status(w,DEMO_EXIT_STATUS);
    w->done=true;
    wait_current(0);
    /* EOF dopo waitpid dimostra che il figlio non ha inviato un messaggio RESULT. */
    char unexpected;
    ssize_t bytes=read(w->output,&unexpected,1);
    printf("    Pipe dei risultati: byte letti=%zd (atteso 0, EOF).\n",bytes);
    check(bytes==0,"nonreturning ha inviato un risultato inatteso");
    struct st_statistics_snapshot s=snapshot();
    check(s.current_blocked==0,"waiter residui dopo la syscall senza ritorno");
    printf("    Waiter residui=%u (atteso 0).\n",s.current_blocked);
    printf("    PASS: %s ammessa, worker terminato con stato %d e nessun messaggio di ritorno.\n",
           name,DEMO_EXIT_STATUS);
    puts("    La conferma dei contatori interni del kernel arrivera' dopo lo scaricamento finale.");
    reset_case();
}
static void delete_module_case(bool selected, bool force)
{
    program_change(0,true);
    syscall_change(CALL_DELETE_MODULE,true);
    set_max(1);
    struct worker *w=spawn_mode(selected ? 0 : 1,0,SYS_delete_module,force);
    command(ST_IOCTL_ENABLE,NULL);
    printf("\n    Sottocaso: programma %s (%s), EUID=0.\n",
           programs[selected?0:1],selected?"registrato":"non registrato");
    printf("    Monitor ON, MAX=1, delete_module (%d) registrata; solo %s nel registro programmi.\n",
           SYS_delete_module,programs[0]);
    puts("    Nessun EUID registrato: il secondo programma e' effettivamente fuori matching.");
    if (force) {
        puts("    Richiesta: O_NONBLOCK | O_TRUNC su un nome di modulo inesistente, verificato dal worker.");
        puts("    Atteso: ritorno -1 e EPERM, perche' l'hook rifiuta O_TRUNC anche fuori matching.");
        puts("    Non viene richiesta la rimozione forzata di alcun modulo reale.");
    } else {
        puts("    Richiesta: rimozione di syscall_throttle con O_NONBLOCK, senza forzatura.");
        puts("    Atteso: ritorno -1 e EWOULDBLOCK; il riferimento temporaneo dell'hook protegge il modulo.");
    }

    /* Senza close, il riferimento .owner del device potrebbe mascherare il pin dell'hook. */
    check(close(device)==0,"chiusura device prima di delete_module");
    device=-1;
    FILE *refs=fopen("/sys/module/syscall_throttle/refcnt","r");
    if (!refs) fail("refcnt non leggibile: impossibile isolare il pin dell'hook",3);
    unsigned int count;
    int parsed=fscanf(refs,"%u",&count);
    fclose(refs);
    if (parsed!=1 || count!=0)
        fail("riferimenti esterni al modulo: autorimozione non isolabile",3);
    printf("    Prima della chiamata: device chiuso, riferimenti al modulo=%u (atteso 0).\n",count);
    puts("    Cosi' il device aperto non puo' essere la causa del rifiuto.");
    go(w);
    finish(w);
    check(access("/sys/module/syscall_throttle",F_OK)==0,
          "delete_module ha rimosso il monitor mentre era intercettata");
    device=open(ST_DEVICE_PATH,O_RDWR|O_CLOEXEC);
    check(device>=0,"riapertura device dopo delete_module");
    struct st_monitor_status state;
    command(ST_IOCTL_GET_STATUS,&state);
    check(state.enabled,"monitor non piu' attivo dopo il rifiuto di delete_module");
    wait_current(0);
    printf("    Osservato: ritorno=%lld (atteso -1), errno=%d (atteso %d: %s).\n",
           (long long)w->result.value,w->result.error,w->expected_error,
           force?"EPERM":"EWOULDBLOCK");
    printf("    Modulo ancora presente, device riaperto e GET_STATUS riuscita: monitor=%s (atteso ON).\n",
           state.enabled?"ON":"OFF");
    puts("    PASS: rifiuto previsto verificato; il monitor resta utilizzabile.");
    reset_case();
}
static void special_calls_demo(void)
{
    puts("\n[6/6] Syscall senza ritorno e protezione durante delete_module");
    puts("  Ogni prova riparte con configurazione pulita; i figli per exit/exit_group sono monothread.");
    puts("  Verifico l'uscita del processo e il cleanup dell'hook, non la terminazione di un gruppo multithread.");

    puts("\n  1. exit: dal blocco nel limiter alla terminazione del worker");
    nonreturning_case(CALL_EXIT,"exit");
    puts("\n  2. exit_group: stessa verifica su un figlio monothread");
    nonreturning_case(CALL_EXIT_GROUP,"exit_group");

    puts("\n  3. Rimozione del monitor mentre delete_module e' intercettata");
    puts("    Eseguo la prova sia con programma registrato sia con programma fuori matching.");
    /* Anche fuori matching, una delete_module intercettata deve mantenere il pin. */
    delete_module_case(true,false);
    delete_module_case(false,false);

    puts("\n  4. Rifiuto della richiesta con O_TRUNC");
    puts("    O_TRUNC potrebbe aggirare la protezione dei riferimenti: il wrapper deve rifiutarlo.");
    /* Un nome inesistente evita di forzare l'unload anche in caso di hook difettoso. */
    delete_module_case(true,true);
    delete_module_case(false,true);

    puts("\n  5. Scaricamento ordinario e diagnostica kernel (prosegue nel Makefile)");
    puts("    Le prove user-space sono completate; manca ancora la conferma finale dal kernel.");
    puts("    Dopo OFF e chiusura del device, il Makefile esegue rmmod e verifica che il modulo sia assente.");
    puts("    Atteso: questa rimozione ordinaria riesce, a differenza delle autorimozioni intercettate con ON.");
    puts("    Il modulo stampa i contatori nonreturning durante lo scaricamento:");
    puts("      avviate=2: una exit e una exit_group hanno iniziato il percorso nonreturning;");
    puts("      completate=2: il cleanup ha gestito entrambe le terminazioni;");
    puts("      ritorni_inattesi=0: nessuna delle due syscall ha restituito il controllo al wrapper.");
    puts("    Il Makefile legge il messaggio reale di questa esecuzione e confronta i tre contatori.");
    puts("    DEMO SUPERATA appare soltanto dopo scaricamento e confronto riusciti.");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    const char *uid_text=getenv("SUDO_UID");
    if (geteuid()!=0 || !uid_text || !*uid_text) {
        fprintf(stderr,"Uso: sudo ./demo/soa_demo (da utente non-root, modulo caricato e pulito)\n");
        return 2;
    }
    char *end; errno=0; unsigned long uid=strtoul(uid_text,&end,10);
    if (errno || *end || !uid || uid>UINT32_MAX) return 2;
    demo_uid=(uid_t)uid;
    struct passwd *pw=getpwuid(demo_uid);
    if (!pw) { fputs("Utente SUDO_UID non trovato.\n",stderr); return 2; }
    demo_gid=pw->pw_gid;
    ssize_t n=readlink("/proc/self/exe",directory,sizeof(directory)-1);
    if (n<0 || (size_t)n>=sizeof(directory)-1) return 2;
    directory[n]=0; char *slash=strrchr(directory,'/'); if (!slash) return 2; *slash=0;
    struct sigaction sa={0}; sa.sa_handler=stop_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); signal(SIGPIPE,SIG_IGN);
    device=open(ST_DEVICE_PATH,O_RDWR|O_CLOEXEC);
    if (device<0) { perror(ST_DEVICE_PATH); return 2; }
    struct st_monitor_status status; struct st_max_config max;
    struct st_uid_count u; struct st_program_count p; struct st_syscall_count s;
    command(ST_IOCTL_GET_STATUS,&status); command(ST_IOCTL_MAX_GET,&max);
    command(ST_IOCTL_UID_GET_COUNT,&u); command(ST_IOCTL_PROGRAM_GET_COUNT,&p);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&s);
    check(!status.enabled && !max.max_invocations && !u.count && !p.count && !s.count,
          "richiesti OFF, MAX=0 e registri vuoti; configurazione preesistente non modificata");
    owned=true;
    printf("DEMO di limitazione delle syscall: UID dei processi non-root=%u, controllo eseguito come root.\n",demo_uid);
    registry_demo(); matching_demo(); budget_demo(); statistics_demo(); blocking_demo(); special_calls_demo();
    command(ST_IOCTL_GET_STATUS,&status); command(ST_IOCTL_MAX_GET,&max);
    command(ST_IOCTL_UID_GET_COUNT,&u); command(ST_IOCTL_PROGRAM_GET_COUNT,&p);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&s);
    check(!status.enabled && !max.max_invocations && !u.count && !p.count && !s.count,
          "stato finale non pulito");
    owned=false; close(device);
    puts("\nVERIFICHE USER-SPACE COMPLETATE: 6 fasi; monitor OFF, MAX=0, registri vuoti.\nRestano scaricamento e diagnostica kernel della fase 6 (eseguiti dal Makefile).");
    return 0;
}
