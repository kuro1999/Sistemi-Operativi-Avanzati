# Comportamento e test correnti

Aggiornamento: 2 ottobre 2026. La guida descrive le modifiche verificate sulla
VM dell'autore. Non attesta una copertura esaustiva della traccia.

## Selezione e permessi

La condizione di throttling è:

`monitor ON AND syscall registrata AND (programma registrato OR EUID registrato)`.

Il nome è il basename dell'eseguibile ottenuto da `mm->exe_file`, non `argv[0]`
o il nome del thread. Per programmi interpretati va considerato l'eseguibile
interpreter effettivamente associato al processo. Il basename non identifica
univocamente un file e non costituisce una barriera di sicurezza.

Programma ed EUID sono criteri alternativi: rimuoverne uno non libera il thread
se l'altro continua a corrispondere. Gli aggiornamenti dei registri notificano
i waiter, che rivalutano la policy.

Il device ha modo 0666. Le letture sono pubbliche; modifiche, ON/OFF, MAX_SET
e reset richiedono EUID uguale a GLOBAL_ROOT_UID, nel namespace iniziale.
Essere root soltanto in un user namespace non soddisfa tale controllo.

## Budget e finestre

MAX limita le ammissioni nella finestra corrente, con un unico budget condiviso
tra programmi, EUID e syscall selezionati. Non limita il numero di syscall
contemporaneamente in esecuzione: una syscall bloccante già ammessa non trattiene
il budget delle finestre successive.

La finestra è fissa, avviata da ENABLE, con rinnovo tramite timer e jiffies/HZ.
Non è mobile né allineata ai secondi del calendario. La callback può subire
ritardi: non si tratta di una garanzia hard real-time.

Intorno a un confine possono passare chiamate appartenenti a due finestre:
non si promette MAX in ogni possibile intervallo mobile di un secondo.

| Operazione | Budget e scadenza | Statistiche |
|---|---|---|
| ENABLE da OFF | Nuova finestra, consumo zero | Nuova sessione |
| ENABLE già ON | Invariati | Invariate |
| MAX_SET allo stesso valore | Invariati | Invariate |
| MAX_SET diverso, ON | Consumo e scadenza conservati | Nuova sessione con waiter trasferiti |
| MAX_SET da OFF | Configura il prossimo avvio | Snapshot congelato conservato |
| DISABLE | Ferma limiter e risveglia i waiter | Raccoglie le attese contabilizzate, poi congela |
| STATS_RESET riuscito | Invariati | Azzera l'osservazione |

Con 3 ammissioni già consumate, aumentare MAX a 5 consente altre 2 ammissioni.
Ridurre MAX a 2 non revoca le precedenti e impedisce nuove ammissioni finché
il budget non si rinnova o il limite non viene nuovamente aumentato.

MAX=0 blocca le nuove chiamate rilevanti: non equivale a DISABLE.

## Sessioni e misure statistiche

Ogni cambio effettivo di MAX a monitor ON azzera lo storico statistico.
Se rimangono N waiter contabilizzati, la nuova osservazione parte con:

- N invocazioni rilevanti e N bloccate;
- N thread attualmente bloccati e picco iniziale N;
- zero completamenti, interruzioni, tempo integrato e somma dei ritardi;
- peak delay non disponibile fino a un completamento.

Le attese trasferite misurano soltanto il tempo successivo all'inizio della
nuova sessione. Il ritardo visualizzato può essere inferiore all'attesa totale
attraverso più MAX. I dati precedenti non vengono conservati in uno storico.

Una chiamata rilevante prima del cambio, ma contabilizzata come bloccata dopo,
viene inclusa nella nuova osservazione.

Il ritardo decorre dal primo THROTTLE, limitato all'inizio della sessione
corrente per i waiter trasferiti, e termina prima dell'esecuzione originale.
Non comprende il tempo trascorso dentro la syscall, per esempio una read
sospesa su una pipe vuota.

Le attese interrotte contribuiscono al numero delle interruzioni e al tempo
di blocco, ma non al peak o alla media dei ritardi delle attese completate.
I campioni del peak provengono dalle attese concluse, non da quelle pendenti.

La media temporale dei thread bloccati è:

`integrale del numero di thread bloccati / durata dell'osservazione`.

È distinta dalla media dei ritardi individuali. Il peak riporta anche
numero di syscall, programma ed EUID associati.

DISABLE attende la contabilizzazione dei waiter rilasciati, senza attendere
il completamento delle syscall originali. Lo snapshot successivo è stabile.
STATS_RESET restituisce EPERM ai non-root ed EBUSY con waiter contabilizzati.

## Accesso amministrativo

Le ioctl ST_IOCTL_* riconosciute, dirette al vero file del device, saltano
il limiter. Il controllo usa l'oggetto file e ne mantiene un riferimento
durante l'operazione; i controlli LSM e i permessi del driver restano applicati.

Comandi sconosciuti o comandi noti diretti ad altri oggetti non hanno il bypass.
L'eccezione non copre avvio di stctl, caricamento delle librerie o apertura
del device: una syscall precedente alla ioctl può essere soggetta al limite.

Non esiste un'esenzione generale per root o per il nome stctl. Non si garantisce
raggiungibilità amministrativa sotto ogni possibile configurazione.

## Terminazione e scaricamento

exit (60) ed exit_group (231) sono controllabili. Il percorso dedicato usa
sched_process_exit per completare il riferimento interno alla chiamata
che non ritorna normalmente al wrapper.

Lo scaricamento risveglia i waiter e attende le chiamate attive. Una syscall
originale già ammessa e bloccata può mantenere rmmod in attesa finché termina.

delete_module (176) è registrabile. Ogni invocazione entrata nel wrapper
acquisisce un riferimento al monitor, anche se l'identità non è selezionata
per il throttling. La rimozione ordinaria di un altro modulo conserva
i controlli della syscall originale.

L'autorimozione attraverso il wrapper viene normalmente rifiutata dal kernel
con EWOULDBLOCK per il riferimento attivo. Se il riferimento non è acquisibile,
il wrapper restituisce EBUSY.

O_TRUNC, richiesta di scaricamento forzato, viene rifiutato con EPERM nel
percorso intercettato; per chiamate rilevanti il rifiuto segue l'ammissione.
Questa è una restrizione esplicita rispetto al comportamento originale.

La procedura ordinaria è DISABLE seguito da rmmod. Riferimenti di chiamate
già entrate o descriptor del device ancora aperti possono richiedere di attendere
la loro chiusura prima di ripetere la rimozione. Non usare rmmod forzato.

## Esecuzione dei test

Eseguire dalla radice del repository su VM di sviluppo.
`make -C tests` compila i programmi C; le regressioni Python vanno lanciate
esplicitamente. Alcune compilano ulteriori worker temporanei.

I test ordinari richiedono modulo caricato, monitor OFF e registri vuoti.
Esempio, partendo da modulo scaricato:

```bash
sudo insmod module/syscall_throttle.ko
sudo -v
PYTHONDONTWRITEBYTECODE=1 python3 tests/max_session_regression.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/statistics_regression.py
```

Controllare l'esito di ogni test prima del successivo. FAIL e INCONCLUDENTE
non sono esiti positivi. Dopo un timeout verificare stato del modulo e log
prima di rilanciare operazioni di caricamento o scaricamento.

| Test | Esecuzione e stato iniziale | Stato finale al successo |
|---|---|---|
| statistics, max_session, max_same_value, max_change | Utente normale dopo sudo -v; modulo caricato, OFF, registri vuoti | OFF, registri vuoti, MAX=0 |
| disable, disable_blocking, statistics_reset | Come sopra | OFF, registri vuoti, MAX=0 |
| deregistration, registry_concurrency | Come sopra | OFF, registri vuoti, MAX=0 |
| signal, restart, admitted_signal, blocking_syscall | Come sopra | OFF, registri vuoti, MAX=0 |
| identity_deregistration | sudo da utente normale; modulo caricato, OFF, registri vuoti | OFF, registri vuoti, MAX=0 |
| control_ioctl_regression, binario C | sudo; modulo caricato, OFF, registri vuoti | OFF, registri vuoti, MAX=0 |
| nonreturning, unload_waiters, unload_active | Utente normale dopo sudo -v; modulo caricato, OFF, registri vuoti | Modulo scaricato |
| delete_module, delete_module_edges | sudo; monitor scaricato | Monitor e moduli di prova scaricati |

I nomi abbreviati corrispondono a `tests/<nome>_regression.py`, salvo il binario C.
Il test delete_module compila anche un modulo vuoto con gli header del kernel.

Esempi, da eseguire rispettando lo stato iniziale della tabella:

```bash
sudo env PYTHONDONTWRITEBYTECODE=1 python3 tests/identity_deregistration_regression.py
sudo env PYTHONDONTWRITEBYTECODE=1 python3 tests/delete_module_regression.py
sudo env PYTHONDONTWRITEBYTECODE=1 python3 tests/delete_module_edges_regression.py
```

## Evidenze e attività residue

L'autore ha riportato esiti positivi sulla VM, in momenti diversi dello sviluppo,
per statistiche concorrenti, deregistrazione e selezione OR, ioctl amministrative,
syscall bloccanti, DISABLE, reset, segnali e SA_RESTART, transizioni concorrenti,
scaricamento, exit/exit_group e gestione protetta di delete_module.

Questi esiti riguardano casi specifici. Non costituiscono una nuova esecuzione
integrale della suite sull'ultimo commit né una verifica di ogni syscall.

### Budget globale: test aggiornato e superato

Il test `tests/global_budget_regression.py` prepara i worker prima di
ENABLE e mantiene MAX=3 per tutta la prova. Non presume più che MAX_SET
avvii una nuova finestra temporale.

Esecuzione riportata dall'autore il 5 ottobre 2026, su kernel
7.0.0-28-generic, con il test aggiornato sulla base del commit `ba2f67e`:

- 16 worker: due nomi di programma, due EUID (0 e 1000), due syscall
  (getpid e getppid), due processi per combinazione; registro UID vuoto.
- Tre chiamate ammesse complessivamente e tredici waiter, con 20 snapshot
  stabili fino a 0,522 secondi dal timestamp precedente a ENABLE.
- Tutti i worker completati senza ulteriori cambi di MAX.
- Statistiche finali: 16 invocazioni rilevanti, 13 bloccate,
  13 attese completate, zero interruzioni e zero waiter residui.
- Cleanup completato: monitor OFF, registri vuoti, MAX=0.

Il risultato verifica la condivisione del budget nel caso provato.
Il test controlla la prima parte della finestra e il completamento finale;
non misura separatamente ogni finestra successiva. Un campionamento
insufficiente entro il margine temporale produce un esito INCONCLUSIVO.

La gestione di MAX resta distinta dalla sessione statistica: un cambio
effettivo conserva consumo e scadenza della finestra, ma da ON apre una
nuova osservazione, trasferendo i waiter e misurandone il ritardo dalla
nuova sessione. Lo stesso MAX non rinnova né budget né statistiche.
Da OFF, cambiare MAX conserva lo snapshot precedente.

Prima della consegna:

1. Verificare gli altri script storici e preparare una sequenza finale
   riproducibile, indicando commit e risultati.
2. Esplicitare nella relazione finestre fisse, sessioni al cambio di MAX,
   ritardi limitati alla sessione, eccezione ioctl e restrizioni di delete_module.
   Concordare eventuali interpretazioni della traccia.
3. Consolidare il confronto requisito-per-requisito e i limiti di piattaforma:
   percorso nativo x86-64, header, Ftrace/Kprobes e configurazione kernel.
   Il numero di hook installati non dimostra il comportamento di ogni syscall.
