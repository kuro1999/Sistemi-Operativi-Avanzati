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
static bool owned, uid_added, program_added[2], syscall_added[3];
static const char *programs[] = {"demo_worker", "demo_alt"};
static const unsigned int numbers[] = {SYS_getpid, SYS_getppid, SYS_read};
static char directory[PATH_MAX];
static volatile sig_atomic_t interrupted;
struct worker {
    pid_t pid;
    int gate, output, data;
    bool done;
    unsigned int number;
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
    printf("  %s: correnti=%u, picco=%u, media=%.6f, osservazione=%.3f s\n",
           label, s.current_blocked, s.peak_blocked,
           s.observation_ns ? (double)s.blocked_thread_time_ns / s.observation_ns : 0,
           (double)s.observation_ns / NS);
    if (s.peak_valid)
        printf("  Peak: %.3f ms, programma=%s, EUID=%u\n",
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
static void reap(struct worker *w)
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
    w->pid = 0;
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "worker fallito");
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
static struct worker *spawn(int program, uid_t uid, unsigned int number)
{
    check(worker_count < MAX_WORKERS, "troppi worker");
    int gate[2], output[2], data[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, gate) == 0, "socketpair");
    check(pipe2(output, O_CLOEXEC) == 0, "pipe risultato");
    check(pipe2(data, O_CLOEXEC) == 0, "pipe dati");
    struct worker *w = &workers[worker_count++];
    *w = (struct worker){.gate=gate[0], .output=output[0], .data=data[1], .number=number};
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
        execl(path, path, a, b, c, d, (char *)NULL);
        _exit(122);
    }
    w->pid=pid;
    close(gate[1]); close(output[1]); close(data[0]);
    struct demo_result ready;
    check(receive(w->output,&ready,5000) && ready.kind == DEMO_READY && ready.euid == uid,
          "worker non pronto: verificare permessi dei percorsi e credenziali");
    return w;
}
static void go(struct worker *w)
{
    check(send(w->gate,"G",1,MSG_NOSIGNAL) == 1, "rilascio gate");
}
static bool collect(struct worker *w, int timeout_ms)
{
    if (w->done) return true;
    if (!receive(w->output,&w->result,timeout_ms)) return false;
    check(w->result.kind == DEMO_RESULT && !w->result.error &&
          w->result.end_ns >= w->result.start_ns, "syscall worker fallita");
    check(w->number == SYS_read ? w->result.value == 1 :
          (w->number == SYS_getpid ? w->result.value == w->pid : w->result.value == getpid()),
          "valore restituito dalla syscall inatteso");
    w->done=true;
    return true;
}
static void finish(struct worker *w)
{
    check(collect(w,12000), "timeout completamento syscall");
    reap(w);
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
        if (readable && strstr(state, "st_rate_limiter_wait_for_change")) return;
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
        printf("  Worker PID=%ld nel limiter; correnti globali=%u, picco globale=%u.\n",
               (long)w->pid, s.current_blocked, s.peak_blocked);
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
    for (int i=0;i<3;i++) if (syscall_added[i]) {
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
static void expect_error(unsigned long op, void *arg, int expected)
{
    errno=0;
    check(ioctl(device,op,arg)==-1 && errno==expected,"errno ioctl inatteso");
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
    expect_error(ST_IOCTL_UID_LIST,&u,ENOSPC);
    expect_error(ST_IOCTL_PROGRAM_LIST,&p,ENOSPC);
    expect_error(ST_IOCTL_SYSCALL_LIST,&s,ENOSPC);
    check(u.count==1 && p.count==1 && s.count==1,"dimensione richiesta errata");
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
        close(fd); _exit(0);
    }
    int status;
    check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "letture pubbliche o protezione EUID fallite");
}
static void registry_demo(void)
{
    puts("\n[1/5] Registri, consultazione e privilegi");
    uid_change(true); program_change(0,true); syscall_change(0,true);
    read_lists(); permissions();
    struct st_uid_request u={.uid=demo_uid};
    struct st_program_request p={0}; strcpy(p.name,programs[0]);
    struct st_syscall_request s={.number=SYS_getpid};
    expect_error(ST_IOCTL_UID_ADD,&u,EEXIST); expect_error(ST_IOCTL_PROGRAM_ADD,&p,EEXIST);
    expect_error(ST_IOCTL_SYSCALL_ADD,&s,EEXIST);
    uid_change(false); program_change(0,false); syscall_change(0,false);
    expect_error(ST_IOCTL_UID_REMOVE,&u,ENOENT); expect_error(ST_IOCTL_PROGRAM_REMOVE,&p,ENOENT);
    expect_error(ST_IOCTL_SYSCALL_REMOVE,&s,ENOENT);
    puts("  PASS: liste, ENOSPC, duplicati, rimozioni e tutte le ioctl non-root.");
    reset_case();
}
static void matching_demo(void)
{
    puts("\n[2/5] Selezione syscall AND (programma OR EUID), deregistrazione e OFF");
    const char *labels[]={"solo EUID","solo programma","entrambi: OR",
                          "nessuna identita'","syscall non registrata","monitor OFF"};
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
            if (p) program_change(0,false);
            if (mode==2) expect_blocked(w);
            if (u) uid_change(false);
        }
        finish(w);
        if (!uid_added) wait_current(0);
        printf("  PASS: %s\n",labels[mode]);
        reset_case();
    }
}
static void budget_demo(void)
{
    puts("\n[3/5] Budget globale: 8 worker, 2 programmi, 2 EUID, 2 syscall; MAX=2");
    program_change(0,true); program_change(1,true);
    syscall_change(0,true); syscall_change(1,true); set_max(2);
    for (int i=0;i<8;i++) spawn((i/4)%2,(i/2)%2?demo_uid:0,i%2?SYS_getppid:SYS_getpid);
    uint64_t start=now_ns(); command(ST_IOCTL_ENABLE,NULL);
    for (size_t i=0;i<worker_count;i++) go(&workers[i]);
    unsigned int samples=0;
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
            samples++;
        } else if (observed) fail("budget o waiter cambiati prima del rinnovo",1);
        pause_ms(10);
    }
    if (!observed || samples<3 || last_sample<500000000ULL ||
        last_sample-first_sample<200000000ULL)
        fail("VM troppo lenta: campioni insufficienti nella prima finestra",3);
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    struct st_statistics_snapshot s=snapshot();
    check(!s.current_blocked && s.peak_blocked==6 && s.peak_valid,
          "statistiche budget globale inattese");
    check(s.peak_euid==0 || s.peak_euid==demo_uid,"EUID peak inatteso");
    check(!strcmp(s.peak_program,programs[0])||!strcmp(s.peak_program,programs[1]),"programma peak inatteso");
    check(s.peak_delay_ns>=2*NS && s.peak_delay_ns<=s.observation_ns,"peak fuori sessione");
    printf("  PASS: 2 ammissioni globali, 6 waiter, %u campioni; tutti i worker completati.\n",samples);
    print_stats("budget globale"); reset_case();
}
static void statistics_demo(void)
{
    puts("\n[4/5] Media temporale, picco, identita' del peak e congelamento OFF");
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
    for (size_t i=0;i<worker_count;i++) check(!collect(&workers[i],0),"worker passato con MAX=0");
    print_stats("prima di DISABLE"); command(ST_IOCTL_DISABLE,NULL);
    for (size_t i=0;i<worker_count;i++) finish(&workers[i]);
    a=snapshot();
    check(!a.session_active && !a.current_blocked && a.peak_blocked==4 && a.peak_valid &&
          a.peak_delay_ns>=200000000ULL && a.peak_delay_ns<=a.observation_ns &&
          a.peak_euid==demo_uid && !strcmp(a.peak_program,programs[0]),"peak o chiusura sessione errati");
    struct worker *w=spawn(0,demo_uid,SYS_getpid); go(w); finish(w);
    b=snapshot();
    check(!memcmp(&a,&b,sizeof(a)),"snapshot modificato a monitor OFF");
    print_stats("dopo DISABLE");
    puts("  PASS: integrale esatto nell'intervallo stabile, picco=4, identita' e snapshot congelato.");
    reset_case();
}
static void blocking_demo(void)
{
    puts("\n[5/5] Syscall bloccante: read su pipe e rinnovo del budget");
    program_change(0,true); syscall_change(2,true); syscall_change(0,true); set_max(0);
    struct worker *reader=spawn(0,demo_uid,SYS_read);
    struct worker *fast=spawn(0,demo_uid,SYS_getpid);
    command(ST_IOCTL_ENABLE,NULL); go(reader); expect_blocked(reader);
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
    puts("  Read originale confermata in attesa sulla pipe.");
    if (now_ns()-admitted_after>400000000ULL)
        fail("VM troppo lenta per osservare il consumo nella finestra corrente",3);
    go(fast); wait_current(1);
    check(!collect(fast,0),"getpid ammessa nonostante il budget consumato dalla read");
    finish(fast);
    check(!collect(reader,0),"read terminata prima di ricevere il byte");
    puts("  getpid completata mentre read e' ancora attiva: budget rinnovato.");
    print_stats("prima del dato");
    command(ST_IOCTL_DISABLE,NULL);
    check(!collect(reader,0),"DISABLE ha completato una read senza dati");
    struct st_statistics_snapshot frozen=snapshot();
    check(write(reader->data,"X",1)==1,"scrittura dato"); finish(reader);
    struct st_statistics_snapshot after=snapshot();
    check(!memcmp(&frozen,&after,sizeof(after)),"durata della read inclusa nello snapshot congelato");
    puts("  PASS: DISABLE non attende la syscall originale; read completata dopo il byte.");
    reset_case();
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
    printf("DEMO syscall throttling: EUID worker=%u, controller root.\n",demo_uid);
    registry_demo(); matching_demo(); budget_demo(); statistics_demo(); blocking_demo();
    command(ST_IOCTL_GET_STATUS,&status); command(ST_IOCTL_MAX_GET,&max);
    command(ST_IOCTL_UID_GET_COUNT,&u); command(ST_IOCTL_PROGRAM_GET_COUNT,&p);
    command(ST_IOCTL_SYSCALL_GET_COUNT,&s);
    check(!status.enabled && !max.max_invocations && !u.count && !p.count && !s.count,
          "stato finale non pulito");
    owned=false; close(device);
    puts("\nDEMO SUPERATA: 5 fasi; monitor OFF, MAX=0, registri vuoti, modulo ancora caricato.");
    return 0;
}
