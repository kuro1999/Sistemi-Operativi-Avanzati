# Syscall Throttle — Stato dello sviluppo

Questo documento descrive lo stato corrente del progetto **Syscall Throttle**, le funzionalità già implementate, l’architettura adottata, le principali decisioni progettuali e i test eseguiti.

L’obiettivo finale è realizzare un Linux Kernel Module capace di configurare e applicare un meccanismo di throttling sulle system call. Al momento sono state completate:

- l’infrastruttura di comunicazione user-space/kernel;
- la gestione dello stato attivo/disattivo del monitor;
- la gestione completa del registro degli UID.

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
- registro kernel degli UID basato su lista concatenata;
- protezione concorrente del registro UID tramite `mutex`;
- aggiunta e rimozione degli UID;
- rifiuto degli UID duplicati;
- conteggio degli UID registrati;
- consultazione completa della lista degli UID;
- snapshot consistente della lista prima della copia verso lo user-space;
- gestione del caso in cui il registro cresca durante una consultazione;
- logging delle operazioni principali e degli errori.

Non sono ancora implementati:

- registrazione dei nomi degli eseguibili;
- registrazione dei numeri di system call;
- configurazione del limite massimo di invocazioni;
- intercettazione delle system call;
- logica temporale della finestra di un secondo;
- blocco temporaneo dei thread;
- raccolta delle statistiche richieste;
- test automatici e test concorrenti completi.

---

## 2. Architettura attuale

```text
                         USER SPACE
┌──────────────────────────────────────────────────────┐
│                     user/stctl                      │
│                                                      │
│  open("/dev/syscall_throttle")                       │
│  ioctl(fd, ST_IOCTL_...)                             │
│  close(fd)                                           │
└──────────────────────────┬───────────────────────────┘
                           │ system call
                           ▼
                         KERNEL SPACE
┌──────────────────────────────────────────────────────┐
│                         VFS                          │
│                                                      │
│  individua la struct file associata al descrittore   │
│  e consulta la struct file_operations del device     │
└──────────────────────────┬───────────────────────────┘
                           ▼
┌──────────────────────────────────────────────────────┐
│                   module/device.c                    │
│                                                      │
│  st_device_open()                                    │
│  st_device_release()                                 │
│  st_device_ioctl()                                   │
│                                                      │
│  validazione privilegi                               │
│  copy_from_user() / copy_to_user()                   │
└─────────────────┬──────────────────────┬─────────────┘
                  │                      │
                  ▼                      ▼
┌────────────────────────────┐  ┌──────────────────────────────┐
│ module/monitor_state.c     │  │ module/uid_registry.c        │
│                            │  │                              │
│ st_monitor_enable()        │  │ st_uid_registry_add()        │
│ st_monitor_disable()       │  │ st_uid_registry_remove()     │
│ st_monitor_is_enabled()    │  │ st_uid_registry_count()      │
│                            │  │ st_uid_registry_snapshot()   │
└────────────────────────────┘  └──────────────────────────────┘
```

Il character device è il punto di controllo del modulo. La logica interna viene mantenuta in componenti separati, così il device si occupa soprattutto di:

- ricevere gli `ioctl`;
- validare il chiamante e i dati;
- convertire i dati tra UAPI e rappresentazione kernel;
- invocare il sottosistema corretto.

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
│   └── uid_registry.h
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
- strutture a dimensione fissa;
- numeri dei comandi;
- campi `reserved` per estensioni future.

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

### Struttura dello stato del monitor

```c
struct st_monitor_status {
    __u32 enabled;
    __u32 reserved;
};
```

`enabled` vale:

```text
0 = monitor disattivato
1 = monitor attivo
```

### Richiesta relativa a un UID

```c
struct st_uid_request {
    __u32 uid;
    __u32 reserved;
};
```

Viene utilizzata da:

```c
ST_IOCTL_UID_ADD
ST_IOCTL_UID_REMOVE
```

### Conteggio degli UID

```c
struct st_uid_count {
    __u32 count;
    __u32 reserved;
};
```

Viene restituita da:

```c
ST_IOCTL_UID_GET_COUNT
```

### Richiesta della lista degli UID

```c
struct st_uid_list_request {
    __aligned_u64 uids_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};
```

Significato dei campi:

```text
uids_ptr  = indirizzo user-space di un array di __u32
capacity  = numero massimo di elementi contenibili nell’array
count     = numero di UID richiesti o effettivamente copiati
reserved  = campi da inizializzare a zero
```

`__aligned_u64` permette di rappresentare il puntatore con dimensione e allineamento stabili nell’UAPI.

---

## 5. Ciclo di vita del modulo

### Caricamento

Durante:

```bash
sudo insmod syscall_throttle.ko
```

viene eseguita questa sequenza:

```text
syscall_throttle_init()
    ├── st_monitor_state_init()
    ├── st_uid_registry_init()
    ├── st_device_init()
    └── modulo operativo
```

Il registro UID è quindi inizializzato prima che il device inizi ad accettare comandi.

Se la registrazione del device fallisce:

```text
st_device_init() fallisce
    ↓
st_uid_registry_exit()
    ↓
st_monitor_state_exit()
    ↓
caricamento del modulo fallito
```

### Rimozione

Durante:

```bash
sudo rmmod syscall_throttle
```

viene eseguita:

```text
syscall_throttle_exit()
    ├── st_device_exit()
    ├── st_uid_registry_exit()
    ├── st_monitor_state_exit()
    └── modulo rimosso
```

Il device viene deregistrato per primo, impedendo l’arrivo di nuovi comandi mentre le strutture interne vengono rilasciate.

`st_uid_registry_exit()` attraversa la lista, rimuove tutte le entry e libera ogni allocazione con `kfree()`.

---

## 6. Character device e VFS

### Registrazione

Il device viene registrato tramite:

```c
misc_register(&st_misc_device);
```

e reso disponibile come:

```text
/dev/syscall_throttle
```

La deregistrazione avviene con:

```c
misc_deregister(&st_misc_device);
```

### Operazioni VFS

```c
static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .open = st_device_open,
    .release = st_device_release,
    .unlocked_ioctl = st_device_ioctl,
};
```

### `.owner = THIS_MODULE`

Quando un processo mantiene aperto il device, il reference count del modulo aumenta.

Di conseguenza:

```bash
sudo rmmod syscall_throttle
```

fallisce con:

```text
Module syscall_throttle is in use
```

finché il file descriptor non viene chiuso.

### Apertura

```c
open("/dev/syscall_throttle", O_RDWR);
```

provoca:

```text
system call open
    ↓
VFS
    ↓
st_device_open()
```

L’apertura non crea il device. Il device esiste già dalla chiamata a `misc_register()`.

### Chiusura

```c
close(fd);
```

provoca:

```text
system call close
    ↓
st_device_release()
```

La chiusura termina soltanto quella specifica apertura. Non rimuove il modulo e non deregistra il device.

---

## 7. Stato del monitor

Il file `module/monitor_state.c` contiene:

```c
static bool st_monitor_enabled;
```

La variabile è privata al componente e viene gestita tramite:

```c
st_monitor_enable();
st_monitor_disable();
st_monitor_is_enabled();
```

Il monitor parte sempre disattivato.

Gli accessi usano:

```c
READ_ONCE()
WRITE_ONCE()
```

per rendere espliciti gli accessi concorrenti al valore booleano.

### `ST_IOCTL_ENABLE`

- richiede effective UID `0`;
- imposta lo stato a `true`;
- restituisce `-EPERM` ai chiamanti non privilegiati.

### `ST_IOCTL_DISABLE`

- richiede effective UID `0`;
- imposta lo stato a `false`;
- restituisce `-EPERM` ai chiamanti non privilegiati.

### `ST_IOCTL_GET_STATUS`

- è accessibile anche senza privilegi;
- costruisce una `struct st_monitor_status`;
- trasferisce il risultato con `copy_to_user()`;
- restituisce `-EFAULT` se il puntatore user-space non è valido.

---

## 8. Registro UID

Il registro UID è implementato in:

```text
module/uid_registry.c
module/uid_registry.h
```

### Struttura di una entry

```c
struct st_uid_entry {
    kuid_t uid;
    struct list_head node;
};
```

Il kernel conserva gli UID come `kuid_t`, non come normali interi.

La lista è dichiarata tramite:

```c
static LIST_HEAD(st_uid_entries);
```

Il numero di elementi è mantenuto in:

```c
static unsigned int st_uid_entries_count;
```

### Sincronizzazione

Lista e contatore sono protetti da:

```c
static DEFINE_MUTEX(st_uid_registry_lock);
```

Il mutex serializza:

- aggiunte;
- rimozioni;
- ricerche;
- lettura del conteggio;
- creazione degli snapshot.

La soluzione privilegia correttezza e chiarezza. Quando il registro verrà consultato nel percorso delle system call, sarà necessario rivalutare se un mutex e una lista lineare siano adeguati al percorso più frequente.

---

## 9. Aggiunta di un UID

Il comando user-space è:

```bash
sudo ./stctl uid-add <UID>
```

Il percorso è:

```text
validazione testuale dell’UID
    ↓
struct st_uid_request
    ↓
ioctl(ST_IOCTL_UID_ADD)
    ↓
controllo effective UID 0
    ↓
copy_from_user()
    ↓
controllo reserved == 0
    ↓
make_kuid(&init_user_ns, request.uid)
    ↓
st_uid_registry_add()
```

### Allocazione e controllo duplicati

`st_uid_registry_add()`:

1. verifica la validità del `kuid_t`;
2. alloca una nuova entry con `kmalloc(..., GFP_KERNEL)`;
3. acquisisce il mutex;
4. attraversa la lista alla ricerca di duplicati;
5. inserisce con `list_add_tail()`;
6. incrementa il contatore;
7. rilascia il mutex.

L’allocazione avviene prima del mutex, così il registro non rimane bloccato durante `kmalloc()`.

### Errori

```text
0         UID aggiunto
-EPERM    chiamante non privilegiato
-EFAULT   puntatore user-space non valido
-EINVAL   richiesta o UID non valido
-EEXIST   UID già registrato
-ENOMEM   memoria kernel insufficiente
```

In caso di duplicato, la nuova entry temporanea viene liberata.

---

## 10. Rimozione di un UID

Il comando user-space è:

```bash
sudo ./stctl uid-remove <UID>
```

Il driver esegue:

```text
controllo effective UID 0
    ↓
copy_from_user()
    ↓
conversione numerica in kuid_t
    ↓
st_uid_registry_remove()
```

La rimozione:

1. acquisisce il mutex;
2. cerca l’entry;
3. la scollega con `list_del()`;
4. decrementa il contatore;
5. rilascia il mutex;
6. libera l’entry con `kfree()`.

La memoria viene liberata dopo il rilascio del mutex.

### Errori

```text
0         UID rimosso
-EPERM    chiamante non privilegiato
-EFAULT   puntatore user-space non valido
-EINVAL   UID non valido
-ENOENT   UID non registrato
```

---

## 11. Conteggio degli UID

Il comando:

```bash
./stctl uid-count
```

utilizza:

```c
ST_IOCTL_UID_GET_COUNT
```

La funzione:

```c
st_uid_registry_count();
```

acquisisce il mutex, legge il contatore e lo rilascia.

Il valore viene trasferito tramite:

```c
copy_to_user();
```

La consultazione è permessa agli utenti non privilegiati.

Esempio:

```text
UID registrati: 2
```

---

## 12. Consultazione completa degli UID

Il comando:

```bash
./stctl uid-list
```

restituisce tutti gli UID registrati.

Esempio:

```text
UID registrati: 2
  1000
  0
```

### Protocollo in due fasi

`stctl` esegue prima:

```text
ST_IOCTL_UID_GET_COUNT
```

per conoscere la dimensione necessaria.

Poi:

```text
calloc(count, sizeof(__u32))
    ↓
ST_IOCTL_UID_LIST
```

### Snapshot kernel

La funzione:

```c
st_uid_registry_snapshot(__u32 *uids,
                         __u32 capacity,
                         __u32 *count);
```

crea una copia consistente:

```text
mutex_lock
    ↓
lettura count
    ↓
attraversamento della lista
    ↓
conversione kuid_t → UID numerico
    ↓
scrittura nell’array kernel
    ↓
mutex_unlock
```

Solo dopo il rilascio del mutex il driver esegue `copy_to_user()`.

Questo evita di mantenere il registro bloccato durante un accesso potenzialmente lento alla memoria user-space.

### Registro cresciuto durante la consultazione

Tra `UID_GET_COUNT` e `UID_LIST`, un altro thread può aggiungere un elemento.

In questo caso il driver restituisce:

```text
-ENOSPC
```

e scrive in `request.count` la nuova capacità necessaria.

`stctl` rialloca l’array e ripete il comando, fino a un massimo di quattro tentativi.

### Protezione dalle allocazioni arbitrarie

Il kernel non alloca in base a `request.capacity`, perché il valore è controllato dallo user-space.

Alloca invece soltanto in base al numero reale di UID registrati:

```c
required = st_uid_registry_count();
uids = kcalloc(required, sizeof(*uids), GFP_KERNEL);
```

Questo impedisce a un utente non privilegiato di richiedere allocazioni kernel enormi indicando una capacità arbitraria.

---

## 13. Validazione nello user-space

`stctl` valida l’UID con `strtoull()` e controlli espliciti.

Sono rifiutati:

```text
-1
abc
1000abc
12.5
4294967296
```

Non viene utilizzato `atoi()`, perché non consente una gestione affidabile di errori e overflow.

Gli input non validi vengono rifiutati prima dell’apertura del device e quindi non generano richieste al kernel.

---

## 14. Controllo dei privilegi

Il device è attualmente creato con permessi:

```text
0666
```

Gli utenti normali possono quindi aprirlo e consultare lo stato.

Le operazioni di modifica protette nel kernel sono:

```text
ST_IOCTL_ENABLE
ST_IOCTL_DISABLE
ST_IOCTL_UID_ADD
ST_IOCTL_UID_REMOVE
```

Le operazioni di sola lettura sono:

```text
ST_IOCTL_PING
ST_IOCTL_GET_STATUS
ST_IOCTL_UID_GET_COUNT
ST_IOCTL_UID_LIST
```

Il controllo utilizza:

```c
uid_eq(current_euid(), GLOBAL_ROOT_UID)
```

La sicurezza non dipende dal controller `stctl`: un programma alternativo che inviasse direttamente gli `ioctl` sarebbe sottoposto agli stessi controlli kernel.

---

## 15. Comandi disponibili in `stctl`

```bash
./stctl ping
./stctl status
./stctl enable
./stctl disable
./stctl uid-add <UID>
./stctl uid-remove <UID>
./stctl uid-count
./stctl uid-list
```

Ogni comando valido esegue:

```text
open("/dev/syscall_throttle")
    ↓
uno o più ioctl()
    ↓
close(fd)
```

`uid-list` può eseguire più `ioctl` sullo stesso file descriptor.

---

## 16. Logging

Il modulo registra attualmente:

- inizializzazione e rilascio dello stato;
- inizializzazione e rilascio del registro UID;
- registrazione e deregistrazione del device;
- caricamento e rimozione del modulo;
- apertura e chiusura del device;
- ricezione di `PING`;
- lettura dello stato;
- attivazione e disattivazione del monitor;
- tentativi non autorizzati;
- aggiunta e rimozione degli UID;
- UID duplicati;
- rimozioni di UID assenti;
- conteggio degli UID;
- numero di UID restituiti da `UID_LIST`;
- errori di puntatori user-space;
- necessità di ripetere `UID_LIST` per capacità insufficiente.

Esempi:

```text
syscall_throttle: UID_ADD rifiutato: pid=8016 euid=1000
syscall_throttle: UID 1000 registrato
syscall_throttle: UID 1000 già registrato
syscall_throttle: UID_REMOVE rifiutato: pid=8372 euid=1000
syscall_throttle: UID 1000 rimosso
syscall_throttle: impossibile rimuovere UID 1000: non registrato
syscall_throttle: UID_GET_COUNT da pid=9381: 2 UID registrati
syscall_throttle: UID_LIST da pid=9381: 2 UID restituiti
```

Durante lo sviluppo questi log sono utili. Nella fase finale sarà opportuno ridurre quelli associati alle operazioni frequenti.

---

## 17. Compilazione

### Modulo kernel

```bash
cd module
make clean
make
```

Il modulo finale:

```text
syscall_throttle.ko
```

combina:

```text
main.o
device.o
monitor_state.o
uid_registry.o
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

Kernel e user-space includono lo stesso header:

```text
include/uapi/syscall_throttle.h
```

---

## 18. Test manuali eseguiti

### Registro vuoto

```bash
./stctl uid-count
./stctl uid-list
```

Risultato:

```text
UID registrati: 0
UID registrati: 0
  nessuno
```

### Aggiunta senza privilegi

```bash
./stctl uid-add "$(id -u)"
```

Risultato:

```text
Registrazione UID non consentita: sono richiesti privilegi root.
```

### Aggiunta come root

```bash
sudo ./stctl uid-add "$(id -u)"
```

Risultato:

```text
UID 1000 registrato.
```

### Duplicato

```bash
sudo ./stctl uid-add "$(id -u)"
```

Risultato:

```text
UID 1000 già registrato.
```

### Rimozione senza privilegi

```bash
./stctl uid-remove "$(id -u)"
```

Risultato:

```text
Rimozione UID non consentita: sono richiesti privilegi root.
```

### Rimozione come root

```bash
sudo ./stctl uid-remove "$(id -u)"
```

Risultato:

```text
UID 1000 rimosso.
```

### Rimozione di UID assente

```bash
sudo ./stctl uid-remove "$(id -u)"
```

Risultato:

```text
UID 1000 non registrato.
```

### Lista con più elementi

```bash
sudo ./stctl uid-add "$(id -u)"
sudo ./stctl uid-add 0
./stctl uid-count
./stctl uid-list
```

Risultato:

```text
UID registrati: 2
UID registrati: 2
  1000
  0
```

### Coerenza dopo rimozione

```bash
sudo ./stctl uid-remove 0
./stctl uid-count
./stctl uid-list
```

Risultato:

```text
UID registrati: 1
UID registrati: 1
  1000
```

---

## 19. Test del reference count

Per mantenere aperto il device:

```bash
exec 3<> /dev/syscall_throttle
```

Il modulo mostra un reference count maggiore di zero:

```bash
lsmod | grep syscall_throttle
```

e la rimozione fallisce:

```bash
sudo rmmod syscall_throttle
```

Dopo:

```bash
exec 3>&-
```

la rimozione può riuscire.

---

## 20. Decisioni progettuali attuali

### Uso di `miscdevice`

È stato scelto `miscdevice` perché il progetto necessita di un singolo character device di controllo.

### Separazione delle responsabilità

```text
main.c          → ciclo di vita del modulo
device.c        → VFS, ioctl, validazione e trasferimenti UAPI
monitor_state.c → stato del monitor
uid_registry.c  → lista, contatore e snapshot degli UID
stctl.c         → controller user-space
```

### Stato iniziale disattivato

Il monitor parte sempre disattivato, così il semplice caricamento del modulo non modifica il comportamento delle system call.

### User-space non fidato

Il kernel valida:

- privilegi;
- campi `reserved`;
- puntatori user-space;
- capacità degli array;
- conversioni degli UID;
- dimensioni delle allocazioni.

### Lista concatenata e mutex

La struttura attuale è semplice e corretta per la fase di configurazione.

Prima di usarla nel percorso di ogni system call sarà necessario valutare:

- costo della ricerca lineare;
- possibilità di usare hash table;
- necessità di letture concorrenti più efficienti;
- eventuale uso di RCU o altre tecniche read-mostly.

---

## 21. Prossimi passi

La prossima milestone è **M5: registro dei nomi degli eseguibili**.

Prima dell’implementazione sarà necessario definire con precisione:

- quale identificatore del programma confrontare;
- se usare `current->comm`;
- il limite di lunghezza del nome;
- il comportamento in caso di `exec`;
- la distinzione tra nome dell’eseguibile, percorso completo e nome del task;
- la sincronizzazione del registro;
- le operazioni `program-add`, `program-remove`, `program-count` e `program-list`.

Successivamente verranno implementati:

1. registro dei numeri di system call;
2. configurazione di `MAX`;
3. meccanismo di intercettazione;
4. finestra temporale di un secondo;
5. blocco temporaneo dei thread;
6. statistiche;
7. test concorrenti e finali.

---

## 22. Stato della roadmap

```text
[M1] Character device minimale                     COMPLETATO
[M2] UAPI e primo ioctl PING                       COMPLETATO
[M3] Stato ENABLE/DISABLE/GET_STATUS               COMPLETATO
[M4] Registro UID                                  COMPLETATO
[M5] Registro nomi degli eseguibili                DA IMPLEMENTARE
[M6] Registro numeri di system call                DA IMPLEMENTARE
[M7] Configurazione MAX e finestra temporale        DA IMPLEMENTARE
[M8] Intercettazione delle system call             DA IMPLEMENTARE
[M9] Blocco temporaneo dei thread                  DA IMPLEMENTARE
[M10] Statistiche e test finali                     DA IMPLEMENTARE
```
