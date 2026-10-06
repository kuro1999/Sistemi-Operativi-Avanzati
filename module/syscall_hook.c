#include <asm/ptrace.h>
#include <asm/compat.h>
#include <asm/unistd.h>

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/ftrace.h>
#include <linux/fcntl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/kprobes.h>
#include <linux/linkage.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "device.h"
#include "monitor_state.h"
#include "program_identity.h"
#include "program_registry.h"
#include "rate_limiter.h"
#include "statistics.h"
#include "syscall_hook.h"
#include "syscall_registry.h"
#include "uid_registry.h"

/*
 * Tutti i wrapper __x64_sys_* ricevono un puntatore ai registri
 * salvati lungo il percorso di ingresso della system call.
 */
typedef asmlinkage long (*st_x64_syscall_t)(
    const struct pt_regs *regs);

/*
 * Descrizione statica delle system call native x86-64.
 *
 * Il contenuto deriva dalla syscall table generata dal kernel
 * contro cui il modulo viene compilato.
 */
struct st_syscall_definition {
    unsigned int syscall_nr;
    const char *symbol_name;
    bool nonreturning;
};

#ifdef __SYSCALL
#undef __SYSCALL
#endif

#ifdef __SYSCALL_NORETURN
#undef __SYSCALL_NORETURN
#endif

/*
 * "__x64_" e la stringa prodotta da #symbol sono string literal
 * adiacenti e vengono concatenate dal compilatore.
 */
#define __SYSCALL(number, symbol)                     \
    {                                                 \
        .syscall_nr = (unsigned int)(number),         \
        .symbol_name = "__x64_" #symbol,              \
        .nonreturning = false,                        \
    },

#define __SYSCALL_NORETURN(number, symbol)            \
    {                                                 \
        .syscall_nr = (unsigned int)(number),         \
        .symbol_name = "__x64_" #symbol,              \
        .nonreturning = true,                         \
    },

static const struct st_syscall_definition
st_syscall_definitions[] = {
#include <asm/syscalls_64.h>
};

#undef __SYSCALL_NORETURN
#undef __SYSCALL

/*
 * Mapping runtime completo, indicizzato direttamente tramite il
 * numero della system call.
 *
 * Ogni slot rappresenta una voce della tabella x86-64 generata
 * dal kernel e conserva il simbolo, l'indirizzo risolto e
 * l'eventuale proprietà nonreturning.
 */
struct st_hook_target {
    const char *symbol_name;
    unsigned long original_ip;
    bool nonreturning;
    bool explicit_entry;
};

/*
 * Più numeri di system call possono condividere lo stesso
 * indirizzo Ftrace. I filtri verranno quindi conservati
 * separatamente e deduplicati per original_ip.
 */
struct st_hook_filter {
    unsigned long original_ip;
    bool installed;
};

static struct st_hook_target
    st_hook_targets[NR_syscalls];

static struct st_hook_filter
    st_hook_filters[NR_syscalls];

static size_t
    st_hook_filter_count;

/*
 * Prepara tutti gli slot della tabella.
 *
 * Inizialmente ogni numero viene associato alla fallback
 * sys_ni_syscall. Le definizioni esplicite generate dal kernel
 * sovrascrivono poi i rispettivi slot.
 */
static int st_hook_targets_prepare(void)
{
    const struct st_syscall_definition *definition;
    struct st_hook_target *target;
    size_t index;
    unsigned int syscall_nr;

    for (index = 0;
         index < ARRAY_SIZE(st_hook_targets);
         index++) {
        target = &st_hook_targets[index];

        target->symbol_name =
            "__x64_sys_ni_syscall";
        target->original_ip = 0U;
        target->nonreturning = false;
        target->explicit_entry = false;
    }

    for (index = 0;
         index < ARRAY_SIZE(st_syscall_definitions);
         index++) {
        definition =
            &st_syscall_definitions[index];

        syscall_nr = definition->syscall_nr;

        if (syscall_nr >=
            ARRAY_SIZE(st_hook_targets)) {
            pr_err("syscall_throttle: definizione syscall "
                   "fuori range: nr=%u, limite=%zu\n",
                   syscall_nr,
                   ARRAY_SIZE(st_hook_targets));

            return -EINVAL;
        }

        target =
            &st_hook_targets[syscall_nr];

        if (target->explicit_entry) {
            pr_err("syscall_throttle: definizione syscall "
                   "duplicata: nr=%u\n",
                   syscall_nr);

            return -EINVAL;
        }

        target->symbol_name =
            definition->symbol_name;
        target->nonreturning =
            definition->nonreturning;
        target->explicit_entry = true;
    }

    return 0;
}


/*
 * Risolve un singolo simbolo kernel tramite una Kprobe
 * temporanea.
 *
 * La Kprobe non installa handler e viene rimossa subito dopo
 * avere acquisito l'indirizzo.
 */
static int st_resolve_symbol(
    const char *symbol_name,
    unsigned long *original_ip)
{
    struct kprobe probe = {
        .symbol_name = symbol_name,
    };
    int ret;

    *original_ip = 0U;

    ret = register_kprobe(&probe);
    if (ret != 0) {
        pr_err("syscall_throttle: impossibile risolvere %s "
               "tramite Kprobe: errore=%d\n",
               symbol_name,
               ret);
        return ret;
    }

    *original_ip = (unsigned long)probe.addr;

    unregister_kprobe(&probe);

    if (*original_ip == 0U) {
        pr_err("syscall_throttle: indirizzo nullo per %s\n",
               symbol_name);
        return -ENOENT;
    }

    return 0;
}

/*
 * Risolve tutti i target completi.
 *
 * Se due numeri di syscall usano lo stesso nome di simbolo,
 * l'indirizzo viene risolto una sola volta e copiato nelle entry
 * successive. Questo evita soprattutto centinaia di Kprobe
 * ripetute su __x64_sys_ni_syscall.
 */
static int st_resolve_all_targets(void)
{
    struct st_hook_target *target;
    const struct st_hook_target *previous;
    size_t syscall_nr;
    size_t previous_nr;
    bool reused;
    int ret;

    for (syscall_nr = 0;
         syscall_nr < ARRAY_SIZE(st_hook_targets);
         syscall_nr++) {
        target = &st_hook_targets[syscall_nr];

        if (target->symbol_name == NULL) {
            pr_err("syscall_throttle: simbolo assente "
                   "per syscall=%zu\n",
                   syscall_nr);
            return -EINVAL;
        }

        target->original_ip = 0U;
        reused = false;

        for (previous_nr = 0;
             previous_nr < syscall_nr;
             previous_nr++) {
            previous =
                &st_hook_targets[previous_nr];

            if (strcmp(
                    target->symbol_name,
                    previous->symbol_name) != 0)
                continue;

            if (previous->original_ip == 0U) {
                pr_err("syscall_throttle: indirizzo precedente "
                       "nullo per syscall=%zu, simbolo=%s\n",
                       previous_nr,
                       previous->symbol_name);
                return -EINVAL;
            }

            target->original_ip =
                previous->original_ip;

            reused = true;
            break;
        }

        if (reused)
            continue;

        ret = st_resolve_symbol(
            target->symbol_name,
            &target->original_ip);
        if (ret != 0) {
            pr_err("syscall_throttle: risoluzione fallita "
                   "per syscall=%zu, simbolo=%s\n",
                   syscall_nr,
                   target->symbol_name);
            return ret;
        }
    }

    return 0;
}

/*
 * Costruisce l'insieme degli indirizzi Ftrace distinti.
 *
 * Due simboli diversi possono condividere lo stesso indirizzo,
 * come avviene con alcuni alias interni del kernel. La
 * deduplicazione deve quindi essere effettuata su original_ip e
 * non soltanto sul nome del simbolo.
 */
static int st_hook_filters_build(void)
{
    const struct st_hook_target *target;
    struct st_hook_filter *filter;
    size_t syscall_nr;
    size_t filter_index;
    bool already_present;

    memset(
        st_hook_filters,
        0,
        sizeof(st_hook_filters));

    st_hook_filter_count = 0U;

    for (syscall_nr = 0;
         syscall_nr < ARRAY_SIZE(st_hook_targets);
         syscall_nr++) {
        target = &st_hook_targets[syscall_nr];

        if (target->original_ip == 0U) {
            pr_err("syscall_throttle: target non risolto "
                   "per syscall=%zu, simbolo=%s\n",
                   syscall_nr,
                   target->symbol_name);
            return -EINVAL;
        }

        already_present = false;

        for (filter_index = 0;
             filter_index < st_hook_filter_count;
             filter_index++) {
            filter =
                &st_hook_filters[filter_index];

            if (filter->original_ip !=
                target->original_ip)
                continue;

            already_present = true;
            break;
        }

        if (already_present)
            continue;

        if (st_hook_filter_count >=
            ARRAY_SIZE(st_hook_filters)) {
            pr_err("syscall_throttle: capacità filtri "
                   "Ftrace esaurita\n");
            return -ENOSPC;
        }

        filter =
            &st_hook_filters[
                st_hook_filter_count];

        filter->original_ip =
            target->original_ip;
        filter->installed = false;

        st_hook_filter_count++;
    }

    if (st_hook_filter_count == 0U) {
        pr_err("syscall_throttle: nessun filtro "
               "Ftrace costruito\n");
        return -EINVAL;
    }

    return 0;
}

/*
 * Azzera esclusivamente lo stato runtime. La preparazione
 * successiva ricostruirà nomi e proprietà dalla syscall table
 * generata.
 */
static void st_hook_runtime_clear(void)
{
    memset(
        st_hook_targets,
        0,
        sizeof(st_hook_targets));

    memset(
        st_hook_filters,
        0,
        sizeof(st_hook_filters));

    st_hook_filter_count = 0U;
}

static atomic_t st_hook_active_calls = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(st_hook_active_wait_queue);


/*
 * Per exit ed exit_group la funzione originale non restituisce
 * il controllo al wrapper.
 *
 * Il record vive sul kernel stack del wrapper. Il tracepoint
 * sched_process_exit viene eseguito dallo stesso task prima che
 * tale stack venga distrutto.
 */
struct st_nonreturning_call {
    struct list_head link;
    struct task_struct *task;
};

static LIST_HEAD(st_nonreturning_calls);
static DEFINE_SPINLOCK(st_nonreturning_calls_lock);

static struct tracepoint *
    st_sched_process_exit_tracepoint;

static bool st_sched_process_exit_registered;

static atomic64_t st_nonreturning_started =
    ATOMIC64_INIT(0);

static atomic64_t st_nonreturning_completed =
    ATOMIC64_INIT(0);

static atomic64_t st_nonreturning_unexpected_returns =
    ATOMIC64_INIT(0);

/*
 * Forward declaration: st_call_original_syscall() è definita
 * prima degli helper che gestiscono il record nonreturning.
 */
static void notrace st_nonreturning_call_track(
    struct st_nonreturning_call *call);

static bool notrace st_nonreturning_call_cancel(
    struct st_nonreturning_call *call);

/*
 * Contatori diagnostici della prima integrazione con i registry.
 *
 * Non vengono esposti nello UAPI e non fanno parte delle
 * statistiche finali richieste dal progetto. Servono a verificare
 * che il wrapper classifichi correttamente le chiamate.
 */
static atomic64_t st_hook_total_calls = ATOMIC64_INIT(0);
static atomic64_t st_hook_monitor_disabled_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_unregistered_syscall_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_unmatched_identity_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_relevant_calls = ATOMIC64_INIT(0);

static atomic64_t st_hook_rate_bypass_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_rate_allow_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_rate_throttle_calls =
    ATOMIC64_INIT(0);
static atomic64_t st_hook_rate_shutdown_calls =
    ATOMIC64_INIT(0);

/*
 * Chiamate terminate prima della system call originale perché
 * l'attesa è stata interrotta da un segnale.
 */
static atomic64_t st_hook_wait_interrupted_calls =
    ATOMIC64_INIT(0);

static bool st_hook_accepting_calls;
static bool st_hook_installed;

static asmlinkage long notrace
st_generic_syscall_wrapper(
    const struct pt_regs *regs,
    unsigned long original_ip);

/*
 * Il callback Ftrace viene eseguito in un contesto nel quale non
 * è consentito dormire.
 *
 * Esegue quindi soltanto controlli lockless e non bloccanti:
 *
 * - evita la ricorsione quando il wrapper richiama l'originale;
 * - esce immediatamente quando il monitor è disattivato;
 * - valida il percorso syscall nativo x86-64;
 * - scarta le chiamate non registrate;
 * - trasporta l'indirizzo originale nel secondo argomento;
 * - devia l'instruction pointer verso il wrapper generico.
 */
static void notrace st_ftrace_callback(
    unsigned long ip,
    unsigned long parent_ip,
    struct ftrace_ops *ops,
    struct ftrace_regs *fregs)
{
    struct pt_regs *kernel_regs;
    struct pt_regs *syscall_regs;
    unsigned long first_argument;
    unsigned long original_ip;
    unsigned long raw_syscall_nr;
    unsigned int syscall_nr;

    (void)ops;

    /*
     * Quando il wrapper richiama la funzione originale,
     * parent_ip appartiene al modulo. In quel caso Ftrace
     * deve lasciare proseguire la vera funzione __x64_sys_*.
     */
    if (within_module(parent_ip, THIS_MODULE))
        return;

    /*
     * Il monitor disattivato è il caso più economico:
     * la static key permette di uscire prima di recuperare
     * e validare il frame pt_regs della system call.
     */
    if (!st_monitor_fast_path_enabled())
        return;

    /*
     * Durante il teardown il callback può essere ancora
     * osservato da una CPU già entrata nel percorso Ftrace.
     */
    if (!READ_ONCE(st_hook_accepting_calls))
        return;

    /*
     * FTRACE_OPS_FL_SAVE_REGS garantisce un frame pt_regs
     * completo per questa configurazione x86-64.
     */
    kernel_regs = ftrace_get_regs(fregs);
    if (unlikely(kernel_regs == NULL))
        return;

    /*
     * Per un vero wrapper __x64_sys_* il primo argomento C
     * deve essere esattamente il frame pt_regs della syscall
     * corrente.
     *
     * Questo controllo è particolarmente importante quando il
     * punto Ftrace è pubblicato con un alias __do_sys_*: la
     * medesima funzione potrebbe essere raggiunta anche da un
     * chiamante interno al kernel con una firma differente.
     */
    first_argument =
        ftrace_regs_get_argument(fregs, 0);

    syscall_regs =
        (struct pt_regs *)first_argument;

    if (unlikely(syscall_regs != current_pt_regs()))
        return;

    /*
     * Accettiamo soltanto il percorso syscall nativo x86-64.
     *
     * user_64bit_mode() esclude IA32, mentre
     * in_32bit_syscall() esclude sia IA32 sia x32.
     */
    if (unlikely(
            !user_mode(syscall_regs) ||
            !user_64bit_mode(syscall_regs) ||
            in_32bit_syscall()))
        return;

    /*
     * Il callback deve evitare di creare wrapper inutili.
     *
     * Il registro delle syscall espone una lettura lockless e
     * non bloccante, quindi può essere consultato prima del
     * redirect Ftrace.
     */
    raw_syscall_nr =
        READ_ONCE(syscall_regs->orig_ax);

    syscall_nr =
        (unsigned int)raw_syscall_nr;

    /*
     * Escludiamo numeri negativi, valori non rappresentabili
     * esattamente e numeri esterni alla tabella x86-64.
     */
    if (unlikely(
            raw_syscall_nr !=
                (unsigned long)syscall_nr ||
            syscall_nr >=
                ARRAY_SIZE(st_hook_targets)))
        return;

    /*
     * Con una syscall non registrata non serve eseguire
     * classificazione, identity matching o rate limiting.
     */
    if (!st_syscall_registry_contains(syscall_nr))
        return;


    /*
     * Ricaviamo l'inizio canonico della funzione intercettata.
     * Il valore ip ricevuto dal callback può rappresentare il
     * punto di instrumentazione Ftrace.
     */
    original_ip = ftrace_get_symaddr(ip);

    /*
     * La funzione __x64_sys_* originale riceve il proprio primo
     * argomento in %rdi. Quel valore contiene il puntatore ai
     * pt_regs della system call e non viene modificato.
     *
     * Inseriamo invece original_ip in %rsi, che diventerà il
     * secondo argomento del wrapper generico dopo il redirect.
     *
     * Questa operazione è specifica per l'ABI kernel x86-64.
     */
    kernel_regs->si = original_ip;

    /*
     * Il conteggio viene incrementato prima della deviazione.
     * Il wrapper lo decrementerà su ogni percorso di uscita.
     */
    atomic_inc(&st_hook_active_calls);

    ftrace_regs_set_instruction_pointer(
        fregs,
        (unsigned long)st_generic_syscall_wrapper);
}

static struct ftrace_ops st_syscall_ftrace_ops = {
    .func = st_ftrace_callback,
    .flags = FTRACE_OPS_FL_SAVE_REGS |
             FTRACE_OPS_FL_RECURSION |
             FTRACE_OPS_FL_IPMODIFY,
};



/*
 * Verifica la corrispondenza completa:
 *
 * numero x86-64 -> simbolo intercettato.
 *
 * Durante il normale funzionamento la tabella è immutabile.
 * Nel teardown gli indirizzi vengono azzerati soltanto dopo
 * che active_calls è tornato a zero.
 */

static const struct st_hook_target *
st_hook_target_find(
    unsigned int syscall_nr,
    unsigned long original_ip)
{
    const struct st_hook_target *target;

    if (syscall_nr >=
        ARRAY_SIZE(st_hook_targets))
        return NULL;

    target = &st_hook_targets[syscall_nr];

    if (READ_ONCE(target->original_ip) == 0U)
        return NULL;

    if (READ_ONCE(target->original_ip) !=
        original_ip)
        return NULL;

    return target;
}



/*
 * Invoca il vero wrapper x86-64.
 *
 * Per exit ed exit_group viene prima pubblicato un record locale
 * nella lista osservata da sched_process_exit.
 */
static asmlinkage long notrace st_call_original_syscall(
    const struct st_hook_target *target,
    st_x64_syscall_t original_syscall,
    const struct pt_regs *regs,
    bool *release_active_call)
{
    struct st_nonreturning_call call;
    long result;

    /*
     * O_TRUNC richiede lo scaricamento forzato e potrebbe ignorare
     * il riferimento a THIS_MODULE. Il controllo e' comune al
     * percorso rilevante e a quello che non richiede throttling.
     * Per le chiamate rilevanti avviene dopo l'ammissione.
     */
    if ((unsigned long)regs->orig_ax ==
            (unsigned long)__NR_delete_module &&
        ((unsigned int)regs->si & O_TRUNC))
        return -EPERM;

    if (!target->nonreturning)
        return original_syscall(regs);

    st_nonreturning_call_track(&call);

    result = original_syscall(regs);

    /*
     * Il kernel ha marcato queste entry tramite
     * __SYSCALL_NORETURN. Questo ramo non dovrebbe quindi essere
     * mai raggiunto.
     */
    atomic64_inc(
        &st_nonreturning_unexpected_returns);

    /*
     * Normalmente il record è ancora nella lista e il normale
     * percorso out del wrapper rilascerà active_calls.
     *
     * Se il tracepoint lo avesse già rimosso, active_calls sarebbe
     * già stato rilasciato dalla callback.
     */
    if (!st_nonreturning_call_cancel(&call))
        *release_active_call = false;

    pr_err_once(
        "syscall_throttle: syscall marcata nonreturning "
        "ha restituito il controllo\n");

    return result;
}

/*
 * Classifica una system call secondo la politica del monitor:
 *
 * monitor attivo
 * AND numero di syscall registrato
 * AND (effective UID registrato OR programma registrato).
 *
 * La funzione viene eseguita nel wrapper, quindi in process
 * context e non nel callback Ftrace.
 */
static bool st_syscall_is_relevant(
    unsigned int syscall_nr,
    char *program_name,
    size_t program_name_capacity,
    bool *program_name_valid)
{
    int identity_ret;

    if (program_name_valid != NULL)
        *program_name_valid = false;

    if (program_name != NULL &&
        program_name_capacity != 0U) {
        program_name[0] = '\0';
    }

    if (!st_monitor_fast_path_enabled()) {
        atomic64_inc(&st_hook_monitor_disabled_calls);
        return false;
    }

    if (!st_syscall_registry_contains(syscall_nr)) {
        atomic64_inc(
            &st_hook_unregistered_syscall_calls);
        return false;
    }

    /*
     * Quando l'UID è registrato non serve identificare subito
     * l'eseguibile. Il basename sarà acquisito soltanto se la
     * chiamata subirà effettivamente un THROTTLE.
     */
    if (st_uid_registry_contains(current_euid())) {
        atomic64_inc(&st_hook_relevant_calls);
        return true;
    }

    if (program_name == NULL ||
        program_name_capacity == 0U) {
        atomic64_inc(&st_hook_unmatched_identity_calls);
        return false;
    }

    /*
     * Nel percorso basato sul programma conserviamo il basename
     * già usato per il matching, evitando di ricavarlo nuovamente
     * al primo THROTTLE.
     */
    identity_ret =
        st_program_get_current_name(
            program_name,
            program_name_capacity);

    if (identity_ret == 0 &&
        st_program_registry_contains(program_name)) {
        if (program_name_valid != NULL)
            *program_name_valid = true;

        atomic64_inc(&st_hook_relevant_calls);
        return true;
    }

    atomic64_inc(&st_hook_unmatched_identity_calls);
    return false;
}


/* Rivalutazione senza modificare i contatori diagnostici. */
static bool st_syscall_policy_still_matches(unsigned int syscall_nr)
{
    char name[ST_PROGRAM_NAME_CAPACITY];

    if (!st_monitor_fast_path_enabled() ||
        !st_syscall_registry_contains(syscall_nr))
        return false;
    if (st_uid_registry_contains(current_euid()))
        return true;
    return st_program_get_current_name(name, sizeof(name)) == 0 &&
           st_program_registry_contains(name);
}

/*
 * Registra ogni decisione presa dal rate limiter.
 *
 * THROTTLE conta gli ingressi nell'attesa, non gli esiti finali
 * delle chiamate. Uno stesso thread può quindi incrementarlo più
 * volte prima di ottenere ALLOW.
 */
static void st_record_rate_limiter_decision(
    enum st_rate_limiter_decision decision)
{
    switch (decision) {
    case ST_RATE_LIMITER_RETRY:
        break;
    case ST_RATE_LIMITER_BYPASS:
        atomic64_inc(&st_hook_rate_bypass_calls);
        break;

    case ST_RATE_LIMITER_ALLOW:
        atomic64_inc(&st_hook_rate_allow_calls);
        break;

    case ST_RATE_LIMITER_THROTTLE:
        atomic64_inc(&st_hook_rate_throttle_calls);
        break;

    case ST_RATE_LIMITER_SHUTDOWN:
        atomic64_inc(&st_hook_rate_shutdown_calls);
        break;
    }
}

/*
 * Rilascia una reference acquisita dal callback Ftrace.
 *
 * Deve essere chiamata su ogni percorso di uscita dal wrapper,
 * compreso quello interrotto da un segnale.
 */
static void notrace st_hook_active_call_put(void)
{
    if (atomic_dec_and_test(&st_hook_active_calls))
        wake_up_all(&st_hook_active_wait_queue);
}


/*
 * Inserisce un record sullo stack del wrapper nella lista dei
 * task che stanno per eseguire una syscall non ritornante.
 */
static void notrace st_nonreturning_call_track(
    struct st_nonreturning_call *call)
{
    unsigned long flags;

    INIT_LIST_HEAD(&call->link);
    call->task = current;

    spin_lock_irqsave(
        &st_nonreturning_calls_lock,
        flags);

    list_add_tail(
        &call->link,
        &st_nonreturning_calls);

    spin_unlock_irqrestore(
        &st_nonreturning_calls_lock,
        flags);

    atomic64_inc(&st_nonreturning_started);
}

/*
 * Percorso difensivo utilizzato soltanto se una funzione marcata
 * nonreturning restituisce inaspettatamente il controllo.
 *
 * Restituisce true se il record era ancora posseduto dal wrapper.
 */
static bool notrace st_nonreturning_call_cancel(
    struct st_nonreturning_call *call)
{
    unsigned long flags;
    bool removed;

    removed = false;

    spin_lock_irqsave(
        &st_nonreturning_calls_lock,
        flags);

    if (!list_empty(&call->link)) {
        list_del_init(&call->link);
        removed = true;
    }

    spin_unlock_irqrestore(
        &st_nonreturning_calls_lock,
        flags);

    return removed;
}

/*
 * Callback del tracepoint:
 *
 * sched_process_exit(task, group_dead)
 *
 * La callback cerca un record appartenente al task in uscita.
 * Quando lo trova, il wrapper non potrà più riprendere
 * l'esecuzione: active_calls può quindi essere rilasciato.
 */
static void notrace st_sched_process_exit_callback(
    void *ignore,
    struct task_struct *task,
    bool group_dead)
{
    struct st_nonreturning_call *call;
    struct st_nonreturning_call *next;
    unsigned long flags;
    bool completed;

    (void)ignore;
    (void)group_dead;

    completed = false;

    spin_lock_irqsave(
        &st_nonreturning_calls_lock,
        flags);

    list_for_each_entry_safe(
        call,
        next,
        &st_nonreturning_calls,
        link) {
        if (call->task != task)
            continue;

        list_del_init(&call->link);
        completed = true;
        break;
    }

    spin_unlock_irqrestore(
        &st_nonreturning_calls_lock,
        flags);

    if (!completed)
        return;

    atomic64_inc(&st_nonreturning_completed);

    st_hook_active_call_put();
}

/*
 * Individua sched_process_exit senza richiedere un riferimento
 * diretto al simbolo __tracepoint_sched_process_exit.
 */
static void st_find_sched_process_exit_tracepoint(
    struct tracepoint *tracepoint,
    void *private_data)
{
    struct tracepoint **result;

    result = private_data;

    if (*result != NULL)
        return;

    if (strcmp(
            tracepoint->name,
            "sched_process_exit") == 0)
        *result = tracepoint;
}

static int st_nonreturning_tracepoint_init(void)
{
    int ret;

    if (st_sched_process_exit_registered)
        return 0;

    st_sched_process_exit_tracepoint = NULL;

    for_each_kernel_tracepoint(
        st_find_sched_process_exit_tracepoint,
        &st_sched_process_exit_tracepoint);

    if (st_sched_process_exit_tracepoint == NULL) {
        pr_err("syscall_throttle: tracepoint "
               "sched_process_exit non trovato\n");
        return -ENOENT;
    }

    ret = tracepoint_probe_register(
        st_sched_process_exit_tracepoint,
        (void *)st_sched_process_exit_callback,
        NULL);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione tracepoint "
               "sched_process_exit fallita: errore=%d\n",
               ret);

        st_sched_process_exit_tracepoint = NULL;
        return ret;
    }

    st_sched_process_exit_registered = true;

    pr_info("syscall_throttle: tracepoint "
            "sched_process_exit registrato\n");

    return 0;
}

static void st_nonreturning_tracepoint_exit(void)
{
    int ret;

    if (!st_sched_process_exit_registered)
        return;

    ret = tracepoint_probe_unregister(
        st_sched_process_exit_tracepoint,
        (void *)st_sched_process_exit_callback,
        NULL);
    if (ret != 0) {
        pr_err("syscall_throttle: rimozione tracepoint "
               "sched_process_exit fallita: errore=%d\n",
               ret);
    }

    /*
     * Garantisce che nessuna callback del modulo sia ancora in
     * esecuzione prima del completamento di module_exit.
     */
    tracepoint_synchronize_unregister();

    st_sched_process_exit_registered = false;
    st_sched_process_exit_tracepoint = NULL;

    pr_info("syscall_throttle: tracepoint "
            "sched_process_exit rimosso\n");
}

static asmlinkage long notrace
st_generic_syscall_wrapper(
    const struct pt_regs *regs,
    unsigned long original_ip)
{
    struct st_statistics_block_context
        statistics_context;

    enum st_rate_limiter_decision decision;
    const struct st_hook_target *target;
    st_x64_syscall_t original_syscall;

    char statistics_program_name[
        ST_PROGRAM_NAME_CAPACITY];

    u64 observed_generation;
    u64 throttle_start_ns = 0U;
    u64 statistics_generation = 0U;
    unsigned long raw_syscall_nr;
    unsigned int syscall_nr;

    bool statistics_program_name_valid;
    bool first_throttle_seen;
    bool release_active_call;
    bool module_pinned = false;

    long result;
    int identity_ret;
    int wait_ret;

    /*
     * Il contesto statistico viene inizializzato in modo lazy.
     *
     * Finché la syscall non riceve il primo THROTTLE serve
     * soltanto sapere che nessun blocco è stato contabilizzato.
     * st_statistics_block_begin() inizializzerà completamente
     * la struttura quando il contesto diventerà necessario.
     */
    statistics_context.counted = false;

    statistics_program_name[0] = '\0';
    statistics_program_name_valid = false;
    first_throttle_seen = false;

    release_active_call = true;
    target = NULL;

    raw_syscall_nr = READ_ONCE(regs->orig_ax);
    syscall_nr = (unsigned int)raw_syscall_nr;

    if (raw_syscall_nr == (unsigned long)syscall_nr) {
        target = st_hook_target_find(
            syscall_nr,
            original_ip);
    }

    if (unlikely(target == NULL)) {
        pr_err_once(
            "syscall_throttle: target syscall non coerente: "
            "nr=%lu, ip=%px\n",
            raw_syscall_nr,
            (void *)original_ip);

        result = -ENOSYS;
        goto out;
    }

    /*
     * Protegge ogni delete_module entrata nel wrapper, anche quando
     * l'identita' non corrisponde alla policy. active_calls protegge
     * gia' questo ingresso se un teardown concorrente e' iniziato.
     */
    if (syscall_nr == (unsigned int)__NR_delete_module) {
        if (!try_module_get(THIS_MODULE)) {
            result = -EBUSY;
            goto out;
        }
        module_pinned = true;
    }

    original_syscall =
        (st_x64_syscall_t)original_ip;

    atomic64_inc(&st_hook_total_calls);

    /*
     * Process context: prima di policy, budget e statistiche.
     * L'helper esegue la richiesta sul file verificato, senza
     * bypassare LSM o i controlli dei privilegi nel driver.
     */
    if (syscall_nr == (unsigned int)__NR_ioctl &&
        st_device_try_control_ioctl(
            (unsigned int)regs->di,
            (unsigned int)regs->si,
            (unsigned long)regs->dx,
            &result)) {
        goto out;
    }

    /*
     * La classificazione può restituire anche il basename già
     * usato per il matching sul registro dei programmi.
     */
    if (!st_syscall_is_relevant(
            syscall_nr,
            statistics_program_name,
            sizeof(statistics_program_name),
            &statistics_program_name_valid)) {
        result = st_call_original_syscall(
            target,
            original_syscall,
            regs,
            &release_active_call);

        goto out;
    }

    /*
     * La chiamata rilevante viene contata una sola volta,
     * indipendentemente dai retry del rate limiter.
     */
    statistics_generation =
        st_statistics_record_relevant_invocation();

    for (;;) {
        /* Il token precede i registry; nessun lock resta acquisito. */
        observed_generation = st_rate_limiter_get_generation();
        if (!st_syscall_policy_still_matches(syscall_nr)) {
            decision = ST_RATE_LIMITER_BYPASS;
        } else {
            decision = st_rate_limiter_try_acquire(
                observed_generation,
                &observed_generation,
                first_throttle_seen ? NULL : &throttle_start_ns);
        }

        st_record_rate_limiter_decision(decision);

        switch (decision) {
        case ST_RATE_LIMITER_RETRY:
            continue;

        case ST_RATE_LIMITER_ALLOW:
        case ST_RATE_LIMITER_BYPASS:
        case ST_RATE_LIMITER_SHUTDOWN:
            /*
             * Se esiste un contesto bloccato, la misura termina
             * immediatamente prima della syscall originale.
             *
             * Una chiamata mai entrata in THROTTLE non possiede
             * invece alcun contesto statistico da completare.
             */
            if (statistics_context.counted) {
                st_statistics_block_complete(
                    &statistics_context);
            }

            st_statistics_relevant_release(&statistics_generation);
            result = st_call_original_syscall(
                target,
                original_syscall,
                regs,
                &release_active_call);

            goto out;

        case ST_RATE_LIMITER_THROTTLE:
            /*
             * Una stessa invocazione può osservare più decisioni
             * THROTTLE, ma deve aprire un solo contesto statistico.
             */
            if (!first_throttle_seen) {
                kuid_t blocked_euid;

                first_throttle_seen = true;
                blocked_euid = current_euid();

                /*
                 * Nel percorso di rilevanza basato sull'UID il
                 * basename non è stato ancora acquisito.
                 */
                if (!statistics_program_name_valid) {
                    identity_ret =
                        st_program_get_current_name(
                            statistics_program_name,
                            sizeof(
                                statistics_program_name));

                    if (identity_ret == 0) {
                        statistics_program_name_valid =
                            true;
                    } else {
                        /*
                         * Un fallimento nella sola raccolta
                         * statistica non deve alterare throttling
                         * o risultato della syscall.
                         */
                        strscpy(
                            statistics_program_name,
                            "<unavailable>",
                            sizeof(
                                statistics_program_name));
                    }
                }

                st_statistics_block_begin(
                    &statistics_context,
                    statistics_generation,
                    throttle_start_ns,
                    syscall_nr,
                    blocked_euid,
                    statistics_program_name);
                st_statistics_relevant_release(&statistics_generation);
            }

            wait_ret =
                st_rate_limiter_wait_for_change(
                    observed_generation);

            if (wait_ret != 0) {
                atomic64_inc(
                    &st_hook_wait_interrupted_calls);

                /*
                 * La syscall originale non verrà eseguita.
                 * L'attesa viene registrata come interrotta e
                 * non contribuisce al peak delay.
                 */
                if (statistics_context.counted) {
                    st_statistics_block_interrupted(
                        &statistics_context);
                }

                result = wait_ret;
                goto out;
            }

            /*
             * Il wake-up non assegna automaticamente il budget:
             * la richiesta compete nuovamente tramite acquire.
             */
            break;
        }
    }

out:
    st_statistics_relevant_release(&statistics_generation);
    WARN_ON_ONCE(statistics_context.counted);

    /* active_calls resta acquisito durante il rilascio del pin. */
    if (module_pinned)
        module_put(THIS_MODULE);

    if (release_active_call)
        st_hook_active_call_put();

    return result;
}


int st_syscall_hook_init(void)
{
    struct st_hook_filter *filter;
    size_t filter_index;
    size_t syscall_nr;
    size_t explicit_count;
    size_t nonreturning_count;
    int cleanup_ret;
    int ret;

    if (READ_ONCE(st_hook_installed))
        return 0;

    if (WARN_ON(!list_empty(
            &st_nonreturning_calls)))
        return -EBUSY;

    atomic_set(&st_hook_active_calls, 0);

    atomic64_set(&st_hook_total_calls, 0);
    atomic64_set(&st_hook_monitor_disabled_calls, 0);
    atomic64_set(&st_hook_unregistered_syscall_calls, 0);
    atomic64_set(&st_hook_unmatched_identity_calls, 0);
    atomic64_set(&st_hook_relevant_calls, 0);

    atomic64_set(&st_hook_rate_bypass_calls, 0);
    atomic64_set(&st_hook_rate_allow_calls, 0);
    atomic64_set(&st_hook_rate_throttle_calls, 0);
    atomic64_set(&st_hook_rate_shutdown_calls, 0);
    atomic64_set(&st_hook_wait_interrupted_calls, 0);

    atomic64_set(&st_nonreturning_started, 0);
    atomic64_set(&st_nonreturning_completed, 0);
    atomic64_set(
        &st_nonreturning_unexpected_returns,
        0);

    WRITE_ONCE(st_hook_accepting_calls, false);
    WRITE_ONCE(st_hook_installed, false);

    st_hook_runtime_clear();

    ret = st_hook_targets_prepare();
    if (ret != 0)
        goto fail_clear_state;

    ret = st_nonreturning_tracepoint_init();
    if (ret != 0)
        goto fail_clear_state;

    ret = st_resolve_all_targets();
    if (ret != 0)
        goto fail_tracepoint;

    ret = st_hook_filters_build();
    if (ret != 0)
        goto fail_tracepoint;

    /*
     * Ogni indirizzo Ftrace viene installato una sola volta,
     * anche quando più numeri di syscall condividono la stessa
     * funzione kernel.
     */
    for (filter_index = 0;
         filter_index < st_hook_filter_count;
         filter_index++) {
        filter = &st_hook_filters[filter_index];

        ret = ftrace_set_filter_ip(
            &st_syscall_ftrace_ops,
            filter->original_ip,
            0,
            0);
        if (ret != 0) {
            pr_err("syscall_throttle: filtro Ftrace "
                   "indice=%zu, indirizzo=%px fallito: "
                   "errore=%d\n",
                   filter_index,
                   (void *)filter->original_ip,
                   ret);

            goto fail_remove_filters;
        }

        filter->installed = true;
    }

    ret = register_ftrace_function(
        &st_syscall_ftrace_ops);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione Ftrace "
               "globale fallita: errore=%d\n",
               ret);

        goto fail_remove_filters;
    }

    WRITE_ONCE(st_hook_accepting_calls, true);
    WRITE_ONCE(st_hook_installed, true);

    explicit_count = 0U;
    nonreturning_count = 0U;

    for (syscall_nr = 0;
         syscall_nr < ARRAY_SIZE(st_hook_targets);
         syscall_nr++) {
        if (st_hook_targets[syscall_nr].explicit_entry)
            explicit_count++;

        if (st_hook_targets[syscall_nr].nonreturning)
            nonreturning_count++;
    }

    pr_info("syscall_throttle: hook Ftrace x86-64 "
            "installato: target=%zu, definizioni=%zu, "
            "esplicite=%zu, implicite=%zu, "
            "filtri_unici=%zu, nonreturning=%zu\n",
            ARRAY_SIZE(st_hook_targets),
            ARRAY_SIZE(st_syscall_definitions),
            explicit_count,
            ARRAY_SIZE(st_hook_targets) -
                explicit_count,
            st_hook_filter_count,
            nonreturning_count);

    return 0;

fail_remove_filters:
    for (filter_index = 0;
         filter_index < st_hook_filter_count;
         filter_index++) {
        filter = &st_hook_filters[filter_index];

        if (!filter->installed)
            continue;

        cleanup_ret = ftrace_set_filter_ip(
            &st_syscall_ftrace_ops,
            filter->original_ip,
            1,
            0);
        if (cleanup_ret != 0) {
            pr_err("syscall_throttle: rollback filtro "
                   "indice=%zu, indirizzo=%px fallito: "
                   "errore=%d\n",
                   filter_index,
                   (void *)filter->original_ip,
                   cleanup_ret);
        }

        filter->installed = false;
    }

fail_tracepoint:
    st_nonreturning_tracepoint_exit();

fail_clear_state:
    st_hook_runtime_clear();

    return ret;
}



void st_syscall_hook_exit(void)
{
    struct st_hook_filter *filter;
    size_t filter_index;
    size_t removed_filter_count;
    int unregister_ret;
    int filter_ret;

    if (!READ_ONCE(st_hook_installed))
        return;

    /*
     * Le callback già iniziate possono completare il redirect,
     * ma nessuna nuova callback può entrare nel wrapper.
     */
    WRITE_ONCE(st_hook_accepting_calls, false);

    unregister_ret = unregister_ftrace_function(
        &st_syscall_ftrace_ops);
    if (unregister_ret != 0) {
        pr_err("syscall_throttle: rimozione funzione Ftrace "
               "fallita: errore=%d\n",
               unregister_ret);
    }

    removed_filter_count = 0U;

    for (filter_index = 0;
         filter_index < st_hook_filter_count;
         filter_index++) {
        filter = &st_hook_filters[filter_index];

        if (!filter->installed)
            continue;

        filter_ret = ftrace_set_filter_ip(
            &st_syscall_ftrace_ops,
            filter->original_ip,
            1,
            0);
        if (filter_ret != 0) {
            pr_err("syscall_throttle: rimozione filtro "
                   "indice=%zu, indirizzo=%px fallita: "
                   "errore=%d\n",
                   filter_index,
                   (void *)filter->original_ip,
                   filter_ret);
        } else {
            removed_filter_count++;
        }

        filter->installed = false;
    }

    /*
     * Il tracepoint sched_process_exit deve rimanere attivo
     * durante questa attesa per completare exit ed exit_group.
     */
    wait_event(
        st_hook_active_wait_queue,
        atomic_read(&st_hook_active_calls) == 0);

    st_nonreturning_tracepoint_exit();

    WARN_ON(!list_empty(
        &st_nonreturning_calls));

    pr_info("syscall_throttle: diagnostica hook x86-64: "
            "totali=%lld, monitor_spento=%lld, "
            "syscall_non_registrata=%lld, "
            "identita_non_corrispondente=%lld, "
            "rilevanti=%lld, limiter_bypass=%lld, "
            "limiter_allow=%lld, limiter_throttle=%lld, "
            "limiter_shutdown=%lld, wait_interrotte=%lld\n",
            (long long)atomic64_read(
                &st_hook_total_calls),
            (long long)atomic64_read(
                &st_hook_monitor_disabled_calls),
            (long long)atomic64_read(
                &st_hook_unregistered_syscall_calls),
            (long long)atomic64_read(
                &st_hook_unmatched_identity_calls),
            (long long)atomic64_read(
                &st_hook_relevant_calls),
            (long long)atomic64_read(
                &st_hook_rate_bypass_calls),
            (long long)atomic64_read(
                &st_hook_rate_allow_calls),
            (long long)atomic64_read(
                &st_hook_rate_throttle_calls),
            (long long)atomic64_read(
                &st_hook_rate_shutdown_calls),
            (long long)atomic64_read(
                &st_hook_wait_interrupted_calls));

    pr_info("syscall_throttle: diagnostica nonreturning: "
            "avviate=%lld, completate=%lld, "
            "ritorni_inattesi=%lld\n",
            (long long)atomic64_read(
                &st_nonreturning_started),
            (long long)atomic64_read(
                &st_nonreturning_completed),
            (long long)atomic64_read(
                &st_nonreturning_unexpected_returns));

    pr_info("syscall_throttle: hook Ftrace x86-64 "
            "rimosso: filtri=%zu/%zu\n",
            removed_filter_count,
            st_hook_filter_count);

    WRITE_ONCE(st_hook_installed, false);

    st_hook_runtime_clear();
}
