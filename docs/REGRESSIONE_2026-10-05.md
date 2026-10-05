# Regressione del 5 ottobre 2026

## Ambiente e versioni

Esecuzioni effettuate dall'autore sulla VM Ubuntu, kernel
7.0.0-28-generic, architettura x86-64, con due CPU disponibili ai test.

- Base della regressione: `230187c31b0b099ef15ef122057094156f1ca514`.
- Versione finale: `94e664f17650cd914adc4a46a79dbd0f0b7528a8`.
- La differenza tra questi commit riguarda esclusivamente
  `tests/control_ioctl_regression.c`.
- Il codice del modulo non è stato modificato durante la regressione.

## Risultati

Tutte le 22 prove della sequenza selezionata hanno ottenuto un esito
positivo. Non si intende con questo l'esecuzione di ogni test storico
presente nel repository.

| Gruppo | Prove | Esito |
|---|---:|---|
| MAX invariato, MAX aumentato/ridotto, sessioni MAX, statistiche concorrenti, reset, syscall bloccante, DISABLE, DISABLE con read bloccante | 8 | PASS |
| Segnali, SA_RESTART nel limiter, SA_RESTART nella syscall reale, deregistrazione syscall, concorrenza registri, selezione OR programma/EUID, budget globale | 7 | PASS |
| Ioctl amministrative, permessi, descriptor duplicato e casi non esentati | 1 | PASS |
| Transizioni amministrative concorrenti con waiter iniziali | 1 | PASS |
| exit/exit_group, unload con waiter, unload con syscall attiva, delete_module e relativi casi limite | 5 | PASS |

Lo stress amministrativo ha eseguito 600 operazioni da sei thread,
partendo da otto worker sospesi: zero errori, tutti i worker terminati,
registri conservati e zero waiter residui.

Il test del budget globale ha verificato tre ammissioni complessive e
tredici waiter per sedici worker distribuiti su due programmi, due EUID
e due syscall. Tutti sono poi terminati senza ulteriori cambi di MAX.
Il controllo temporale riguarda la prima parte della prima finestra;
non misura separatamente ogni finestra successiva.

## Tentativi e correzioni conservati

- La prima esecuzione di `max_change_regression.py` è risultata
  inconcludente perché la preparazione ha superato il margine di 0,40 s.
  Una ripetizione sullo stesso codice è passata, senza ampliare il margine.
- Il test ioctl conservava un'aspettativa cumulativa incompatibile con
  le nuove sessioni al cambio di MAX. Ora verifica ogni completamento
  prima del successivo cambio e poi verifica la nuova sessione vuota.
- Un primo tentativo di applicare la patch ioctl si è fermato per una
  differenza di formattazione del sorgente, senza modificare file.
  La patch adattata al sorgente effettivo è stata compilata e provata.

## Log kernel e stato finale

La ricerca automatica nel log kernel finale non ha trovato corrispondenze
ai pattern selezionati per BUG, WARNING, Oops, panic, sanitizer,
deadlock e stall. Questa ricerca non dimostra l'assenza di ogni anomalia
e non sostituisce un'analisi completa del log.

Nelle ultime diagnostiche riportate risultano rimossi 369/369 filtri
Ftrace. Nel test exit/exit_group la diagnostica riportava 21 operazioni
nonreturning avviate e completate, senza ritorni inattesi.

Stato finale verificato:

- `syscall_throttle`: scaricato;
- `st_delete_fixture`: scaricato.

I log sono stati copiati fuori da `/tmp`, nella directory locale:

`/home/vboxuser/soa-test-results/regressione-2026-10-05-miKquz`

Questa directory è esterna al repository: un push Git non ne include
automaticamente il contenuto.

## Limiti e attività residue

Queste prove costituiscono evidenza sui casi eseguiti, non una
dimostrazione di correttezza per tutte le syscall o configurazioni kernel.

Prima della consegna restano:

1. Confronto finale tra requisiti della traccia, implementazione e prove,
   includendo registrazione, enumerazione, permessi e identità eseguibile.
2. Esplicitazione delle interpretazioni adottate: finestre fisse,
   sessioni statistiche al cambio di MAX, ritardi limitati alla sessione,
   eccezione ioctl amministrative e restrizioni di delete_module.
3. Consolidamento delle istruzioni riproducibili e preparazione della
   relazione e della dimostrazione.

## Verifiche C aggiuntive

Dopo la regressione sono state eseguite tre ulteriori prove in C,
con sorgenti, binari e log conservati fuori dal repository.
Non sono state necessarie modifiche al modulo.

### Registri e permessi — PASS

Verificati sui tre registri:

- consultazione pubblica di liste vuote e popolate;
- inserimento e rimozione da root;
- rifiuto con EPERM dei sei comandi ADD/REMOVE da non-root;
- duplicati, elementi assenti e capacità insufficiente;
- richieste non valide e conservazione dei dati nei casi provati.

Risultati locali:
`/home/vboxuser/soa-test-results/registry-check-6cquBj`

### Liste durante modifiche concorrenti — PASS

Eseguite 1200 modifiche da un writer con tre lettori concorrenti.
Ottenuti 3320 snapshot riusciti e 1186 risposte ENOSPC verificate,
includendo i controlli preliminari.

Verificati contenuti ammessi, assenza di duplicati, rispetto della
capacità e mancata scrittura oltre gli elementi restituiti.
Non si richiede che COUNT e LIST separati rappresentino lo stesso istante.

Risultati locali:
`/home/vboxuser/soa-test-results/registry-concurrent-yPAStb`

### Identificazione dell'eseguibile — PASS

- Basename lungo registrato: throttling applicato e identità corretta
  nelle statistiche, anche con argv[0] e nome del thread differenti.
- Basename non registrato: falsificare argv[0] e nome del thread
  non causa una falsa selezione.
- Basename diversi con prefisso condiviso: nessuna falsa corrispondenza.

Il registro UID è rimasto vuoto durante la prova.
Non sono stati verificati rinomina concorrente e script interpretati.

Risultati locali:
`/home/vboxuser/soa-test-results/identity-check-G1hbxW`

### Stato conclusivo e consegna

Le tre prove aggiuntive sono terminate con monitor OFF e registri vuoti.
L'ultima prova ha lasciato MAX=0 e il modulo caricato.

La sequenza comprende quindi 22 prove di regressione e tre prove C
aggiuntive. Gli esiti valgono per i casi eseguiti e non dimostrano
correttezza universale o portabilità verso altri kernel.

Per la consegna è prevista una demo con controlli PASS/FAIL scritta in C.
I test Python di sviluppo saranno archiviati fuori dal repository
e rimossi dall'albero finale, aggiornando i riferimenti nella guida.
La loro presenza nei commit storici non viene modificata.
