# Syscall Throttling Linux Kernel Module

Progetto del corso di Sistemi Operativi Avanzati.

## Obiettivo

Realizzare un Linux Kernel Module che implementi un meccanismo di
throttling delle system call in funzione di:

- nome dell'eseguibile;
- effective user ID;
- numero della system call x86-64.

Il modulo esporrà un device driver configurabile da user space
attraverso la system call ioctl().

## Struttura

- `module/`: codice del Linux Kernel Module e del device driver.
- `include/uapi/`: header condivisi tra kernel e user space.
- `user/`: programmi per configurare e interrogare il monitor.
- `tests/`: programmi e script di test.
- `scripts/`: script per compilazione, caricamento e rimozione.
- `docs/`: documentazione, decisioni progettuali e configurazione.
