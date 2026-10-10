#ifndef ST_STATISTICS_H
#define ST_STATISTICS_H

#include <linux/types.h>
#include <linux/uidgid.h>

#include <syscall_throttle.h>

/*
 * Contesto locale di una singola invocazione bloccata.
 *
 * La struttura vive sullo stack del wrapper e conserva:
 *
 * - la generazione statistica di appartenenza;
 * - il timestamp del primo THROTTLE;
 * - l'identità associata all'invocazione;
 * - l'informazione che current_blocked è stato incrementato.
 */
struct st_statistics_block_context {
    u64 generation;
    u64 start_ns;

    kuid_t effective_uid;
    unsigned int syscall_nr;

    char program_name[ST_PROGRAM_NAME_CAPACITY];

    bool counted;
};

void st_statistics_init(void);
void st_statistics_exit(void);

/*
 * Avvia una nuova sessione statistica.
 *
 * Il chiamante deve invocarla soltanto durante una reale
 * transizione del monitor da disattivato ad attivo.
 */
void st_statistics_session_start(void);

/*
 * Chiamare sotto il lock del limiter per un effettivo cambio di MAX.
 * Apre una nuova osservazione trasferendo i waiter; a monitor OFF
 * conserva lo snapshot. Non attende e non acquisisce lock del limiter.
 */
void st_statistics_max_changed(void);

/*
 * Chiude e congela la sessione corrente.
 *
 * Chiamare dopo l'arresto del limiter e il risveglio dei waiter.
 * Attende la fine delle attese contabilizzate, includendone i ritardi,
 * poi congela lo snapshot. Non attende le syscall originali.
 * Il chiamante deve serializzare ENABLE con la chiusura.
 */
void st_statistics_session_stop(void);

/*
 * Produce uno snapshot completamente inizializzato.
 */
void st_statistics_get_snapshot(
    struct st_statistics_snapshot *snapshot);

/*
 * Registra una invocazione risultata rilevante per la policy.
 *
 * Restituisce il token della generazione nella quale
 * relevant_invocations è stato incrementato.
 *
 * Il valore zero indica che nessuna sessione statistica era
 * attiva al momento della registrazione.
 */
u64 st_statistics_record_relevant_invocation(void);

/*
 * Registra il primo ingresso in THROTTLE di una invocazione.
 * throttle_start_ns proviene dalla prima decisione del limiter;
 * il tempo contabile viene acquisito separatamente sotto lock.
 *
 * Restituisce true se la chiamata è stata associata alla
 * sessione statistica corrente.
 */
bool st_statistics_block_begin(
    struct st_statistics_block_context *context,
    u64 invocation_generation,
    u64 throttle_start_ns,
    unsigned int syscall_nr,
    kuid_t effective_uid,
    const char *program_name);

/*
 * Termina una precedente attesa perché la syscall sta per
 * essere realmente eseguita.
 */
void st_statistics_block_complete(
    struct st_statistics_block_context *context);

/*
 * Termina una precedente attesa interrotta da un segnale.
 *
 * In questo caso il peak delay non viene aggiornato, perché la
 * syscall originale non viene eseguita.
 */
void st_statistics_block_interrupted(
    struct st_statistics_block_context *context);

#endif
