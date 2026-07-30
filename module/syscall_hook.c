#include <asm/ptrace.h>
#include <asm/unistd.h>

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/linkage.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "monitor_state.h"
#include "program_registry.h"
#include "rate_limiter.h"
#include "syscall_hook.h"
#include "syscall_registry.h"
#include "uid_registry.h"

/*
 * Tutti i wrapper __x64_sys_* ricevono un puntatore ai registri
 * salvati lungo il percorso di ingresso della system call.
 */
typedef asmlinkage long (*st_x64_syscall_t)(
    const struct pt_regs *regs);

static const char st_hook_symbol[] = "__x64_sys_nanosleep";

static unsigned long st_hook_target_ip;
static st_x64_syscall_t st_original_nanosleep;

static atomic_t st_hook_active_calls = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(st_hook_active_wait_queue);

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
st_nanosleep_wrapper(const struct pt_regs *regs);

/*
 * Il callback Ftrace viene eseguito in un contesto nel quale non
 * è consentito dormire.
 *
 * Per questo motivo svolge soltanto due operazioni:
 *
 * 1. evita la ricorsione quando il wrapper richiama l'originale;
 * 2. devia l'instruction pointer verso il wrapper pass-through.
 */
static void notrace st_ftrace_callback(
    unsigned long ip,
    unsigned long parent_ip,
    struct ftrace_ops *ops,
    struct ftrace_regs *fregs)
{
    (void)ip;
    (void)ops;

    /*
     * Quando st_nanosleep_wrapper() chiama la funzione originale,
     * parent_ip appartiene al nostro modulo. In quel caso Ftrace
     * deve lasciare proseguire la vera __x64_sys_nanosleep().
     */
    if (within_module(parent_ip, THIS_MODULE))
        return;

    /*
     * Durante il teardown il callback può essere ancora osservato
     * da una CPU che era già entrata nel percorso Ftrace.
     */
    if (!READ_ONCE(st_hook_accepting_calls))
        return;

    /*
     * Il conteggio viene incrementato prima della deviazione.
     * Il wrapper lo decrementerà soltanto dopo il ritorno della
     * vera system call.
     */
    atomic_inc(&st_hook_active_calls);

    ftrace_regs_set_instruction_pointer(
        fregs,
        (unsigned long)st_nanosleep_wrapper);
}

static struct ftrace_ops st_nanosleep_ftrace_ops = {
    .func = st_ftrace_callback,
    .flags = FTRACE_OPS_FL_SAVE_REGS |
             FTRACE_OPS_FL_RECURSION |
             FTRACE_OPS_FL_IPMODIFY,
};

/*
 * Risolve un simbolo kernel non esportato utilizzando una Kprobe
 * temporanea.
 *
 * La probe non possiede handler e viene rimossa immediatamente
 * dopo avere letto il relativo indirizzo.
 */
static int st_resolve_hook_target(void)
{
    struct kprobe probe = {
        .symbol_name = st_hook_symbol,
    };
    int ret;

    ret = register_kprobe(&probe);
    if (ret != 0) {
        pr_err("syscall_throttle: impossibile risolvere %s "
               "tramite Kprobe: errore=%d\n",
               st_hook_symbol,
               ret);
        return ret;
    }

    st_hook_target_ip = (unsigned long)probe.addr;

    unregister_kprobe(&probe);

    if (st_hook_target_ip == 0U) {
        pr_err("syscall_throttle: indirizzo nullo per %s\n",
               st_hook_symbol);
        return -ENOENT;
    }

    st_original_nanosleep =
        (st_x64_syscall_t)st_hook_target_ip;

    return 0;
}

/*
 * Classifica una invocazione di nanosleep secondo la politica
 * finale del monitor:
 *
 * monitor attivo
 * AND nanosleep registrata
 * AND (effective UID registrato OR programma registrato).
 *
 * La funzione viene eseguita nel wrapper, quindi in process
 * context e non nel callback Ftrace.
 */
static bool st_nanosleep_is_relevant(void)
{
    if (!st_monitor_is_enabled()) {
        atomic64_inc(&st_hook_monitor_disabled_calls);
        return false;
    }

    if (!st_syscall_registry_contains(
            (unsigned int)__NR_nanosleep)) {
        atomic64_inc(
            &st_hook_unregistered_syscall_calls);
        return false;
    }

    /*
     * Lo short-circuit evita l'identificazione più costosa
     * dell'eseguibile quando l'effective UID è già registrato.
     */
    if (st_uid_registry_contains(current_euid()) ||
        st_program_registry_contains_current()) {
        atomic64_inc(&st_hook_relevant_calls);
        return true;
    }

    atomic64_inc(&st_hook_unmatched_identity_calls);
    return false;
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

static asmlinkage long notrace
st_nanosleep_wrapper(const struct pt_regs *regs)
{
    enum st_rate_limiter_decision decision;
    u64 observed_generation;
    long result;
    int wait_ret;

    atomic64_inc(&st_hook_total_calls);

    /*
     * Le chiamate non rilevanti attraversano immediatamente
     * il wrapper senza consultare il rate limiter.
     */
    if (!st_nanosleep_is_relevant()) {
        result = st_original_nanosleep(regs);
        goto out;
    }

    for (;;) {
        /*
         * try_acquire() restituisce insieme alla decisione la
         * generazione sulla quale essa è stata presa.
         */
        decision = st_rate_limiter_try_acquire(
            &observed_generation);

        st_record_rate_limiter_decision(decision);

        switch (decision) {
        case ST_RATE_LIMITER_ALLOW:
        case ST_RATE_LIMITER_BYPASS:
        case ST_RATE_LIMITER_SHUTDOWN:
            /*
             * ALLOW consuma una posizione della finestra.
             *
             * BYPASS viene prodotto, per esempio, quando un
             * DISABLE concorrente ha già arrestato il limiter.
             *
             * SHUTDOWN viene prodotto durante la rimozione del
             * modulo. In entrambi questi ultimi casi preserviamo
             * la semantica della syscall originale.
             */
            result = st_original_nanosleep(regs);
            goto out;

        case ST_RATE_LIMITER_THROTTLE:
            /*
             * Il thread viene bloccato prima dell'esecuzione
             * della vera system call.
             */
            wait_ret =
                st_rate_limiter_wait_for_change(
                    observed_generation);

            if (wait_ret != 0) {
                /*
                 * wait_event_interruptible() restituisce
                 * -ERESTARTSYS quando arriva un segnale.
                 *
                 * Restituendolo dal wrapper, lasciamo al normale
                 * percorso syscall del kernel la gestione di
                 * EINTR o del restart automatico.
                 */
                atomic64_inc(
                    &st_hook_wait_interrupted_calls);

                result = wait_ret;
                goto out;
            }

            /*
             * La generazione o lo stato sono cambiati.
             * Il budget non viene assegnato dal wake-up:
             * tutti i thread risvegliati devono competere
             * nuovamente tramite try_acquire().
             */
            break;
        }
    }

out:
    st_hook_active_call_put();
    return result;
}

int st_syscall_hook_init(void)
{
    int ret;

    if (READ_ONCE(st_hook_installed))
        return 0;

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

    WRITE_ONCE(st_hook_accepting_calls, false);

    ret = st_resolve_hook_target();
    if (ret != 0)
        return ret;

    /*
     * Il filtro limita il callback esclusivamente alla funzione
     * __x64_sys_nanosleep.
     */
    ret = ftrace_set_filter_ip(
        &st_nanosleep_ftrace_ops,
        st_hook_target_ip,
        0,
        0);
    if (ret != 0) {
        pr_err("syscall_throttle: filtro Ftrace su %s fallito: "
               "errore=%d\n",
               st_hook_symbol,
               ret);
        goto fail_clear_target;
    }

    ret = register_ftrace_function(
        &st_nanosleep_ftrace_ops);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione Ftrace su %s "
               "fallita: errore=%d\n",
               st_hook_symbol,
               ret);
        goto fail_remove_filter;
    }

    /*
     * Soltanto dopo la registrazione completa accettiamo nuove
     * deviazioni verso il wrapper.
     */
    WRITE_ONCE(st_hook_accepting_calls, true);
    WRITE_ONCE(st_hook_installed, true);

    pr_info("syscall_throttle: hook Ftrace installato su %s, "
            "indirizzo=%px\n",
            st_hook_symbol,
            (void *)st_hook_target_ip);

    return 0;

fail_remove_filter:
    ftrace_set_filter_ip(
        &st_nanosleep_ftrace_ops,
        st_hook_target_ip,
        1,
        0);

fail_clear_target:
    st_original_nanosleep = NULL;
    st_hook_target_ip = 0U;

    return ret;
}

void st_syscall_hook_exit(void)
{
    int unregister_ret;
    int filter_ret;

    if (!READ_ONCE(st_hook_installed))
        return;

    /*
     * Impedisce a callback ancora concorrenti di accettare nuove
     * deviazioni durante la rimozione.
     */
    WRITE_ONCE(st_hook_accepting_calls, false);

    unregister_ret = unregister_ftrace_function(
        &st_nanosleep_ftrace_ops);
    if (unregister_ret != 0) {
        pr_err("syscall_throttle: rimozione funzione Ftrace "
               "fallita: errore=%d\n",
               unregister_ret);
    }

    filter_ret = ftrace_set_filter_ip(
        &st_nanosleep_ftrace_ops,
        st_hook_target_ip,
        1,
        0);
    if (filter_ret != 0) {
        pr_err("syscall_throttle: rimozione filtro Ftrace "
               "fallita: errore=%d\n",
               filter_ret);
    }

    /*
     * unregister_ftrace_function() impedisce nuove callback.
     * Rimangono però possibili wrapper che avevano già effettuato
     * la deviazione prima della rimozione.
     */
    wait_event(
        st_hook_active_wait_queue,
        atomic_read(&st_hook_active_calls) == 0);

    /*
     * A questo punto nessun wrapper può più modificare i
     * contatori, quindi il riepilogo è internamente coerente.
     */
    pr_info("syscall_throttle: diagnostica hook %s: "
            "totali=%lld, monitor_spento=%lld, "
            "syscall_non_registrata=%lld, "
            "identita_non_corrispondente=%lld, "
            "rilevanti=%lld, limiter_bypass=%lld, "
            "limiter_allow=%lld, limiter_throttle=%lld, "
            "limiter_shutdown=%lld, wait_interrotte=%lld\n",
            st_hook_symbol,
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

    WRITE_ONCE(st_hook_installed, false);

    st_original_nanosleep = NULL;
    st_hook_target_ip = 0U;

    pr_info("syscall_throttle: hook Ftrace rimosso da %s\n",
            st_hook_symbol);
}
