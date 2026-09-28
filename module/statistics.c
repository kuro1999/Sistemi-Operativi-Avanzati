#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include "statistics.h"

struct st_statistics_state {
    /*
     * Cambia a ogni nuova sessione o reset.
     *
     * I contesti locali dei waiter permettono così di riconoscere
     * eventi appartenenti a una sessione precedente.
     */
    u64 generation;

    bool session_active;

    u64 session_start_ns;
    u64 session_stop_ns;
    u64 last_blocked_change_ns;

    /*
     * Integrale temporale:
     *
     * current_blocked * elapsed_ns
     */
    u64 blocked_thread_time_ns;

    u64 relevant_invocations;
    u64 blocked_invocations;
    u64 completed_blocked_invocations;
    u64 interrupted_blocked_invocations;

    u64 total_delay_ns;
    u64 peak_delay_ns;

    u32 current_blocked;
    u32 peak_blocked;

    u32 peak_syscall_nr;
    kuid_t peak_euid;
    char peak_program[ST_PROGRAM_NAME_CAPACITY];

    bool peak_valid;
};

/*
 * I timestamp usati per aggiornare o fotografare lo stato condiviso
 * vengono acquisiti sotto questo lock: l'ordine temporale deve
 * coincidere con l'ordine degli aggiornamenti, anche tra CPU diverse.
 */
static DEFINE_SPINLOCK(st_statistics_lock);
static struct st_statistics_state st_statistics;


static u64 st_statistics_saturating_add(
    u64 left,
    u64 right)
{
    u64 result;

    if (check_add_overflow(left, right, &result))
        return U64_MAX;

    return result;
}


static u64 st_statistics_saturating_multiply(
    u64 value,
    u32 multiplier)
{
    u64 result;

    if (check_mul_overflow(
            value,
            (u64)multiplier,
            &result)) {
        return U64_MAX;
    }

    return result;
}


static u64 st_statistics_saturating_increment(
    u64 value)
{
    if (value == U64_MAX)
        return U64_MAX;

    return value + 1U;
}


/*
 * Produce una nuova generazione diversa da zero.
 *
 * Il wrap-around richiederebbe 2^64 nuove sessioni durante lo
 * stesso caricamento del modulo ed è quindi soltanto teorico.
 */
static u64 st_statistics_next_generation(
    u64 generation)
{
    generation++;

    if (generation == 0U)
        generation = 1U;

    return generation;
}


/*
 * Deve essere chiamata con st_statistics_lock acquisito.
 */
static void st_statistics_account_blocked_time_locked(
    u64 now_ns)
{
    u64 elapsed_ns;
    u64 weighted_ns;

    if (!st_statistics.session_active)
        return;

    if (now_ns <=
        st_statistics.last_blocked_change_ns) {
        return;
    }

    elapsed_ns =
        now_ns -
        st_statistics.last_blocked_change_ns;

    weighted_ns =
        st_statistics_saturating_multiply(
            elapsed_ns,
            st_statistics.current_blocked);

    st_statistics.blocked_thread_time_ns =
        st_statistics_saturating_add(
            st_statistics.blocked_thread_time_ns,
            weighted_ns);

    st_statistics.last_blocked_change_ns = now_ns;
}


/*
 * Apre o azzera una sessione statistica.
 *
 * Ogni generazione parte con current_blocked e peak_blocked
 * uguali a zero.
 *
 * I contesti appartenenti a una generazione precedente possono
 * essere ancora presenti sullo stack di waiter risvegliati da un
 * DISABLE. Tali contesti verranno riconosciuti tramite generation
 * e non modificheranno la nuova sessione.
 *
 * Deve essere chiamata con st_statistics_lock acquisito.
 */
static void st_statistics_reset_session_locked(
    u64 now_ns,
    bool session_active)
{
    u64 next_generation;

    next_generation =
        st_statistics_next_generation(
            st_statistics.generation);

    memset(
        &st_statistics,
        0,
        sizeof(st_statistics));

    st_statistics.generation =
        next_generation;

    st_statistics.session_active =
        session_active;

    if (session_active) {
        st_statistics.session_start_ns =
            now_ns;

        st_statistics.last_blocked_change_ns =
            now_ns;
    }
}


void st_statistics_init(void)
{
    unsigned long flags;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    memset(
        &st_statistics,
        0,
        sizeof(st_statistics));

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    pr_info("syscall_throttle: statistiche inizializzate\n");
}


void st_statistics_session_start(void)
{
    unsigned long flags;
    u64 generation;
    u64 now_ns;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    st_statistics_reset_session_locked(
        now_ns,
        true);

    generation =
        st_statistics.generation;

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    pr_info("syscall_throttle: sessione statistiche "
            "avviata: generazione=%llu\n",
            (unsigned long long)generation);
}


/*
 * Chiude definitivamente la sessione statistica corrente.
 *
 * Il punto di congelamento è protetto da st_statistics_lock.
 * Tutto ciò che è stato completato prima di questo punto resta
 * visibile; gli eventi successivi dei waiter ancora presenti
 * vengono ignorati.
 *
 * Il tempo pesato dei waiter viene prima integrato fino a now_ns.
 * Soltanto dopo current_blocked viene portato a zero.
 */
void st_statistics_session_stop(void)
{
    unsigned long flags;
    u64 generation;
    u64 now_ns;
    u32 released_blocked;
    bool stopped;

    generation = 0U;
    released_blocked = 0U;
    stopped = false;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    if (st_statistics.session_active) {
        /*
         * Account finale:
         *
         * blocked_thread_time_ns +=
         *     current_blocked *
         *     (now_ns - last_blocked_change_ns)
         */
        st_statistics_account_blocked_time_locked(
            now_ns);

        st_statistics.session_stop_ns =
            now_ns;

        st_statistics.last_blocked_change_ns =
            now_ns;

        released_blocked =
            st_statistics.current_blocked;

        /*
         * Dal punto di vista dello snapshot non esistono più
         * waiter appartenenti a una sessione ormai chiusa.
         *
         * I relativi contesti locali rimangono validi sullo
         * stack dei wrapper, ma block_finish() li disarmerà
         * senza modificare i contatori.
         */
        st_statistics.current_blocked = 0U;

        st_statistics.session_active = false;

        generation =
            st_statistics.generation;

        stopped = true;
    }

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    if (stopped) {
        pr_info("syscall_throttle: sessione statistiche "
                "arrestata: generazione=%llu, "
                "waiter_congelati=%u\n",
                (unsigned long long)generation,
                released_blocked);
    }
}


int st_statistics_reset(void)
{
    unsigned long flags;
    u64 generation;
    u64 now_ns;
    bool session_active;
    int ret;

    generation = 0U;
    session_active = false;
    ret = 0;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    /*
     * Ogni waiter conserva la generazione e una reference logica
     * su current_blocked.
     *
     * Cambiare sessione mentre questi contesti sono attivi
     * renderebbe ambiguo il reset esplicito.
     */
    if (st_statistics.current_blocked != 0U) {
        ret = -EBUSY;
        goto out_unlock;
    }

    session_active =
        st_statistics.session_active;

    st_statistics_reset_session_locked(
        now_ns,
        session_active);

    generation =
        st_statistics.generation;

out_unlock:
    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    if (ret == 0) {
        pr_info("syscall_throttle: statistiche azzerate: "
                "sessione=%s, generazione=%llu\n",
                session_active ? "attiva" : "inattiva",
                (unsigned long long)generation);
    }

    return ret;
}


u64 st_statistics_record_relevant_invocation(void)
{
    unsigned long flags;
    u64 generation;

    generation = 0U;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    if (st_statistics.session_active) {
        /*
         * L'incremento e l'acquisizione del token avvengono
         * sotto lo stesso lock.
         *
         * STATS_RESET non può quindi inserirsi tra le due
         * operazioni e attribuire alla chiamata una generazione
         * diversa da quella del relativo relevant counter.
         */
        st_statistics.relevant_invocations =
            st_statistics_saturating_increment(
                st_statistics.relevant_invocations);

        generation =
            st_statistics.generation;
    }

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    return generation;
}


bool st_statistics_block_begin(
    struct st_statistics_block_context *context,
    u64 invocation_generation,
    u64 throttle_start_ns,
    unsigned int syscall_nr,
    kuid_t effective_uid,
    const char *program_name)
{
    unsigned long flags;
    u64 now_ns;
    bool counted;

    if (context == NULL)
        return false;

    memset(context, 0, sizeof(*context));

    context->syscall_nr = syscall_nr;
    context->effective_uid = effective_uid;

    if (program_name != NULL) {
        strscpy(
            context->program_name,
            program_name,
            sizeof(context->program_name));
    }

    counted = false;

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    if (!st_statistics.session_active)
        goto out_unlock;

    /*
     * Una syscall può entrare in THROTTLE soltanto nella stessa
     * generazione nella quale era stata registrata come rilevante.
     *
     * Un RESET o un nuovo ENABLE intervenuto nel frattempo rende
     * il token obsoleto e l'evento non viene attribuito alla nuova
     * sessione.
     */
    if (invocation_generation == 0U ||
        invocation_generation !=
            st_statistics.generation) {
        goto out_unlock;
    }

    if (WARN_ON_ONCE(
            st_statistics.current_blocked ==
                (u32)~0U)) {
        goto out_unlock;
    }

    st_statistics_account_blocked_time_locked(
        now_ns);

    st_statistics.current_blocked++;

    if (st_statistics.current_blocked >
        st_statistics.peak_blocked) {
        st_statistics.peak_blocked =
            st_statistics.current_blocked;
    }

    st_statistics.blocked_invocations =
        st_statistics_saturating_increment(
            st_statistics.blocked_invocations);

    context->generation =
        invocation_generation;

    /* Il delay parte dalla decisione; la contabilita usa now_ns. */
    context->start_ns = throttle_start_ns;
    context->counted = true;

    counted = true;

out_unlock:
    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    return counted;
}


/*
 * Conclude un contesto precedentemente bloccato.
 *
 * Soltanto un contesto appartenente alla generazione corrente
 * può modificare current_blocked, i contatori di completamento
 * e il peak delay.
 *
 * Un contesto di una vecchia generazione viene semplicemente
 * disarmato. Questo caso può verificarsi quando:
 *
 *   vecchia sessione -> DISABLE -> nuova ENABLE
 *
 * prima che tutti i waiter risvegliati abbiano completato il
 * proprio percorso nel wrapper.
 */
static void st_statistics_block_finish(
    struct st_statistics_block_context *context,
    bool interrupted)
{
    unsigned long flags;
    u64 delay_ns;
    u64 now_ns;
    bool same_generation;

    if (context == NULL ||
        !context->counted) {
        return;
    }

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    delay_ns = 0U;

    if (now_ns >= context->start_ns)
        delay_ns = now_ns - context->start_ns;

    same_generation =
        context->generation ==
            st_statistics.generation;

    /*
     * Il contesto non può modificare le statistiche quando:
     *
     * - appartiene a una generazione precedente;
     * - la propria sessione è già stata chiusa da DISABLE.
     *
     * Nel secondo caso la generazione può essere ancora uguale:
     * il successivo ENABLE non è necessariamente già avvenuto.
     */
    if (!same_generation ||
        !st_statistics.session_active) {
        goto out_unlock;
    }

    st_statistics_account_blocked_time_locked(
        now_ns);

    if (WARN_ON_ONCE(
            st_statistics.current_blocked == 0U)) {
        goto out_unlock;
    }

    st_statistics.current_blocked--;

    if (interrupted) {
        st_statistics.interrupted_blocked_invocations =
            st_statistics_saturating_increment(
                st_statistics
                    .interrupted_blocked_invocations);

        goto out_unlock;
    }

    st_statistics.completed_blocked_invocations =
        st_statistics_saturating_increment(
            st_statistics
                .completed_blocked_invocations);

    st_statistics.total_delay_ns =
        st_statistics_saturating_add(
            st_statistics.total_delay_ns,
            delay_ns);

    if (!st_statistics.peak_valid ||
        delay_ns > st_statistics.peak_delay_ns) {
        st_statistics.peak_valid = true;
        st_statistics.peak_delay_ns = delay_ns;

        st_statistics.peak_syscall_nr =
            context->syscall_nr;

        st_statistics.peak_euid =
            context->effective_uid;

        strscpy(
            st_statistics.peak_program,
            context->program_name,
            sizeof(st_statistics.peak_program));
    }

out_unlock:
    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    /*
     * Anche un contesto appartenente a una vecchia generazione
     * deve essere disarmato prima dell'uscita dal wrapper.
     */
    context->counted = false;
}


void st_statistics_block_complete(
    struct st_statistics_block_context *context)
{
    st_statistics_block_finish(
        context,
        false);
}


void st_statistics_block_interrupted(
    struct st_statistics_block_context *context)
{
    st_statistics_block_finish(
        context,
        true);
}


void st_statistics_get_snapshot(
    struct st_statistics_snapshot *snapshot)
{
    unsigned long flags;
    u64 observation_end_ns;
    u64 now_ns;

    if (snapshot == NULL)
        return;

    memset(snapshot, 0, sizeof(*snapshot));

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    now_ns = ktime_get_ns();

    if (st_statistics.session_active) {
        st_statistics_account_blocked_time_locked(
            now_ns);
    }

    if (st_statistics.session_active) {
        observation_end_ns = now_ns;
    } else {
        observation_end_ns =
            st_statistics.session_stop_ns;
    }

    if (st_statistics.session_start_ns != 0U &&
        observation_end_ns >=
            st_statistics.session_start_ns) {
        snapshot->observation_ns =
            observation_end_ns -
            st_statistics.session_start_ns;
    }

    snapshot->blocked_thread_time_ns =
        st_statistics.blocked_thread_time_ns;

    snapshot->relevant_invocations =
        st_statistics.relevant_invocations;

    snapshot->blocked_invocations =
        st_statistics.blocked_invocations;

    snapshot->completed_blocked_invocations =
        st_statistics.completed_blocked_invocations;

    snapshot->interrupted_blocked_invocations =
        st_statistics.interrupted_blocked_invocations;

    snapshot->total_delay_ns =
        st_statistics.total_delay_ns;

    snapshot->peak_delay_ns =
        st_statistics.peak_delay_ns;

    snapshot->current_blocked =
        st_statistics.current_blocked;

    snapshot->peak_blocked =
        st_statistics.peak_blocked;

    snapshot->peak_syscall_nr =
        st_statistics.peak_syscall_nr;

    snapshot->peak_valid =
        st_statistics.peak_valid ? 1U : 0U;

    snapshot->session_active =
        st_statistics.session_active ? 1U : 0U;

    if (st_statistics.peak_valid) {
        snapshot->peak_euid =
            from_kuid_munged(
                &init_user_ns,
                st_statistics.peak_euid);

        strscpy(
            snapshot->peak_program,
            st_statistics.peak_program,
            sizeof(snapshot->peak_program));
    }

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);
}


void st_statistics_exit(void)
{
    unsigned long flags;

    st_statistics_session_stop();

    spin_lock_irqsave(
        &st_statistics_lock,
        flags);

    WARN_ON_ONCE(
        st_statistics.current_blocked != 0U);

    memset(
        &st_statistics,
        0,
        sizeof(st_statistics));

    spin_unlock_irqrestore(
        &st_statistics_lock,
        flags);

    pr_info("syscall_throttle: statistiche rilasciate\n");
}
