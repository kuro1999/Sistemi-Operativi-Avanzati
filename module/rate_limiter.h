#ifndef ST_RATE_LIMITER_H
#define ST_RATE_LIMITER_H

#include <linux/types.h>

enum st_rate_limiter_decision {
    ST_RATE_LIMITER_BYPASS = 0,
    ST_RATE_LIMITER_ALLOW,
    ST_RATE_LIMITER_THROTTLE,
    ST_RATE_LIMITER_SHUTDOWN,
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

/*
 * Prova a consumare una unità del budget della finestra corrente.
 *
 * window_generation viene valorizzata con la generazione osservata.
 * Questo valore servirà successivamente ai thread in attesa per
 * riconoscere l'apertura di una nuova finestra.
 */
enum st_rate_limiter_decision
st_rate_limiter_try_acquire(u64 *window_generation);
/*
 * Imposta il numero massimo di invocazioni ammesse
 * in una finestra globale di un secondo.
 *
 * La modifica apre una nuova generazione logica e
 * azzera il numero di invocazioni già ammesse.
 */
void st_rate_limiter_set_max(u64 max_invocations);

/*
 * Restituisce il limite globale corrente.
 */
u64 st_rate_limiter_get_max(void);

#endif
