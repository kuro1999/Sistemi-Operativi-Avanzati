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
static void expect_error(const char *label, unsigned long op, void *arg, int expected)
{
    errno=0;
    int result=ioctl(device,op,arg);
    int observed=errno;
    printf("    %s: ritorno=%d (atteso -1), errno=%d (atteso %d, %s).\n",
           label,result,observed,expected,strerror(expected));
    check(result==-1 && observed==expected,"errno ioctl inatteso");
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
    printf("  Liste: UID=%u, programma=%s, syscall=%u\n",ids[0],names[0].name,calls[0]);
    u.capacity=p.capacity=s.capacity=0;
    expect_error("UID_LIST",ST_IOCTL_UID_LIST,&u,ENOSPC);
    expect_error("PROGRAM_LIST",ST_IOCTL_PROGRAM_LIST,&p,ENOSPC);
    expect_error("SYSCALL_LIST",ST_IOCTL_SYSCALL_LIST,&s,ENOSPC);
    check(u.count==1 && p.count==1 && s.count==1,"dimensione richiesta errata");
    printf("    LIST con capacita' 0: dimensioni richieste UID=%u, programmi=%u, syscall=%u (atteso 1 ciascuna).\n",
           u.count,p.count,s.count);
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
    puts("\n[1/6] Registrazione degli elementi e controllo dei permessi");
    puts("  Registro un UID, un programma e una syscall, poi verifico gli elenchi.");
    uid_change(true); program_change(0,true); syscall_change(0,true);
    read_lists();
    puts("  Controllo che un utente normale possa consultare i dati ma non modificarli.");
    permissions();
    puts("  Verifico registrazioni duplicate e rimozioni.");
    struct st_uid_request u={.uid=demo_uid};
    struct st_program_request p={0}; strcpy(p.name,programs[0]);
    struct st_syscall_request s={.number=SYS_getpid};
    expect_error("UID_ADD",ST_IOCTL_UID_ADD,&u,EEXIST); expect_error("PROGRAM_ADD",ST_IOCTL_PROGRAM_ADD,&p,EEXIST);
    expect_error("SYSCALL_ADD",ST_IOCTL_SYSCALL_ADD,&s,EEXIST);
    uid_change(false); program_change(0,false); syscall_change(0,false);
    expect_error("UID_REMOVE",ST_IOCTL_UID_REMOVE,&u,ENOENT); expect_error("PROGRAM_REMOVE",ST_IOCTL_PROGRAM_REMOVE,&p,ENOENT);
    expect_error("SYSCALL_REMOVE",ST_IOCTL_SYSCALL_REMOVE,&s,ENOENT);
    struct st_uid_count uc;
    struct st_program_count pc;
    struct st_syscall_count sc;
    command(ST_IOCTL_UID_GET_COUNT,&uc);
    command(ST_IOCTL_PROGRAM_GET_COUNT,&pc);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&sc);
    printf("    Dopo le rimozioni: UID=%u, programmi=%u, syscall=%u (attesi tutti 0).\n",
           uc.count,pc.count,sc.count);
    check(!uc.count && !pc.count && !sc.count,"registri non vuoti dopo le rimozioni");
    puts("  PASS: registri e permessi funzionano correttamente.");
    reset_case();
}
static void matching_demo(void)
{
    puts("\n[2/6] Quali chiamate vengono limitate?");
    puts("  In questa implementazione MAX=0 sospende tutte le chiamate selezionate.");
    puts("  Lo uso per osservare matching e rilascio senza dipendere dal tick del timer.");
    const char *labels[]={"selezione tramite UID dell'utente",
                          "selezione tramite nome del programma",
                          "entrambi registrati: rimuoverne uno deve mantenere il blocco",
                          "nessuna identita' registrata: la chiamata deve passare",
                          "syscall non registrata: la chiamata deve passare",
                          "monitor disattivato: la chiamata deve passare"};
    for (int mode=0;mode<6;mode++) {
        printf("  Caso: %s\n", labels[mode]);
        bool u=mode==0||mode==2||mode==4||mode==5;
        bool p=mode==1||mode==2||mode==4||mode==5;
        if (u) uid_change(true);
        if (p) program_change(0,true);
        syscall_change(0,true); set_max(0);
        struct worker *w=spawn(0,demo_uid,mode==4?SYS_getppid:SYS_getpid);
        if (mode!=5) command(ST_IOCTL_ENABLE,NULL);
        go(w);
        if (mode<3) {
            expect_blocked(w);
            if (p) {
                program_change(0,false);
                puts("    Rimosso il programma dal registro.");
            }
            if (mode==2) expect_blocked(w);
            if (u) {
                uid_change(false);
                puts("    Rimosso l'UID dal registro: nessun criterio residuo per il worker.");
            }
        }
        finish(w);
        printf("    Risultato syscall=%u: valore=%lld, errno=%d, durata=%.3f ms.\n",
               w->number,(long long)w->result.value,w->result.error,
               (double)(w->result.end_ns-w->result.start_ns)/1000000);
        if (!uid_added) wait_current(0);
        printf("  PASS: %s\n",labels[mode]);
        reset_case();
    }
    puts("  PASS: selezione, rimozione dei criteri e disattivazione corrette.");
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
    puts("\n[3/6] Un unico limite condiviso tra tutti i processi");
    puts("  Avvio 8 processi: due programmi, due utenti e due syscall.");
    puts("  Imposto MAX=2: nella prima finestra devono passare 2 chiamate, mentre le altre 6 attendono.");
    puts("  Verifico anche che impostare nuovamente MAX=2 non rinnovi il budget.");
    program_change(0,true); program_change(1,true);
    syscall_change(0,true); syscall_change(1,true); set_max(2);
    for (int i=0;i<8;i++) spawn((i/4)%2,(i/2)%2?demo_uid:0,i%2?SYS_getppid:SYS_getpid);
    uint64_t start=now_ns(); command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    unsigned int samples=0, sampled_done=0, sampled_blocked=0;
    uint64_t first_sample=0, last_sample=0;
    bool observed=false;
    while (now_ns()-start < 750000000ULL) {
        unsigned int done=0;
        for (size_t i=0;i<worker_count;i++) done+=collect(&workers[i],0);
        struct st_statistics_snapshot s=snapshot();
        uint64_t elapsed=now_ns()-start;
        if (elapsed>=750000000ULL) break;
        check(done<=2,"budget globale superato prima del primo rinnovo");
        if (done==2 && s.current_blocked==6) {
            if (!observed) { set_max(2); observed=true; first_sample=elapsed; }
            last_sample=elapsed;
            sampled_done=done;
            sampled_blocked=s.current_blocked;
            samples++;
        } else if (observed) fail("budget o waiter cambiati prima del rinnovo",1);
        pause_ms(10);
    }
    if (!observed || samples<3 || last_sample<500000000ULL ||
        last_sample-first_sample<200000000ULL)
        fail("VM troppo lenta: campioni insufficienti nella prima finestra",3);
    printf("    Prima finestra: completate=%u (atteso 2), waiter=%u (atteso 6).\n"
           "    %u campioni coerenti da +%.3f a +%.3f ms; dopo MAX=2 ripetuto il budget non riparte.\n",
           sampled_done,sampled_blocked,samples,
           (double)first_sample/1000000,(double)last_sample/1000000);
    puts("  Attendo che le chiamate rimanenti passino nelle finestre successive.");
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    struct st_statistics_snapshot s=snapshot();
    check(!s.current_blocked && s.peak_blocked==6 && s.peak_valid,
          "statistiche budget globale inattese");
    check(s.peak_euid==0 || s.peak_euid==demo_uid,"EUID peak inatteso");
    check(!strcmp(s.peak_program,programs[0])||!strcmp(s.peak_program,programs[1]),"programma peak inatteso");
    check(s.peak_delay_ns>=2*NS && s.peak_delay_ns<=s.observation_ns,"peak fuori sessione");
    printf("  PASS: limite globale rispettato; 2 chiamate ammesse e 6 sospese nella prima finestra (%u campioni).\n  Tutti i processi hanno completato la chiamata.\n",samples);
    print_completions(start);
    print_stats("budget globale"); reset_case();
}
static void statistics_demo(void)
{
    puts("\n[4/6] Misurazione delle attese e delle statistiche");
    puts("  Con MAX=0 sospendo 4 processi e verifico il calcolo della media temporale.");
    program_change(0,true); syscall_change(0,true); set_max(0);
    for (int i=0;i<4;i++) spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    wait_current(4);
    struct st_statistics_snapshot a=snapshot();
    pause_ms(200);
    struct st_statistics_snapshot b=snapshot();
    check(!a.peak_valid && !b.peak_valid && b.peak_blocked==4,"peak di attesa ancora pendente");
    uint64_t elapsed=b.observation_ns-a.observation_ns;
    check(elapsed>0 && b.blocked_thread_time_ns-a.blocked_thread_time_ns==4*elapsed,
          "integrale dei quattro waiter errato");
    printf("    Campioni A/B: waiter=%u/%u (attesi 4/4), picco=%u, peak_valid=%u/%u (attesi 0/0).\n",
           a.current_blocked,b.current_blocked,b.peak_blocked,a.peak_valid,b.peak_valid);
    printf("    Delta tempo=%.6f s; delta integrale=%.6f thread*s; atteso 4 x delta tempo=%.6f thread*s.\n",
           (double)elapsed/NS,(double)(b.blocked_thread_time_ns-a.blocked_thread_time_ns)/NS,
           (double)(4*elapsed)/NS);
    puts("    Uguaglianza verificata sui valori interi in nanosecondi, prima dell'arrotondamento.");
    for (size_t i=0;i<worker_count;i++) check(!collect(&workers[i],0),"worker passato con MAX=0");
    print_stats("prima della disattivazione");
    puts("  Disattivo il monitor: i processi devono ripartire.");
    command(ST_IOCTL_DISABLE,NULL);
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    puts("  Controllo il ritardo massimo e il programma e utente associati.");
    a=snapshot();
    check(!a.session_active && !a.current_blocked && a.peak_blocked==4 && a.peak_valid &&
          a.peak_delay_ns>=200000000ULL && a.peak_delay_ns<=a.observation_ns &&
          a.peak_euid==demo_uid && !strcmp(a.peak_program,programs[0]),"peak o chiusura sessione errati");
    puts("  Verifico che le statistiche restino invariate a monitor disattivato.");
    struct worker *w=spawn(0,demo_uid,SYS_getpid); go(w); finish(w);
    b=snapshot();
    bool unchanged=memcmp(&a,&b,sizeof(a))==0;
    printf("    Dopo un'altra getpid con OFF: snapshot intero identico=%s; "
           "osservazione prima/dopo=%.6f/%.6f s.\n",
           unchanged ? "si" : "no",(double)a.observation_ns/NS,(double)b.observation_ns/NS);
    check(unchanged,"snapshot modificato a monitor OFF");
    print_stats("dopo la disattivazione");
    puts("  PASS: statistiche coerenti e conservate dopo la disattivazione.");
    reset_case();
}
static void blocking_demo(void)
{
    puts("\n[5/6] Una syscall in attesa di dati non deve fermare il sistema di limitazione");
    puts("  Sospendo una read nel limiter con MAX=0.");
    program_change(0,true); syscall_change(2,true); syscall_change(0,true); set_max(0);
    struct worker *reader=spawn(0,demo_uid,SYS_read);
    struct worker *fast=spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL); go(reader); expect_blocked(reader);
    puts("  Imposto MAX=1: la read viene ammessa, ma deve attendere dati sulla pipe vuota.");
    puts("  Verifico il consumo del posto disponibile e il rinnovo del budget per getpid.");
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
    puts("  Confermato: read ha superato il limiter ed e' in attesa di dati sulla pipe.");
    if (now_ns()-admitted_after>400000000ULL)
        fail("VM troppo lenta per osservare il consumo nella finestra corrente",3);
    go(fast); wait_current(1);
    check(!collect(fast,0),"getpid ammessa nonostante il budget consumato dalla read");
    finish(fast);
    check(!collect(reader,0),"read terminata prima di ricevere il byte");
    state[strcspn(state,"\n")]=0;
    printf("    Attesa del reader osservata dopo l'ammissione: /proc/%ld/wchan = %s.\n",
           (long)reader->pid,state);
    printf("    getpid restituisce %lld dopo %.3f ms; read non ha ancora inviato un risultato.\n",
           (long long)fast->result.value,(double)(fast->result.end_ns-fast->result.start_ns)/1000000);
    puts("  Confermato: getpid completa al rinnovo del budget, mentre read attende ancora dati.");
    print_stats("prima dell'invio del dato");
    puts("  Disattivo il monitor mentre read e' ancora in attesa di dati.");
    command(ST_IOCTL_DISABLE,NULL);
    check(!collect(reader,0),"DISABLE ha completato una read senza dati");
    struct st_statistics_snapshot frozen=snapshot();
    puts("  Monitor disattivato: invio un byte nella pipe per completare read.");
    check(write(reader->data,"X",1)==1,"scrittura dato"); finish(reader);
    struct st_statistics_snapshot after=snapshot();
    printf("    Dopo il byte X: read restituisce %lld, errno=%d, durata totale=%.3f ms.\n",
           (long long)reader->result.value,reader->result.error,
           (double)(reader->result.end_ns-reader->result.start_ns)/1000000);
    bool unchanged=memcmp(&frozen,&after,sizeof(after))==0;
    printf("    Snapshot OFF prima/dopo completamento read: identico=%s; "
           "picco ritardo=%.3f/%.3f ms, waiter=%u/%u.\n",
           unchanged ? "si" : "no",(double)frozen.peak_delay_ns/1000000,
           (double)after.peak_delay_ns/1000000,frozen.current_blocked,after.current_blocked);
    check(unchanged,"durata della read inclusa nello snapshot congelato");
    puts("  PASS: rinnovo del budget e disattivazione funzionano anche con read attiva.");
    reset_case();
}
/* Non aspettiamo RESULT: exit/exit_group terminano il processo senza tornare. */
static void nonreturning_case(int call_index, const char *name)
{
    printf("  %s: verifico prima il blocco con MAX=0, poi l'uscita con stato %d.\n",
           name, DEMO_EXIT_STATUS);
    program_change(0,true);
    syscall_change(call_index,true);
    set_max(0);
    struct worker *w=spawn(0,demo_uid,numbers[call_index]);
    command(ST_IOCTL_ENABLE,NULL);
    go(w);
    expect_blocked(w);
    /* Ammissione con monitor ancora ON: esercita il record nonreturning e il tracepoint. */
    set_max(1);
    reap_status(w,DEMO_EXIT_STATUS);
    w->done=true;
    wait_current(0);
    /* Nessun messaggio dopo la syscall: una nonreturning non misura un ritorno. */
    char unexpected;
    ssize_t bytes=read(w->output,&unexpected,1);
    printf("    Pipe dei risultati dopo l'uscita: byte letti=%zd (atteso 0, EOF).\n",bytes);
    check(bytes==0,"nonreturning ha inviato un risultato inatteso");
    printf("  PASS: %s ha terminato il worker, senza ritorno e senza waiter residui.\n",name);
    reset_case();
}
static void delete_module_case(bool selected, bool force)
{
    program_change(0,true);
    syscall_change(CALL_DELETE_MODULE,true);
    set_max(1);
    struct worker *w=spawn_mode(selected ? 0 : 1,0,SYS_delete_module,force);
    command(ST_IOCTL_ENABLE,NULL);

    /* Senza close, .owner del device basterebbe da solo a far rifiutare rmmod. */
    check(close(device)==0,"chiusura device prima di delete_module");
    device=-1;
    FILE *refs=fopen("/sys/module/syscall_throttle/refcnt","r");
    if (!refs) fail("refcnt non leggibile: impossibile isolare il pin dell'hook",3);
    unsigned int count;
    int parsed=fscanf(refs,"%u",&count);
    fclose(refs);
    if (parsed!=1 || count!=0)
        fail("riferimenti esterni al modulo: autorimozione non isolabile",3);

    printf("  delete_module: %s, %s; device chiuso e riferimenti iniziali=%u (atteso 0).\n",
           selected ? "programma selezionato" : "programma fuori policy",
           force ? "O_TRUNC su nome inesistente" : "autorimozione O_NONBLOCK",count);
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
    printf("    delete_module: ritorno=%lld (atteso -1), errno=%d (atteso %d, %s), stato monitor=%u (atteso 1).\n",
           (long long)w->result.value,w->result.error,w->expected_error,
           strerror(w->result.error),state.enabled);
    puts("  PASS: rifiuto atteso, monitor ancora ON e device utilizzabile.");
    reset_case();
}
static void special_calls_demo(void)
{
    puts("\n[6/6] Syscall senza ritorno e protezione di delete_module");
    puts("  exit ed exit_group sono provate in processi figli monothread.");
    nonreturning_case(CALL_EXIT,"exit");
    nonreturning_case(CALL_EXIT_GROUP,"exit_group");
    /* Anche fuori matching, una delete_module intercettata deve mantenere il pin. */
    delete_module_case(true,false);
    delete_module_case(false,false);
    /* Non usiamo mai O_TRUNC sul modulo reale: un hook difettoso non deve forzare l'unload. */
    delete_module_case(true,true);
    delete_module_case(false,true);
    puts("  PASS: casi speciali completati; resta la verifica dello scaricamento finale.");
    puts("  Il Makefile esegue rmmod dopo OFF e chiusura del device.");
    puts("  Dopo rmmod il Makefile legge la diagnostica kernel di questa esecuzione:");
    puts("  attesi avviate=2, completate=2, ritorni_inattesi=0; il PASS finale dipende dal confronto.");
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
