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

/* ABI x86-64: i wrapper ricevono il frame pt_regs della syscall. */
typedef asmlinkage long (*st_x64_syscall_t)(const struct pt_regs *regs);

/* Definizioni dalla syscall table del kernel di compilazione. */
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

static const struct st_syscall_definition st_syscall_definitions[] = {
#include <asm/syscalls_64.h>
};

#undef __SYSCALL_NORETURN
#undef __SYSCALL

struct st_hook_target {
    const char *symbol_name;
    unsigned long original_ip;
    bool nonreturning;
    bool explicit_entry;
};

/* Filtri separati dai target: piu syscall possono condividere un indirizzo. */
struct st_hook_filter {
    unsigned long original_ip;
    bool installed;
};

static struct st_hook_target st_hook_targets[NR_syscalls];

static struct st_hook_filter st_hook_filters[NR_syscalls];

static size_t st_hook_filter_count;

/* Gli slot impliciti restano associati a sys_ni_syscall. */
static int st_hook_targets_prepare(void)
{
    const struct st_syscall_definition *definition;
    struct st_hook_target *target;
    size_t index;
    unsigned int syscall_nr;

    for (index = 0; index < ARRAY_SIZE(st_hook_targets); index++) {
        target = &st_hook_targets[index];
        target->symbol_name = "__x64_sys_ni_syscall";
        target->original_ip = 0U;
        target->nonreturning = false;
        target->explicit_entry = false;
    }

    for (index = 0; index < ARRAY_SIZE(st_syscall_definitions); index++) {
        definition = &st_syscall_definitions[index];
        syscall_nr = definition->syscall_nr;

        if (syscall_nr >= ARRAY_SIZE(st_hook_targets)) {
            pr_err("syscall_throttle: definizione syscall " "fuori range: nr=%u, limite=%zu\n", syscall_nr,
                ARRAY_SIZE(st_hook_targets));
            return -EINVAL;
        }

        target = &st_hook_targets[syscall_nr];

        if (target->explicit_entry) {
            pr_err("syscall_throttle: definizione syscall " "duplicata: nr=%u\n", syscall_nr);
            return -EINVAL;
        }

        target->symbol_name = definition->symbol_name;
        target->nonreturning = definition->nonreturning;
        target->explicit_entry = true;
    }

    return 0;
}

static int st_resolve_symbol(const char *symbol_name, unsigned long *original_ip)
{
    struct kprobe probe = {
        .symbol_name = symbol_name,
    };
    int ret;

    *original_ip = 0U;
    ret = register_kprobe(&probe);
    if (ret != 0) {
        pr_err("syscall_throttle: impossibile risolvere %s " "tramite Kprobe: errore=%d\n", symbol_name,
            ret);
        return ret;
    }

    *original_ip = (unsigned long)probe.addr;
    unregister_kprobe(&probe);

    if (*original_ip == 0U) {
        pr_err("syscall_throttle: indirizzo nullo per %s\n", symbol_name);
        return -ENOENT;
    }

    return 0;
}

/* Riusa i simboli gia risolti, inclusa la fallback sys_ni_syscall. */
static int st_resolve_all_targets(void)
{
    struct st_hook_target *target;
    const struct st_hook_target *previous;
    size_t syscall_nr;
    size_t previous_nr;
    int ret;

    for (syscall_nr = 0; syscall_nr < ARRAY_SIZE(st_hook_targets); syscall_nr++) {
        target = &st_hook_targets[syscall_nr];

        if (target->symbol_name == NULL) {
            pr_err("syscall_throttle: simbolo assente " "per syscall=%zu\n", syscall_nr);
            return -EINVAL;
        }

        target->original_ip = 0U;

        for (previous_nr = 0; previous_nr < syscall_nr; previous_nr++) {
            previous = &st_hook_targets[previous_nr];

            if (strcmp(target->symbol_name, previous->symbol_name) != 0)
                continue;

            if (previous->original_ip == 0U) {
                pr_err("syscall_throttle: indirizzo precedente " "nullo per syscall=%zu, simbolo=%s\n",
                    previous_nr, previous->symbol_name);
                return -EINVAL;
            }

            target->original_ip = previous->original_ip;
            break;
        }

        if (previous_nr < syscall_nr)
            continue;
        ret = st_resolve_symbol(target->symbol_name, &target->original_ip);
        if (ret != 0) {
            pr_err("syscall_throttle: risoluzione fallita " "per syscall=%zu, simbolo=%s\n", syscall_nr,
                target->symbol_name);
            return ret;
        }
    }

    return 0;
}

/* Deduplica per indirizzo: anche simboli diversi possono essere alias. */
static int st_hook_filters_build(void)
{
    const struct st_hook_target *target;
    struct st_hook_filter *filter;
    size_t syscall_nr;
    size_t filter_index;

    memset(st_hook_filters, 0, sizeof(st_hook_filters));
    st_hook_filter_count = 0U;

    for (syscall_nr = 0; syscall_nr < ARRAY_SIZE(st_hook_targets); syscall_nr++) {
        target = &st_hook_targets[syscall_nr];

        if (target->original_ip == 0U) {
            pr_err("syscall_throttle: target non risolto " "per syscall=%zu, simbolo=%s\n", syscall_nr,
                target->symbol_name);
            return -EINVAL;
        }

        for (filter_index = 0; filter_index < st_hook_filter_count; filter_index++) {
            if (st_hook_filters[filter_index].original_ip == target->original_ip)
                break;
        }
        if (filter_index < st_hook_filter_count)
            continue;

        if (st_hook_filter_count >= ARRAY_SIZE(st_hook_filters)) {
            pr_err("syscall_throttle: capacitÃ  filtri " "Ftrace esaurita\n");
            return -ENOSPC;
        }

        filter = &st_hook_filters[st_hook_filter_count];
        filter->original_ip = target->original_ip;
        filter->installed = false;

        st_hook_filter_count++;
    }

    if (st_hook_filter_count == 0U) {
        pr_err("syscall_throttle: nessun filtro " "Ftrace costruito\n");
        return -EINVAL;
    }

    return 0;
}

static void st_hook_runtime_clear(void)
{
    memset(st_hook_targets, 0, sizeof(st_hook_targets));

    memset(st_hook_filters, 0, sizeof(st_hook_filters));
    st_hook_filter_count = 0U;
}

static atomic_t st_hook_active_calls = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(st_hook_active_wait_queue);

/* Record sullo stack del wrapper; sched_process_exit lo rimuove prima che lo stack scompaia. */
struct st_nonreturning_call {
    struct list_head link;
    struct task_struct *task;
};

static LIST_HEAD(st_nonreturning_calls);
static DEFINE_SPINLOCK(st_nonreturning_calls_lock);

static struct tracepoint * st_sched_process_exit_tracepoint;

static bool st_sched_process_exit_registered;

static atomic64_t st_nonreturning_started = ATOMIC64_INIT(0);

static atomic64_t st_nonreturning_completed = ATOMIC64_INIT(0);

static atomic64_t st_nonreturning_unexpected_returns = ATOMIC64_INIT(0);

static void notrace st_nonreturning_call_track(struct st_nonreturning_call *call);

static bool notrace st_nonreturning_call_cancel(struct st_nonreturning_call *call);

static bool st_hook_accepting_calls;
static bool st_hook_installed;

static asmlinkage long notrace st_generic_syscall_wrapper(const struct pt_regs *regs,
    unsigned long original_ip);

/* Contesto Ftrace: solo controlli non bloccanti, senza attese. */
static void notrace st_ftrace_callback(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *ops,
    struct ftrace_regs *fregs)
{
    struct pt_regs *kernel_regs;
    struct pt_regs *syscall_regs;
    unsigned long first_argument;
    unsigned long original_ip;
    unsigned long raw_syscall_nr;
    unsigned int syscall_nr;

    (void)ops;

    /* Evita la ricorsione quando il modulo richiama la syscall originale. */
    if (within_module(parent_ip, THIS_MODULE))
        return;

    if (!st_monitor_fast_path_enabled())
        return;

    /* Il teardown puo sovrapporsi a callback gia iniziate. */
    if (!READ_ONCE(st_hook_accepting_calls))
        return;

    kernel_regs = ftrace_get_regs(fregs);
    if (unlikely(kernel_regs == NULL))
        return;

    /* Esclude chiamanti kernel con firma diversa, anche in presenza di alias __do_sys_*. */
    first_argument = ftrace_regs_get_argument(fregs, 0);
    syscall_regs = (struct pt_regs *)first_argument;

    if (unlikely(syscall_regs != current_pt_regs()))
        return;

    /* Solo syscall native x86-64: esclude IA32 e x32. */
    if (unlikely(!user_mode(syscall_regs) || !user_64bit_mode(syscall_regs) || in_32bit_syscall()))
        return;

    raw_syscall_nr = READ_ONCE(syscall_regs->orig_ax);
    syscall_nr = (unsigned int)raw_syscall_nr;

    if (unlikely(raw_syscall_nr != (unsigned long)syscall_nr || syscall_nr >= ARRAY_SIZE(st_hook_targets)))
        return;

    if (!st_syscall_registry_contains(syscall_nr))
        return;

    /* Usa l'inizio canonico della funzione, non il punto di strumentazione. */
    original_ip = ftrace_get_symaddr(ip);

    /* Conserva pt_regs in %rdi; passa original_ip al wrapper in %rsi (ABI x86-64). */
    kernel_regs->si = original_ip;

    /* Acquisisce active_calls prima del redirect. */
    atomic_inc(&st_hook_active_calls);

    ftrace_regs_set_instruction_pointer(fregs, (unsigned long)st_generic_syscall_wrapper);
}

static struct ftrace_ops st_syscall_ftrace_ops = {
    .func = st_ftrace_callback,
    .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION | FTRACE_OPS_FL_IPMODIFY,
};

/* La tabella resta immutabile fino all'azzeramento di active_calls. */

static const struct st_hook_target * st_hook_target_find(unsigned int syscall_nr,
    unsigned long original_ip)
{
    const struct st_hook_target *target;

    if (syscall_nr >= ARRAY_SIZE(st_hook_targets))
        return NULL;
    target = &st_hook_targets[syscall_nr];

    if (READ_ONCE(target->original_ip) == 0U)
        return NULL;

    if (READ_ONCE(target->original_ip) != original_ip)
        return NULL;
    return target;
}

static asmlinkage long notrace st_call_original_syscall(const struct st_hook_target *target,
    st_x64_syscall_t original_syscall, const struct pt_regs *regs, bool *release_active_call)
{
    struct st_nonreturning_call call;
    long result;

    /* O_TRUNC puo ignorare il pin del modulo: negalo anche senza matching, dopo l'eventuale ammissione. */
    if ((unsigned long)regs->orig_ax == (unsigned long)__NR_delete_module &&
        ((unsigned int)regs->si & O_TRUNC))
        return -EPERM;

    if (!target->nonreturning)
        return original_syscall(regs);

    st_nonreturning_call_track(&call);
    result = original_syscall(regs);

    atomic64_inc(&st_nonreturning_unexpected_returns);

    /* Se il tracepoint ha gia rimosso il record, ha anche rilasciato active_calls. */
    if (!st_nonreturning_call_cancel(&call))
        *release_active_call = false;

    pr_err_once("syscall_throttle: syscall marcata nonreturning " "ha restituito il controllo\n");
    return result;
}

/* Policy: monitor ON && syscall registrata && (EUID registrato || programma registrato). */
static bool st_syscall_is_relevant(unsigned int syscall_nr, char *program_name,
    size_t program_name_capacity, bool *program_name_valid)
{
    int identity_ret;

    if (program_name_valid != NULL)
        *program_name_valid = false;

    if (program_name != NULL && program_name_capacity != 0U) {
        program_name[0] = '\0';
    }

    if (!st_monitor_fast_path_enabled())
        return false;

    if (!st_syscall_registry_contains(syscall_nr))
        return false;

    /* Matching UID: acquisisci il nome solo al primo THROTTLE. */
    if (st_uid_registry_contains(current_euid()))
        return true;

    if (program_name == NULL || program_name_capacity == 0U)
        return false;

    /* Conserva il nome usato per il matching anche per le statistiche. */
    identity_ret = st_program_get_current_name(program_name, program_name_capacity);

    if (identity_ret == 0 && st_program_registry_contains(program_name)) {
        if (program_name_valid != NULL)
            *program_name_valid = true;
        return true;
    }

    return false;
}

/* Rivaluta a ogni iterazione senza sovrascrivere il nome della prima classificazione. */
static bool st_syscall_policy_still_matches(unsigned int syscall_nr)
{
    char name[ST_PROGRAM_NAME_CAPACITY];

    return st_syscall_is_relevant(syscall_nr, name, sizeof(name), NULL);
}

static void notrace st_hook_active_call_put(void)
{
    if (atomic_dec_and_test(&st_hook_active_calls))
        wake_up_all(&st_hook_active_wait_queue);
}

static void notrace st_nonreturning_call_track(struct st_nonreturning_call *call)
{
    unsigned long flags;

    INIT_LIST_HEAD(&call->link);
    call->task = current;

    spin_lock_irqsave(&st_nonreturning_calls_lock, flags);

    list_add_tail(&call->link, &st_nonreturning_calls);

    spin_unlock_irqrestore(&st_nonreturning_calls_lock, flags);
    atomic64_inc(&st_nonreturning_started);
}

/* Ritorno inatteso: true solo se il wrapper possiede ancora il record. */
static bool notrace st_nonreturning_call_cancel(struct st_nonreturning_call *call)
{
    unsigned long flags;
    bool removed;

    removed = false;

    spin_lock_irqsave(&st_nonreturning_calls_lock, flags);

    if (!list_empty(&call->link)) {
        list_del_init(&call->link);
        removed = true;
    }

    spin_unlock_irqrestore(&st_nonreturning_calls_lock, flags);
    return removed;
}

/* Il task in uscita non tornera al wrapper: il tracepoint rilascia active_calls. */
static void notrace st_sched_process_exit_callback(void *ignore, struct task_struct *task,
    bool group_dead)
{
    struct st_nonreturning_call *call;
    struct st_nonreturning_call *next;
    unsigned long flags;
    bool completed;

    (void)ignore;
    (void)group_dead;
    completed = false;

    spin_lock_irqsave(&st_nonreturning_calls_lock, flags);

    list_for_each_entry_safe(call, next, &st_nonreturning_calls, link) {
        if (call->task != task)
            continue;

        list_del_init(&call->link);
        completed = true;
        break;
    }

    spin_unlock_irqrestore(&st_nonreturning_calls_lock, flags);

    if (!completed)
        return;
    atomic64_inc(&st_nonreturning_completed);

    st_hook_active_call_put();
}

static void st_find_sched_process_exit_tracepoint(struct tracepoint *tracepoint, void *private_data)
{
    struct tracepoint **result;

    result = private_data;

    if (*result != NULL)
        return;

    if (strcmp(tracepoint->name, "sched_process_exit") == 0)
        *result = tracepoint;
}

static int st_nonreturning_tracepoint_init(void)
{
    int ret;

    if (st_sched_process_exit_registered)
        return 0;
    st_sched_process_exit_tracepoint = NULL;

    for_each_kernel_tracepoint(st_find_sched_process_exit_tracepoint, &st_sched_process_exit_tracepoint);

    if (st_sched_process_exit_tracepoint == NULL) {
        pr_err("syscall_throttle: tracepoint " "sched_process_exit non trovato\n");
        return -ENOENT;
    }

    ret = tracepoint_probe_register(st_sched_process_exit_tracepoint,
        (void *)st_sched_process_exit_callback, NULL);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione tracepoint " "sched_process_exit fallita: errore=%d\n",
            ret);
        st_sched_process_exit_tracepoint = NULL;
        return ret;
    }

    st_sched_process_exit_registered = true;

    pr_info("syscall_throttle: tracepoint " "sched_process_exit registrato\n");
    return 0;
}

static void st_nonreturning_tracepoint_exit(void)
{
    int ret;

    if (!st_sched_process_exit_registered)
        return;
    ret = tracepoint_probe_unregister(st_sched_process_exit_tracepoint,
        (void *)st_sched_process_exit_callback, NULL);
    if (ret != 0) {
        pr_err("syscall_throttle: rimozione tracepoint " "sched_process_exit fallita: errore=%d\n", ret);
    }

    /* Attende anche le callback gia in esecuzione prima di module_exit. */
    tracepoint_synchronize_unregister();
    st_sched_process_exit_registered = false;
    st_sched_process_exit_tracepoint = NULL;

    pr_info("syscall_throttle: tracepoint " "sched_process_exit rimosso\n");
}

static asmlinkage long notrace st_generic_syscall_wrapper(const struct pt_regs *regs,
    unsigned long original_ip)
{
    struct st_statistics_block_context statistics_context;

    enum st_rate_limiter_decision decision;
    const struct st_hook_target *target;
    st_x64_syscall_t original_syscall;

    char statistics_program_name[ST_PROGRAM_NAME_CAPACITY];

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

    /* block_begin inizializzera gli altri campi al primo THROTTLE. */
    statistics_context.counted = false;
    statistics_program_name[0] = '\0';
    statistics_program_name_valid = false;
    first_throttle_seen = false;
    release_active_call = true;
    target = NULL;
    raw_syscall_nr = READ_ONCE(regs->orig_ax);
    syscall_nr = (unsigned int)raw_syscall_nr;

    if (raw_syscall_nr == (unsigned long)syscall_nr) {
        target = st_hook_target_find(syscall_nr, original_ip);
    }

    if (unlikely(target == NULL)) {
        pr_err_once("syscall_throttle: target syscall non coerente: " "nr=%lu, ip=%px\n", raw_syscall_nr,
            (void *)original_ip);
        result = -ENOSYS;
        goto out;
    }

    /* Pin per ogni delete_module, anche fuori policy; active_calls protegge l'ingresso. */
    if (syscall_nr == (unsigned int)__NR_delete_module) {
        if (!try_module_get(THIS_MODULE)) {
            result = -EBUSY;
            goto out;
        }
        module_pinned = true;
    }

    original_syscall = (st_x64_syscall_t)original_ip;

    /* Controllo sul file verificato, prima di policy/budget; conserva LSM e permessi del driver. */
    if (syscall_nr == (unsigned int)__NR_ioctl && st_device_try_control_ioctl((unsigned int)regs->di,
        (unsigned int)regs->si, (unsigned long)regs->dx, &result)) {
        goto out;
    }

    if (!st_syscall_is_relevant(syscall_nr, statistics_program_name, sizeof(statistics_program_name),
        &statistics_program_name_valid)) {
        goto call_original;
    }

    /* Conta una sola invocazione, anche in presenza di retry. */
    statistics_generation = st_statistics_record_relevant_invocation();

    for (;;) {
        /* Il token precede i registry; nessun lock resta acquisito. */
        observed_generation = st_rate_limiter_get_generation();
        if (!st_syscall_policy_still_matches(syscall_nr)) {
            decision = ST_RATE_LIMITER_BYPASS;
        } else {
            decision = st_rate_limiter_try_acquire(observed_generation, &observed_generation,
                first_throttle_seen ? NULL : &throttle_start_ns);
        }

        switch (decision) {
        case ST_RATE_LIMITER_RETRY:
            continue;

        case ST_RATE_LIMITER_ALLOW:
        case ST_RATE_LIMITER_BYPASS:
        case ST_RATE_LIMITER_SHUTDOWN:
            /* Conclude la misura prima della syscall originale. */
            if (statistics_context.counted) {
                st_statistics_block_complete(&statistics_context);
            }

            goto call_original;

        case ST_RATE_LIMITER_THROTTLE:

            if (!first_throttle_seen) {
                kuid_t blocked_euid;

                first_throttle_seen = true;
                blocked_euid = current_euid();

                if (!statistics_program_name_valid) {
                    identity_ret = st_program_get_current_name(statistics_program_name,
                        sizeof(statistics_program_name));

                    if (identity_ret == 0) {
                        statistics_program_name_valid = true;
                    } else {
                        /* Un errore nella raccolta del nome non altera il throttling. */
                        strscpy(statistics_program_name, "<unavailable>", sizeof(statistics_program_name));
                    }
                }

                st_statistics_block_begin(&statistics_context, statistics_generation, throttle_start_ns,
                    blocked_euid, statistics_program_name);
            }

            wait_ret = st_rate_limiter_wait_for_change(observed_generation);

            if (wait_ret != 0) {
                /* Interruzione: niente syscall originale e nessun contributo al peak. */
                if (statistics_context.counted) {
                    st_statistics_block_interrupted(&statistics_context);
                }

                result = wait_ret;
                goto out;
            }

            /* Il risveglio richiede una nuova acquisizione del budget. */
            break;
        }
    }

call_original:
    result = st_call_original_syscall(target, original_syscall, regs, &release_active_call);
out:
    WARN_ON_ONCE(statistics_context.counted);

    /* active_calls resta acquisito durante il rilascio del pin. */
    if (module_pinned)
        module_put(THIS_MODULE);

    if (release_active_call)
        st_hook_active_call_put();
    return result;
}

static size_t st_hook_filters_remove(const char *phase)
{
    size_t index;
    size_t removed = 0U;
    int ret;

    for (index = 0; index < st_hook_filter_count; index++) {
        struct st_hook_filter *filter = &st_hook_filters[index];

        if (!filter->installed)
            continue;
        ret = ftrace_set_filter_ip(&st_syscall_ftrace_ops, filter->original_ip, 1, 0);
        if (ret != 0) {
            pr_err("syscall_throttle: %s filtro indice=%zu, " "indirizzo=%px fallito: errore=%d\n", phase,
                index, (void *)filter->original_ip, ret);
        } else {
            removed++;
        }

        filter->installed = false;
    }

    return removed;
}

int st_syscall_hook_init(void)
{
    struct st_hook_filter *filter;
    size_t filter_index;
    size_t syscall_nr;
    size_t explicit_count;
    size_t nonreturning_count;
    int ret;

    if (READ_ONCE(st_hook_installed))
        return 0;

    if (WARN_ON(!list_empty(&st_nonreturning_calls)))
        return -EBUSY;
    atomic_set(&st_hook_active_calls, 0);
    atomic64_set(&st_nonreturning_started, 0);
    atomic64_set(&st_nonreturning_completed, 0);
    atomic64_set(&st_nonreturning_unexpected_returns, 0);
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

    for (filter_index = 0; filter_index < st_hook_filter_count; filter_index++) {
        filter = &st_hook_filters[filter_index];
        ret = ftrace_set_filter_ip(&st_syscall_ftrace_ops, filter->original_ip, 0, 0);
        if (ret != 0) {
            pr_err("syscall_throttle: filtro Ftrace " "indice=%zu, indirizzo=%px fallito: " "errore=%d\n",
                filter_index, (void *)filter->original_ip, ret);
            goto fail_remove_filters;
        }

        filter->installed = true;
    }

    ret = register_ftrace_function(&st_syscall_ftrace_ops);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione Ftrace " "globale fallita: errore=%d\n", ret);
        goto fail_remove_filters;
    }

    WRITE_ONCE(st_hook_accepting_calls, true);
    WRITE_ONCE(st_hook_installed, true);
    explicit_count = 0U;
    nonreturning_count = 0U;

    for (syscall_nr = 0; syscall_nr < ARRAY_SIZE(st_hook_targets); syscall_nr++) {
        if (st_hook_targets[syscall_nr].explicit_entry)
            explicit_count++;

        if (st_hook_targets[syscall_nr].nonreturning)
            nonreturning_count++;
    }

    pr_info("syscall_throttle: hook Ftrace x86-64 " "installato: target=%zu, definizioni=%zu, "
        "esplicite=%zu, implicite=%zu, " "filtri_unici=%zu, nonreturning=%zu\n",
        ARRAY_SIZE(st_hook_targets), ARRAY_SIZE(st_syscall_definitions), explicit_count,
        ARRAY_SIZE(st_hook_targets) - explicit_count, st_hook_filter_count, nonreturning_count);
    return 0;

fail_remove_filters:
    st_hook_filters_remove("rollback");

fail_tracepoint:
    st_nonreturning_tracepoint_exit();

fail_clear_state:
    st_hook_runtime_clear();
    return ret;
}

void st_syscall_hook_exit(void)
{
    size_t removed_filter_count;
    int unregister_ret;

    if (!READ_ONCE(st_hook_installed))
        return;

    WRITE_ONCE(st_hook_accepting_calls, false);
    unregister_ret = unregister_ftrace_function(&st_syscall_ftrace_ops);
    if (unregister_ret != 0) {
        pr_err("syscall_throttle: rimozione funzione Ftrace " "fallita: errore=%d\n", unregister_ret);
    }

    removed_filter_count = st_hook_filters_remove("rimozione");

    /* Mantiene il tracepoint attivo finche exit/exit_group non hanno rilasciato i riferimenti. */
    wait_event(st_hook_active_wait_queue, atomic_read(&st_hook_active_calls) == 0);

    st_nonreturning_tracepoint_exit();

    WARN_ON(!list_empty(&st_nonreturning_calls));

    pr_info("syscall_throttle: diagnostica nonreturning: " "avviate=%lld, completate=%lld, "
        "ritorni_inattesi=%lld\n", (long long)atomic64_read(&st_nonreturning_started),
        (long long)atomic64_read(&st_nonreturning_completed),
        (long long)atomic64_read(&st_nonreturning_unexpected_returns));

    pr_info("syscall_throttle: hook Ftrace x86-64 " "rimosso: filtri=%zu/%zu\n", removed_filter_count,
        st_hook_filter_count);
    WRITE_ONCE(st_hook_installed, false);

    st_hook_runtime_clear();
}
