#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "rate_limiter.h"

struct st_rate_limiter_state {
    spinlock_t lock;
    struct timer_list window_timer;
    wait_queue_head_t wait_queue;

    u64 max_invocations;
    u64 admitted;
    u64 generation;

    unsigned long next_expiry;

    bool running;
    bool stopping;
};

static struct st_rate_limiter_state st_rate_limiter;

static void
st_rate_limiter_timer_callback(struct timer_list *timer)
{
    struct st_rate_limiter_state *state;
    bool wake_waiters = false;

    state = timer_container_of(state, timer, window_timer);

    /*
     * La callback viene eseguita in softirq context.
     * Non è quindi necessario usare spin_lock_bh() qui.
     */
    spin_lock(&state->lock);

    if (state->running && !state->stopping) {
        state->admitted = 0U;
        state->generation++;

        /*
         * Manteniamo la cadenza rispetto alla scadenza precedente,
         * evitando però di riarmare il timer nel passato qualora
         * la callback sia stata eseguita con forte ritardo.
         */
        state->next_expiry += HZ;

        if (time_before_eq(state->next_expiry, jiffies))
            state->next_expiry = jiffies + HZ;

        mod_timer(&state->window_timer,
                  state->next_expiry);

        wake_waiters = true;
    }

    spin_unlock(&state->lock);

    /*
     * Il wake-up viene eseguito fuori dalla sezione critica.
     * I thread risvegliati dovranno ripetere l'acquisizione
     * del budget e non saranno ammessi automaticamente.
     */
    if (wake_waiters)
        wake_up_all(&state->wait_queue);
}

void st_rate_limiter_init(void)
{
    spin_lock_init(&st_rate_limiter.lock);
    init_waitqueue_head(&st_rate_limiter.wait_queue);

    timer_setup(&st_rate_limiter.window_timer,
                st_rate_limiter_timer_callback,
                0);

    st_rate_limiter.max_invocations = 0U;
    st_rate_limiter.admitted = 0U;
    st_rate_limiter.generation = 0U;
    st_rate_limiter.next_expiry = 0U;
    st_rate_limiter.running = false;
    st_rate_limiter.stopping = false;

    pr_info("syscall_throttle: rate limiter inizializzato, MAX=0\n");
}

int st_rate_limiter_start(void)
{
    bool started = false;
    int ret = 0;

    spin_lock_bh(&st_rate_limiter.lock);

    if (st_rate_limiter.stopping) {
        ret = -ESHUTDOWN;
    } else if (!st_rate_limiter.running) {
        st_rate_limiter.admitted = 0U;
        st_rate_limiter.generation++;
        st_rate_limiter.next_expiry = jiffies + HZ;
        st_rate_limiter.running = true;

        mod_timer(&st_rate_limiter.window_timer,
                  st_rate_limiter.next_expiry);

        started = true;
    }

    spin_unlock_bh(&st_rate_limiter.lock);

    if (ret != 0) {
        pr_warn("syscall_throttle: avvio rate limiter rifiutato: "
                "componente in arresto\n");
    } else if (started) {
        pr_info("syscall_throttle: rate limiter avviato\n");
    }

    return ret;
}

void st_rate_limiter_stop(void)
{
    bool stopped = false;

    /*
     * Rendiamo prima invisibile lo stato running alla callback.
     * Una callback già avviata potrà terminare, ma non riarmare
     * nuovamente il timer dopo aver osservato running == false.
     */
    spin_lock_bh(&st_rate_limiter.lock);

    if (st_rate_limiter.running) {
        st_rate_limiter.running = false;
        st_rate_limiter.admitted = 0U;
        st_rate_limiter.generation++;
        st_rate_limiter.next_expiry = 0U;

        stopped = true;
    }

    spin_unlock_bh(&st_rate_limiter.lock);

    if (stopped)
        wake_up_all(&st_rate_limiter.wait_queue);

    /*
     * Non possiamo chiamare timer_delete_sync() mentre
     * possediamo state.lock: la callback potrebbe essere
     * già in esecuzione e in attesa dello stesso lock.
     */
    timer_delete_sync(&st_rate_limiter.window_timer);

    if (stopped)
        pr_info("syscall_throttle: rate limiter arrestato\n");
}

enum st_rate_limiter_decision
st_rate_limiter_try_acquire(
    u64 *window_generation,
    u64 *throttle_start_ns)
{
    enum st_rate_limiter_decision decision;

    /*
     * La chiamata avviene in process context, mentre il timer
     * modifica lo stesso stato in softirq context.
     */
    spin_lock_bh(&st_rate_limiter.lock);

    if (window_generation != NULL)
        *window_generation = st_rate_limiter.generation;

    /*
     * Durante il teardown non ammettiamo nuovi utilizzatori
     * del componente.
     */
    if (st_rate_limiter.stopping) {
        decision = ST_RATE_LIMITER_SHUTDOWN;

    /*
     * Questa condizione gestisce anche la race:
     *
     * thread osserva monitor attivo
     * DISABLE arresta il rate limiter
     * thread entra qui successivamente
     *
     * In quel caso la richiesta deve attraversare il sistema
     * senza essere bloccata.
     */
    } else if (!st_rate_limiter.running) {
        decision = ST_RATE_LIMITER_BYPASS;

    /*
     * Incrementiamo soltanto le richieste realmente ammesse.
     * Il contatore non supera mai MAX.
     */
    } else if (st_rate_limiter.admitted <
               st_rate_limiter.max_invocations) {
        st_rate_limiter.admitted++;
        decision = ST_RATE_LIMITER_ALLOW;

    } else {
        decision = ST_RATE_LIMITER_THROTTLE;
        if (throttle_start_ns != NULL)
            *throttle_start_ns = ktime_get_ns();
    }

    spin_unlock_bh(&st_rate_limiter.lock);

    return decision;
}

/*
 * La condizione viene valutata sia prima dell'addormentamento
 * sia dopo ogni wake-up.
 *
 * Acquisiamo lo stesso lock usato dagli aggiornamenti per evitare
 * letture incoerenti tra generation, running e stopping.
 */
static bool st_rate_limiter_state_changed(
    u64 observed_generation)
{
    bool changed;

    spin_lock_bh(&st_rate_limiter.lock);

    changed =
        st_rate_limiter.generation != observed_generation ||
        !st_rate_limiter.running ||
        st_rate_limiter.stopping;

    spin_unlock_bh(&st_rate_limiter.lock);

    return changed;
}

int st_rate_limiter_wait_for_change(
    u64 observed_generation)
{
    /*
     * wait_event_interruptible() evita la lost wake-up race:
     *
     * - il chiamante osserva THROTTLE nella generazione G;
     * - il timer apre G+1 prima che il task si addormenti;
     * - la condizione risulta già vera;
     * - il task non dorme sulla vecchia finestra.
     *
     * Un segnale pendente interrompe l'attesa con -ERESTARTSYS.
     */
    return wait_event_interruptible(
        st_rate_limiter.wait_queue,
        st_rate_limiter_state_changed(
            observed_generation));
}

void st_rate_limiter_set_max(u64 max_invocations)
{
    spin_lock_bh(&st_rate_limiter.lock);

    st_rate_limiter.max_invocations = max_invocations;
    st_rate_limiter.admitted = 0U;
    st_rate_limiter.generation++;

    /*
     * Se il monitor è già in funzione, il cambio di MAX
     * avvia una nuova finestra completa di un secondo.
     *
     * mod_timer() modifica la scadenza del timer già armato
     * oppure lo arma nuovamente se necessario.
     */
    if (st_rate_limiter.running &&
        !st_rate_limiter.stopping) {
        st_rate_limiter.next_expiry = jiffies + HZ;

        mod_timer(&st_rate_limiter.window_timer,
                  st_rate_limiter.next_expiry);
    }

    spin_unlock_bh(&st_rate_limiter.lock);

    wake_up_all(&st_rate_limiter.wait_queue);
}

u64 st_rate_limiter_get_max(void)
{
    u64 max_invocations;

    spin_lock_bh(&st_rate_limiter.lock);

    max_invocations = st_rate_limiter.max_invocations;

    spin_unlock_bh(&st_rate_limiter.lock);

    return max_invocations;
}

void st_rate_limiter_exit(void)
{
    /*
     * Impediamo alla callback di riarmare il timer e rendiamo
     * visibile ai futuri waiter che il componente è in teardown.
     */
    spin_lock_bh(&st_rate_limiter.lock);

    st_rate_limiter.running = false;
    st_rate_limiter.stopping = true;
    st_rate_limiter.admitted = 0U;
    st_rate_limiter.generation++;

    spin_unlock_bh(&st_rate_limiter.lock);

    /*
     * I thread bloccati osservano stopping == true e possono
     * terminare il ciclo di attesa prima del teardown dell'hook.
     */
    wake_up_all(&st_rate_limiter.wait_queue);

    /*
     * Arresto definitivo: attende l'eventuale callback in corso
     * e impedisce riarmi successivi.
     */
    timer_shutdown_sync(&st_rate_limiter.window_timer);

    pr_info("syscall_throttle: rate limiter rilasciato\n");
}
