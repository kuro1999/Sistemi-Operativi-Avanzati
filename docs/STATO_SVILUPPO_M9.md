# Syscall Throttle — Stato dello sviluppo

**Versione aggiornata alla milestone M9.**

Questo documento descrive lo stato corrente del progetto **Syscall Throttle**, le funzionalità implementate, l’architettura adottata, le principali decisioni progettuali, le misure di sicurezza e i test eseguiti.

L’obiettivo del progetto è realizzare un Linux Kernel Module capace di configurare e applicare un meccanismo di throttling sulle system call native x86-64.

Al termine della M8 sono state completate:

- l’infrastruttura di comunicazione user-space/kernel;
- la gestione dello stato attivo/disattivo del monitor;
- i registri completi di UID, nomi degli eseguibili e numeri di system call;
- l’identificazione sicura del basename dell’eseguibile corrente;
- la configurazione a 64 bit del limite globale `MAX`;
- il rate limiter globale a finestre temporali di un secondo;
- la serializzazione delle transizioni amministrative;
- l’intercettazione generale delle system call native x86-64 tramite Ftrace;
- la generazione automatica dell’inventario a partire da `asm/syscalls_64.h`;
- la risoluzione temporanea dei simboli tramite Kprobe;
- la deduplicazione dei target Ftrace per indirizzo;
- il wrapper generico capace di richiamare la vera funzione `__x64_sys_*`;
- l’applicazione della condizione `monitor AND syscall AND (UID OR programma)`;
- il blocco dei thread eccedenti sulla wait queue del rate limiter;
- il risveglio e il retry dopo una nuova finestra, `MAX_SET`, `DISABLE` o teardown;
- la propagazione corretta dei segnali durante l’attesa;
- la gestione sicura delle syscall non ritornanti `exit` ed `exit_group`;
- il teardown sincronizzato con le chiamate ancora in esecuzione;
- il rifiuto esplicito di `delete_module`, necessario per evitare un self-deadlock durante `rmmod`;
- il sottosistema statistico runtime separato dal rate limiter;
- il conteggio delle invocazioni rilevanti, bloccate, completate e interrotte;
- il conteggio corrente e il picco dei thread contemporaneamente bloccati;
- la media temporale dei thread bloccati, calcolata come integrale nel tempo;
- il ritardo medio e il peak delay delle chiamate effettivamente eseguite;
- l’associazione del peak delay a numero di syscall, effective UID e basename dell’eseguibile;
- snapshot statistiche coerenti tramite `ST_IOCTL_STATS_GET`;
- reset esplicito root-only tramite `ST_IOCTL_STATS_RESET`;
- sessioni statistiche con generazioni e isolamento dai waiter appartenenti a sessioni precedenti;
- una regressione M9 conclusa con **6 test superati e 0 fallimenti**;
- la regressione completa M8 rieseguita dopo M9 con **12 test superati e 0 fallimenti**.

---

## 1. Stato attuale del progetto

Sono disponibili:

- un Linux Kernel Module multi-file;
- un character device `/dev/syscall_throttle`;
- registrazione e deregistrazione tramite `miscdevice`;
- UAPI condivisa tra kernel e user-space;
- comunicazione tramite `ioctl()`;
- controller user-space `stctl`;
- comandi `PING`, `ENABLE`, `DISABLE` e `GET_STATUS`;
- controllo root basato sull’effective UID per le modifiche amministrative;
- registri completi di UID, programmi e numeri di system call;
- snapshot consistenti e retry limitati per le liste;
- identificazione del vero file eseguibile corrente senza usare `current->comm`;
- configurazione e consultazione del limite globale `MAX`;
- rate limiter globale a finestra fissa di un secondo;
- decisioni `BYPASS`, `ALLOW`, `THROTTLE` e `SHUTDOWN`;
- timer periodico, generazione della finestra e contatore delle ammissioni;
- wait queue interruptible per i thread eccedenti;
- risveglio dopo scadenza della finestra, cambio di `MAX`, disattivazione o teardown;
- retry atomico dell’ammissione dopo ogni wake-up;
- intercettazione Ftrace delle funzioni `__x64_sys_*`;
- inventario automatico di 472 definizioni native x86-64;
- mapping diretto numero → target tramite `st_hook_targets[NR_syscalls]`;
- 369 filtri Ftrace distinti dopo deduplicazione per indirizzo;
- risoluzione dei simboli mediante Kprobe temporanee;
- callback Ftrace non bloccante;
- validazione del percorso nativo x86-64;
- esclusione delle chiamate kernel interne e delle ABI IA-32/x32;
- wrapper generico comune a tutte le syscall intercettate;
- controllo della ricorsione quando il wrapper richiama l’originale;
- classificazione completa tramite monitor, registro syscall e identità;
- esecuzione della syscall reale nello stesso thread chiamante;
- propagazione di `-ERESTARTSYS`/`EINTR` in caso di segnale;
- conteggio atomico delle chiamate attive;
- gestione speciale di `exit` ed `exit_group` tramite `sched_process_exit`;
- teardown sicuro anche con thread bloccati;
- rollback completo in caso di errore durante l’installazione Ftrace;
- diagnostica finale del data path e dei target non ritornanti;
- rifiuto di `delete_module` con `-EOPNOTSUPP`;
- sottosistema `statistics.c` protetto da spinlock;
- contatori saturanti a 64 bit;
- sessioni statistiche attive/inattive con generazioni;
- durata di osservazione e area temporale dei waiter;
- peak delay con syscall, effective UID e programma;
- reset delle statistiche rifiutato con `-EBUSY` quando esistono waiter contabilizzati;
- isolamento dei completamenti appartenenti a vecchie generazioni;
- comandi `stctl stats` e `stctl stats-reset`;
- test concorrenti, funzionali, di segnale, nonreturning, unload e statistiche.

La condizione applicata dal data path è:

```text
monitor attivo
AND system call registrata
AND (effective UID registrato OR programma registrato)
```

Quando la condizione è falsa, la syscall reale prosegue senza rate limiting. Quando è vera, il thread deve ottenere un’ammissione dal rate limiter prima di eseguire la vera funzione kernel.

Restano da implementare o perfezionare nella fase M10:

- stabilizzazione finale della semantica delle sessioni in presenza di `DISABLE` e waiter ancora in uscita;
- chiusura della race tra conteggio della chiamata rilevante e reset concorrente delle statistiche;
- integrazione permanente dei test M9 nel repository;
- profiling e riduzione della contesa sullo spinlock globale delle statistiche;
- ottimizzazione read-mostly dei registri UID e programmi;
- valutazione della riservatezza delle statistiche pubbliche, che espongono UID e basename del peak;
- test di fault injection, load/unload ripetuto e stress prolungato;
- verifica su più versioni del kernel e configurazioni SMP;
- documentazione dell’ABI, delle limitazioni e delle proprietà temporali;
- eventuale supporto delle ABI IA-32 e x32;
- eventuale reinserimento sicuro di `delete_module`, che richiede un ridisegno del lifecycle;
- relazione finale, istruzioni di installazione e matrice dei test riproducibili.


## 2. Architettura attuale

```text
                              USER SPACE
┌──────────────────────────────────────────────────────────────┐
│                         user/stctl                           │
│                                                              │
│ validazione argomenti → open device → ioctl → close          │
└─────────────────────────────┬────────────────────────────────┘
                              ▼
                            KERNEL
┌──────────────────────────────────────────────────────────────┐
│                       module/device.c                        │
│                                                              │
│ UAPI, privilegi, copy_from_user/copy_to_user                 │
│ serializzazione ENABLE / DISABLE / MAX_SET / STATS_RESET     │
└──────────┬────────────┬──────────────┬──────────────┬─────────┘
           ▼            ▼              ▼              ▼
   monitor_state    registri      rate_limiter     statistics
                                    │                 │
                                    │ timer +         │ contatori,
                                    │ wait queue      │ sessioni,
                                    ▼                 │ peak e area
                          budget / generazione        ▼
                              / wake-up          snapshot coerente

                     PERCORSO DI UNA SYSTEM CALL
programma
   ↓ istruzione syscall
entry x86-64 del kernel
   ↓
funzione __x64_sys_*
   ↓ punto Ftrace
st_ftrace_callback()
   ├─ verifica ricorsione e teardown
   ├─ verifica frame pt_regs e ABI x86-64
   ├─ legge regs->orig_ax
   ├─ scarta delete_module
   ├─ scarta monitor spento o syscall non registrata
   ├─ salva original_ip nel secondo argomento
   └─ modifica RIP verso st_generic_syscall_wrapper()
            ↓
st_generic_syscall_wrapper()
   ├─ valida numero + indirizzo
   ├─ verifica UID oppure programma
   ├─ registra l’invocazione rilevante
   ├─ prova l’ammissione
   ├─ primo THROTTLE → apre il contesto statistico
   ├─ THROTTLE → attesa interruptible e retry
   ├─ segnale → registra l’interruzione senza peak delay
   └─ ALLOW/BYPASS/SHUTDOWN
          ├─ chiude il contesto e aggiorna delay/peak
          └─ invoca la vera __x64_sys_*(regs)
            ↓
ritorno al processo
```

La `sys_call_table` non viene modificata. Il modulo usa Ftrace come meccanismo permanente di intercettazione e usa Kprobe soltanto durante `insmod` per risolvere gli indirizzi dei simboli.

Le responsabilità principali sono:

```text
device.c            → VFS, ioctl, UAPI e transizioni amministrative
monitor_state.c     → stato attivo/disattivo con READ_ONCE/WRITE_ONCE
uid_registry.c      → registro degli UID
program_registry.c  → registro programmi e matching del task corrente
program_identity.c  → current → mm → exe_file → dentry → basename
syscall_registry.c  → bitmap delle syscall configurate
rate_limiter.c      → MAX, timer, budget, generazioni, wait queue
statistics.c        → sessioni, contatori, area temporale, delay e peak
syscall_hook.c      → inventario, Ftrace, wrapper, statistiche, teardown
stctl.c             → controller user-space e rendering delle statistiche
tests/              → test permanenti user-space
```

Il callback Ftrace non dorme. Le operazioni che possono comportare lookup più costosi o attesa vengono eseguite nel wrapper, cioè nel process context del thread che ha invocato la syscall.

## 3. Struttura dei file

```text
.
├── include
│   └── uapi
│       └── syscall_throttle.h
├── module
│   ├── Makefile
│   ├── main.c
│   ├── device.c
│   ├── device.h
│   ├── monitor_state.c
│   ├── monitor_state.h
│   ├── uid_registry.c
│   ├── uid_registry.h
│   ├── program_registry.c
│   ├── program_registry.h
│   ├── program_identity.c
│   ├── program_identity.h
│   ├── syscall_registry.c
│   ├── syscall_registry.h
│   ├── rate_limiter.c
│   ├── rate_limiter.h
│   ├── statistics.c
│   ├── statistics.h
│   ├── syscall_hook.c
│   └── syscall_hook.h
├── tests
│   ├── Makefile
│   ├── policy_transition_stress.c
│   ├── syscall_hook_smoke.c
│   └── syscall_hook_signal.c
└── user
    ├── Makefile
    └── stctl.c
```

I binari prodotti dalla compilazione sono esclusi dal controllo di versione:

```text
/user/stctl
/tests/policy_transition_stress
/tests/syscall_hook_smoke
/tests/syscall_hook_signal
```

Gli script di regressione completi della M8 sono stati conservati nella home dell’ambiente di sviluppo e non fanno parte dei sorgenti del repository.

## 4. Interfaccia UAPI

Il file:

```text
include/uapi/syscall_throttle.h
```

definisce l’interfaccia binaria condivisa tra kernel e user-space.

Contiene:

- nome e percorso del device;
- magic number degli `ioctl`;
- tipi a dimensione fissa;
- strutture delle richieste e delle risposte;
- campi `reserved`;
- dimensione massima dei nomi dei programmi;
- configurazione a 64 bit del limite globale `MAX`.

### Comandi generali

```c
ST_IOCTL_PING
ST_IOCTL_ENABLE
ST_IOCTL_DISABLE
ST_IOCTL_GET_STATUS
```

### Comandi del registro UID

```c
ST_IOCTL_UID_ADD
ST_IOCTL_UID_REMOVE
ST_IOCTL_UID_GET_COUNT
ST_IOCTL_UID_LIST
```

### Comandi del registro programmi

```c
ST_IOCTL_PROGRAM_ADD
ST_IOCTL_PROGRAM_REMOVE
ST_IOCTL_PROGRAM_GET_COUNT
ST_IOCTL_PROGRAM_LIST
```

### Comandi del registro system call

```c
ST_IOCTL_SYSCALL_ADD
ST_IOCTL_SYSCALL_REMOVE
ST_IOCTL_SYSCALL_GET_COUNT
ST_IOCTL_SYSCALL_LIST
```

### Comandi del rate limiter

```c
ST_IOCTL_MAX_SET
ST_IOCTL_MAX_GET
```

### Comandi delle statistiche

```c
ST_IOCTL_STATS_GET
ST_IOCTL_STATS_RESET
```

`STATS_GET` è pubblico e restituisce uno snapshot coerente a dimensione fissa.  
`STATS_RESET` è root-only e non trasferisce dati.

```text
0x50 → STATS_GET
0x51 → STATS_RESET
```

```text
STATS_GET   → _IOR
STATS_RESET → _IO
```

La numerazione riservata alla M7 è:

```text
0x40 → MAX_SET
0x41 → MAX_GET
```

Le direzioni sono:

```text
MAX_SET → _IOW
MAX_GET → _IOR
```

### Lunghezza dei nomi

```c
#define ST_PROGRAM_NAME_MAX 255U
#define ST_PROGRAM_NAME_CAPACITY (ST_PROGRAM_NAME_MAX + 1U)
```

`ST_PROGRAM_NAME_MAX` non comprende il terminatore NUL.

La capacità comprende invece:

```text
255 caratteri
+ 1 byte per '\0'
= 256 byte
```

### Struttura dello stato

```c
struct st_monitor_status {
    __u32 enabled;
    __u32 reserved;
};
```

### Strutture UID

```c
struct st_uid_request {
    __u32 uid;
    __u32 reserved;
};

struct st_uid_count {
    __u32 count;
    __u32 reserved;
};

struct st_uid_list_request {
    __aligned_u64 uids_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};
```

### Strutture dei programmi

```c
struct st_program_request {
    char name[ST_PROGRAM_NAME_CAPACITY];
    __u32 reserved[2];
};

struct st_program_count {
    __u32 count;
    __u32 reserved;
};

struct st_program_name {
    char name[ST_PROGRAM_NAME_CAPACITY];
};

struct st_program_list_request {
    __aligned_u64 programs_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};
```

`programs_ptr` indica un array user-space di:

```c
struct st_program_name
```

### Struttura del limite globale

```c
struct st_max_config {
    __aligned_u64 max_invocations;
    __u32 reserved[2];
};
```

L’uso di `__aligned_u64` mantiene dimensione e allineamento stabili anche nell’interfaccia condivisa. Il campo può rappresentare tutti i valori da `0` a `UINT64_MAX`.

I campi `reserved` sono impostati a zero dal controller e devono essere zero anche per il kernel. `MAX_GET` restituisce una struttura completamente inizializzata.

### Struttura dello snapshot statistico

```c
struct st_statistics_snapshot {
    __aligned_u64 observation_ns;
    __aligned_u64 blocked_thread_time_ns;

    __aligned_u64 relevant_invocations;
    __aligned_u64 blocked_invocations;
    __aligned_u64 completed_blocked_invocations;
    __aligned_u64 interrupted_blocked_invocations;

    __aligned_u64 total_delay_ns;
    __aligned_u64 peak_delay_ns;

    __u32 current_blocked;
    __u32 peak_blocked;

    __u32 peak_syscall_nr;
    __u32 peak_euid;

    __u32 peak_valid;
    __u32 session_active;

    char peak_program[ST_PROGRAM_NAME_CAPACITY];

    __u32 reserved[4];
};
```

Layout verificato:

```text
dimensione struttura = 360 byte
peak_delay_ns        = offset 56
current_blocked      = offset 64
peak_program         = offset 88
reserved             = offset 344
STATS_GET ioctl      = 0x81685350
STATS_RESET ioctl    = 0x5351
```

Il kernel esporta nanosecondi e contatori interi. `stctl` calcola in user-space:

```text
media temporale =
    blocked_thread_time_ns / observation_ns

ritardo medio =
    total_delay_ns / completed_blocked_invocations
```

### Motivazioni di sicurezza dell’UAPI

Le strutture hanno dimensione fissa per:

- evitare dipendenze dall’ABI dei puntatori nativi;
- rendere prevedibile la dimensione degli `ioctl`;
- semplificare la validazione;
- impedire stringhe kernel di lunghezza non controllata;
- mantenere compatibilità tra user-space e kernel.

I puntatori inclusi nelle strutture sono rappresentati con:

```c
__aligned_u64
```

e vengono convertiti nel kernel tramite:

```c
u64_to_user_ptr()
```

I campi `reserved` devono essere zero. Questo permette di:

- rifiutare richieste ambigue o non inizializzate;
- riservare spazio per estensioni future;
- evitare che dati casuali vengano interpretati come funzionalità future.

---

## 5. Ciclo di vita del modulo

### Caricamento

L’ordine corrente è:

```text
syscall_throttle_init()
    ├── st_monitor_state_init()
    ├── st_uid_registry_init()
    ├── st_program_registry_init()
    ├── st_syscall_registry_init()
    ├── st_rate_limiter_init()
    ├── st_statistics_init()
    ├── st_syscall_hook_init()
    ├── st_device_init()
    └── modulo operativo
```

L’hook viene installato dopo che monitor, registri e rate limiter sono pronti, ma prima che il device venga pubblicato. In questo modo nessun comando amministrativo può rendere operativa una policy mentre il data path non è ancora inizializzato.

`st_syscall_hook_init()`:

```text
azzera lo stato runtime e i contatori
    ↓
prepara st_hook_targets[NR_syscalls]
    ↓
registra sched_process_exit
    ↓
risolve i simboli con Kprobe temporanee
    ↓
deduplica gli indirizzi
    ↓
installa i filtri Ftrace
    ↓
registra la callback globale
    ↓
pubblica accepting_calls=true e installed=true
```

Se una fase fallisce, vengono rimossi in ordine inverso i filtri già installati, il tracepoint e lo stato runtime.

### Rimozione

L’ordine corrente è:

```text
syscall_throttle_exit()
    ├── st_device_exit()
    ├── st_rate_limiter_exit()
    ├── st_syscall_hook_exit()
    ├── st_statistics_exit()
    ├── st_syscall_registry_exit()
    ├── st_program_registry_exit()
    ├── st_uid_registry_exit()
    ├── st_monitor_state_exit()
    └── modulo rimosso
```

Il device viene rimosso per primo, impedendo nuovi comandi.

Il rate limiter viene arrestato prima dell’hook:

```text
running = false
stopping = true
generation++
wake_up_all(wait_queue)
timer_shutdown_sync()
```

Questo libera i thread eventualmente sospesi. Il wrapper interpreta `SHUTDOWN` come autorizzazione a eseguire la syscall reale, così le chiamate possono terminare.

Il teardown dell’hook esegue:

```text
accepting_calls = false
    ↓
unregister_ftrace_function()
    ↓
rimozione di tutti i filtri Ftrace
    ↓
attesa active_calls == 0
    ↓
rimozione di sched_process_exit
    ↓
diagnostica finale
    ↓
azzeramento delle strutture runtime
```

Il tracepoint resta registrato durante l’attesa di `active_calls`, perché potrebbe essere ancora necessario per completare una `exit` o `exit_group`.

`delete_module` non viene mai deviata nel wrapper. Questo evita che la stessa syscall responsabile dell’unload incrementi `active_calls` e poi rimanga bloccata dentro `module_exit()`.

## 6. Character device e VFS

Il device:

```text
/dev/syscall_throttle
```

è registrato tramite `miscdevice`.

```c
static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .open = st_device_open,
    .release = st_device_release,
    .unlocked_ioctl = st_device_ioctl,
};
```

### Protezione tramite `.owner`

```c
.owner = THIS_MODULE
```

incrementa il reference count del modulo mentre il device è aperto.

Di conseguenza, `rmmod` fallisce finché esiste un file descriptor aperto sul device.

Questa protezione impedisce che il codice del driver venga rimosso mentre un processo potrebbe ancora chiamare una sua operazione.

---

## 7. Stato del monitor

Il monitor usa:

```c
static bool st_monitor_enabled;
```

e viene gestito soltanto tramite:

```c
st_monitor_enable();
st_monitor_disable();
st_monitor_is_enabled();
```

Gli accessi usano:

```c
READ_ONCE()
WRITE_ONCE()
```

Il monitor parte disattivato a ogni caricamento.

Le modifiche sono consentite soltanto a un thread con effective UID zero:

```c
uid_eq(current_euid(), GLOBAL_ROOT_UID)
```

La lettura dello stato è pubblica.

### Coordinamento con il rate limiter

In `device.c` è presente:

```c
static DEFINE_MUTEX(st_policy_lock);
```

Il mutex serializza le operazioni amministrative che modificano la policy globale:

```text
ENABLE
DISABLE
MAX_SET
STATS_RESET
```

Una reale attivazione esegue:

```text
st_rate_limiter_start()
    ↓
st_statistics_session_start()
    ↓
st_monitor_enable()
```

Una reale disattivazione esegue:

```text
st_monitor_disable()
    ↓
st_rate_limiter_stop()
    ↓
st_statistics_session_stop()
```

`ENABLE` e `DISABLE` sono idempotenti:

- un secondo `ENABLE` non resetta budget o statistiche;
- un secondo `DISABLE` non arresta nuovamente timer o sessione;
- `MAX_SET` non azzera le statistiche;
- `STATS_RESET` attivo azzera e riavvia la sola sessione statistica;
- `STATS_RESET` inattivo azzera i dati e lascia la sessione inattiva;
- `STATS_RESET` restituisce `-EBUSY` con waiter contabilizzati.

---

## 8. Registro UID

Il registro UID è basato su:

```c
struct st_uid_entry {
    kuid_t uid;
    struct list_head node;
};
```

Gli UID vengono conservati come `kuid_t`.

La conversione dalla UAPI avviene tramite:

```c
make_kuid(&init_user_ns, request.uid)
```

e viene verificata con:

```c
uid_valid()
```

### Sincronizzazione

Lista e contatore sono protetti da un `mutex`.

Il mutex copre:

- aggiunte;
- rimozioni;
- ricerche;
- conteggio;
- snapshot.

### Sicurezza

Le operazioni di modifica sono root-only.

Le richieste vengono copiate con `copy_from_user()`.

Le risposte vengono copiate con `copy_to_user()`.

Il kernel non dereferenzia direttamente puntatori user-space.

Le allocazioni per `UID_LIST` dipendono dal numero reale di entry nel registro, non dalla capacità dichiarata dall’utente.

Questo impedisce a un chiamante di provocare una grande allocazione kernel inserendo un valore arbitrario in `capacity`.

---

## 9. Registro dei programmi

Il registro dei programmi è implementato in:

```text
module/program_registry.c
module/program_registry.h
```

### Struttura di una entry

```c
struct st_program_entry {
    char name[ST_PROGRAM_NAME_CAPACITY];
    struct list_head node;
};
```

Il registro mantiene:

```c
static LIST_HEAD(st_program_entries);
static DEFINE_MUTEX(st_program_registry_lock);
static unsigned int st_program_entries_count;
```

### Operazioni disponibili

```c
st_program_registry_add()
st_program_registry_remove()
st_program_registry_contains()
st_program_registry_count()
st_program_registry_snapshot()
st_program_registry_contains_current()
```

---

## 10. Semantica del nome del programma

Il registro conserva il **basename dell’eseguibile**, non il percorso completo.

Esempi:

```text
/usr/bin/curl        → curl
/usr/bin/python3     → python3
/home/user/test_app  → test_app
```

Il confronto è:

- esatto;
- case-sensitive;
- limitato a 255 caratteri;
- privo di `/`.

Quindi:

```text
curl != Curl
```

Sono accettati:

```text
curl
python3
test_app
```

Sono rifiutati:

```text
/usr/bin/curl
./curl
directory/curl
```

### Motivazione

La traccia richiede la registrazione dei nomi degli eseguibili.

Conservare il basename:

- evita di dipendere dal percorso di installazione;
- rende coerente il matching con la dentry del file eseguibile;
- elimina percorsi relativi o completi ambigui;
- semplifica la UAPI;
- evita la necessità di gestire `PATH_MAX`.

---

## 11. Validazione dei nomi

La validazione kernel controlla:

```text
puntatore non nullo
nome non vuoto
terminatore NUL entro la capacità
lunghezza massima rispettata
assenza del carattere '/'
```

La lunghezza viene calcolata con:

```c
strnlen(name, ST_PROGRAM_NAME_CAPACITY)
```

e il carattere `/` viene cercato soltanto entro la lunghezza validata.

### Perché non si stampa un nome non validato

Una richiesta malevola potrebbe inviare un array senza terminatore NUL.

Usare:

```c
pr_warn("%s", request.name);
```

prima della validazione potrebbe leggere oltre il buffer.

Per questo i log stampano il nome soltanto quando il registro ha già confermato che la stringa è valida.

In caso di errore viene registrato un messaggio generico:

```text
nome non valido
```

### Validazione user-space

`stctl` rifiuta preventivamente:

- stringhe vuote;
- nomi oltre il limite;
- nomi contenenti `/`.

La validazione user-space migliora l’esperienza dell’utente, ma non sostituisce quella kernel.

Il kernel considera sempre lo user-space non fidato.

---

## 12. Aggiunta di un programma

Comando:

```bash
sudo ./stctl program-add <nome>
```

Percorso:

```text
validazione user-space
    ↓
struct st_program_request azzerata
    ↓
ST_IOCTL_PROGRAM_ADD
    ↓
controllo effective UID 0
    ↓
copy_from_user()
    ↓
controllo reserved
    ↓
validazione kernel del nome
    ↓
st_program_registry_add()
```

L’inserimento:

1. valida il nome;
2. alloca la nuova entry con `kmalloc()`;
3. copia il nome con `strscpy()`;
4. acquisisce il mutex;
5. ricerca eventuali duplicati;
6. inserisce con `list_add_tail()`;
7. incrementa il contatore;
8. rilascia il mutex.

L’allocazione avviene prima del mutex, evitando di mantenere il registro bloccato durante `kmalloc()`.

### Errori

```text
0         programma aggiunto
-EPERM    chiamante non privilegiato
-EFAULT   richiesta user-space non accessibile
-EINVAL   nome o campi reserved non validi
-EEXIST   programma già registrato
-ENOMEM   memoria kernel insufficiente
```

---

## 13. Rimozione di un programma

Comando:

```bash
sudo ./stctl program-remove <nome>
```

Il percorso è analogo all’aggiunta.

La rimozione:

1. valida il nome;
2. acquisisce il mutex;
3. cerca l’entry;
4. la rimuove dalla lista;
5. decrementa il contatore;
6. rilascia il mutex;
7. libera l’entry.

Il rilascio della memoria avviene fuori dalla sezione critica.

### Errori

```text
0         programma rimosso
-EPERM    chiamante non privilegiato
-EFAULT   puntatore user-space non valido
-EINVAL   nome non valido
-ENOENT   programma non registrato
```

---

## 14. Conteggio dei programmi

Comando:

```bash
./stctl program-count
```

Il driver costruisce:

```c
struct st_program_count response = {
    .count = st_program_registry_count(),
    .reserved = 0U,
};
```

e la copia con `copy_to_user()`.

La consultazione non richiede privilegi.

---

## 15. Lista dei programmi

Comando:

```bash
./stctl program-list
```

Esempio:

```text
Programmi registrati: 3
  curl
  bash
  python3
```

### Protocollo in due fasi

```text
PROGRAM_GET_COUNT
    ↓
calloc() nello user-space
    ↓
PROGRAM_LIST
```

### Rappresentazione

Lo user-space alloca un array di:

```c
struct st_program_name {
    char name[ST_PROGRAM_NAME_CAPACITY];
};
```

Non viene utilizzato un array di puntatori, perché i puntatori user-space non sono dereferenziabili direttamente dal kernel.

### Snapshot consistente

```c
st_program_registry_snapshot(struct st_program_name *programs,
                             __u32 capacity,
                             __u32 *count);
```

esegue:

```text
mutex_lock
    ↓
lettura del conteggio reale
    ↓
verifica della capacità
    ↓
copia dei nomi
    ↓
mutex_unlock
```

Lo snapshot non produce liste parziali.

Se la capacità è insufficiente:

```text
-ENOSPC
count = capacità necessaria
```

### Protezione dalla perdita di dati kernel

Prima di copiare ogni nome, il record di destinazione viene azzerato:

```c
memset(&programs[index], 0, sizeof(programs[index]));
```

Poi il nome viene copiato con:

```c
strscpy()
```

Questo è importante perché le entry originali sono allocate con `kmalloc()` e i byte dopo il terminatore NUL potrebbero non essere inizializzati.

Senza l’azzeramento, copiare l’intero record verso lo user-space potrebbe esporre byte residui della memoria kernel.

### Buffer kernel intermedio

Il driver alloca:

```c
kcalloc(required, sizeof(*programs), GFP_KERNEL)
```

Lo snapshot scrive nel buffer kernel.

Solo dopo il rilascio del mutex viene eseguito:

```c
copy_to_user()
```

Questo evita:

- accessi user-space mentre il registro è bloccato;
- fault di pagina dentro la sezione critica;
- accoppiamento tra il registro e la memoria utente.

### Protezione da capacità arbitrarie

L’allocazione usa il conteggio reale del registro:

```c
required = st_program_registry_count();
```

e non:

```c
request.capacity
```

Il valore `capacity` è controllato dall’utente e non deve poter determinare direttamente la quantità di memoria kernel allocata.

### Modifiche concorrenti

Tra `PROGRAM_GET_COUNT` e `PROGRAM_LIST`, il registro può cambiare.

Se cresce:

```text
snapshot → -ENOSPC
request.count → nuova dimensione
stctl → riallocazione e nuovo tentativo
```

`stctl` esegue al massimo quattro tentativi.

Se il registro continua a cambiare, l’operazione fallisce invece di entrare in un ciclo illimitato.

Se il registro diminuisce, il buffer precedentemente allocato rimane sufficiente e viene restituito il nuovo numero effettivo di elementi.

---

## 16. Identificazione dell’eseguibile corrente

L’identificazione è implementata in:

```text
module/program_identity.c
module/program_identity.h
```

Funzione:

```c
int st_program_get_current_name(char *name, size_t capacity);
```

Il risultato è il basename dell’eseguibile associato a `current`.

### Perché non viene usato `current->comm`

`current->comm` identifica il nome del task o del thread, non necessariamente il file eseguibile.

Può:

- essere modificato;
- differire tra thread dello stesso processo;
- essere troncato;
- non rappresentare il basename reale dell’eseguibile.

Per la semantica del progetto è più corretto seguire il riferimento al file eseguibile associato all’address space.

---

## 17. Percorso `current → basename`

La sequenza implementata è:

```text
current
    ↓
get_task_mm(current)
    ↓
mm->exe_file
    ↓
get_file_rcu()
    ↓
file_dentry(exe_file)
    ↓
take_dentry_name_snapshot()
    ↓
copia del basename nel buffer locale
```

### Reference su `mm_struct`

```c
mm = get_task_mm(current);
```

acquisisce una reference stabile sull’address space.

Può restituire `NULL` per task privi di address space user-space.

La reference viene rilasciata con:

```c
mmput(mm);
```

### Lettura RCU di `exe_file`

Il campo è dichiarato:

```c
struct file __rcu *exe_file;
```

Non viene letto con una dereferenziazione ordinaria.

Il codice usa:

```c
rcu_read_lock();
exe_file = get_file_rcu(&mm->exe_file);
rcu_read_unlock();
```

`get_file_rcu()` acquisisce una reference sulla `struct file`, rendendola valida anche dopo il rilascio dell’`mm_struct`.

La reference viene rilasciata con:

```c
fput(exe_file);
```

### Snapshot del nome della dentry

Il nome di una dentry può cambiare a causa di una `rename()` concorrente.

Per evitare di conservare o leggere un puntatore instabile viene utilizzato:

```c
take_dentry_name_snapshot(&snapshot, exe_dentry);
```

Il nome viene copiato dal campo:

```c
snapshot.name.name
```

usando la lunghezza:

```c
snapshot.name.len
```

Lo snapshot viene sempre rilasciato con:

```c
release_dentry_name_snapshot(&snapshot);
```

### Coppie acquisizione/rilascio

```text
get_task_mm()                    → mmput()
get_file_rcu()                   → fput()
take_dentry_name_snapshot()      → release_dentry_name_snapshot()
```

La corretta gestione di queste coppie impedisce:

- use-after-free;
- reference leak;
- accesso a `mm_struct` già distrutte;
- accesso a `struct file` già liberate;
- uso di nomi di dentry instabili.

---

## 18. Matching del programma corrente

Il registro espone:

```c
bool st_program_registry_contains_current(void);
```

La funzione:

```text
st_program_get_current_name()
    ↓
st_program_registry_contains()
    ↓
true oppure false
```

Un task privo di eseguibile user-space o un errore di identificazione produce:

```text
false
```

Non vengono generati log dentro `contains_current()`.

Questa scelta è importante perché, nel monitor definitivo, la funzione potrà essere invocata nel percorso di ogni system call. Un log a ogni errore o mancata corrispondenza potrebbe saturare il kernel log e degradare le prestazioni.

---

## 19. Controllo dei privilegi

Il device è creato con permessi:

```text
0666
```

Le operazioni di sola lettura sono accessibili agli utenti normali.

### Operazioni protette

```text
ST_IOCTL_ENABLE
ST_IOCTL_DISABLE
ST_IOCTL_UID_ADD
ST_IOCTL_UID_REMOVE
ST_IOCTL_PROGRAM_ADD
ST_IOCTL_PROGRAM_REMOVE
ST_IOCTL_SYSCALL_ADD
ST_IOCTL_SYSCALL_REMOVE
ST_IOCTL_MAX_SET
ST_IOCTL_STATS_RESET
```

### Operazioni pubbliche

```text
ST_IOCTL_PING
ST_IOCTL_GET_STATUS
ST_IOCTL_UID_GET_COUNT
ST_IOCTL_UID_LIST
ST_IOCTL_PROGRAM_GET_COUNT
ST_IOCTL_PROGRAM_LIST
ST_IOCTL_SYSCALL_GET_COUNT
ST_IOCTL_SYSCALL_LIST
ST_IOCTL_MAX_GET
ST_IOCTL_STATS_GET
```

La verifica avviene nel kernel tramite effective UID.

Il controllo è eseguito prima di `copy_from_user()` per le operazioni protette.

Questo permette di rifiutare subito un chiamante non autorizzato senza accedere al puntatore da lui fornito.

Le operazioni `ENABLE`, `DISABLE` e `MAX_SET`, oltre al controllo root, acquisiscono `st_policy_lock` per rendere atomica la modifica della policy composta da stato del monitor, timer e configurazione del rate limiter.

---

## 20. Comandi disponibili in `stctl`

```bash
./stctl ping
./stctl status
./stctl enable
./stctl disable

./stctl uid-add <UID>
./stctl uid-remove <UID>
./stctl uid-count
./stctl uid-list

./stctl program-add <nome>
./stctl program-remove <nome>
./stctl program-count
./stctl program-list

./stctl syscall-add <numero>
./stctl syscall-remove <numero>
./stctl syscall-count
./stctl syscall-list

sudo ./stctl max-set <valore>
./stctl max-get

./stctl stats
sudo ./stctl stats-reset
```

`max-set` accetta soltanto una sequenza di cifre decimali. Sono validi anche:

```text
0
18446744073709551615
```

ovvero `UINT64_MAX`.

Sono rifiutati prima dell’`ioctl`:

```text
-1
+1
1.5
0x10
10abc
18446744073709551616
```

---

## 21. Logging

Il modulo registra:

- caricamento e rimozione;
- inizializzazione e rilascio dei registri;
- avvio, arresto e rilascio del rate limiter;
- registrazione e deregistrazione del device;
- modifiche alla policy;
- aggiunta e rimozione di UID, programmi e syscall;
- installazione e rimozione del tracepoint `sched_process_exit`;
- installazione dell’hook Ftrace;
- dimensione dell’inventario e numero dei filtri unici;
- errori di risoluzione dei simboli;
- errori di installazione o rollback dei filtri;
- rifiuto esplicito di `delete_module`;
- inizializzazione e rilascio del sottosistema statistiche;
- avvio e arresto delle sessioni statistiche con generazione;
- reset delle statistiche e rifiuto con waiter attivi;
- diagnostica aggregata del data path durante l’unload;
- diagnostica delle syscall non ritornanti;
- numero dei filtri rimossi.

Esempio di installazione M8:

```text
syscall_throttle: hook Ftrace x86-64 installato:
target=472, definizioni=472, esplicite=472,
implicite=0, filtri_unici=369, nonreturning=2
```

Esempio di diagnostica:

```text
syscall_throttle: diagnostica hook x86-64:
totali=..., monitor_spento=..., syscall_non_registrata=...,
identita_non_corrispondente=..., rilevanti=...,
limiter_bypass=..., limiter_allow=..., limiter_throttle=...,
limiter_shutdown=..., wait_interrotte=...
```

Esempio nonreturning:

```text
syscall_throttle: diagnostica nonreturning:
avviate=..., completate=..., ritorni_inattesi=...
```

Il callback Ftrace, il matching e il timer non producono log per ogni evento ordinario. Questa scelta evita di saturare il kernel log nel percorso frequente.

## 22. Compilazione

### Modulo kernel

```bash
cd module
make clean
make
```

Il modulo combina:

```text
main.o
device.o
monitor_state.o
uid_registry.o
program_registry.o
program_identity.o
syscall_registry.o
rate_limiter.o
statistics.o
syscall_hook.o
```

### Controller user-space

```bash
cd user
make clean
make
```

Risultato:

```text
user/stctl
```

### Test user-space

```bash
cd tests
make clean
make
```

Risultati:

```text
tests/policy_transition_stress
tests/syscall_hook_smoke
tests/syscall_hook_signal
```

La compilazione user-space usa:

```text
-Wall -Wextra -Wpedantic -std=c11 -O2
```

Il test concorrente usa anche `-pthread`.

Le compilazioni finali della M8 e della M9 sono riuscite sul kernel:

```text
7.0.0-28-generic
```

I warning relativi al nome del compilatore, alla versione di `pahole` e all’assenza di `vmlinux` per BTF non derivano dai sorgenti del progetto e non impediscono la produzione di `syscall_throttle.ko`.

## 23. Test del registro programmi

### Aggiunta senza privilegi

```bash
./stctl program-add curl
```

Risultato:

```text
Registrazione programma non consentita:
sono richiesti privilegi root.
```

### Aggiunta come root

```bash
sudo ./stctl program-add curl
```

Risultato:

```text
Programma 'curl' registrato.
```

### Duplicato

```bash
sudo ./stctl program-add curl
```

Risultato:

```text
Programma 'curl' già registrato.
```

### Rimozione senza privilegi

```bash
./stctl program-remove curl
```

Risultato:

```text
Rimozione programma non consentita:
sono richiesti privilegi root.
```

### Rimozione come root

```bash
sudo ./stctl program-remove curl
```

Risultato:

```text
Programma 'curl' rimosso.
```

### Rimozione di elemento assente

```bash
sudo ./stctl program-remove curl
```

Risultato:

```text
Programma 'curl' non registrato.
```

### Registro vuoto

```bash
./stctl program-count
./stctl program-list
```

Risultato:

```text
Programmi registrati: 0
Programmi registrati: 0
  nessuno
```

### Lista con più programmi

```bash
sudo ./stctl program-add curl
sudo ./stctl program-add bash
sudo ./stctl program-add python3

./stctl program-count
./stctl program-list
```

Risultato:

```text
Programmi registrati: 3
Programmi registrati: 3
  curl
  bash
  python3
```

L’ordine corrisponde a quello di inserimento grazie a:

```c
list_add_tail()
```

### Coerenza dopo rimozione

```bash
sudo ./stctl program-remove bash
./stctl program-count
./stctl program-list
```

Risultato:

```text
Programmi registrati: 2
Programmi registrati: 2
  curl
  python3
```

---

## 24. Test dell’identità corrente

Per verificare `st_program_get_current_name()`, `PING` è stato temporaneamente strumentato.

Eseguendo:

```bash
./stctl ping
```

il kernel ha identificato:

```text
programma 'stctl'
```

Il test ha confermato il percorso:

```text
current
→ mm_struct
→ exe_file
→ dentry
→ basename "stctl"
```

La strumentazione temporanea è stata successivamente rimossa e `PING` è tornato al comportamento originario.

---

## 25. Test del matching corrente

È stato verificato:

```text
registro senza "stctl"  → false
registro con "stctl"    → true
rimozione di "stctl"    → false
```

Quindi:

```c
st_program_registry_contains_current()
```

segue dinamicamente il contenuto del registro.

---

## 26. Test del ciclo di vita

Sono stati aggiunti più programmi, quindi il modulo è stato rimosso senza svuotare manualmente il registro.

La rimozione è riuscita e il log ha confermato:

```text
device deregistrato
registro programmi rilasciato
registro UID rilasciato
stato del monitor rilasciato
modulo rimosso
```

Dopo un nuovo `insmod`:

```bash
./stctl program-count
./stctl program-list
```

ha restituito un registro vuoto.

Questo conferma l’assenza di entry persistenti tra due caricamenti.

---

## 27. Test del reference count del modulo

Con il device aperto:

```bash
exec 3<> /dev/syscall_throttle
```

`rmmod` fallisce.

Dopo:

```bash
exec 3>&-
```

la rimozione riesce.

---

## 28. Decisioni progettuali e motivazioni di sicurezza

### User-space non fidato

Ogni dato proveniente dallo user-space viene validato nuovamente nel kernel.

### Copie esplicite

Si usano:

```c
copy_from_user()
copy_to_user()
```

e non dereferenziazioni dirette.

### Nomi a dimensione fissa

I nomi hanno un limite esplicito e devono essere terminati da NUL.

### Basename invece di percorso

Il registro non accetta `/`, riducendo ambiguità e complessità.

### Identità reale invece di `comm`

L’identità deriva dal file eseguibile associato all’address space.

### RCU per `exe_file`

Il campo `mm->exe_file` viene letto con le primitive previste dal kernel.

### Snapshot della dentry

Il nome viene copiato in modo stabile rispetto alle rinominazioni concorrenti.

### Reference counting

Ogni reference acquisita ha un rilascio corrispondente.

### Snapshot dei registri

Le copie vengono prodotte sotto mutex, ma trasferite allo user-space dopo il rilascio del lock.

### Nessuna lista parziale

Una capacità insufficiente produce `-ENOSPC` e la dimensione richiesta.

### Allocazioni limitate dallo stato kernel

La capacità dichiarata dall’utente non determina direttamente l’allocazione.

### Azzeramento dei record

I record vengono azzerati per evitare la fuoriuscita di memoria kernel non inizializzata.

### Limite ai retry

Lo user-space non ripete indefinitamente una lista in presenza di modifiche concorrenti.

### Logging fuori dal percorso frequente

Le funzioni che verranno usate per ogni system call non emettono log ordinari.

---

## 29. Registro dei numeri di system call

Il registro delle system call è implementato in:

```text
module/syscall_registry.c
module/syscall_registry.h
```

### Obiettivo

Il registro indica quali numeri di system call sono considerate critiche dal monitor.

La condizione di attivazione usata dal wrapper M8 è:

```c
st_monitor_is_enabled() &&
st_syscall_registry_contains(syscall_nr) &&
(st_uid_registry_contains(current_euid()) ||
 st_program_registry_contains_current())
```

La M6 ha introdotto il registro; la M8 lo consulta direttamente nel callback Ftrace e nel wrapper generico.

---

## 30. Dominio x86-64 e limite architetturale

Il modulo include:

```c
#include <asm/unistd.h>
```

e usa:

```c
NR_syscalls
```

Sul kernel usato durante lo sviluppo:

```text
NR_syscalls = 472
```

Gli indici architetturalmente validi sono:

```text
0 ... 471
```

La M8 usa inoltre:

```c
#include <asm/syscalls_64.h>
```

per ricavare automaticamente numero, nome del simbolo e proprietà `nonreturning` di tutte le entry native x86-64.

Un numero può essere valido per l’ABI ma non supportato dalla policy del modulo. Il caso attuale è:

```text
176 → delete_module → -EOPNOTSUPP
```

La syscall è valida nell’ABI x86-64, ma viene esclusa perché attraversare il wrapper durante `rmmod` produrrebbe un’attesa circolare nel teardown.

Il limite non è hard-coded nel controller. Deriva dagli header del kernel contro cui il modulo viene compilato.

## 31. Rappresentazione tramite bitmap

Il registro usa:

```c
static DECLARE_BITMAP(st_syscall_bitmap, ST_SYSCALL_LIMIT);
static DEFINE_MUTEX(st_syscall_registry_lock);
static unsigned int st_syscall_entries_count;
```

Ogni bit rappresenta un numero:

```text
bit 0   → system call 0 registrata?
bit 1   → system call 1 registrata?
bit 39  → system call 39 registrata?
bit 257 → system call 257 registrata?
```

### Perché una bitmap

Il dominio è:

- numerico;
- limitato;
- noto durante la compilazione;
- piccolo.

La bitmap offre:

- matching in tempo costante;
- memoria fissa e prevedibile;
- nessuna allocazione per entry;
- nessuna frammentazione;
- nessuna lista concatenata;
- elenco naturalmente ordinato;
- teardown senza oggetti dinamici.

Con 472 posizioni sono necessari circa 59 byte, arrotondati alla granularità interna delle bitmap kernel.

---

## 32. Operazioni del registro

Il componente espone:

```c
st_syscall_registry_init()
st_syscall_registry_exit()
st_syscall_registry_add()
st_syscall_registry_remove()
st_syscall_registry_contains()
st_syscall_registry_count()
st_syscall_registry_snapshot()
```

### Inizializzazione

```c
bitmap_zero(st_syscall_bitmap, ST_SYSCALL_LIMIT);
st_syscall_entries_count = 0U;
```

### Aggiunta

```text
validazione number < NR_syscalls
    ↓
verifica del supporto
    ↓
delete_module → -EOPNOTSUPP
    ↓
mutex_lock
    ↓
duplicato → -EEXIST
    ↓
set_bit
    ↓
incremento del contatore
    ↓
mutex_unlock
```

### Rimozione

```text
validazione del range
    ↓
mutex_lock
    ↓
numero assente → -ENOENT
    ↓
clear_bit
    ↓
decremento del contatore
    ↓
mutex_unlock
```

### Matching

```c
bool st_syscall_registry_contains(unsigned int number);
```

usa `test_bit()` senza acquisire il mutex. Questa lettura è usata direttamente anche dal callback Ftrace, quindi deve restare lockless e non bloccante.

La rimozione accetta qualsiasi numero architetturalmente valido; per `delete_module` restituisce normalmente `-ENOENT`, perché l’aggiunta viene sempre rifiutata.

## 33. Sincronizzazione e lettura lockless

Aggiunta, rimozione, conteggio e snapshot usano il mutex.

Il mutex protegge l’invariante:

```text
numero di bit impostati == st_syscall_entries_count
```

La funzione `contains()` non usa il mutex.

### Motivazione

Le modifiche amministrative saranno rare, mentre il matching verrà potenzialmente eseguito per ogni system call.

Il modello è:

```text
aggiornamenti rari → mutex + set_bit/clear_bit
letture frequenti  → test_bit senza mutex
```

`set_bit()`, `clear_bit()` e `test_bit()` operano sul singolo bit con le primitive previste dal kernel.

Un lettore concorrente può osservare lo stato precedente oppure quello successivo a un aggiornamento; entrambi sono stati validi del registro.

Questa scelta evita nel futuro percorso frequente:

- attese su mutex;
- serializzazione tra CPU;
- scansioni lineari;
- allocazioni;
- overhead amministrativo.

---

## 34. UAPI del registro system call

Sono state aggiunte:

```c
struct st_syscall_request {
    __u32 number;
    __u32 reserved;
};

struct st_syscall_count {
    __u32 count;
    __u32 reserved;
};

struct st_syscall_list_request {
    __aligned_u64 numbers_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};
```

Comandi:

```c
ST_IOCTL_SYSCALL_ADD
ST_IOCTL_SYSCALL_REMOVE
ST_IOCTL_SYSCALL_GET_COUNT
ST_IOCTL_SYSCALL_LIST
```

Numerazione:

```text
0x30 → ADD
0x31 → REMOVE
0x32 → GET_COUNT
0x33 → LIST
```

### Direzioni

```text
ADD / REMOVE → _IOW
GET_COUNT    → _IOR
LIST         → _IOWR
```

`numbers_ptr` è un `__aligned_u64`, non un puntatore nativo, per mantenere dimensione e allineamento stabili nell’UAPI.

Nel kernel viene convertito con:

```c
u64_to_user_ptr()
```

---

## 35. Aggiunta e rimozione tramite ioctl

Le operazioni di modifica sono riservate a effective UID zero.

Il percorso è:

```text
controllo privilegi
    ↓
copy_from_user
    ↓
reserved == 0
    ↓
validazione del numero
    ↓
aggiornamento del registro
```

Il controllo root avviene prima di `copy_from_user()`.

### Errori di aggiunta

```text
-EPERM      privilegi insufficienti
-EFAULT     richiesta user-space non accessibile
-EINVAL     numero o reserved non valido
-EEXIST     numero già registrato
-EOPNOTSUPP syscall valida ma non controllabile in sicurezza
```

Attualmente `-EOPNOTSUPP` è restituito per:

```text
__NR_delete_module = 176
```

Il device scrive un log specifico e `stctl` spiega che `delete_module` è necessaria alla rimozione sicura del modulo.

### Errori di rimozione

```text
-EPERM  privilegi insufficienti
-EFAULT richiesta user-space non accessibile
-EINVAL numero o reserved non valido
-ENOENT numero non registrato
```

Non è previsto `-ENOMEM`, perché la bitmap non alloca una entry dinamica per ogni numero.

## 36. Conteggio delle system call

Il comando:

```bash
./stctl syscall-count
```

usa:

```c
ST_IOCTL_SYSCALL_GET_COUNT
```

Il kernel restituisce:

```c
struct st_syscall_count {
    .count = st_syscall_registry_count(),
    .reserved = 0U,
};
```

Il conteggio è pubblico.

La lettura del contatore avviene sotto mutex, quindi non osserva una modifica parziale tra bit e contatore.

---

## 37. Snapshot della bitmap

La funzione:

```c
int st_syscall_registry_snapshot(__u32 *numbers,
                                 __u32 capacity,
                                 __u32 *count);
```

esegue sotto mutex:

```text
lettura del conteggio
    ↓
verifica della capacità
    ↓
for_each_set_bit()
    ↓
copia dei numeri nel buffer kernel
```

`for_each_set_bit()` visita i bit in ordine crescente.

Esempio:

```text
ordine di inserimento: 257, 39, 1, 0
snapshot:              0, 1, 39, 257
```

### Nessuna lista parziale

Se:

```text
required = 3
capacity = 2
```

la funzione restituisce:

```text
-ENOSPC
*count = 3
```

e non copia i primi due elementi.

### Controlli dell’invariante

La funzione verifica che:

```text
elementi attraversati == contatore
```

Una violazione restituisce `-EOVERFLOW` invece di produrre dati incoerenti o scrivere oltre il buffer.

---

## 38. Handler `SYSCALL_LIST`

Il driver riceve:

```c
struct st_syscall_list_request
```

e applica:

```text
copy_from_user della richiesta
    ↓
validazione reserved
    ↓
validazione puntatore/capacità
    ↓
lettura del conteggio reale
    ↓
allocazione del buffer kernel
    ↓
snapshot
    ↓
copy_to_user dell’array
    ↓
copy_to_user dei metadati
```

### Buffer kernel intermedio

Lo snapshot non scrive direttamente nello user-space.

Questo evita:

- page fault sotto il mutex;
- accessi user-space dentro la sezione critica;
- lock mantenuto durante `copy_to_user()`;
- accoppiamento tra registro e memoria utente.

### Allocazione non controllata dall’utente

Il driver alloca in base a:

```c
required = st_syscall_registry_count();
```

e non a:

```c
request.capacity
```

Inoltre passa allo snapshot la dimensione reale del buffer kernel:

```c
st_syscall_registry_snapshot(numbers, required, &actual);
```

Questo impedisce che una capacità arbitraria induca un’allocazione enorme o faccia credere allo snapshot di possedere più spazio di quello realmente allocato.

---

## 39. Concorrenza tra conteggio e lista

Tra `GET_COUNT` e `LIST`, oppure tra il conteggio interno e lo snapshot, il registro può cambiare.

### Crescita

```text
conteggio iniziale = 2
buffer kernel da 2
aggiunta concorrente
snapshot richiede 3
```

Il driver:

```text
libera il buffer
aggiorna request.count = 3
restituisce -ENOSPC
```

`stctl` rialloca e ripete.

### Riduzione

```text
conteggio iniziale = 3
buffer kernel da 3
rimozione concorrente
snapshot produce 2
```

Il buffer rimane sufficiente e vengono copiati soltanto i due elementi effettivi.

### Retry limitati

`stctl` esegue al massimo quattro tentativi.

Questo evita un ciclo infinito se il registro continua a crescere durante la consultazione.

---

## 40. Validazione user-space dei numeri

`stctl` usa una funzione dedicata:

```c
parse_syscall_number()
```

Sono accettate soltanto cifre decimali.

Sono rifiutati:

```text
-1
+1
 39
39abc
12.5
0x27
4294967296
```

La conversione usa:

```c
strtoull()
```

con controllo di:

- stringa vuota;
- caratteri residui;
- `ERANGE`;
- superamento di `UINT32_MAX`.

Il limite `NR_syscalls` resta nel kernel, così il controller non contiene il valore hard-coded del kernel corrente.

---

## 41. Comandi M6 disponibili

```bash
sudo ./stctl syscall-add <numero>
sudo ./stctl syscall-remove <numero>

./stctl syscall-count
./stctl syscall-list
```

Esempio:

```bash
sudo ./stctl syscall-add 257
sudo ./stctl syscall-add 39
sudo ./stctl syscall-add 1
sudo ./stctl syscall-add 0

./stctl syscall-list
```

Output:

```text
System call registrate: 4
  0
  1
  39
  257
```

---

## 42. Test eseguiti nella M6

Sono stati verificati:

### Ciclo di vita

```text
inizializzazione limite=472
teardown ordinato
registro vuoto dopo un nuovo insmod
```

### Privilegi

```text
ADD senza root    → -EPERM
REMOVE senza root → -EPERM
COUNT/LIST        → accessibili senza root
```

### Aggiunta

```text
numero valido → successo
duplicato     → -EEXIST
472           → -EINVAL
```

### Rimozione

```text
numero presente → successo
numero assente  → -ENOENT
numero 472      → -EINVAL
```

### Limiti

```text
471 → ultimo numero valido
472 → primo numero non valido
```

### Contatore

```text
duplicato non incrementa
rimozione assente non decrementa
numero invalido non modifica il registro
```

### Lista

```text
registro vuoto
ordine crescente
coerenza dopo rimozione
coerenza con il conteggio
consultazione non privilegiata
```

### Compilazione

Sono state eseguite compilazioni pulite di:

```text
modulo kernel
controller stctl
```

con `git diff --check` senza errori.

I warning relativi alla differenza nominale del compilatore, alla versione di `pahole` e all’assenza di `vmlinux` per BTF non riguardano il codice della milestone e non impediscono la produzione del modulo.

---

## 43. Motivazioni di sicurezza della M6

### Bitmap limitata da `NR_syscalls`

Impedisce accessi fuori indice e si adatta al kernel corrente.

### Validazione duplicata

Lo user-space valida la sintassi; il kernel valida UAPI, privilegi e dominio architetturale.

### Controllo root nel kernel

Un client alternativo non può aggirare `stctl`.

### Controllo privilegi prima della copia

Un chiamante non autorizzato viene rifiutato prima di elaborare il puntatore fornito.

### Nessuna dereferenziazione diretta

Si usano esclusivamente `copy_from_user()` e `copy_to_user()`.

### Campi `reserved` obbligatoriamente a zero

Riducono ambiguità e permettono future estensioni compatibili.

### Nessuna allocazione per ADD/REMOVE

L’utente non può provocare allocazioni dinamiche registrando numeri.

### Aggiornamenti serializzati

Il mutex impedisce duplicati concorrenti, doppie rimozioni e contatori incoerenti.

### Matching lockless

Il futuro percorso frequente non dovrà attendere un mutex amministrativo.

### Nessuna lista parziale

Una capacità insufficiente produce `-ENOSPC` e la dimensione necessaria.

### Allocazione basata sullo stato kernel

`request.capacity` non determina direttamente la quantità di memoria allocata nel kernel.

### Snapshot fuori dallo user-space

La bitmap viene copiata in un buffer kernel sotto mutex; la copia utente avviene dopo il rilascio del lock.

### Retry limitati

Una configurazione in modifica continua non può bloccare indefinitamente il controller.

---

## 44. M7 — Obiettivo e semantica del rate limiter

La M7 introduce la configurazione del limite globale e la gestione della finestra temporale richiesta dalla traccia.

La policy scelta è un **budget globale** condiviso da tutte le system call rilevanti. Non esiste un contatore distinto per numero di system call, UID o programma.

La finestra è di tipo **fixed/tumbling window**: viene allineata all’avvio del rate limiter e viene riallineata al momento di un `MAX_SET` eseguito mentre il monitor è attivo. Non è allineata ai secondi del calendario.

La futura invocazione del rate limiter avverrà soltanto quando è vera la condizione:

```text
monitor attivo
AND system call registrata
AND (effective UID registrato OR programma registrato)
```

Una system call non registrata, oppure invocata da un’identità non registrata, non deve toccare il budget.

### Semantica di `MAX`

`MAX` indica il numero massimo di invocazioni rilevanti che possono essere ammesse nella finestra corrente di un secondo.

Esempio con:

```text
MAX = 3
```

```text
prima invocazione   → ALLOW, admitted 0 → 1
seconda invocazione → ALLOW, admitted 1 → 2
terza invocazione   → ALLOW, admitted 2 → 3
quarta invocazione  → THROTTLE, admitted resta 3
```

Le invocazioni eccedenti non incrementano il contatore.

### Semantica di `MAX == 0`

```text
monitor disattivato → nessun limite applicato
monitor attivo      → zero invocazioni rilevanti ammesse
```

Quindi `MAX == 0` non equivale a `DISABLE`.

### Persistenza della configurazione

`MAX`:

- parte da zero a ogni nuovo caricamento del modulo;
- può essere modificato anche con monitor disattivato;
- non viene perso durante `DISABLE`;
- viene riutilizzato al successivo `ENABLE`;
- viene azzerato soltanto da una nuova inizializzazione del modulo o da un esplicito `MAX_SET 0`.

---

## 45. Struttura interna del rate limiter

Il componente è implementato in:

```text
module/rate_limiter.c
module/rate_limiter.h
```

Lo stato globale è rappresentato concettualmente da:

```c
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
```

### Significato dei campi

```text
lock             → protegge lo stato composto
window_timer     → chiude e riapre periodicamente la finestra
wait_queue       → predisposta per i futuri thread bloccati
max_invocations  → limite configurato
admitted         → richieste ammesse nella finestra corrente
generation       → identificatore logico della finestra/stato
next_expiry      → scadenza in jiffies
running          → timer e rate limiter operativi
stopping         → teardown definitivo in corso
```

### Perché uno spinlock

Il medesimo stato viene toccato da:

- process context, durante `ENABLE`, `DISABLE`, `MAX_SET` e `try_acquire`;
- softirq context, durante la callback del timer.

Non è sufficiente un semplice contatore atomico, perché occorre mantenere coerenti insieme:

```text
MAX
admitted
generation
next_expiry
running
stopping
```

Nel process context viene usato:

```c
spin_lock_bh()
```

così la softirq locale non può interrompere il thread mentre detiene lo stesso lock.

Nella callback del timer viene usato:

```c
spin_lock()
```

perché l’esecuzione avviene già in softirq context.

---

## 46. Finestra temporale e callback del timer

La finestra ha durata:

```c
HZ
```

jiffies, corrispondenti a un secondo in base alla frequenza configurata del timer del kernel.

All’avvio:

```text
admitted = 0
generation++
next_expiry = jiffies + HZ
running = true
mod_timer(...)
```

Alla scadenza la callback:

```text
acquisisce lo spinlock
verifica running && !stopping
azzera admitted
incrementa generation
calcola la scadenza successiva
riarma il timer
rilascia lo spinlock
wake_up_all(wait_queue)
```

### Finestre periodiche

La scadenza successiva è normalmente calcolata a partire dalla precedente:

```text
next_expiry += HZ
```

Questo evita che piccoli ritardi della callback si accumulino progressivamente spostando tutte le finestre.

Se la callback è arrivata così tardi che la nuova scadenza sarebbe già trascorsa:

```text
next_expiry = jiffies + HZ
```

In questo modo non viene riarmato un timer nel passato.

### Generazione

`generation` aumenta quando:

- parte il rate limiter;
- scade una finestra;
- cambia `MAX`;
- il rate limiter viene arrestato;
- inizia il teardown.

Il valore consentirà a un futuro thread in attesa di capire che la condizione osservata è cambiata e che deve riprovare l’ammissione.

---

## 47. Configurazione e consultazione di `MAX`

### Setter kernel

```c
void st_rate_limiter_set_max(u64 max_invocations);
```

L’operazione:

```text
aggiorna max_invocations
azzera admitted
incrementa generation
se running:
    next_expiry = jiffies + HZ
    mod_timer(...)
wake_up_all(wait_queue)
```

Quindi `MAX_SET` apre immediatamente una nuova finestra logica completa, senza ereditare il consumo della configurazione precedente.

### Getter kernel

```c
u64 st_rate_limiter_get_max(void);
```

La lettura è protetta dallo stesso spinlock e restituisce un valore coerente.

### Handler ioctl

`ST_IOCTL_MAX_SET`:

```text
controllo root
copy_from_user
reserved == 0
st_policy_lock
st_rate_limiter_set_max
unlock
```

Errori principali:

```text
-EPERM  effective UID diverso da zero
-EFAULT struttura user-space non accessibile
-EINVAL campi reserved non nulli
```

`ST_IOCTL_MAX_GET` è pubblico e restituisce una struttura completamente inizializzata.

### Controller user-space

Comandi:

```bash
sudo ./stctl max-set <valore>
./stctl max-get
```

Il parser:

- accetta soltanto cifre ASCII decimali;
- usa `strtoull()` in base 10;
- controlla `ERANGE` e caratteri residui;
- accetta `0`;
- accetta esattamente `UINT64_MAX`;
- rifiuta segni, notazione esadecimale, decimali, suffissi e overflow.

---

## 48. Transizioni amministrative

Le operazioni:

```text
ENABLE
DISABLE
MAX_SET
```

sono serializzate da:

```c
static DEFINE_MUTEX(st_policy_lock);
```

Questo mutex non protegge il percorso frequente delle system call. Protegge soltanto le rare modifiche amministrative della policy globale.

### `ENABLE`

```text
controllo root
lock policy
se già attivo:
    successo idempotente
altrimenti:
    st_rate_limiter_start()
    st_monitor_enable()
unlock
```

Il timer è quindi armato prima che il monitor sia visibile come attivo.

Se il componente è già entrato nel teardown definitivo (`stopping == true`), l’avvio viene rifiutato con `-ESHUTDOWN`.

### `DISABLE`

```text
controllo root
lock policy
se già disattivato:
    successo idempotente
altrimenti:
    st_monitor_disable()
    st_rate_limiter_stop()
unlock
```

Il monitor viene reso inattivo prima dell’arresto del timer.

### Race gestita da `BYPASS`

Può comunque accadere che un thread abbia letto il monitor come attivo immediatamente prima di un `DISABLE` e raggiunga il rate limiter dopo l’arresto.

Per questo `try_acquire()` restituisce:

```text
BYPASS
```

quando `running == false`, permettendo alla chiamata di proseguire senza essere bloccata da una policy ormai disattivata.

---

## 49. Algoritmo di ammissione

L’API predisposta per il futuro hook è:

```c
enum st_rate_limiter_decision
st_rate_limiter_try_acquire(u64 *window_generation);
```

Le decisioni sono:

```c
ST_RATE_LIMITER_BYPASS
ST_RATE_LIMITER_ALLOW
ST_RATE_LIMITER_THROTTLE
ST_RATE_LIMITER_SHUTDOWN
```

Il comportamento è:

```text
stopping == true
    → SHUTDOWN

running == false
    → BYPASS

admitted < max_invocations
    → admitted++
    → ALLOW

admitted >= max_invocations
    → admitted invariato
    → THROTTLE
```

La generazione osservata viene restituita tramite il parametro opzionale `window_generation`.

### Proprietà

L’intera decisione è eseguita sotto spinlock, quindi su più CPU non può verificarsi:

```text
due thread osservano admitted = MAX - 1
e vengono entrambi ammessi
```

Il contatore registra soltanto le richieste effettivamente ammesse, non i tentativi eccedenti.

### Integrazione completata nella M8

`THROTTLE` viene ora consumato dal wrapper generico.

Il thread:

1. conserva la generazione osservata;
2. dorme in modo interruptible sulla wait queue;
3. si sveglia dopo una nuova finestra, `MAX_SET`, `DISABLE`, teardown o segnale;
4. riprova atomicamente l’ammissione;
5. ritorna con errore quando l’attesa viene interrotta da un segnale.

---

## 50. Arresto temporaneo e teardown definitivo

### Arresto riavviabile

```c
st_rate_limiter_stop();
```

imposta sotto lock:

```text
running = false
admitted = 0
generation++
next_expiry = 0
```

poi:

```text
wake_up_all(wait_queue)
timer_delete_sync(window_timer)
```

`timer_delete_sync()` viene invocato fuori dallo spinlock. In caso contrario si potrebbe creare un deadlock aspettando una callback che necessita dello stesso lock per terminare.

Questa operazione è riavviabile: un successivo `ENABLE` può armare nuovamente il timer.

### Teardown definitivo

```c
st_rate_limiter_exit();
```

imposta:

```text
running = false
stopping = true
admitted = 0
generation++
```

poi sveglia la wait queue e usa:

```c
timer_shutdown_sync()
```

A differenza di `timer_delete_sync()`, questa primitiva impedisce anche futuri tentativi di riarmo durante il teardown.

---

## 51. Test della configurazione `MAX`

Sono stati verificati:

### Stato iniziale

```text
MAX_GET dopo insmod → 0
```

### Privilegi

```text
MAX_SET senza root → -EPERM
MAX invariato
MAX_GET senza root → consentito
```

### Valori

```text
10         → memorizzato correttamente
0          → accettato
UINT64_MAX → round-trip esatto
```

### Input non validi nello user-space

```text
-1
+1
1.5
0x10
10abc
overflow oltre UINT64_MAX
```

sono stati rifiutati prima dell’`ioctl`.

### Persistenza attraverso lo stato del monitor

È stato verificato:

```text
MAX_SET 5
ENABLE
DISABLE
MAX_GET → 5
```

---

## 52. Test della finestra e dell’ammissione

Durante lo sviluppo, `PING` è stato temporaneamente collegato a `st_rate_limiter_try_acquire()` per osservare la logica prima dell’intercettazione reale.

La strumentazione produceva:

```text
ALLOW    → successo
THROTTLE → -EAGAIN
BYPASS   → successo
```

### Test sequenziale con `MAX = 3`

È stata osservata nella stessa generazione:

```text
ALLOW
ALLOW
ALLOW
THROTTLE
```

Dopo una nuova finestra:

```text
ALLOW con generation maggiore
```

### Test con `MAX = 0`

È stato verificato:

```text
THROTTLE
THROTTLE
THROTTLE
```

anche dopo la scadenza di una nuova finestra.

### Reset tramite `MAX_SET`

Con:

```text
MAX 0 → MAX 2
```

il risultato immediato è stato:

```text
ALLOW
ALLOW
THROTTLE
```

Senza attendere il timer, cambiando:

```text
MAX 2 → MAX 1
```

è stato osservato:

```text
ALLOW
THROTTLE
```

Questo conferma che `MAX_SET` azzera il budget e incrementa la generazione.

### Rimozione della strumentazione

Terminati i test, `PING` è stato ripristinato come semplice controllo del device.

Con:

```text
monitor attivo
MAX = 0
```

`PING` restituisce comunque successo e non consulta più il rate limiter.

---

## 53. Test multicore del limite globale

È stato realizzato un test temporaneo con:

```text
64 thread
barriera pthread
MAX = 10
rilascio simultaneo
```

Risultato:

```text
Thread totali: 64
ALLOW: 10
THROTTLE: 54
Errori inattesi: 0
```

Il test dimostra che lo spinlock rende atomica la verifica e l’incremento di `admitted`, impedendo il superamento di `MAX` anche con richieste concorrenti su più CPU.

Il sorgente dipendeva dalla strumentazione temporanea di `PING` ed è stato rimosso dopo la prova. Verrà sostituito in M8/M9 da un test end-to-end su system call realmente intercettate.

---

## 54. Test concorrente permanente delle transizioni

La directory:

```text
tests/
```

contiene:

```text
tests/Makefile
tests/policy_transition_stress.c
```

Il test crea sei thread concorrenti:

```text
2 thread → ENABLE / DISABLE
2 thread → MAX_SET
1 thread → MAX_GET
1 thread → GET_STATUS
```

Ogni thread esegue cento operazioni, per un totale di:

```text
600 operazioni concorrenti
```

Risultato verificato:

```text
Thread concorrenti: 6
Operazioni per thread: 100
Operazioni concorrenti totali: 600
Errori durante lo stress: 0
Stato finale verificato: monitor disattivato, MAX=7.
TEST SUPERATO
```

Il test è eseguito con `timeout 30s`, così un eventuale deadlock produce un fallimento osservabile invece di bloccare indefinitamente la sessione.

La verifica finale normalizza e controlla entrambi gli stati:

```text
MAX = 7, monitor attivo
DISABLE
MAX = 7, monitor disattivato
```

---

## 55. Test del timer e del teardown

Sono stati eseguiti due scenari.

### Arresto tramite `DISABLE`

Ordine osservato:

```text
monitor disattivato
rate limiter arrestato
```

Un secondo `DISABLE` è risultato idempotente.

### `rmmod` con monitor attivo

Il modulo è stato rimosso direttamente con:

```text
monitor attivo
timer periodico armato
nessun DISABLE preventivo
```

Ordine finale osservato:

```text
device deregistrato
rate limiter rilasciato
registro system call rilasciato
registro programmi rilasciato
registro UID rilasciato
stato del monitor rilasciato
modulo rimosso
```

La rimozione è terminata con successo e non sono comparsi:

```text
BUG
Oops
deadlock
use-after-free
timer warning
general protection fault
```

Il warning RCU già presente con timestamp prossimo al boot è stato verificato come precedente e non correlato al modulo.

### Verifica finale di compilazione

Sono state completate compilazioni pulite di:

```text
module/
user/
tests/
```

con:

```bash
git diff --check
```

senza errori. I warning relativi alla denominazione del compilatore, alla versione di `pahole` e all’assenza di `vmlinux` per la generazione BTF sono esterni ai sorgenti del progetto.

---

## 56. Motivazioni di sicurezza e robustezza della M7

### Stato composto protetto da spinlock

Evita incoerenze tra limite, contatore, generazione e timer.

### Distinzione tra process context e softirq

`spin_lock_bh()` impedisce il deadlock con la callback locale del timer.

### Nessuna saturazione del contatore per tentativi eccedenti

`admitted` viene incrementato soltanto nel ramo `ALLOW`.

### Serializzazione amministrativa

`st_policy_lock` evita interleaving incoerenti tra `ENABLE`, `DISABLE` e `MAX_SET`.

### Ordine sicuro delle transizioni

Il timer viene avviato prima di pubblicare il monitor come attivo; il monitor viene disattivato prima di arrestare il timer.

### Operazioni idempotenti

Comandi ripetuti non creano timer duplicati e non azzerano accidentalmente il budget.

### Wake-up fuori dallo spinlock

Riduce la sezione critica ed evita di coinvolgere il codice della wait queue mentre lo stato interno è bloccato.

### Cancellazione sincrona fuori dal lock

Evita deadlock con una callback già in esecuzione.

### Distinzione stop/shutdown

`timer_delete_sync()` permette il riavvio dopo `DISABLE`; `timer_shutdown_sync()` protegge il teardown definitivo.

### `MAX` a dimensione fissa

L’UAPI usa `__aligned_u64`, validazione dei campi `reserved` e strutture completamente inizializzate.

### Parsing restrittivo nello user-space

Riduce input ambigui, pur mantenendo la validazione definitiva nel kernel.

### Nessun logging periodico o per decisione

Evita una crescita incontrollata del kernel log nel percorso temporale e nel futuro hot path.

### Test concorrenti

Il limite globale e la policy amministrativa sono stati verificati sotto concorrenza reale, non soltanto con comandi sequenziali.

---

## 57. M8 — Obiettivo e risultato

La M8 aveva come obiettivo iniziale l’intercettazione delle system call. Durante lo sviluppo è stata completata anche l’integrazione end-to-end con il rate limiter: il modulo non si limita a osservare le chiamate, ma può sospendere realmente il thread prima dell’esecuzione della syscall, risvegliarlo e propagare correttamente i segnali.

Il risultato finale è un data path generale per le syscall native x86-64:

```text
funzione __x64_sys_*
    ↓ Ftrace
callback non bloccante
    ↓ redirect
wrapper generico
    ↓
monitor AND syscall AND (UID OR programma)
    ↓
rate limiter
    ├── ALLOW/BYPASS/SHUTDOWN → syscall reale
    └── THROTTLE → wait interruptible → retry
```

La M8 non modifica la `sys_call_table`. Tutti i redirect sono gestiti tramite l’API Ftrace del kernel.

---

## 58. Inventario automatico delle syscall x86-64

La precedente implementazione sperimentale usava pochi target manuali. La soluzione definitiva genera l’inventario da:

```c
#include <asm/syscalls_64.h>
```

Prima dell’inclusione vengono ridefinite:

```c
__SYSCALL(number, symbol)
__SYSCALL_NORETURN(number, symbol)
```

Ogni voce produce:

```c
struct st_syscall_definition {
    unsigned int syscall_nr;
    const char *symbol_name;
    bool nonreturning;
};
```

Il prefisso del simbolo viene costruito come:

```text
"__x64_" + nome generato dal kernel
```

Sul kernel di sviluppo sono state ottenute:

```text
definizioni = 472
entry esplicite = 472
entry implicite = 0
entry nonreturning = 2
```

Le due entry non ritornanti sono:

```text
60  → exit
231 → exit_group
```

La tabella runtime è:

```c
struct st_hook_target {
    const char *symbol_name;
    unsigned long original_ip;
    bool nonreturning;
    bool explicit_entry;
};

static struct st_hook_target st_hook_targets[NR_syscalls];
```

Il numero della syscall viene usato direttamente come indice. Il lookup è quindi O(1).

---

## 59. Preparazione e risoluzione dei target

`st_hook_targets_prepare()` inizializza tutti gli slot con una fallback e sovrascrive le entry presenti nella tabella generata.

La funzione controlla:

- numeri fuori range;
- definizioni duplicate;
- nomi di simbolo mancanti;
- coerenza tra dimensione dell’inventario e `NR_syscalls`.

Gli indirizzi delle funzioni kernel vengono risolti mediante una Kprobe temporanea:

```text
register_kprobe(nome)
    ↓
lettura di probe.addr
    ↓
unregister_kprobe()
```

La Kprobe non viene usata nel data path. Serve soltanto durante `insmod` per ottenere gli indirizzi dei simboli non esportati direttamente al modulo.

Se più numeri usano lo stesso nome di simbolo, l’indirizzo già risolto viene riutilizzato evitando registrazioni Kprobe ripetute.

Un fallimento nella risoluzione impedisce il caricamento dell’hook e attiva il rollback delle risorse già preparate.

---

## 60. Deduplicazione dei filtri Ftrace

Più numeri di syscall possono condividere la stessa implementazione o simboli differenti possono risolversi allo stesso indirizzo.

Per questo viene costruito un insieme separato:

```c
struct st_hook_filter {
    unsigned long original_ip;
    bool installed;
};
```

La deduplicazione avviene per `original_ip`, non soltanto per nome.

Risultato sul kernel di sviluppo:

```text
472 target logici
369 indirizzi Ftrace distinti
```

Ogni numero conserva la propria entry in `st_hook_targets`, ma ogni indirizzo viene inserito una sola volta nel filtro Ftrace.

La flag `installed` permette un rollback preciso se l’installazione fallisce a metà.

---

## 61. Installazione Ftrace

Per ogni indirizzo distinto viene chiamato:

```c
ftrace_set_filter_ip(
    &st_syscall_ftrace_ops,
    original_ip,
    0,
    0
);
```

Dopo avere installato tutti i filtri viene registrata un’unica callback:

```c
register_ftrace_function(&st_syscall_ftrace_ops);
```

Le flag sono:

```c
FTRACE_OPS_FL_SAVE_REGS
FTRACE_OPS_FL_RECURSION
FTRACE_OPS_FL_IPMODIFY
```

Significato:

```text
SAVE_REGS → rende disponibile un frame completo dei registri
RECURSION → abilita la protezione Ftrace dalla ricorsione
IPMODIFY  → autorizza la modifica dell’instruction pointer
```

`IPMODIFY` consente di sostituire temporaneamente la funzione originale con il wrapper generico.

---

## 62. Callback Ftrace e validazione dell’ABI

La callback:

```c
st_ftrace_callback(...)
```

viene eseguita in un contesto in cui non deve dormire. Per questo effettua soltanto controlli lockless e manipolazione dei registri.

### Controllo della ricorsione

Quando il wrapper richiama la funzione originale, la funzione passa nuovamente per Ftrace. La callback riconosce che il chiamante appartiene al modulo:

```c
within_module(parent_ip, THIS_MODULE)
```

e non effettua un secondo redirect.

### Controllo del teardown

```c
READ_ONCE(st_hook_accepting_calls)
```

impedisce nuovi ingressi nel wrapper dopo l’inizio dell’unload.

### Validazione del frame

La callback legge il primo argomento C del wrapper `__x64_sys_*` e verifica:

```c
syscall_regs == current_pt_regs()
```

Questo esclude alias o chiamate interne al kernel che raggiungono lo stesso indirizzo con una firma differente.

### Validazione dell’ABI

Sono accettate soltanto chiamate:

```text
provenienti da user mode
in modalità nativa a 64 bit
non IA-32
non x32
```

I controlli usano:

```c
user_mode()
user_64bit_mode()
in_32bit_syscall()
```

### Numero della syscall

Il numero viene letto da:

```c
syscall_regs->orig_ax
```

La conversione a `unsigned int` deve essere esatta e il numero deve essere minore di `NR_syscalls`.

### Fast path di bypass

La callback evita il redirect quando:

```text
delete_module
OR monitor spento
OR syscall non registrata
```

`st_monitor_is_enabled()` usa una lettura `READ_ONCE`; `st_syscall_registry_contains()` usa `test_bit()`. Nessuna delle due operazioni dorme o acquisisce un mutex.

---

## 63. Trasporto dell’indirizzo originale e wrapper generico

Il wrapper ha firma:

```c
static asmlinkage long notrace
st_generic_syscall_wrapper(
    const struct pt_regs *regs,
    unsigned long original_ip);
```

Il primo argomento, in `%rdi`, è già il puntatore ai registri della syscall.

La callback calcola l’indirizzo canonico con:

```c
ftrace_get_symaddr(ip)
```

e lo inserisce in `%rsi`, cioè nel secondo argomento secondo l’ABI x86-64 del kernel:

```c
kernel_regs->si = original_ip;
```

Infine modifica il RIP:

```c
ftrace_regs_set_instruction_pointer(
    fregs,
    (unsigned long)st_generic_syscall_wrapper
);
```

Prima del redirect incrementa:

```c
st_hook_active_calls
```

Il wrapper valida nuovamente la coppia:

```text
numero della syscall + original_ip
```

tramite un accesso diretto a `st_hook_targets[syscall_nr]`.

Se la coppia è incoerente viene restituito `-ENOSYS` e viene prodotto un solo log di errore.

---

## 64. Classificazione e integrazione con il rate limiter

Nel wrapper viene applicata la condizione:

```text
monitor attivo
AND syscall registrata
AND (effective UID registrato OR programma registrato)
```

Monitor e registro syscall vengono controllati una seconda volta per gestire le corse tra callback e modifiche amministrative.

Il matching dell’identità avviene soltanto nel wrapper, quindi nel process context del task corrente:

```c
st_uid_registry_contains(current_euid())
||
st_program_registry_contains_current()
```

Una chiamata non rilevante invoca immediatamente la funzione originale.

Una chiamata rilevante entra nel ciclo:

```text
try_acquire()
    ├── ALLOW
    ├── BYPASS
    ├── SHUTDOWN
    └── THROTTLE
```

Nei primi tre casi viene chiamata la vera funzione:

```c
original_syscall(regs)
```

Nel caso `THROTTLE` il thread attende sulla wait queue.

---

## 65. Blocco, risveglio, retry e segnali

La M8 completa il comportamento che nella M7 era soltanto predisposto.

Il thread eccedente chiama:

```c
st_rate_limiter_wait_for_change(observed_generation)
```

L’attesa è interruptible.

Il thread può essere risvegliato da:

- apertura di una nuova finestra;
- `MAX_SET`;
- `DISABLE`;
- teardown del rate limiter;
- un segnale pendente.

Dopo un wake-up ordinario il budget non viene assegnato automaticamente. Il thread torna a:

```c
st_rate_limiter_try_acquire()
```

e compete nuovamente con gli altri waiter.

Questo evita di superare `MAX` quando molti thread vengono svegliati insieme.

Se l’attesa viene interrotta da un segnale:

```text
la syscall originale non viene eseguita
il wrapper restituisce l’errore
la libc espone EINTR quando appropriato
active_calls viene rilasciato
```

Il test con `SIGUSR1` ha verificato:

```text
wchan = st_rate_limiter_wait_for_change
syscall_result = -1
errno = EINTR
signal_received = 1
wait_interrotte = 1
```

---

## 66. Protezione delle chiamate attive e teardown

Ogni redirect incrementa:

```c
atomic_inc(&st_hook_active_calls);
```

Ogni percorso normale di uscita esegue:

```c
st_hook_active_call_put();
```

che decrementa il contatore e risveglia l’unloader quando arriva a zero.

Durante `st_syscall_hook_exit()`:

```text
accepting_calls = false
    ↓
unregister callback Ftrace
    ↓
rimozione dei 369 filtri
    ↓
wait_event(active_calls == 0)
```

Questa sequenza impedisce che il codice o le tabelle del modulo vengano liberati mentre un thread:

- si trova nel wrapper;
- dorme nel rate limiter;
- sta richiamando la funzione originale;
- sta terminando tramite `exit` o `exit_group`.

Il rate limiter viene posto in `SHUTDOWN` prima dell’attesa dell’hook, così i waiter vengono liberati e possono completare.

Il test `rmmod` con un waiter attivo ha verificato:

```text
waiter bloccato nella wait queue
rmmod_status = 0
waiter_status = 0
limiter_shutdown = 1
filtri rimossi = 369/369
module_removed = yes
```

---

## 67. Gestione di `exit` ed `exit_group`

`exit` ed `exit_group` non ritornano al wrapper. Senza una gestione speciale, il loro ingresso incrementerebbe `active_calls` senza poterlo decrementare.

La tabella generata dal kernel le marca tramite:

```c
__SYSCALL_NORETURN
```

Prima di chiamare l’originale, il wrapper crea un record sul proprio stack:

```c
struct st_nonreturning_call {
    struct list_head link;
    struct task_struct *task;
};
```

Il record viene inserito in una lista globale protetta da spinlock.

Il modulo registra il tracepoint:

```text
sched_process_exit
```

Quando il task termina, la callback del tracepoint:

- cerca il record associato al task;
- lo rimuove dalla lista;
- incrementa il contatore delle chiamate completate;
- rilascia `active_calls`.

Il tracepoint resta attivo fino a quando tutte le chiamate wrappate sono terminate.

La regressione ha verificato:

```text
exit status = 17
exit_group status = 23
nonreturning avviate = 5
nonreturning completate = 5
ritorni inattesi = 0
```

---

## 68. Esclusione corrente di `delete_module`

`delete_module`, numero x86-64 `176`, è la syscall usata da `rmmod`.

Se venisse deviata nel wrapper, la sequenza sarebbe:

```text
delete_module
    ↓
active_calls++
    ↓
module_exit()
    ↓
attesa active_calls == 0
```

La stessa `delete_module` potrebbe decrementare il contatore soltanto dopo il ritorno di `module_exit()`. Si produrrebbe quindi un self-deadlock.

La M8 introduce due livelli di protezione:

### Data path

La callback Ftrace esegue:

```c
if (syscall_nr == __NR_delete_module)
    return;
```

### Control plane

`st_syscall_registry_add()` restituisce:

```c
-EOPNOTSUPP
```

Il device produce un log specifico e `stctl` mostra un messaggio esplicativo.

Questo evita una configurazione silenziosamente inefficace.

---

## 69. Test permanenti e regressione finale

I test permanenti nel repository sono:

```text
policy_transition_stress
syscall_hook_smoke
syscall_hook_signal
```

### `policy_transition_stress`

Verifica 600 operazioni amministrative concorrenti:

```text
6 thread
100 operazioni per thread
0 errori
stato finale: monitor disattivato, MAX=7
```

### `syscall_hook_smoke`

Esegue direttamente `SYS_nanosleep` e verifica il normale passaggio attraverso hook, classificazione e `ALLOW`.

### `syscall_hook_signal`

Configura `MAX=0`, blocca `SYS_nanosleep`, invia `SIGUSR1` e verifica il ritorno con `EINTR`.

### Test aggiuntivi della M8

Sono stati eseguiti anche:

- smoke test dell’intera tabella con monitor spento;
- pass-through di programmi ordinari;
- test program-scoped di `getuid`, syscall 102;
- blocco con `MAX=0` e risveglio dopo `MAX_SET 1`;
- test di `exit` ed `exit_group`;
- test di unload con waiter attivo;
- test del rifiuto di `delete_module`;
- verifica dell’inventario runtime;
- verifica della rimozione di tutti i filtri;
- controllo globale di `dmesg`.

Risultato finale:

```text
passes = 12
failures = 0
m8_final_regression = ok
```

Non sono comparsi:

```text
BUG
Oops
WARNING
hung task
use-after-free
scheduling while atomic
general protection fault
```

---

## 70. Motivazioni di sicurezza e robustezza della M8

### Nessuna modifica della `sys_call_table`

Il modulo usa API kernel dedicate invece di riscrivere una tabella globale in memoria.

### Inventario derivato dal kernel corrente

Riduce il rischio di numeri o nomi hard-coded obsoleti.

### Kprobe soltanto temporanee

La risoluzione dei simboli è separata dal data path permanente.

### Deduplicazione per indirizzo

Evita filtri duplicati su alias della stessa funzione.

### Callback non bloccante

Nel callback vengono usate soltanto letture lockless e manipolazione dei registri.

### Validazione del vero percorso syscall

Il confronto con `current_pt_regs()` e i controlli dell’ABI evitano di reinterpretare chiamate kernel interne.

### Verifica numero/indirizzo

Il wrapper non si fida soltanto del numero o soltanto dell’IP.

### Seconda verifica della policy

Gestisce le corse tra callback e `ENABLE`, `DISABLE`, registrazioni o rimozioni.

### Esecuzione nello stesso thread

La syscall reale conserva `current`, credenziali, address space, signal state e semantica originale.

### Attesa interruptible

Un segnale non lascia il task bloccato indefinitamente nel throttler.

### Reference delle chiamate attive

Il modulo non viene liberato mentre il wrapper è ancora in uso.

### Tracepoint per i target nonreturning

`exit` ed `exit_group` non causano perdite del contatore attivo.

### Wake-up prima del teardown dell’hook

I waiter vengono liberati prima dell’attesa di `active_calls`.

### Esclusione esplicita di `delete_module`

La limitazione è visibile sia nel data path sia nell’interfaccia amministrativa.

### Rollback di inizializzazione

Ogni filtro installato viene tracciato e rimosso in caso di errore.

---

## 71. Limiti attuali, punti deboli e priorità di hardening

La M9 completa le metriche obbligatorie, ma lascia alcuni rischi semantici, prestazionali e di portabilità da chiudere nella M10.

### Race tra `relevant_invocations` e `STATS_RESET`

Il wrapper registra prima l’invocazione rilevante e apre il contesto di blocco soltanto al primo `THROTTLE`. Un reset concorrente può separare i due eventi tra generazioni differenti e produrre temporaneamente:

```text
blocked_invocations > relevant_invocations
```

La soluzione raccomandata è un unico contesto per invocazione rilevante, con generazione catturata da `invocation_begin()` e riutilizzata da tutti gli eventi successivi.

### Semantica di `DISABLE` con waiter ancora in uscita

`DISABLE` congela l’osservazione, ma i waiter possono completare poco dopo. Se completano prima di un nuovo `ENABLE`, possono ancora aggiornare la vecchia sessione; se completano dopo l’apertura della nuova generazione, vengono ignorati.

La M10 deve scegliere una semantica deterministica:

1. **chiusura netta**: `DISABLE` incrementa subito la generazione e scarta gli eventi successivi;
2. **drain**: la sessione entra in stato `closing` e resta aperta fino alla fine dei contesti già attivi.

### Spinlock globale delle statistiche

Ogni invocazione rilevante usa lo stesso spinlock. Su molte CPU può diventare un punto di contesa.

Possibili miglioramenti:

- contatori per-CPU;
- aggregazione nello snapshot;
- `u64_stats_sync` o `seqcount`;
- lock globale riservato a `current_blocked`, peak e metadati.

### Registri UID e programmi

UID e programmi usano liste e mutex. Il costo include scansioni lineari e identificazione ripetuta dell’eseguibile.

Evoluzioni:

- hash table;
- RCU;
- strutture read-mostly;
- cache per `mm_struct`;
- invalidazione su `execve()` e teardown.

### Thundering herd e fairness

`wake_up_all()` preserva la correttezza tramite retry, ma può produrre forte contesa e non garantisce FIFO.

Da valutare:

- wait queue esclusiva;
- wake-up limitato al budget;
- ticketing;
- coda FIFO;
- misure di fairness e percentili.

### Fixed window e burst ai confini

Una fixed window può consentire fino a `2 × MAX` ammissioni ravvicinate attorno al confine tra due finestre. È coerente con la policy scelta, ma deve essere documentato.

Alternative future:

- token bucket;
- leaky bucket;
- sliding window;
- budget per syscall, UID o programma.

### Esposizione pubblica dei metadati

`STATS_GET` è pubblico e può rivelare basename, effective UID e syscall del peak. Con device `0666`, queste informazioni sono leggibili da utenti non privilegiati.

La M10 deve valutare lettura root-only dei campi identificativi, dati aggregati o permessi configurabili.

### Basename e collisioni

Due file con lo stesso basename sono indistinguibili. Estensioni possibili: device/inode, hash del file o identificatore opzionale più forte.

### Namespace

Privilegi e UID sono riferiti alla user namespace iniziale. Root in un container non equivale necessariamente a root dell’host. La scelta va documentata e testata.

### Compatibilità UAPI

La dimensione della struttura è codificata nel numero ioctl. Una futura v2 deve usare un nuovo ioctl oppure una richiesta con `size` e `version`; non si deve estendere in-place la struttura esistente.

### Precisione user-space

Il kernel usa interi. `stctl` converte in floating point. Su osservazioni estremamente lunghe può perdere precisione; è preferibile conservare anche l’output raw e valutare `long double` o divisione intera formattata.

### Test M9 non ancora nel repository

I sei script M9 sono nella home di sviluppo, non in `tests/`. È un debito tecnico prioritario.

La M10 deve versionarli, renderli autosufficienti, creare i binari di supporto, garantire cleanup e fornire un comando unico di regressione.

### Portabilità kernel

La soluzione è stata verificata su Linux `7.0.0-28-generic`, x86-64 e GCC 13.3. Mancano test su kernel LTS differenti, PREEMPT/RT, Clang e configurazioni senza alcune opzioni Ftrace/Kprobe.

### Scalabilità dell’installazione

La risoluzione di 472 target e l’installazione di 369 filtri richiedono alcuni secondi. Serve profiling di Kprobe, deduplicazione, filtri e unload.

### Fault injection e sanitizer

Mancano campagne sistematiche con KASAN, KCSAN, lockdep, KFENCE, kmemleak e fault injection su init, copy user e teardown.

### Syscall riavviate

Una syscall interrotta e riavviata rientra nel wrapper ed è conteggiata come una nuova invocazione. Questa semantica deve essere dichiarata nella relazione.

## 72. Possibile reinserimento futuro di `delete_module`

Il supporto di `delete_module` è lasciato intenzionalmente come attività futura opzionale.

Non è sufficiente rimuovere:

```c
if (syscall_nr == __NR_delete_module)
```

oppure il controllo `-EOPNOTSUPP`. Farlo ripristinerebbe il self-deadlock osservato durante lo sviluppo.

Un reinserimento sicuro richiede un ridisegno del rapporto tra hook e unload. Possibili direzioni da studiare:

1. **Percorso speciale di lifecycle**  
   Trattare `delete_module` separatamente dalle syscall ordinarie, evitando che la sua reference venga attesa dal `module_exit()` dello stesso modulo.

2. **Trampoline non appartenente al modulo in unload**  
   Spostare la parte che deve sopravvivere al ritorno di `module_exit()` in un componente built-in o in un modulo di supporto che non venga rimosso insieme al throttler.

3. **Coordinamento con una fase preventiva di disarmo**  
   Disabilitare e scollegare l’hook prima dell’esecuzione effettiva di `delete_module`. Questa soluzione renderebbe però difficile applicare un vero throttling alla stessa chiamata di unload.

4. **Architettura a due moduli**  
   Un modulo core stabile potrebbe possedere Ftrace e il wrapper, mentre il modulo di policy potrebbe essere rimosso separatamente.

5. **Verifica formale del lifetime**  
   Qualsiasi soluzione deve dimostrare che nessun instruction pointer, callback, stack frame o puntatore a funzione continua a riferirsi al modulo dopo l’inizio della liberazione.

Fino a quando una di queste architetture non sarà implementata e sottoposta a test specifici, la scelta sicura resta:

```text
delete_module valida nell’ABI
ma non registrabile nel throttler
```

Questa attività è facoltativa e può essere affrontata dopo il completamento delle funzionalità obbligatorie.

---

## 73. Prossimi passi

La M10 deve privilegiare correttezza e riproducibilità prima delle ottimizzazioni.

```text
1. chiudere la race relevant/reset
2. definire la semantica DISABLE/drain
3. migrare i test M9 in tests/
4. creare una regressione unica M1–M9
5. eseguire stress, fault injection e sanitizer
6. profilare hot path, init e unload
7. verificare più kernel
8. completare audit UAPI, namespace e privacy
9. redigere relazione e istruzioni di consegna
```

Priorità:

```text
P0  race statistiche e semantica DISABLE
P0  test M9 permanenti
P1  lockdep/KASAN/KCSAN e load/unload ripetuto
P1  profiling statistiche e registry
P1  audit UAPI e visibilità dei dati
P2  fairness e thundering herd
P2  portabilità multi-kernel
P3  IA-32/x32
OPT delete_module
```

La M10 è conclusa soltanto quando tutte le race note hanno una semantica testata, i test M9 sono nel repository, un comando unico esegue l’intera regressione e la relazione documenta proprietà e limitazioni.

## 74. Stato della roadmap

```text
[M1] Character device minimale                          COMPLETATO
[M2] UAPI e primo ioctl PING                            COMPLETATO
[M3] Stato ENABLE/DISABLE/GET_STATUS                    COMPLETATO
[M4] Registro UID                                       COMPLETATO
[M5] Registro nomi degli eseguibili                     COMPLETATO
[M6] Registro numeri di system call                     COMPLETATO
[M7] Configurazione MAX e finestra temporale            COMPLETATO
[M8] Hook Ftrace, throttling reale e teardown sicuro    COMPLETATO
[M9] Statistiche runtime e metriche di ritardo          COMPLETATO
[M10] Hardening, test permanenti e relazione            DA IMPLEMENTARE
[OPT] Reinserimento sicuro di delete_module             FUTURO OPZIONALE
```

Verifiche finali M9:

```text
regressione M9: 6/6 PASS
regressione M8 dopo M9: 12/12 PASS
errori kernel: nessuno
filtri rimossi: 369/369
modulo residuo: assente
```

---

## 75. M9 — Obiettivo e risultato

La M9 implementa le statistiche richieste dalla traccia senza floating point nel kernel e senza modificare la semantica di ammissione del rate limiter.

Metriche disponibili:

```text
durata di osservazione
tempo-thread complessivo in blocco
invocazioni rilevanti
invocazioni bloccate
invocazioni bloccate completate
attese interrotte
thread correntemente bloccati
picco dei thread bloccati
ritardo totale e medio
peak delay
syscall, EUID e programma del peak
stato della sessione
```

Il delay è misurato dal primo `THROTTLE` fino all’istante immediatamente precedente la syscall reale. Un’attesa interrotta non contribuisce al peak.

---

## 76. Sottosistema `statistics`

La M9 aggiunge:

```text
module/statistics.c
module/statistics.h
```

API:

```c
st_statistics_init()
st_statistics_exit()
st_statistics_session_start()
st_statistics_session_stop()
st_statistics_reset()
st_statistics_get_snapshot()
st_statistics_record_relevant_invocation()
st_statistics_block_begin()
st_statistics_block_complete()
st_statistics_block_interrupted()
```

Lo stato è protetto da uno spinlock dedicato. Incrementi e somme sono saturanti a `U64_MAX`.

---

## 77. Contesto di una invocazione bloccata

Il wrapper conserva sul proprio stack:

```c
struct st_statistics_block_context {
    u64 generation;
    u64 start_ns;
    kuid_t effective_uid;
    unsigned int syscall_nr;
    char program_name[ST_PROGRAM_NAME_CAPACITY];
    bool counted;
};
```

Il contesto viene aperto soltanto al primo `THROTTLE`. Più retry della stessa syscall producono una sola invocazione bloccata.

---

## 78. Media temporale dei waiter

Il kernel integra:

```text
blocked_thread_time_ns +=
    current_blocked ×
    (now - last_change)
```

Lo user-space calcola:

```text
average_blocked =
    blocked_thread_time_ns / observation_ns
```

La metrica rappresenta il numero medio di thread simultaneamente sospesi e può essere maggiore di uno.

---

## 79. Identità e peak delay

Se la rilevanza dipende dal programma, il basename già usato dal matching viene riutilizzato. Se dipende dall’UID, il basename viene acquisito soltanto al primo blocco.

`block_complete()` viene eseguita immediatamente prima della syscall originale e aggiorna:

```text
completed
total_delay
peak_delay
peak_syscall_nr
peak_euid
peak_program
peak_valid
```

Il peak cambia soltanto per un delay strettamente maggiore; in caso di parità resta il primo record.

---

## 80. Segnali

Un segnale durante la wait queue produce:

```text
current_blocked--
interrupted++
nessun completed
nessun total_delay
nessun peak
nessuna syscall originale
```

Il test con `SIGTERM` ha verificato `child_status=143`, `interrupted=1` e `current_blocked=0`.

---

## 81. Sessioni e reset

Una reale `ENABLE` apre una nuova generazione statistica. Un `ENABLE` idempotente non resetta.

`MAX_SET` non azzera le statistiche.

`DISABLE` chiude la sessione e congela l’osservazione.

`STATS_RESET`:

```text
root-only
serializzato da st_policy_lock
attivo   → azzera e resta attivo
inattivo → azzera e resta inattivo
waiter   → -EBUSY
```

---

## 82. Isolamento delle generazioni

Ogni nuova generazione parte con `current_blocked=0` e `peak_blocked=0`.

Un contesto appartenente a una vecchia generazione viene disarmato senza modificare la nuova. Il test con 32 waiter ha confermato che la nuova sessione resta completamente vuota anche mentre i vecchi waiter terminano.

---

## 83. Controller `stctl`

Comandi:

```bash
./stctl stats
sudo ./stctl stats-reset
```

`stats` valida campi riservati, coerenza dei contatori e terminazione del nome. Calcola media temporale e ritardo medio in user-space.

Esempio verificato:

```text
Invocazioni rilevanti: 1
Invocazioni bloccate: 1
Invocazioni bloccate completate: 1
Thread attualmente bloccati: 0
Picco thread bloccati: 1
Ritardo medio: circa 313–354 ms
Peak syscall: 102
Peak EUID: 1000
Peak programma: m9_getuid_once
```

---

## 84. Test M9

Sono stati eseguiti sei test:

```text
lifecycle delle sessioni
conteggio delle invocazioni rilevanti
singolo waiter e reset EBUSY
interruzione tramite segnale
quattro waiter concorrenti
isolamento DISABLE/ENABLE con 32 vecchi waiter
```

Risultato:

```text
passes=6
failures=0
m9_final_regression=ok
```

---

## 85. Regressione M8 dopo M9

La regressione M8 è stata rieseguita integralmente.

```text
passes=12
failures=0
m8_final_regression=ok
```

Sono rimaste valide:

```text
472 target
369 filtri unici
2 syscall nonreturning
rimozione 369/369
nessun BUG/Oops/WARNING correlato
```

---

## 86. Proprietà garantite al termine della M9

Nell’ambiente verificato:

- intercettazione generale native x86-64;
- throttling prima della syscall;
- esecuzione nello stesso thread;
- retry atomico;
- gestione segnali;
- teardown con waiter e syscall nonreturning;
- snapshot coerente;
- nessun floating point kernel;
- conteggio singolo per invocazione bloccata;
- peak soltanto per chiamate realmente eseguite;
- associazione del peak a syscall, EUID e basename;
- media temporale basata sull’area;
- reset protetto;
- isolamento delle vecchie generazioni;
- assenza di regressioni M8/M9.

Non sono ancora garantiti:

- fairness FIFO;
- scalabilità lineare su molte CPU;
- portabilità oltre il kernel testato;
- chiusura della race `relevant/reset`;
- semantica definitiva della coda dopo `DISABLE`;
- riservatezza dei metadati statistici;
- IA-32/x32;
- `delete_module`.
