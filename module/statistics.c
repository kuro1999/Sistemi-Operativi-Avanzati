#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "statistics.h"

struct st_statistics_state {
    /* Identifica la sessione dei waiter; cambia a ogni nuova sessione statistica. */
    u64 generation;
    u64 continuity_generation;

    bool session_active;
    bool session_closing;

    u64 session_start_ns;
    u64 session_stop_ns;
    u64 last_blocked_change_ns;

    /* Integrale temporale di current_blocked: somma di current_blocked * elapsed_ns. */
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

/* Timestamp acquisiti sotto lock: ordine temporale e aggiornamenti devono
 * coincidere anche tra CPU diverse. */
static DEFINE_SPINLOCK(st_statistics_lock);
static struct st_statistics_state st_statistics;
static DECLARE_WAIT_QUEUE_HEAD(st_statistics_drain_queue);

static u64 st_statistics_saturating_add(u64 left, u64 right)
{
    u64 result;

    if (check_add_overflow(left, right, &result))
        return U64_MAX;
    return result;
}

static u64 st_statistics_saturating_multiply(u64 value, u32 multiplier)
{
    u64 result;

    if (check_mul_overflow(value, (u64)multiplier, &result)) {
        return U64_MAX;
    }
    return result;
}

static u64 st_statistics_saturating_increment(u64 value)
{
    if (value == U64_MAX)
        return U64_MAX;
    return value + 1U;
}

/* Generazione non nulla; il wrap-around richiederebbe 2^64 nuove sessioni. */
static u64 st_statistics_next_generation(u64 generation)
{
    generation++;
    if (generation == 0U)
        generation = 1U;
    return generation;
}

/* Richiede st_statistics_lock. */
static void st_statistics_account_blocked_time_locked(u64 now_ns)
{
    u64 elapsed_ns;
    u64 weighted_ns;

    if (!st_statistics.session_active)
        return;
    if (now_ns <= st_statistics.last_blocked_change_ns) {
        return;
    }
    elapsed_ns = now_ns - st_statistics.last_blocked_change_ns;
    weighted_ns = st_statistics_saturating_multiply(elapsed_ns, st_statistics.current_blocked);
    st_statistics.blocked_thread_time_ns =
        st_statistics_saturating_add(st_statistics.blocked_thread_time_ns, weighted_ns);
    st_statistics.last_blocked_change_ns = now_ns;
}

/* Richiede st_statistics_lock. Azzera contatori e apre una nuova continuita'.
 * MAX_SET ripristina poi la continuita' precedente e trasferisce i waiter. */
static void st_statistics_reset_session_locked(u64 now_ns, bool session_active)
{
    u64 next_generation;

    next_generation = st_statistics_next_generation(st_statistics.generation);
    memset(&st_statistics, 0, sizeof(st_statistics));
    st_statistics.generation = next_generation;
    st_statistics.continuity_generation = next_generation;
    st_statistics.session_active = session_active;
    if (session_active) {
        st_statistics.session_start_ns = now_ns;
        st_statistics.last_blocked_change_ns = now_ns;
    }
}

/*
 * MAX_SET: nuova osservazione, continuita' dei waiter contabilizzati.
 * Ordine dei lock: limiter -> statistiche; mai l'ordine inverso.
 * Nessuna allocazione, attesa o modifica degli stack dei waiter.
 */
void st_statistics_max_changed(void)
{
    unsigned long flags;
    u64 continuity;
    u32 carried;

    spin_lock_irqsave(&st_statistics_lock, flags);
    if (st_statistics.session_active && !st_statistics.session_closing) {
        carried = st_statistics.current_blocked;
        continuity = st_statistics.continuity_generation;
        st_statistics_reset_session_locked(ktime_get_ns(), true);
        st_statistics.continuity_generation = continuity;
        st_statistics.current_blocked = carried;
        st_statistics.peak_blocked = carried;
        st_statistics.relevant_invocations = carried;
        st_statistics.blocked_invocations = carried;
    }
    spin_unlock_irqrestore(&st_statistics_lock, flags);
}

void st_statistics_init(void)
{
    unsigned long flags;

    spin_lock_irqsave(&st_statistics_lock, flags);
    memset(&st_statistics, 0, sizeof(st_statistics));
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    pr_info("syscall_throttle: statistiche inizializzate\n");
}

void st_statistics_session_start(void)
{
    unsigned long flags;
    u64 generation;
    u64 now_ns;

    spin_lock_irqsave(&st_statistics_lock, flags);
    now_ns = ktime_get_ns();
    st_statistics_reset_session_locked(now_ns, true);
    generation = st_statistics.generation;
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    pr_info("syscall_throttle: sessione statistiche "
            "avviata: generazione=%llu\n", (unsigned long long)generation);
}

/* Lettura protetta della condizione della wait queue. */
static bool st_statistics_waiters_drained(void)
{
    unsigned long flags;
    bool drained;

    spin_lock_irqsave(&st_statistics_lock, flags);
    drained = st_statistics.current_blocked == 0U;
    spin_unlock_irqrestore(&st_statistics_lock, flags);
    return drained;
}

/* Il chiamante ha fermato il limiter e risvegliato i waiter; serializza ENABLE
 * con questa chiusura. Attende le attese contabilizzate, non le syscall
 * originali: block_complete() precede la loro esecuzione. */
void st_statistics_session_stop(void)
{
    unsigned long flags;
    u64 generation = 0U;
    u64 now_ns;
    bool stopped = false;

    spin_lock_irqsave(&st_statistics_lock, flags);
    if (st_statistics.session_active)
        st_statistics.session_closing = true;
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    /* Nessuno spinlock resta acquisito durante l'attesa. */
    wait_event(st_statistics_drain_queue, st_statistics_waiters_drained());

    spin_lock_irqsave(&st_statistics_lock, flags);
    now_ns = ktime_get_ns();
    if (st_statistics.session_active) {
        st_statistics_account_blocked_time_locked(now_ns);
        st_statistics.session_stop_ns = now_ns;
        st_statistics.last_blocked_change_ns = now_ns;
        st_statistics.session_active = false;
        generation = st_statistics.generation;
        stopped = true;
    }
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    if (stopped)
        pr_info("syscall_throttle: sessione statistiche arrestata: "
                "generazione=%llu, waiter_residui=0\n", (unsigned long long)generation);
}

u64 st_statistics_record_relevant_invocation(void)
{
    unsigned long flags;
    u64 generation;

    generation = 0U;

    spin_lock_irqsave(&st_statistics_lock, flags);
    if (st_statistics.session_active && !st_statistics.session_closing) {
        /* Contatore e generazione appartengono allo stesso snapshot sotto lock. */
        st_statistics.relevant_invocations =
            st_statistics_saturating_increment(st_statistics.relevant_invocations);
        generation = st_statistics.generation;
    }
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    return generation;
}

bool st_statistics_block_begin(struct st_statistics_block_context *context,
    u64 invocation_generation, u64 throttle_start_ns, unsigned int syscall_nr,
    kuid_t effective_uid, const char *program_name)
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
        strscpy(context->program_name, program_name, sizeof(context->program_name));
    }
    counted = false;

    spin_lock_irqsave(&st_statistics_lock, flags);
    now_ns = ktime_get_ns();
    if (!st_statistics.session_active || st_statistics.session_closing)
        goto out_unlock;
    /* ENABLE invalida i vecchi token; MAX_SET conserva la continuita'. */
    if (invocation_generation == 0U || invocation_generation < st_statistics.continuity_generation ||
        invocation_generation > st_statistics.generation) {
        goto out_unlock;
    }
    if (WARN_ON_ONCE(st_statistics.current_blocked == (u32)~0U)) {
        goto out_unlock;
    }
    st_statistics_account_blocked_time_locked(now_ns);
    /* Rilevante prima di MAX_SET, contabilizzata come bloccata dopo. */
    if (invocation_generation != st_statistics.generation)
        st_statistics.relevant_invocations =
            st_statistics_saturating_increment(st_statistics.relevant_invocations);
    st_statistics.current_blocked++;
    if (st_statistics.current_blocked > st_statistics.peak_blocked) {
        st_statistics.peak_blocked = st_statistics.current_blocked;
    }
    st_statistics.blocked_invocations =
        st_statistics_saturating_increment(st_statistics.blocked_invocations);
    context->generation = st_statistics.generation;
    /* Il delay parte dalla decisione; la contabilita usa now_ns. */
    context->start_ns = throttle_start_ns;
    context->counted = true;
    counted = true;
out_unlock:
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    return counted;
}

/* MAX_SET conserva i contesti; ENABLE delimita una nuova continuita'.
 * I contesti esterni a tale intervallo non modificano i contatori. */
static void st_statistics_block_finish(struct st_statistics_block_context *context, bool interrupted)
{
    unsigned long flags;
    u64 delay_ns;
    u64 delay_start_ns;
    u64 now_ns;
    bool same_generation;
    bool wake_drain = false;

    if (context == NULL || !context->counted) {
        return;
    }

    spin_lock_irqsave(&st_statistics_lock, flags);
    now_ns = ktime_get_ns();
    delay_ns = 0U;
    /* Un waiter trasferito misura solo il tratto della nuova sessione. */
    delay_start_ns = max(context->start_ns, st_statistics.session_start_ns);
    if (now_ns >= delay_start_ns)
        delay_ns = now_ns - delay_start_ns;
    same_generation = context->generation >= st_statistics.continuity_generation &&
                      context->generation <= st_statistics.generation;
    /* Escludi contesti di vecchie continuita' e sessioni gia' chiuse da DISABLE,
     * anche se non e' ancora avvenuto un successivo ENABLE. */
    if (!same_generation || !st_statistics.session_active) {
        goto out_unlock;
    }
    st_statistics_account_blocked_time_locked(now_ns);
    if (WARN_ON_ONCE(st_statistics.current_blocked == 0U)) {
        goto out_unlock;
    }
    st_statistics.current_blocked--;
    wake_drain = st_statistics.session_closing && st_statistics.current_blocked == 0U;
    if (interrupted) {
        st_statistics.interrupted_blocked_invocations =
            st_statistics_saturating_increment(st_statistics.interrupted_blocked_invocations);
        goto out_unlock;
    }
    st_statistics.completed_blocked_invocations =
        st_statistics_saturating_increment(st_statistics.completed_blocked_invocations);
    st_statistics.total_delay_ns = st_statistics_saturating_add(st_statistics.total_delay_ns, delay_ns);
    if (!st_statistics.peak_valid || delay_ns > st_statistics.peak_delay_ns) {
        st_statistics.peak_valid = true;
        st_statistics.peak_delay_ns = delay_ns;
        st_statistics.peak_syscall_nr = context->syscall_nr;
        st_statistics.peak_euid = context->effective_uid;
        strscpy(st_statistics.peak_program, context->program_name, sizeof(st_statistics.peak_program));
    }
out_unlock:
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    /* Disarma anche i contesti appartenenti a vecchie generazioni. */
    context->counted = false;
    if (wake_drain)
        wake_up_all(&st_statistics_drain_queue);
}

void st_statistics_block_complete(struct st_statistics_block_context *context)
{
    st_statistics_block_finish(context, false);
}

void st_statistics_block_interrupted(struct st_statistics_block_context *context)
{
    st_statistics_block_finish(context, true);
}

void st_statistics_get_snapshot(struct st_statistics_snapshot *snapshot)
{
    unsigned long flags;
    u64 observation_end_ns;
    u64 now_ns;

    if (snapshot == NULL)
        return;
    memset(snapshot, 0, sizeof(*snapshot));

    spin_lock_irqsave(&st_statistics_lock, flags);
    now_ns = ktime_get_ns();
    if (st_statistics.session_active) {
        st_statistics_account_blocked_time_locked(now_ns);
        observation_end_ns = now_ns;
    } else {
        observation_end_ns = st_statistics.session_stop_ns;
    }
    if (st_statistics.session_start_ns != 0U && observation_end_ns >= st_statistics.session_start_ns) {
        snapshot->observation_ns = observation_end_ns - st_statistics.session_start_ns;
    }
    snapshot->blocked_thread_time_ns = st_statistics.blocked_thread_time_ns;
    snapshot->relevant_invocations = st_statistics.relevant_invocations;
    snapshot->blocked_invocations = st_statistics.blocked_invocations;
    snapshot->completed_blocked_invocations = st_statistics.completed_blocked_invocations;
    snapshot->interrupted_blocked_invocations = st_statistics.interrupted_blocked_invocations;
    snapshot->total_delay_ns = st_statistics.total_delay_ns;
    snapshot->peak_delay_ns = st_statistics.peak_delay_ns;
    snapshot->current_blocked = st_statistics.current_blocked;
    snapshot->peak_blocked = st_statistics.peak_blocked;
    snapshot->peak_syscall_nr = st_statistics.peak_syscall_nr;
    snapshot->peak_valid = st_statistics.peak_valid ? 1U : 0U;
    snapshot->session_active = st_statistics.session_active ? 1U : 0U;
    if (st_statistics.peak_valid) {
        snapshot->peak_euid = from_kuid_munged(&init_user_ns, st_statistics.peak_euid);
        strscpy(snapshot->peak_program, st_statistics.peak_program, sizeof(snapshot->peak_program));
    }
    spin_unlock_irqrestore(&st_statistics_lock, flags);
}

void st_statistics_exit(void)
{
    unsigned long flags;

    st_statistics_session_stop();

    spin_lock_irqsave(&st_statistics_lock, flags);
    WARN_ON_ONCE(st_statistics.current_blocked != 0U);
    memset(&st_statistics, 0, sizeof(st_statistics));
    spin_unlock_irqrestore(&st_statistics_lock, flags);

    pr_info("syscall_throttle: statistiche rilasciate\n");
}
