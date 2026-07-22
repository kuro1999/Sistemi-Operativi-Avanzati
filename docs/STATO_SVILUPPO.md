# Syscall Throttle — Stato dello sviluppo

Questo documento descrive lo stato corrente del progetto **Syscall Throttle**, le funzionalità già implementate, l’architettura adottata e i test eseguiti fino a questo punto.

L’obiettivo finale del progetto è realizzare un Linux Kernel Module capace di configurare e applicare un meccanismo di throttling sulle system call. Attualmente è stata completata l’infrastruttura di controllo user-space/kernel e la gestione dello stato attivo/disattivo del monitor.

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
- trasferimento dati dal kernel allo user-space con `copy_to_user()`;
- controller user-space `stctl`;
- logging delle operazioni principali.

Non sono ancora implementati:

- registrazione degli UID;
- registrazione dei nomi degli eseguibili;
- registrazione dei numeri di system call;
- configurazione del limite massimo di invocazioni;
- intercettazione delle system call;
- blocco temporaneo dei thread;
- raccolta delle statistiche richieste;
- gestione completa della concorrenza sui futuri registri.

---

## 2. Architettura attuale

```text
                    USER SPACE
┌──────────────────────────────────────────────┐
│                  user/stctl                  │
│                                              │
│  open("/dev/syscall_throttle")               │
│  ioctl(fd, ST_IOCTL_...)                     │
│  close(fd)                                   │
└──────────────────────┬───────────────────────┘
                       │ system call
                       ▼
                    KERNEL SPACE
┌──────────────────────────────────────────────┐
│                     VFS                      │
│                                              │
│  individua il file descriptor e consulta     │
│  la struct file_operations del device        │
└──────────────────────┬───────────────────────┘
                       ▼
┌──────────────────────────────────────────────┐
│               module/device.c                │
│                                              │
│  st_device_open()                            │
│  st_device_release()                         │
│  st_device_ioctl()                           │
└──────────────────────┬───────────────────────┘
                       ▼
┌──────────────────────────────────────────────┐
│            module/monitor_state.c            │
│                                              │
│  st_monitor_enable()                         │
│  st_monitor_disable()                        │
│  st_monitor_is_enabled()                     │
└──────────────────────────────────────────────┘
```

Il character device costituisce il punto di controllo del modulo. Il monitor vero e proprio sarà implementato separatamente e utilizzerà la configurazione ricevuta tramite il device.

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
│   └── monitor_state.h
└── user
    ├── Makefile
    └── stctl.c
```

### `include/uapi/syscall_throttle.h`

Definisce l’interfaccia binaria condivisa tra kernel e user-space.

Contiene:

- nome e percorso del device;
- magic number degli `ioctl`;
- struttura restituita da `GET_STATUS`;
- numeri dei comandi `ioctl`.

Comandi attualmente definiti:

```c
ST_IOCTL_PING
ST_IOCTL_ENABLE
ST_IOCTL_DISABLE
ST_IOCTL_GET_STATUS
```

La struttura dello stato è:

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

Il campo `reserved` viene inizializzato a zero e lascia spazio per estensioni future dell’interfaccia.

---

### `module/main.c`

Gestisce il ciclo di vita complessivo del modulo.

Durante il caricamento:

```text
syscall_throttle_init()
    ├── st_monitor_state_init()
    ├── st_device_init()
    └── modulo pronto
```

Durante la rimozione:

```text
syscall_throttle_exit()
    ├── st_device_exit()
    ├── st_monitor_state_exit()
    └── modulo rimosso
```

Il device viene rimosso prima dello stato interno, in modo che non possano arrivare nuovi comandi mentre il modulo sta rilasciando le proprie risorse.

Se la registrazione del device fallisce, lo stato già inizializzato viene rilasciato e il caricamento del modulo termina con errore.

---

### `module/device.c`

Implementa il character device `/dev/syscall_throttle`.

Il device viene registrato tramite:

```c
misc_register(&st_misc_device);
```

e deregistrato tramite:

```c
misc_deregister(&st_misc_device);
```

La tabella delle operazioni VFS contiene:

```c
static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .open = st_device_open,
    .release = st_device_release,
    .unlocked_ioctl = st_device_ioctl,
};
```

#### `.owner = THIS_MODULE`

Quando un processo mantiene aperto il device, il reference count del modulo aumenta.

Di conseguenza:

```bash
sudo rmmod syscall_throttle
```

fallisce con `Module is in use` finché il file descriptor non viene chiuso.

Questa protezione impedisce che il codice del driver venga rimosso mentre un processo potrebbe ancora richiamarne le operazioni.

#### `st_device_open()`

Viene chiamata quando un processo esegue:

```c
open("/dev/syscall_throttle", O_RDWR);
```

Non crea il device: il device esiste già dal momento della chiamata a `misc_register()`.

L’apertura crea invece una nuova istanza associata a una `struct file` e restituisce al processo un file descriptor.

#### `st_device_release()`

Viene chiamata quando il processo esegue:

```c
close(fd);
```

La chiusura termina soltanto quella specifica apertura. Non deregistra il device e non rimuove il modulo.

Il device può quindi essere aperto nuovamente da altri processi o dallo stesso programma.

#### `st_device_ioctl()`

Riceve i comandi inviati dallo user-space:

```c
ioctl(fd, command, argument);
```

Il comando viene selezionato tramite uno `switch`.

Un comando sconosciuto restituisce:

```c
-ENOTTY
```

---

### `module/monitor_state.c`

Contiene lo stato globale attivo/disattivo del monitor.

La variabile è privata al file:

```c
static bool st_monitor_enabled;
```

Gli altri componenti non possono modificarla direttamente e devono usare:

```c
st_monitor_enable();
st_monitor_disable();
st_monitor_is_enabled();
```

Il monitor viene inizializzato come disattivato a ogni caricamento del modulo.

Gli accessi alla variabile usano:

```c
READ_ONCE()
WRITE_ONCE()
```

per impedire ottimizzazioni inappropriate del compilatore durante accessi concorrenti da CPU differenti.

Queste primitive non sostituiscono un lock per strutture complesse, ma sono sufficienti per il valore booleano indipendente attualmente gestito.

---

### `user/stctl.c`

È il controller user-space del modulo.

Comandi disponibili:

```bash
./stctl ping
./stctl status
./stctl enable
./stctl disable
```

Ogni esecuzione segue il percorso:

```text
open("/dev/syscall_throttle")
        ↓
ioctl(fd, comando)
        ↓
close(fd)
```

Il programma valida il comando prima di aprire il device.

---

## 4. Funzionamento degli `ioctl`

### `ST_IOCTL_PING`

Serve a verificare il canale di comunicazione tra user-space e kernel.

Percorso:

```text
./stctl ping
    ↓
open()
    ↓
ioctl(ST_IOCTL_PING)
    ↓
st_device_ioctl()
    ↓
return 0
    ↓
close()
```

Non modifica alcuno stato.

Output user-space:

```text
PING completato correttamente.
```

---

### `ST_IOCTL_GET_STATUS`

Restituisce lo stato attuale del monitor.

Il programma user-space prepara:

```c
struct st_monitor_status status;
```

e passa il suo indirizzo:

```c
ioctl(fd, ST_IOCTL_GET_STATUS, &status);
```

Nel kernel viene creata una struttura locale:

```c
struct st_monitor_status status = {
    .enabled = st_monitor_is_enabled() ? 1U : 0U,
    .reserved = 0U,
};
```

Il trasferimento verso lo user-space avviene con:

```c
copy_to_user((void __user *)argument,
             &status,
             sizeof(status));
```

Il kernel non dereferenzia direttamente il puntatore ricevuto dallo user-space.

Se la copia fallisce, il driver restituisce:

```c
-EFAULT
```

La lettura dello stato è permessa anche a utenti non privilegiati.

---

### `ST_IOCTL_ENABLE`

Attiva il monitor.

Prima della modifica, il kernel controlla l’effective UID del thread chiamante:

```c
uid_eq(current_euid(), GLOBAL_ROOT_UID)
```

Se l’effective UID non è zero, il driver restituisce:

```c
-EPERM
```

In caso contrario richiama:

```c
st_monitor_enable();
```

---

### `ST_IOCTL_DISABLE`

Disattiva il monitor.

Applica lo stesso controllo dei privilegi di `ENABLE`.

Un utente non privilegiato riceve `EPERM`, mentre un processo con effective UID zero può modificare lo stato.

---

## 5. Controllo dei privilegi

Il file del device è attualmente creato con permessi:

```text
0666
```

Questo permette agli utenti normali di aprire il device e leggere lo stato.

La sicurezza delle operazioni di modifica non dipende però dai permessi del file: viene applicata all’interno del kernel.

Questo è importante perché un utente potrebbe non utilizzare `stctl`, ma scrivere un programma alternativo che invochi direttamente gli stessi `ioctl`.

Le operazioni protette sono:

```text
ENABLE
DISABLE
```

Il controllo viene eseguito sull’effective UID del thread che effettua la system call.

---

## 6. Ciclo di vita del device

### Caricamento

```bash
sudo insmod syscall_throttle.ko
```

Sequenza:

```text
inizializzazione dello stato
        ↓
registrazione miscdevice
        ↓
creazione di /dev/syscall_throttle
        ↓
modulo operativo
```

### Apertura

```c
fd = open("/dev/syscall_throttle", O_RDWR);
```

Sequenza:

```text
system call open
        ↓
VFS
        ↓
file_operations.open
        ↓
st_device_open()
```

### Invio di un comando

```c
ioctl(fd, ST_IOCTL_GET_STATUS, &status);
```

Sequenza:

```text
system call ioctl
        ↓
VFS trova la struct file tramite fd
        ↓
file_operations.unlocked_ioctl
        ↓
st_device_ioctl()
```

### Chiusura

```c
close(fd);
```

Sequenza:

```text
system call close
        ↓
rilascio del file descriptor
        ↓
st_device_release()
```

### Rimozione

```bash
sudo rmmod syscall_throttle
```

Sequenza:

```text
deregistrazione del device
        ↓
rimozione di /dev/syscall_throttle
        ↓
rilascio dello stato
        ↓
rimozione del modulo
```

---

## 7. Logging

Il modulo registra attualmente:

- inizializzazione dello stato;
- registrazione del device;
- caricamento del modulo;
- apertura del device;
- chiusura del device;
- ricezione del comando `PING`;
- lettura dello stato;
- attivazione del monitor;
- disattivazione del monitor;
- tentativi non autorizzati di `ENABLE`;
- tentativi non autorizzati di `DISABLE`;
- errori di `copy_to_user()`;
- deregistrazione del device;
- rilascio dello stato;
- rimozione del modulo.

Esempio:

```text
syscall_throttle: GET_STATUS da pid=6933: monitor disattivato
syscall_throttle: ENABLE rifiutato: pid=6934 euid=1000
syscall_throttle: monitor attivato
syscall_throttle: DISABLE rifiutato: pid=6977 euid=1000
```

Durante lo sviluppo questi messaggi facilitano il debug.

Nella fase finale sarà opportuno ridurre i messaggi associati a operazioni molto frequenti, per evitare di riempire inutilmente il kernel log.

---

## 8. Compilazione

### Modulo kernel

```bash
cd module
make clean
make
```

Il Makefile genera un unico modulo:

```text
syscall_throttle.ko
```

combinando:

```text
main.o
device.o
monitor_state.o
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

Kernel e user-space includono lo stesso header UAPI:

```text
include/uapi/syscall_throttle.h
```

---

## 9. Esecuzione e test manuali

### Caricamento

```bash
cd module
sudo insmod syscall_throttle.ko
```

### Stato iniziale

```bash
cd ../user
./stctl status
```

Risultato atteso:

```text
Monitor: disattivato
```

### Tentativo di attivazione senza privilegi

```bash
./stctl enable
echo $?
```

Risultato atteso:

```text
ioctl ST_IOCTL_ENABLE fallita: Operation not permitted
1
```

### Attivazione come root

```bash
sudo ./stctl enable
./stctl status
```

Risultato atteso:

```text
Monitor attivato.
Monitor: attivo
```

### Tentativo di disattivazione senza privilegi

```bash
./stctl disable
echo $?
./stctl status
```

Risultato atteso:

```text
ioctl ST_IOCTL_DISABLE fallita: Operation not permitted
1
Monitor: attivo
```

### Disattivazione come root

```bash
sudo ./stctl disable
./stctl status
```

Risultato atteso:

```text
Monitor disattivato.
Monitor: disattivato
```

### Controllo dei log

```bash
sudo dmesg | grep syscall_throttle | tail -n 30
```

### Rimozione del modulo

```bash
sudo rmmod syscall_throttle
```

---

## 10. Test del reference count

Per verificare che il modulo non possa essere rimosso mentre il device è aperto:

```bash
exec 3<> /dev/syscall_throttle
lsmod | grep syscall_throttle
sudo rmmod syscall_throttle
```

Il reference count deve risultare maggiore di zero e `rmmod` deve fallire con:

```text
Module syscall_throttle is in use
```

Dopo la chiusura:

```bash
exec 3>&-
sudo rmmod syscall_throttle
```

la rimozione deve riuscire.

---

## 11. Decisioni progettuali attuali

### Uso di `miscdevice`

È stato scelto `miscdevice` perché il progetto necessita di un solo character device di controllo.

Questa soluzione evita di gestire manualmente:

```text
alloc_chrdev_region()
cdev_add()
class_create()
device_create()
```

e permette di concentrarsi sulla logica centrale del monitor.

### Separazione delle responsabilità

Il codice è diviso per responsabilità:

```text
main.c          → ciclo di vita del modulo
device.c        → interfaccia VFS e ioctl
monitor_state.c → stato interno del monitor
stctl.c         → interfaccia user-space
```

Questa separazione evita che `main.c` e `device.c` diventino file monolitici durante l’aggiunta delle funzionalità successive.

### Stato iniziale disattivato

Il monitor parte sempre disattivato.

Il semplice caricamento del modulo non deve modificare immediatamente il comportamento delle system call del sistema.

### Controlli di sicurezza nel kernel

La validazione dei privilegi viene eseguita nel driver e non soltanto nel controller user-space.

Lo user-space viene considerato non fidato.

---

## 12. Prossimi passi

La prossima milestone prevista è il registro degli UID.

L’implementazione richiederà:

1. una struttura dati kernel per memorizzare gli UID registrati;
2. inizializzazione e distruzione del registro;
3. sincronizzazione degli accessi concorrenti;
4. comandi `ioctl` per aggiunta, rimozione e consultazione;
5. controllo root sulle operazioni di modifica;
6. estensione di `stctl`;
7. test di duplicati, UID inesistenti, registro vuoto e accessi concorrenti.

Successivamente verranno introdotti i registri dei programmi e dei numeri di system call, seguiti dall’intercettazione e dalla logica di throttling.

---

## 13. Stato della roadmap

```text
[M1] Character device minimale                     COMPLETATO
[M2] UAPI e primo ioctl PING                       COMPLETATO
[M3] Stato ENABLE/DISABLE/GET_STATUS               COMPLETATO
[M4] Registro UID                                  DA IMPLEMENTARE
[M5] Registro nomi degli eseguibili                DA IMPLEMENTARE
[M6] Registro numeri di system call                DA IMPLEMENTARE
[M7] Configurazione MAX e finestra temporale        DA IMPLEMENTARE
[M8] Intercettazione delle system call             DA IMPLEMENTARE
[M9] Blocco temporaneo dei thread                  DA IMPLEMENTARE
[M10] Statistiche e test finali                     DA IMPLEMENTARE
```
