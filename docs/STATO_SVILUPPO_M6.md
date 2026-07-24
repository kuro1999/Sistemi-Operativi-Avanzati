# Syscall Throttle — Stato dello sviluppo

Questo documento descrive lo stato corrente del progetto **Syscall Throttle**, le funzionalità già implementate, l’architettura adottata, le principali decisioni progettuali, le misure di sicurezza e i test eseguiti.

L’obiettivo finale è realizzare un Linux Kernel Module capace di configurare e applicare un meccanismo di throttling sulle system call.

Al momento sono state completate:

- l’infrastruttura di comunicazione user-space/kernel;
- la gestione dello stato attivo/disattivo del monitor;
- la gestione completa del registro degli UID;
- la gestione completa del registro dei nomi degli eseguibili;
- l’identificazione sicura del basename dell’eseguibile associato al task corrente;
- il matching tra eseguibile corrente e registro dei programmi;
- la gestione completa del registro dei numeri di system call x86-64;
- il matching lockless di un numero di system call tramite bitmap.

---

## 1. Stato attuale del progetto

Sono già disponibili:

- un Linux Kernel Module multi-file;
- un character device `/dev/syscall_throttle`;
- registrazione e deregistrazione del device tramite `miscdevice`;
- collegamento del device al VFS tramite `struct file_operations`;
- protezione dalla rimozione del modulo mentre il device è aperto;
- una UAPI condivisa tra kernel e user-space;
- comunicazione tramite `ioctl()`;
- comando di test `PING`;
- stato persistente del monitor: attivo oppure disattivato;
- comandi `ENABLE`, `DISABLE` e `GET_STATUS`;
- controllo dei privilegi basato sull’effective UID;
- trasferimenti user-space/kernel con `copy_from_user()` e `copy_to_user()`;
- controller user-space `stctl`;
- registro kernel degli UID;
- registro kernel dei nomi dei programmi;
- registro kernel dei numeri di system call x86-64;
- bitmap dimensionata automaticamente con `NR_syscalls`;
- sincronizzazione degli aggiornamenti tramite `mutex`;
- matching lockless dei numeri tramite `test_bit()`;
- aggiunta, rimozione, conteggio e consultazione completa degli UID;
- aggiunta, rimozione, conteggio e consultazione completa dei programmi;
- aggiunta, rimozione, conteggio e consultazione completa delle system call;
- ordinamento crescente naturale della lista delle system call;
- validazione del range x86-64 nativo;
- rifiuto dei duplicati;
- snapshot consistenti dei registri;
- gestione della crescita concorrente dei registri durante una consultazione;
- identificazione dell’eseguibile corrente senza usare `current->comm`;
- acquisizione sicura delle reference a `mm_struct`, `struct file` e nome della dentry;
- matching dinamico tra il programma corrente e il registro;
- rilascio completo delle entry durante la rimozione del modulo;
- logging delle operazioni principali e degli errori.

Non sono ancora implementati:

- configurazione del limite massimo di invocazioni;
- intercettazione delle system call;
- logica temporale della finestra di un secondo;
- blocco temporaneo dei thread;
- raccolta delle statistiche richieste;
- test automatici e test concorrenti completi;
- eventuale ottimizzazione delle strutture dati per il percorso frequente.

---

## 2. Architettura attuale

```text
                              USER SPACE
┌────────────────────────────────────────────────────────────┐
│                         user/stctl                         │
│                                                            │
│  validazione degli argomenti                               │
│  open("/dev/syscall_throttle")                             │
│  ioctl(fd, ST_IOCTL_...)                                   │
│  close(fd)                                                 │
└────────────────────────────┬───────────────────────────────┘
                             │ system call
                             ▼
                           KERNEL SPACE
┌────────────────────────────────────────────────────────────┐
│                              VFS                           │
│                                                            │
│  individua la struct file associata al descrittore         │
│  e consulta la struct file_operations del device           │
└────────────────────────────┬───────────────────────────────┘
                             ▼
┌────────────────────────────────────────────────────────────┐
│                       module/device.c                      │
│                                                            │
│  st_device_open()                                          │
│  st_device_release()                                       │
│  st_device_ioctl()                                         │
│                                                            │
│  controllo privilegi                                       │
│  validazione UAPI                                          │
│  copy_from_user() / copy_to_user()                         │
└──────────────┬─────────────────┬───────────────────────────┘
               │                 │
               ▼                 ▼
┌──────────────────────────┐  ┌──────────────────────────────┐
│ module/monitor_state.c   │  │ module/uid_registry.c        │
│                          │  │                              │
│ enable / disable         │  │ add / remove                │
│ lettura stato            │  │ contains / count            │
│                          │  │ snapshot                     │
└──────────────────────────┘  └──────────────────────────────┘
               │
               ▼
┌────────────────────────────────────────────────────────────┐
│                 module/program_registry.c                  │
│                                                            │
│  add / remove / contains / count / snapshot                │
│  contains_current()                                        │
└────────────────────────────┬───────────────────────────────┘
                             ▼
┌────────────────────────────────────────────────────────────┐
│                 module/program_identity.c                  │
│                                                            │
│  current → mm_struct → exe_file → dentry → basename        │
│                                                            │
│  get_task_mm() / get_file_rcu()                            │
│  take_dentry_name_snapshot()                               │
└────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────┐
│                 module/syscall_registry.c                  │
│                                                            │
│  bitmap x86-64                                             │
│  add / remove / contains / count / snapshot                │
│  test_bit() lockless per il futuro percorso frequente      │
└────────────────────────────────────────────────────────────┘
```

Il character device è il punto di controllo del modulo.

Le responsabilità sono separate:

```text
device.c            → interfaccia VFS, ioctl e trasferimenti UAPI
monitor_state.c     → stato attivo/disattivo
uid_registry.c      → registro degli UID
program_registry.c  → registro dei programmi e matching
program_identity.c  → identità dell’eseguibile corrente
syscall_registry.c  → bitmap, conteggio, snapshot e matching delle syscall
stctl.c             → controller user-space
```

---

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
│   └── syscall_registry.h
└── user
    ├── Makefile
    └── stctl.c
```

---

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
- dimensione massima dei nomi dei programmi.

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
ST_IOCTL_SYSCALL_GET_COUNT
ST_IOCTL_SYSCALL_LIST
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

```text
syscall_throttle_init()
    ├── st_monitor_state_init()
    ├── st_uid_registry_init()
    ├── st_program_registry_init()
    ├── st_device_init()
    └── modulo operativo
```

Le strutture interne vengono inizializzate prima della registrazione del device.

Questo ordine evita che un processo possa inviare un `ioctl` mentre un registro non è ancora pronto.

### Gestione degli errori di inizializzazione

Se `st_device_init()` fallisce:

```text
st_program_registry_exit()
    ↓
st_uid_registry_exit()
    ↓
st_monitor_state_exit()
    ↓
caricamento fallito
```

Le risorse già inizializzate vengono rilasciate nell’ordine inverso.

### Rimozione

```text
syscall_throttle_exit()
    ├── st_device_exit()
    ├── st_program_registry_exit()
    ├── st_uid_registry_exit()
    ├── st_monitor_state_exit()
    └── modulo rimosso
```

Il device viene deregistrato per primo, così non possono arrivare nuovi comandi durante il teardown.

I registri vengono attraversati con:

```c
list_for_each_entry_safe(...)
```

e ogni entry viene:

```text
scollegata con list_del()
liberata con kfree()
```

---

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
```

### Operazioni pubbliche

```text
ST_IOCTL_PING
ST_IOCTL_GET_STATUS
ST_IOCTL_UID_GET_COUNT
ST_IOCTL_UID_LIST
ST_IOCTL_PROGRAM_GET_COUNT
ST_IOCTL_PROGRAM_LIST
```

La verifica avviene nel kernel tramite effective UID.

Il controllo è eseguito prima di `copy_from_user()` per le operazioni protette.

Questo permette di rifiutare subito un chiamante non autorizzato senza accedere al puntatore da lui fornito.

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
```

---

## 21. Logging

Il modulo registra:

- caricamento e rimozione;
- inizializzazione e rilascio dei registri;
- registrazione e deregistrazione del device;
- apertura e chiusura;
- `PING`;
- lettura e modifica dello stato;
- tentativi non autorizzati;
- aggiunta e rimozione di UID;
- aggiunta e rimozione di programmi;
- aggiunta e rimozione di numeri di system call;
- conteggio e lista delle system call;
- rifiuto dei numeri fuori dal range x86-64;
- duplicati;
- rimozioni di elementi assenti;
- conteggi;
- numero di elementi restituiti dagli snapshot;
- errori di copia user-space;
- richieste non valide.

Esempi:

```text
syscall_throttle: PROGRAM_ADD rifiutato: pid=6420 euid=1000
syscall_throttle: programma 'curl' registrato
syscall_throttle: programma 'curl' già registrato
syscall_throttle: PROGRAM_REMOVE rifiutato: pid=... euid=1000
syscall_throttle: programma 'curl' rimosso
syscall_throttle: programma 'curl' non registrato
syscall_throttle: PROGRAM_GET_COUNT da pid=...: 3 programmi registrati
syscall_throttle: PROGRAM_LIST da pid=...: 3 programmi restituiti
syscall_throttle: system call 39 registrata
syscall_throttle: system call 39 già registrata
syscall_throttle: system call 39 rimossa
syscall_throttle: numero di system call 472 non valido
syscall_throttle: SYSCALL_GET_COUNT da pid=...: 3 system call registrate
syscall_throttle: SYSCALL_LIST da pid=...: 3 system call restituite
```

Durante la fase di sviluppo il logging è utile.

Nel percorso definitivo delle system call dovrà essere ridotto al minimo.

---

## 22. Compilazione

### Modulo kernel

```bash
cd module
make clean
make
```

Il modulo finale combina:

```text
main.o
device.o
monitor_state.o
uid_registry.o
program_registry.o
program_identity.o
syscall_registry.o
```

### Controller user-space

```bash
cd user
make clean
make
```

Il risultato è:

```text
user/stctl
```

---

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

Il registro indica quali numeri di system call devono essere considerati critici dal futuro monitor.

La futura condizione di attivazione sarà concettualmente:

```c
st_monitor_is_enabled() &&
st_syscall_registry_contains(syscall_nr) &&
(st_uid_registry_contains(current_euid()) ||
 st_program_registry_contains_current())
```

La M6 non intercetta ancora le system call e non applica throttling. Prepara il registro e il matching necessari al futuro percorso di controllo.

---

## 30. Dominio x86-64 e limite architetturale

Il modulo include:

```c
#include <asm/unistd.h>
```

e definisce:

```c
#define ST_SYSCALL_LIMIT NR_syscalls
```

Sul kernel usato durante lo sviluppo:

```text
NR_syscalls = 472
```

Gli indici validi sono quindi:

```text
0 ... 471
```

La validazione kernel è:

```c
number < ST_SYSCALL_LIMIT
```

Il valore `472` è il primo valore non valido.

Il limite non è scritto manualmente nel progetto: deriva dagli header del kernel contro cui il modulo viene compilato.

### Motivazione di sicurezza

Usare `NR_syscalls`:

- evita costanti obsolete;
- adatta il modulo al kernel corrente;
- impedisce accessi oltre il limite della bitmap;
- esclude automaticamente i numeri x32 dotati di `__X32_SYSCALL_BIT`;
- riduce il rischio di errori off-by-one.

---

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
validazione del range
    ↓
mutex_lock
    ↓
test_bit
    ↓
se presente: -EEXIST
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
test_bit
    ↓
se assente: -ENOENT
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

esegue:

```c
test_bit(number, st_syscall_bitmap)
```

dopo aver validato il range.

---

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
-EPERM  privilegi insufficienti
-EFAULT richiesta user-space non accessibile
-EINVAL numero o reserved non valido
-EEXIST numero già registrato
```

### Errori di rimozione

```text
-EPERM  privilegi insufficienti
-EFAULT richiesta user-space non accessibile
-EINVAL numero o reserved non valido
-ENOENT numero non registrato
```

Non è previsto `-ENOMEM` per l’aggiunta, perché non viene allocata una entry dinamica.

---

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

## 44. Limiti attuali e ottimizzazioni future

Il registro delle system call è già adatto a letture molto frequenti grazie alla bitmap e a `test_bit()`.

I registri UID e programmi usano ancora:

```text
lista concatenata
mutex
ricerca lineare
```

Prima di consultarli nel percorso di ogni system call sarà necessario valutare:

- hash table;
- RCU;
- strutture read-mostly;
- cache dell’identità del programma;
- separazione tra aggiornamenti rari e letture frequenti.

Anche `st_program_registry_contains_current()` richiede operazioni che possono dormire e un mutex. La compatibilità con il punto di intercettazione scelto dovrà essere riesaminata nella milestone dedicata.

---

## 45. Prossimi passi

La prossima milestone è:

```text
M7 — Configurazione del limite MAX e finestra temporale
```

Prima dell’implementazione sarà necessario definire:

- semantica precisa di `MAX`;
- comportamento per `MAX == 0`;
- unità temporale e clock kernel;
- rappresentazione della finestra di un secondo;
- stato globale o separato per system call;
- sincronizzazione del contatore temporale;
- interazione con enable/disable;
- UAPI di configurazione e consultazione;
- preparazione al futuro blocco dei thread.

Successivamente:

1. intercettazione delle system call;
2. combinazione di UID, programma e numero;
3. attesa temporanea dei thread eccedenti;
4. gestione delle system call bloccanti e non bloccanti;
5. statistiche;
6. test concorrenti e finali.

---

## 46. Stato della roadmap

```text
[M1] Character device minimale                     COMPLETATO
[M2] UAPI e primo ioctl PING                       COMPLETATO
[M3] Stato ENABLE/DISABLE/GET_STATUS               COMPLETATO
[M4] Registro UID                                  COMPLETATO
[M5] Registro nomi degli eseguibili                COMPLETATO
[M6] Registro numeri di system call                COMPLETATO
[M7] Configurazione MAX e finestra temporale       DA IMPLEMENTARE
[M8] Intercettazione delle system call             DA IMPLEMENTARE
[M9] Blocco temporaneo dei thread                  DA IMPLEMENTARE
[M10] Statistiche e test finali                    DA IMPLEMENTARE
```
