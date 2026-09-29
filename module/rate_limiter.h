#ifndef ST_RATE_LIMITER_H
#define ST_RATE_LIMITER_H

#include <linux/types.h>

enum st_rate_limiter_decision {
    ST_RATE_LIMITER_BYPASS = 0,
    ST_RATE_LIMITER_ALLOW,
    ST_RATE_LIMITER_THROTTLE,
    ST_RATE_LIMITER_SHUTDOWN,
    ST_RATE_LIMITER_RETRY,
};

void st_rate_limiter_init(void);
void st_rate_limiter_exit(void);
/*
 * Avvia una nuova sequenza di finestre globali.
 *
 * Se il rate limiter è già attivo, l'operazione è idempotente
 * e non azzera la finestra corrente.
 */
int st_rate_limiter_start(void);

/*
 * Arresta il timer, azzera la finestra corrente e risveglia
 * gli eventuali thread in attesa.
 *
 * Il componente può essere nuovamente avviato in seguito.
 */
void st_rate_limiter_stop(void);

/* Leggere la generazione PRIMA di rivalutare la policy. */
u64 st_rate_limiter_get_generation(void);
/* Notifica i registry senza azzerare budget o cambiare scadenza. */
void st_rate_limiter_policy_changed(void);

/*
 * Prova a consumare una unità del budget della finestra corrente.
 *
 * Una expected_generation obsoleta produce RETRY senza consumo di budget.
 *
 * window_generation viene valorizzata con la generazione osservata.
 * Questo valore servirà successivamente ai thread in attesa per
 * riconoscere l'apertura di una nuova finestra.
 *
 * throttle_start_ns, se non NULL, riceve il timestamp della
 * decisione THROTTLE sotto il lock del limiter. Negli altri
 * casi non viene scritto e il chiamante non deve leggerlo.
 */
enum st_rate_limiter_decision
st_rate_limiter_try_acquire(
    u64 expected_generation,
    u64 *window_generation,
    u64 *throttle_start_ns);

/*
 * Attende in modo interrompibile che lo stato osservato dal
 * chiamante non sia più valido.
 *
 * L'attesa termina quando:
 *
 * - si apre una nuova finestra;
 * - viene modificato MAX;
 * - cambia un registro della policy;
 * - il rate limiter viene arrestato da DISABLE;
 * - il componente entra in teardown.
 *
 * Restituisce:
 *   0             stato modificato, ripetere try_acquire()
 *   -ERESTARTSYS  attesa interrotta da un segnale
 */
int st_rate_limiter_wait_for_change(
    u64 observed_generation);

/*
 * Imposta il numero massimo di invocazioni ammesse
 * in una finestra globale di un secondo.
 *
 * Conserva le ammissioni gia' effettuate e la scadenza corrente.
 * Se il valore cambia, aggiorna la generazione e risveglia i waiter
 * per rivalutare il budget. Lo stesso valore non modifica lo stato.
 */
void st_rate_limiter_set_max(u64 max_invocations);

/*
 * Restituisce il limite globale corrente.
 */
u64 st_rate_limiter_get_max(void);

#endif
