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
 * Chiude e congela la sessione corrente.
 *
 * Il tempo dei waiter viene contabilizzato fino all'istante di
 * chiusura. Gli eventi successivi dei waiter rilasciati non
 * modificano più lo snapshot conservato.
 */
void st_statistics_session_stop(void);

/*
 * Azzera la sessione corrente.
 *
 * Se esistono chiamate contabilizzate come bloccate, il reset
 * viene rifiutato per non invalidare i relativi contesti locali.
 *
 * Restituisce:
 *
 *   0       reset completato;
 *   -EBUSY  esistono thread bloccati contabilizzati.
 */
int st_statistics_reset(void);

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
 *
 * Restituisce true se la chiamata è stata associata alla
 * sessione statistica corrente.
 */
bool st_statistics_block_begin(
    struct st_statistics_block_context *context,
    u64 invocation_generation,
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
