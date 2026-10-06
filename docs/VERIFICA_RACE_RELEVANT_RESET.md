# Correzione della race relevant/reset

Verifica eseguita il 6 ottobre 2026 su Linux x86-64
7.0.0-28-generic, nella VM di sviluppo.

## Difetto

Una chiamata poteva registrarsi come rilevante prima di STATS_RESET
e tentare di registrarsi come bloccata dopo il reset.

Il reset riusciva se current_blocked era ancora zero, invalidando
il token statistico della chiamata. Il successivo block_begin
rifiutava quel token, mentre il wrapper poteva comunque attendere
nel limiter: l'attesa non veniva contabilizzata.

## Correzione

Un contatore interno, protetto dal lock delle statistiche, tiene
traccia delle invocazioni tra registrazione della rilevanza e
conclusione della fase di decisione.

STATS_RESET restituisce EBUSY anche quando esistono invocazioni
in questa fase.

In caso di THROTTLE, il wrapper registra prima il contesto bloccato,
poi rilascia la protezione intermedia. In caso di ammissione,
rilascia la protezione prima di eseguire la syscall originale.
Il percorso comune di uscita gestisce inoltre il rilascio residuo.

Il token locale viene azzerato al rilascio per evitare decrementi
duplicati. Il contatore intermedio non viene azzerato dai cambi
di sessione, perché può essere ancora posseduto da wrapper attivi.

Non vengono mantenuti spinlock durante attese o syscall originali.
Non cambiano numeri ioctl, layout UAPI, budget o politica delle
sessioni statistiche al cambio di MAX.

## Verifiche eseguite

1. Modulo di prova con copia del vero statistics.c:
   sequenza record_relevant, reset, block_begin.
   Prima della patch: difetto riprodotto.
   Dopo la patch: reset EBUSY e waiter contabilizzato.
   Dopo il completamento: reset nuovamente disponibile.

2. Modulo completo, selezione per nome del programma e SYS_getpid:
   - ammissione senza attesa e successivo reset;
   - attesa con MAX=0, reset EBUSY, SIGUSR1 e successivo reset;
   - attesa con MAX=0, cambio MAX a 1, completamento e reset.

Tutti i casi indicati sono stati superati.

Il primo test riproduce deterministicamente l'ordine problematico
delle operazioni del sottosistema statistico; non è uno stress
concorrente dell'intero hook. Il secondo verifica l'integrazione
nei tre percorsi descritti. Questi risultati non costituiscono
una prova di assenza di ogni possibile race del modulo.

## Evidenze esterne al repository

Sotto /home/vboxuser/soa-test-results:

- stats-race-FAifJy: riproduzione precedente alla patch;
- stats-race-fixed-VottOa: verifica del sottosistema corretto;
- reset-paths-OdSgBN: verifica dei tre percorsi dell'hook.

Le cartelle contengono sorgenti C, informazioni sull'ambiente
e log. I binari e i test temporanei non fanno parte del commit.
