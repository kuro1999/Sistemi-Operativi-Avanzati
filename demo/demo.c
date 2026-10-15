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
    printf("    Statistiche (%s): sospesi=%u, picco=%u, media=%.3f, osservazione=%.3f s.\n",
           label,s.current_blocked,s.peak_blocked,
           s.observation_ns ? (double)s.blocked_thread_time_ns/s.observation_ns : 0,
           (double)s.observation_ns/NS);
    if (!s.peak_valid)
        puts("    Ritardo massimo: non ancora disponibile.");
    else
        printf("    Ritardo massimo: %.3f ms; programma=%s, EUID=%u.\n",
               (double)s.peak_delay_ns/1000000,s.peak_program,s.peak_euid);
}
static void wait_current(unsigned int expected)
{
    uint64_t deadline = now_ns() + 5 * NS;
    while (snapshot().current_blocked != expected) {
        if (now_ns() >= deadline) {
            struct st_statistics_snapshot s = snapshot();
            fprintf(stderr, "Diagnostica: attesi=%u, correnti=%u, picco=%u, sessione=%u\n",
                    expected, s.current_blocked, s.peak_blocked, s.session_active);
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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_status)
        fprintf(stderr,"waitpid PID=%ld: stato atteso=%d, osservato=%d.\n",
                (long)w->pid,expected_status,WIFEXITED(status)?WEXITSTATUS(status):-1);
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
        return;
    }
    wait_current(1);
    check(!collect(w,150), "chiamata passata con MAX=0");
    struct st_statistics_snapshot s=snapshot();
    check(s.current_blocked == 1 && s.peak_blocked == 1 && !s.peak_valid,
          "statistiche attesa non coerenti");
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
        fprintf(stderr,"      %s: ritorno=%d, errno=%d (%s); attesi -1 e errno=%d.\n",
                label,result,observed,strerror(observed),expected);
        fail("risposta ioctl diversa da quella attesa",1);
    }
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
    printf("    ADD e LIST: UID=[%u], programmi=[%s], syscall=[%u] verificati.\n",
           ids[0],names[0].name,calls[0]);
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
    printf("    Buffer con capacita' 0: 3/3 ENOSPC; richiesti %u/%u/%u elementi.\n",
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
        printf("    Non-root EUID=%u: %zu/%zu letture consentite, %zu/%zu modifiche negate (EPERM).\n",
               (unsigned int)geteuid(),sizeof(reads)/sizeof(reads[0]),sizeof(reads)/sizeof(reads[0]),
               sizeof(writes)/sizeof(writes[0]),sizeof(writes)/sizeof(writes[0]));
        close(fd); _exit(0);
    }
    int status;
    check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "letture pubbliche o protezione EUID fallite");
}
static void registry_demo(void)
{
    puts("\n[1/3] Configurazione e selezione\n  Registri e permessi");
    /* Ogni conferma viene stampata solo dopo il controllo della ioctl. */
    uid_change(true);
    program_change(0,true);
    syscall_change(0,true);

    read_lists();

    struct st_uid_request u={.uid=demo_uid};
    struct st_program_request p={0}; strcpy(p.name,programs[0]);
    struct st_syscall_request s={.number=SYS_getpid};
    expect_error("UID",ST_IOCTL_UID_ADD,&u,EEXIST);
    expect_error("Programma",ST_IOCTL_PROGRAM_ADD,&p,EEXIST);
    expect_error("Syscall",ST_IOCTL_SYSCALL_ADD,&s,EEXIST);

    puts("    Duplicati: 3/3 ADD rifiutate con EEXIST.");
    insufficient_lists();

    permissions();

    uid_change(false);
    program_change(0,false);
    syscall_change(0,false);
    struct st_uid_count uc;
    struct st_program_count pc;
    struct st_syscall_count sc;
    command(ST_IOCTL_UID_GET_COUNT,&uc);
    command(ST_IOCTL_PROGRAM_GET_COUNT,&pc);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&sc);
    check(!uc.count && !pc.count && !sc.count,"registri non vuoti dopo le rimozioni");

    expect_error("UID",ST_IOCTL_UID_REMOVE,&u,ENOENT);
    expect_error("Programma",ST_IOCTL_PROGRAM_REMOVE,&p,ENOENT);
    expect_error("Syscall",ST_IOCTL_SYSCALL_REMOVE,&s,ENOENT);
    puts("    REMOVE: registri vuoti; 3/3 nuove rimozioni negate con ENOENT.");
    reset_case();
}
static void matching_demo(void)
{
    puts("\n  Selezione e rilascio");
    puts("    MAX=0; selezione: monitor ON + syscall registrata + (EUID O programma registrato).");
    const char *labels[]={"Solo EUID", "Solo programma", "EUID + programma",
                          "Nessuna identita'", "Syscall non registrata", "Monitor OFF"};
    const char *reasons[]={"sospesa; rimuovo EUID e riparte",
                          "sospesa; rimuovo programma e riparte",
                          "sospesa; tolgo programma e resta sospesa; tolgo EUID e riparte",
                          "bypass verificato", "getppid: bypass verificato", "bypass verificato"};
    for (int mode=0;mode<6;mode++) {
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
            }
            if (mode==2) {
                expect_blocked(w);
            }
            if (u) {
                uid_change(false);
            }
        }
        finish(w);
        if (!uid_added) wait_current(0);
        printf("    %s: %s.\n",labels[mode],reasons[mode]);
        reset_case();
    }
    puts("  PASS FASE 1");
}
static void budget_demo(void)
{
    puts("\n[2/3] Limitazione e statistiche\n  Budget globale condiviso");
    program_change(0,true); program_change(1,true);
    syscall_change(0,true); syscall_change(1,true); set_max(2);
    printf("    MAX=2; 8 worker, 2 programmi, EUID 0/%u, getpid/getppid; selezione per programma.\n",demo_uid);
    for (int i=0;i<8;i++) spawn((i/4)%2,(i/2)%2?demo_uid:0,i%2?SYS_getppid:SYS_getpid);
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

    printf("    Prima finestra (+%.1f ms): %u completati, %u sospesi.\n",(double)before_time/1000000,before_done,before_blocked);

    printf("    Ripeto MAX=2 (+%.1f ms): nessun nuovo budget; %u campioni su %.1f ms, completati=%u, sospesi=%u.\n",
           (double)repeat_end/1000000,after_samples,(double)(last_after-first_after)/1000000,
           after_done,after_blocked);

    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    struct st_statistics_snapshot s=snapshot();
    check(!s.current_blocked && s.peak_blocked==6 && s.peak_valid,
          "statistiche budget globale inattese");
    check(s.peak_euid==0 || s.peak_euid==demo_uid,"EUID peak inatteso");
    check(!strcmp(s.peak_program,programs[0])||!strcmp(s.peak_program,programs[1]),"programma peak inatteso");
    check(s.peak_delay_ns>=2*NS && s.peak_delay_ns<=s.observation_ns,"peak fuori sessione");
    printf("    Finestre successive: completati %zu/8, risultati verificati (ordine FIFO non garantito).\n",worker_count);

    print_stats("budget globale");
    reset_case();
}
static void statistics_demo(void)
{
    puts("\n  Media temporale e picchi");
    program_change(0,true); syscall_change(0,true); set_max(0);
    printf("    MAX=0; 4 worker %s, EUID=%u, syscall=getpid (%d).\n",programs[0],demo_uid,SYS_getpid);
    for (int i=0;i<4;i++) spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    wait_current(4);

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
    printf("    Quattro sospesi: delta integrale=%.6f thread*s = 4 x %.6f s; verifica esatta in ns.\n",
           (double)weighted/NS,(double)elapsed/NS);
    printf("    Media nell'intervallo: %.6f (attesa 4).\n",(double)weighted/elapsed);

    for (size_t i=0;i<worker_count;i++) check(!collect(&workers[i],0),"worker passato con MAX=0");
    printf("    Attese ancora aperte: picco=%u; ritardo massimo non ancora disponibile.\n",b.peak_blocked);

    command(ST_IOCTL_DISABLE,NULL);
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    /* Aspettiamo i risultati prima di leggere il picco delle attese concluse. */
    a=snapshot();
    check(!a.session_active && !a.current_blocked && a.peak_blocked==4 && a.peak_valid &&
          a.peak_delay_ns>=200000000ULL && a.peak_delay_ns<=a.observation_ns &&
          a.peak_euid==demo_uid && !strcmp(a.peak_program,programs[0]),"peak o chiusura sessione errati");
    puts("    OFF: 4/4 worker completati; sessione chiusa, nessun sospeso.");
    printf("    Ritardo massimo=%.3f ms; programma=%s, EUID=%u (valori e limiti verificati).\n",
           (double)a.peak_delay_ns/1000000,a.peak_program,a.peak_euid);

    struct worker *w=spawn(0,demo_uid,SYS_getpid);
    go(w);
    finish(w);
    b=snapshot();
    bool unchanged=memcmp(&a,&b,sizeof(a))==0;
    check(unchanged,"snapshot modificato a monitor OFF");
    puts("    Nuova getpid con OFF: completata; snapshot invariato.\n  PASS FASE 2");
    reset_case();
}
static void blocking_demo(void)
{
    puts("\n[3/3] Syscall particolari e scaricamento\n  Attesa nel limiter e attesa dei dati");
    program_change(0,true); syscall_change(2,true); syscall_change(0,true); set_max(0);
    puts("    Configurazione: read su pipe vuota e getpid; MAX iniziale=0.");
    struct worker *reader=spawn(0,demo_uid,SYS_read);
    struct worker *fast=spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL);
    go(reader);
    expect_blocked(reader);
    puts("    MAX=0: read sospesa nel limiter.");

    /* Nessuna stampa durante la parte sensibile al tempo: la read deve
     * consumare il budget prima dell'avvio di getpid nella stessa finestra. */
    uint64_t admitted_after=now_ns();
    set_max(1); wait_current(0);
    char path[96],state[128];
    snprintf(path,sizeof(path),"/proc/%ld/wchan",(long)reader->pid);
    uint64_t deadline=now_ns()+3*NS;
    for (;;) {
        FILE *f=fopen(path,"r"); check(f!=NULL,"apertura wchan");
        bool read_ok=fgets(state,sizeof(state),f)!=NULL;
        fclose(f);
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
    printf("    MAX=1: read ammessa, attende dati nella pipe; sospesi nel limiter=%u.\n",admitted.current_blocked);

    printf("    getpid: sospesa, poi completata al rinnovo (%.3f ms); read ancora in attesa.\n",
           (double)(fast->result.end_ns-fast->result.start_ns)/1000000);
    (void)snapshot(); /* Mantiene la validazione dello snapshot. */

    command(ST_IOCTL_DISABLE,NULL);
    check(!collect(reader,0),"DISABLE ha completato una read senza dati");
    struct st_statistics_snapshot frozen=snapshot();
    check(!frozen.session_active && !frozen.current_blocked,"sessione o waiter ancora attivi dopo OFF");
    printf("    OFF: sessione attiva=%u, sospesi=%u; read attende ancora dati.\n",frozen.session_active,frozen.current_blocked);

    check(write(reader->data,"X",1)==1,"scrittura dato");
    finish(reader);
    struct st_statistics_snapshot after=snapshot();
    bool unchanged=memcmp(&frozen,&after,sizeof(after))==0;
    check(unchanged,"durata della read inclusa nello snapshot congelato");
    puts("    Invio un byte: read restituisce 1; snapshot congelato invariato.");
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
    go(w);
    expect_blocked(w);
    /* Monitor ancora ON: esercita il record nonreturning e il tracepoint di uscita. */
    set_max(1);
    reap_status(w,DEMO_EXIT_STATUS);
    w->done=true;
    wait_current(0);
    /* EOF dopo waitpid dimostra che il figlio non ha inviato un messaggio RESULT. */
    char unexpected;
    ssize_t bytes=read(w->output,&unexpected,1);
    check(bytes==0,"nonreturning ha inviato un risultato inatteso");
    struct st_statistics_snapshot s=snapshot();
    check(s.current_blocked==0,"waiter residui dopo la syscall senza ritorno");
    printf("    %s: sospesa con MAX=0, ammessa con MAX=1; figlio terminato con stato %d, EOF e zero sospesi verificati.\n", name, DEMO_EXIT_STATUS);
    reset_case();
}
static void delete_module_case(bool selected, bool force)
{
    program_change(0,true);
    syscall_change(CALL_DELETE_MODULE,true);
    set_max(1);
    struct worker *w=spawn_mode(selected ? 0 : 1,0,SYS_delete_module,force);
    command(ST_IOCTL_ENABLE,NULL);

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
    go(w);
    finish(w);
    check(access("/sys/module/syscall_throttle",F_OK)==0, "delete_module ha rimosso il monitor mentre era intercettata");
    device=open(ST_DEVICE_PATH,O_RDWR|O_CLOEXEC);
    check(device>=0,"riapertura device dopo delete_module");
    struct st_monitor_status state;
    command(ST_IOCTL_GET_STATUS,&state);
    check(state.enabled,"monitor non piu' attivo dopo il rifiuto di delete_module");
    wait_current(0);
    printf("    %s, programma %s: %s; refcnt iniziale=0, monitor ancora ON.\n",
           force?"Rimozione forzata del modulo reale":"Rimozione non bloccante di syscall_throttle",
           selected?"registrato":"non registrato",force?"EPERM (hook: rimozione forzata vietata)":
                 "EWOULDBLOCK (kernel: modulo in uso per il riferimento dell'hook)");
    reset_case();
}
static void special_calls_demo(void)
{
    puts("\n  Syscall senza ritorno");
    printf("    Due figli monothread: ciascuno passa %d a exit/exit_group come stato di terminazione.\n",
           DEMO_EXIT_STATUS);
    puts("    Il padre verifica questo stato con waitpid: non e' un valore restituito dalla syscall.");

    nonreturning_case(CALL_EXIT,"exit");
    nonreturning_case(CALL_EXIT_GROUP,"exit_group");

    puts("\n  Protezione del modulo");
    puts("    Ogni prova: root, monitor ON, MAX=1 e delete_module registrata.");
    puts("    Device chiuso prima della chiamata: refcnt=0, ma monitor ancora attivo.");
    puts("    Programma registrato: applica il limiter; non registrato: lo bypassa.");
    puts("    In entrambi i casi l'hook mantiene un riferimento temporaneo al modulo.");
    puts("    O_NONBLOCK: se il modulo e' in uso, fallisce subito senza forzare la rimozione.");

    /* Anche fuori matching, una delete_module intercettata deve mantenere il pin. */
    delete_module_case(true,false);
    delete_module_case(false,false);

    puts("    O_NONBLOCK | O_TRUNC: richiede la rimozione anche con riferimenti attivi; l'hook la vieta.");
    delete_module_case(true,true);
    delete_module_case(false,true);
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
    printf("DEMO syscall throttling: controllo root, EUID non-root=%u.\n",demo_uid);
    registry_demo(); matching_demo(); budget_demo(); statistics_demo(); blocking_demo(); special_calls_demo();
    command(ST_IOCTL_GET_STATUS,&status); command(ST_IOCTL_MAX_GET,&max);
    command(ST_IOCTL_UID_GET_COUNT,&u); command(ST_IOCTL_PROGRAM_GET_COUNT,&p);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&s);
    check(!status.enabled && !max.max_invocations && !u.count && !p.count && !s.count,
          "stato finale non pulito");
    owned=false; close(device);
    puts("  Stato finale user-space: monitor OFF, MAX=0, registri vuoti.");
    return 0;
}
