# Syscall Throttling — Linux Kernel Module

Progetto individuale di **Advanced Operating Systems (and System Security)**,
Università di Roma Tor Vergata, A.A. 2025–2026.

Il modulo `syscall_throttle` controlla le ammissioni alle syscall native x86-64.
Un amministratore registra nomi di eseguibili, effective UID e numeri di syscall,
poi configura un budget globale `MAX` per finestre di un secondo.
I thread oltre il limite attendono prima dell'esecuzione della syscall originale.

## Indice

- [Requisiti e comportamento](#requisiti-e-comportamento)
- [Architettura e file](#architettura-e-file)
- [Flusso di una syscall](#flusso-di-una-syscall)
- [Strutture dati](#strutture-dati)
- [Budget, timer e transizioni](#budget-timer-e-transizioni)
- [Statistiche](#statistiche)
- [Concorrenza e ciclo di vita](#concorrenza-e-ciclo-di-vita)
- [Device, permessi e UAPI](#device-permessi-e-uapi)
- [Controller](#controller)
- [Compilazione e demo](#compilazione-e-demo)
- [Verifiche della demo](#verifiche-della-demo)
- [Ambiente verificato e limiti](#ambiente-verificato-e-limiti)

## Requisiti e comportamento

La condizione di selezione è:

```text
monitor ON AND syscall registrata AND (programma registrato OR EUID registrato)
```

Il budget è condiviso tra tutti i programmi, UID e numeri selezionati: non esiste
un budget indipendente per ogni registro o processo. Si limitano le **ammissioni**,
non il numero delle syscall contemporaneamente in esecuzione.

| Requisito della traccia | Realizzazione |
|---|---|
| Registrare/rimuovere programmi, UID e syscall | Registri gestiti tramite ioctl |
| Selezionare programma oppure effective UID | Matching OR con numero di syscall registrato |
| Limitare le invocazioni a MAX per secondo | Budget globale e timer a finestre fisse |
| Sospendere i thread oltre il limite | Wait queue interruptible nel wrapper |
| Gestire syscall bloccanti e non bloccanti | Attesa del limiter separata dalla syscall originale |
| Consultare i registri | Comandi COUNT e LIST |
| Riservare le modifiche a root | Controllo EUID nel driver |
| Attivare/disattivare il monitor | ENABLE e DISABLE |
| Peak delay, programma e UID associati | Snapshot statistico |
| Numero medio e massimo di thread bloccati | Integrale temporale e picco |
| Controller e test user-space | `user/` e `demo/` |

Il nome selezionato è il **basename dell'eseguibile**, ricavato da `mm->exe_file`.
Non è `argv[0]` né il nome modificabile del thread. Per i programmi interpretati
conta l'eseguibile dell'interprete. Nomi uguali in directory diverse corrispondono
allo stesso criterio; il basename non costituisce una barriera di sicurezza.

## Architettura e file

| Percorso | Responsabilità |
|---|---|
| `module/main.c` | Inizializzazione, rollback e scaricamento |
| `module/device.c` | Miscdevice e riconoscimento delle ioctl di controllo |
| `module/device_ioctl.c` | Tabella di dispatch, permessi e transizioni amministrative |
| `module/monitor_state.c` | Stato ON/OFF tramite static key |
| `module/uid_registry.c` | Lista UID, lookup e snapshot |
| `module/program_registry.c` | Lista dei basename, validazione e snapshot |
| `module/program_identity.c` | Identità dell'eseguibile con riferimenti stabili |
| `module/syscall_registry.c` | Bitmap dei numeri x86-64 e snapshot ordinato |
| `module/rate_limiter.c` | Budget, generazione, timer e attese |
| `module/statistics.c` | Sessioni, integrale dei waiter e peak delay |
| `module/syscall_hook.c` | Target, filtri Ftrace, wrapper e casi speciali |
| `module/*.h` | Interfacce interne dei componenti |
| `include/uapi/syscall_throttle.h` | Tipi e comandi condivisi tra kernel e user-space |
| `user/stctl.c` | Parsing, ioctl e visualizzazione |
| `demo/` | Demo automatica C: controller, worker, protocollo e Makefile |

I Makefile si trovano in `module/`, `user/` e `demo/`.
L'eventuale directory `scripts/` è un segnaposto: non contiene un runner da usare.

```mermaid
flowchart TD
    U["stctl"] --> D["Device e dispatcher ioctl"]
    D --> C["Registri e stato del monitor"]
    D --> L["Rate limiter"]
    F["Callback Ftrace"] --> W["Wrapper syscall"]
    C --> W
    W --> L
    L --> Q["Attesa e rivalutazione"]
    Q --> W
    W --> S["Statistiche"]
    W --> O["Syscall originale"]
```

## Flusso di una syscall

L'intercettazione utilizza **Ftrace**, con risoluzione dei simboli tramite Kprobe.
Non modifica direttamente la syscall table e non richiede un modulo esterno per
scoprirne l'indirizzo. La preparazione deriva i target dagli header del kernel,
controlla le definizioni e deduplica gli indirizzi condivisi da più numeri.

1. La callback Ftrace applica le guardie previste e redirige al wrapper.
2. Il wrapper verifica numero e indirizzo originale e gestisce le eccezioni di
   controllo del device e il riferimento speciale di `delete_module`.
3. Se la policy non corrisponde, passa alla syscall originale.
4. Per una chiamata rilevante acquisisce il token della sessione statistica.
5. Nel ciclo acquisisce la generazione del limiter, rivaluta la policy e prova
   ad acquisire budget.
6. `RETRY` ripete la valutazione; `THROTTLE` registra la prima attesa e sospende
   il thread. Un risveglio richiede una nuova verifica: non assegna un posto.
7. `ALLOW`, `BYPASS` o `SHUTDOWN` concludono l'eventuale misura e consentono
   l'esecuzione originale. Un segnale interrompe invece l'attesa del limiter.
8. All'uscita vengono rilasciate le protezioni del wrapper; le syscall senza
   ritorno hanno un percorso dedicato.

I thread risvegliati competono nuovamente per il budget sotto lock. Non è
implementata una politica FIFO né una garanzia di ordine fra i waiter.

## Strutture dati

### Registri e configurazione

| Tipo / contenitore | Contenuto e protezione |
|---|---|
| `st_uid_entry` | `kuid_t uid` e nodo di lista; rwsem del registro |
| `st_program_entry` | Nome fino a 255 byte più NUL e nodo di lista; rwsem |
| `st_syscall_bitmap` | Un bit per numero, dimensionato con `NR_syscalls` |
| Conteggi dei registri | Numero di elementi; aggiornati insieme al relativo registro |
| `st_monitor_enabled_key` | Static key del monitor |
| `st_ioctl_entry` | Comando, handler, privilegio richiesto e serializzazione della policy |

UID e programmi hanno helper `find_locked` comuni ad aggiunta, rimozione e
ricerca. Il chiamante mantiene il lock. Nel registro syscall gli aggiornamenti
usano `set_bit`/`clear_bit`; count e snapshot sono protetti da mutex, mentre il
matching usa `test_bit`. Le liste restituiscono snapshot, non viste permanenti.

### Limiter e statistiche

`st_rate_limiter_state` contiene spinlock, timer, wait queue, `max_invocations`,
`admitted`, `generation`, `next_expiry`, `running` e `stopping`.
`running` indica l'attività; `stopping` distingue lo scaricamento definitivo da OFF.

`st_statistics_state` conserva generazioni di sessione/continuità, flag di attività
e chiusura, timestamp, integrale temporale, waiter correnti e massimi, peak delay,
EUID e programma del peak. Il suo spinlock e la coda di drenaggio sono separati.

`st_statistics_block_context`, sullo stack del wrapper, conserva `generation`,
`start_ns`, EUID, copia del nome e `counted`. Quest'ultimo indica se l'attesa è
stata contabilizzata e deve essere conclusa.

`st_statistics_snapshot` espone durata, integrale, current/peak blocked, peak
delay, EUID, nome e flag di validità/attività. Nel layout corrente x86-64 occupa
304 byte. Non espone contatori cumulativi di chiamate, media dei ritardi
individuali o numero della syscall del peak.

### Hook e controller

| Tipo | Ruolo |
|---|---|
| `st_syscall_definition` | Numero, simbolo e proprietà nonreturning della definizione |
| `st_hook_target` | Simbolo e indirizzo originale per numero, proprietà e provenienza |
| `st_hook_filter` | Indirizzo univoco e stato di installazione del filtro |
| `st_nonreturning_call` | Record del task per completare il lifetime di exit/exit_group |
| `registry_info` | Metadati per consultazione e stampa dei registri nella CLI |
| `command` | Tabella dei comandi della CLI |
| `worker` (demo) | PID, descriptor e risultato dei processi di prova |
| `demo_result` (demo) | Messaggi READY/RESULT, esito e tempi della syscall |

`st_hook_active_calls`, la sua wait queue e i riferimenti al modulo proteggono
l'esecuzione durante lo scaricamento. I tre contatori diagnostici nonreturning
sono tuttora usati dal relativo test e non fanno parte dello snapshot UAPI.

Le altre strutture UAPI trasportano stato, UID, nomi, numeri, conteggi, MAX e
richieste LIST con indirizzo user-space, capacità e conteggio restituito.
Si usano tipi a larghezza definita e indirizzi `__aligned_u64`; il kernel
inizializza le risposte prima di copiarle in user-space.

## Budget, timer e transizioni

La finestra è **fissa**, avviata da ENABLE e rinnovata tramite timer e `HZ`.
Non è un intervallo mobile né è allineata al calendario. La callback può subire
ritardi: non esiste una garanzia hard real-time o di deriva numerica prefissata.

| Operazione | Effetto sul budget | Effetto sulle statistiche |
|---|---|---|
| ENABLE da OFF | Nuova finestra, consumo zero | Nuova sessione |
| ENABLE già ON | Nessuna modifica | Nessuna modifica |
| MAX_SET identico | Consumo e scadenza invariati | Nessuna modifica |
| MAX_SET diverso, ON | Nuovo limite; consumo e scadenza conservati | Nuova sessione con waiter trasferiti |
| MAX_SET da OFF | Configura il prossimo avvio | Conserva lo snapshot |
| DISABLE | Ferma il limiter e risveglia i waiter | Drena le attese contabilizzate e congela |

Con tre posti già consumati, passare da MAX=3 a MAX=5 offre altri due posti.
Abbassare MAX sotto il consumo non revoca le ammissioni precedenti: impedisce
nuove ammissioni finché il budget torna disponibile.

**MAX=0 è valido:** sospende le chiamate rilevanti a monitor ON. Non equivale a OFF.
Una syscall originale già ammessa e bloccata non impedisce il rinnovo del budget.

## Statistiche

La media richiesta è una media temporale:

```text
media thread bloccati = blocked_thread_time_ns / observation_ns
blocked_thread_time_ns = integrale nel tempo di current_blocked
```

Il picco dei thread bloccati è il massimo del conteggio corrente. Il peak delay
misura il ritardo dal primo THROTTLE fino alla conclusione dell'attesa, prima
della syscall originale: non include il tempo passato dentro quella syscall.
Programma ed EUID appartengono al campione selezionato per il peak.

Dopo un cambio effettivo di MAX a monitor ON, N waiter trasferiti danno
`current_blocked=N`, `peak_blocked=N`, integrale iniziale zero e peak delay non
ancora disponibile. Per quei waiter il ritardo è limitato all'inizio della nuova
sessione; non descrive necessariamente tutta l'attesa attraverso più sessioni.

Le attese interrotte contribuiscono al tempo integrato, ma non al peak delay.
`peak_valid` distingue un campione valido da nessun campione. Una chiamata ammessa
senza attesa può lasciare il peak non disponibile. Non esiste reset manuale.

## Concorrenza e ciclo di vita

- Il mutex di policy serializza le transizioni amministrative; i registri hanno
  lock propri. Le modifiche notificano il limiter per rivalutare i waiter.
- La generazione del limiter collega matching, acquisizione e attesa, evitando
  di addormentarsi su uno stato già superato.
- I timestamp contabili sono acquisiti sotto il lock delle statistiche.
  Quando entrambi servono, l'ordine è **limiter → statistiche**.
- L'integrale è aggiornato prima di modificare il numero dei thread bloccati.
  Risvegli e attese non vengono eseguiti mantenendo il lock delle statistiche.
- `timer_delete_sync` consente lo stop riavviabile; `timer_shutdown_sync`
  gestisce la chiusura definitiva. Non sono intercambiabili.

All'inizializzazione vengono preparati registri, limiter e statistiche, poi hook
e device. I percorsi di errore rilasciano i componenti già inizializzati.
Allo scaricamento si rimuove il device, si arresta definitivamente il limiter,
si smonta l'hook attendendo le chiamate attive, quindi si rilasciano statistiche,
registri e stato del monitor.

DISABLE attende la contabilizzazione delle attese rilasciate, **non** il ritorno
delle syscall originali. Al contrario lo scaricamento può attendere una syscall
originale ancora attiva, per esempio una read su pipe vuota.

Le attese usano `wait_event_interruptible`. Il percorso restituisce il codice
interno di riavvio previsto; il risultato user-space dipende da syscall, segnale
e `SA_RESTART`. Sono verificati SIGUSR1, SIGKILL e riavvio della read.

`exit` e `exit_group` usano record associati al task e il tracepoint
`sched_process_exit` per rilasciare la protezione quando il wrapper non ritorna.

`delete_module` acquisisce un riferimento al monitor anche fuori dal matching.
L'autorimozione ordinaria intercettata viene normalmente rifiutata con
EWOULDBLOCK; se il riferimento non può essere acquisito il wrapper restituisce
EBUSY. `O_TRUNC` è rifiutato con EPERM nel percorso intercettato, anche fuori
policy: è una restrizione esplicita rispetto alla syscall originale.

## Device, permessi e UAPI

Il miscdevice è `/dev/syscall_throttle`, con modo 0666. Le consultazioni sono
pubbliche; gli aggiornamenti richiedono EUID uguale a `GLOBAL_ROOT_UID` nel
namespace iniziale. Essere root soltanto in un user namespace non è sufficiente.

Le ioctl riconosciute dirette al vero oggetto file del device bypassano il
limiter, mantenendo controlli LSM e permessi del dispatcher. Un riferimento al
file preserva l'oggetto anche con close/dup2 concorrenti. Comandi sconosciuti o
comandi noti su altri file non ricevono questa eccezione.

Non esiste esenzione generale per root o per `stctl`. Il bypass non copre avvio
del programma, librerie o apertura del device: una configurazione può bloccare
una syscall precedente al comando amministrativo.

L'UAPI corrente contiene **18 comandi**. Le LIST comunicano la dimensione
necessaria con ENOSPC e non consegnano snapshot parziali per capacità insufficiente.
La CLI limita i retry in caso di crescita concorrente. I percorsi per liste
inizialmente vuote differiscono ancora tra registri; non si presume un unico
istante di osservazione fra COUNT e LIST.

I numeri ioctl 0x00, 0x50 e 0x51 sono ritirati. STATS_GET usa 0x52; il codice
completo include la dimensione della struttura. Ricompilare modulo, controller
e test dalla stessa versione dopo modifiche dell'UAPI.

## Controller

I comandi si eseguono come `./user/stctl <comando>`.

| Comando CLI | Ioctl (`ST_IOCTL_` + suffisso) | Root |
|---|---|---|
| `status` | GET_STATUS | No |
| `enable` / `disable` | ENABLE / DISABLE | Sì |
| `max-set <valore>` / `max-get` | MAX_SET / MAX_GET | Solo SET |
| `uid-add <UID>` / `uid-remove <UID>` | UID_ADD / UID_REMOVE | Sì |
| `uid-count` / `uid-list` | UID_GET_COUNT / UID_LIST | No |
| `program-add <nome>` / `program-remove <nome>` | PROGRAM_ADD / PROGRAM_REMOVE | Sì |
| `program-count` / `program-list` | PROGRAM_GET_COUNT / PROGRAM_LIST | No |
| `syscall-add <numero>` / `syscall-remove <numero>` | SYSCALL_ADD / SYSCALL_REMOVE | Sì |
| `syscall-count` / `syscall-list` | SYSCALL_GET_COUNT / SYSCALL_LIST | No |
| `stats` | STATS_GET | No |

Nomi: basename non vuoto, senza slash e lungo al massimo 255 byte. Numeri syscall:
intervallo derivato da `NR_syscalls` degli header di compilazione. L'aggiunta di un
elemento presente restituisce EEXIST; la rimozione di un elemento assente ENOENT.
Validazioni e privilegi sono controllati anche dal driver, indipendentemente
dalla CLI. Non sono disponibili comandi `ping`, `stats-reset` o selezione FIFO.

## Compilazione e demo

Richiesti Linux x86-64 nativo, compilatore C, make, header del kernel in esecuzione,
`sudo` e `/proc`. Le condizioni del kernel per Ftrace rimangono quelle descritte
nelle sezioni precedenti. Eseguire in una VM dedicata, senza altre configurazioni
concorrenti del monitor. I casi EUID selezionano anche eventuali processi dello
stesso utente: evitare altri carichi durante la prova. Nei casi con UID registrato
la demo verifica l'attesa del singolo worker tramite `/proc/<pid>/wchan`,
ricercando `st_rate_limiter_wait_for_change`; non pretende un conteggio globale
pari a uno. Se il simbolo non e' osservabile, la prova e' inconclusiva.
I controlli numerici esatti rimangono nei casi isolati per nome di programma.

Da utente normale, nella radice del repository:

```bash
make -C module
make -C user
make -C demo
sudo insmod module/syscall_throttle.ko
sudo timeout --kill-after=10s 120s ./demo/soa_demo
sudo rmmod syscall_throttle
```

Eseguire `rmmod` dopo l'esito positivo; in caso di errore leggere prima il log.
Il modulo deve inizialmente essere scaricato per usare la sequenza completa.
Se è già caricato, la demo può essere invocata direttamente solo con monitor
OFF, MAX=0 e registri vuoti: verifica queste precondizioni e rifiuta una
configurazione preesistente senza modificarla. Non lanciare la demo in parallelo.

La demo usa direttamente le ioctl pubbliche. `user/stctl` resta il controller
interattivo del progetto. `sudo` deve essere invocato da un utente non-root:
la demo ricava da `SUDO_UID` l'identità per verificare letture pubbliche, scritture
vietate e processi non privilegiati. I file devono essere accessibili a tale
utente. I worker vengono cercati nella directory di `soa_demo`.

Quattro file sorgente/build, tre eseguibili generati:

| File | Responsabilità |
|---|---|
| `demo/demo.c` | Configurazione, processi, verifiche e cleanup |
| `demo/worker.c` | Esegue la syscall richiesta dopo il segnale di avvio |
| `demo/protocol.h` | Formato dei messaggi tra controller e worker |
| `demo/Makefile` | Compila `soa_demo`, `demo_worker` e `demo_alt` |

I due worker sono compilati dallo stesso sorgente con nomi diversi, per provare
la selezione per basename senza duplicare codice. I worker non vanno lanciati
a mano: ricevono descriptor e parametri dal controller. Il gate usa `recv`,
così non viene confuso con la `read` scelta come syscall bloccante.

## Verifiche della demo

| Fase | Cosa verifica |
|---|---|
| 1. Registri e privilegi | ADD/REMOVE, LIST, duplicati, elementi assenti, capacità insufficiente; letture consentite e modifiche negate al non-root |
| 2. Selezione e OFF | Solo EUID, solo programma, OR con entrambi, deregistrazione, identità non selezionata, syscall non registrata, bypass OFF |
| 3. Budget globale | MAX=2 condiviso tra otto processi: due nomi, due EUID e due syscall; sei waiter, rinnovo automatico e MAX invariato |
| 4. Statistiche | Quattro waiter con MAX=0, integrale temporale esatto in un intervallo stabile, picco, identità del peak, rilascio e congelamento OFF |
| 5. Syscall bloccante | `read` sospesa dal limiter e poi sulla pipe; consumo e rinnovo del budget per `getpid`; DISABLE termina senza aspettare la read originale |

La fase 3 seleziona soltanto i due nomi, lasciando il registro UID vuoto, per
isolare il conteggio da processi estranei. La fase 2 verifica separatamente
la selezione per EUID. I worker sono pronti prima di ENABLE e ricevono il via
dal controller. La prima finestra viene campionata entro 750 ms da prima di
ENABLE: la prova richiede almeno 200 ms di osservazione stabile e un campione
oltre 500 ms. Una VM troppo lenta può produrre un esito inconclusivo.

Nella fase 4, con quattro thread sospesi per tutto l'intervallo, la differenza
dell'integrale deve essere esattamente quattro volte la differenza del tempo
di osservazione. La media mostrata è integrale diviso durata della sessione.
La fase 5 legge `/proc/<pid>/wchan` per distinguere l'attesa della vera `read`
da quella del limiter; simboli non visibili possono rendere la prova inconclusiva.

Esiti del programma:

- `0`: tutte e cinque le fasi superate; monitor OFF, MAX=0 e registri vuoti.
- `1`: verifica fallita o interruzione; viene tentato il cleanup.
- `2`: avvio non valido, utente o device non disponibili.
- `3`: osservazione inconclusiva per tempi o visibilità di `/proc`; ripetere
  nella VM senza carico dopo aver controllato lo stato del modulo.

Il wrapper `timeout` può restituire altri codici, per esempio 124 alla scadenza.
Il cleanup disattiva il monitor, termina e raccoglie i worker residui, rimuove
le sole registrazioni create dalla demo e imposta MAX=0. Non scarica il modulo.
Un arresto forzato o un blocco del kernel può impedire il cleanup: un timeout
non equivale a una prova superata.

La precedente cartella `tests/` è rimossa e resta recuperabile dalla cronologia
Git. Questa demo mantiene prove automatiche sui comportamenti centrali della
traccia, ma **non replica tutta la precedente copertura**. In particolare non
verifica sistematicamente segnali/SA_RESTART, exit/exit_group, scaricamento con
waiter o syscall attive, autorimozione/delete_module, stress concorrente dei
registri, variazioni di MAX durante le attese e parsing del controller `stctl`.
Due syscall non bloccanti e una bloccante sono esempi rappresentativi, non una
verifica di ogni syscall supportata o di tutti gli interleaving.

## Ambiente verificato e limiti

Le esecuzioni più recenti riportate dall'autore sono su VM Ubuntu x86-64,
kernel `7.0.0-38-generic`; il test statistiche rileva quattro CPU disponibili.
Sono riportati 20 esiti positivi tra due esecuzioni più stress da 600 operazioni
per la rimozione dei contatori diagnostici. La prima prova sul budget globale
è risultata inconclusiva; la ripetizione è passata. Le modifiche successive a
controllo device e lookup hanno superato le rispettive regressioni mirate.
Questi sono esiti storici della suite rimossa, non esiti della nuova demo C.
La nuova demo deve essere validata nella VM con il modulo caricato.

La copertura è dei casi eseguiti, non di ogni syscall, kernel o interleaving.
Il percorso supportato è nativo x86-64; compatibilità IA32/x32 e altri kernel
non sono attestati. Il monitor è globale, non isolato per container.

Restano da approfondire i percorsi d'errore nel teardown Ftrace/tracepoint sul
kernel esatto di riferimento. Gli errori sono registrati, ma gli esiti ordinari
di unload non dimostrano che tutti i percorsi eccezionali siano sicuri.

Le scelte da rendere esplicite nella discussione del progetto sono finestre
fisse, sessioni statistiche al cambio di MAX, identità da basename, eccezione
per le ioctl amministrative e restrizione dello scaricamento forzato.
