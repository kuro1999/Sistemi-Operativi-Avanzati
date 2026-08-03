#ifndef SYSCALL_THROTTLE_UAPI_H
#define SYSCALL_THROTTLE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Nome e percorso del character device.
 */
#define ST_DEVICE_NAME "syscall_throttle"
#define ST_DEVICE_PATH "/dev/" ST_DEVICE_NAME

/*
 * Lunghezza massima del basename di un eseguibile
 *
 * ST_PROGRAM_NAME_MAX non comprende il terminatore
 */
#define ST_PROGRAM_NAME_MAX 255U
#define ST_PROGRAM_NAME_CAPACITY (ST_PROGRAM_NAME_MAX + 1U)

/*
 * Identificatore dei comandi ioctl appartenenti al driver
 */
#define ST_IOCTL_MAGIC 'S'

/*
 * Stato del monitor restituito allo user-space
 *
 * Non usiamo bool nell'UAPI: __u32 ha una dimensione fissa e rende
 * l'interfaccia binaria più prevedibile
 */
struct st_monitor_status {
    __u32 enabled;
    __u32 reserved;
};

/*
 * Richiesta relativa a un UID
 *
 * uid contiene il valore numerico dell'UID nello user-space
 * reserved deve essere impostato a zero
 */
struct st_uid_request {
    __u32 uid;
    __u32 reserved;
};

/*
 * Numero di UID attualmente presenti nel registro
 */
struct st_uid_count {
    __u32 count;
    __u32 reserved;
};

/*
 * Richiesta per ottenere l'elenco degli UID registrati.
 *
 * uids_ptr contiene l'indirizzo user-space di un array di __u32.
 *
 * capacity indica quanti elementi può contenere l'array.
 *
 * count viene scritto dal kernel con il numero di UID registrati.
 *
 * reserved deve essere inizializzato a zero.
 */
struct st_uid_list_request {
    __aligned_u64 uids_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};

/*
 * Richiesta relativa al nome di un programma.
 *
 * name contiene esclusivamente il basename dell'eseguibile,
 * terminato da NUL e senza caratteri '/'.
 *
 * reserved deve essere inizializzato a zero.
 */
struct st_program_request {
    char name[ST_PROGRAM_NAME_CAPACITY];
    __u32 reserved[2];
};

/*
 * Numero di programmi attualmente presenti nel registro.
 *
 * reserved viene inizializzato a zero dal kernel.
 */
struct st_program_count {
    __u32 count;
    __u32 reserved;
};

/*
 * Singolo elemento restituito da PROGRAM_LIST.
 */
struct st_program_name {
    char name[ST_PROGRAM_NAME_CAPACITY];
};

/*
 * Richiesta per ottenere uno snapshot dei programmi registrati.
 *
 * programs_ptr indica un array user-space di struct st_program_name.
 * capacity è il numero di elementi disponibili nell'array.
 * count viene aggiornato con il numero di programmi richiesti
 * oppure effettivamente restituiti.
 */
struct st_program_list_request {
    __aligned_u64 programs_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};

/*
 * Richiesta relativa a un numero di system call x86-64.
 *
 * number contiene il numero della system call.
 * reserved deve essere inizializzato a zero.
 *
 * Il kernel verifica che number appartenga al dominio della
 * ABI x86-64 nativa supportata dal kernel corrente.
 */
struct st_syscall_request {
    __u32 number;
    __u32 reserved;
};

/*
 * Risposta contenente il numero di system call attualmente
 * presenti nel registro.
 *
 * reserved viene restituito sempre a zero.
 */
struct st_syscall_count {
    __u32 count;
    __u32 reserved;
};

/*
 * Richiesta per ottenere uno snapshot dei numeri di system call
 * presenti nel registro.
 *
 * numbers_ptr indica un array user-space di elementi __u32.
 *
 * capacity indica quanti elementi possono essere contenuti
 * nell'array.
 *
 * count viene aggiornato dal kernel con il numero di elementi
 * necessari oppure effettivamente restituiti.
 *
 * reserved deve essere inizializzato a zero.
 */
struct st_syscall_list_request {
    __aligned_u64 numbers_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
};

/*
 * Configurazione del limite globale.
 *
 * max_invocations indica il numero massimo di system call
 * rilevanti ammesse durante una finestra di un secondo.
 *
 * I campi reserved devono essere impostati a zero.
 */
struct st_max_config {
    __aligned_u64 max_invocations;
    __u32 reserved[2];
};

/*
 * Snapshot delle statistiche del throttling.
 *
 * observation_ns:
 *   durata della sessione statistica.
 *
 * blocked_thread_time_ns:
 *   integrale temporale del numero di thread bloccati,
 *   espresso in thread-nanosecondi.
 *
 * La media temporale dei thread bloccati viene calcolata
 * nello user-space come:
 *
 *   blocked_thread_time_ns / observation_ns
 *
 * peak_delay_ns misura il massimo intervallo trascorso tra il
 * primo THROTTLE e l'esecuzione effettiva della system call.
 *
 * peak_valid indica se i campi peak_* contengono un campione
 * valido. session_active indica se la sessione è ancora aperta.
 *
 * Tutti i campi reserved devono essere zero.
 */
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

/*
 * Comando minimale usato per verificare la comunicazione con il driver.
 */
#define ST_IOCTL_PING \
    _IO(ST_IOCTL_MAGIC, 0x00)

/*
 * Attivazione e disattivazione del monitor.
 *
 * Questi comandi non trasferiscono dati.
 */
#define ST_IOCTL_ENABLE \
    _IO(ST_IOCTL_MAGIC, 0x01)

#define ST_IOCTL_DISABLE \
    _IO(ST_IOCTL_MAGIC, 0x02)

/*
 * Lettura dello stato corrente.
 *
 * _IOR indica un trasferimento dal kernel verso lo user-space.
 */
#define ST_IOCTL_GET_STATUS \
    _IOR(ST_IOCTL_MAGIC, 0x03, struct st_monitor_status)

/*
 * Gestione del registro UID.
 *
 * _IOW indica che la struttura viene trasferita dallo user-space
 * verso il kernel.
 */
#define ST_IOCTL_UID_ADD \
    _IOW(ST_IOCTL_MAGIC, 0x10, struct st_uid_request)

#define ST_IOCTL_UID_REMOVE \
    _IOW(ST_IOCTL_MAGIC, 0x11, struct st_uid_request)

/*
 * Restituisce il numero di UID registrati.
 *
 * La struttura viene trasferita dal kernel verso lo user-space.
 */
#define ST_IOCTL_UID_GET_COUNT \
    _IOR(ST_IOCTL_MAGIC, 0x12, struct st_uid_count)

/*
 * Restituisce l'elenco degli UID registrati.
 *
 * _IOWR indica che la struttura viene trasferita in entrambe
 * le direzioni tra user-space e kernel.
 */
#define ST_IOCTL_UID_LIST \
    _IOWR(ST_IOCTL_MAGIC, 0x13, struct st_uid_list_request)

/*
 * Gestione del registro dei nomi degli eseguibili.
 *
 * La struttura viene trasferita dallo user-space verso il kernel.
 */
#define ST_IOCTL_PROGRAM_ADD \
    _IOW(ST_IOCTL_MAGIC, 0x20, struct st_program_request)

#define ST_IOCTL_PROGRAM_REMOVE \
    _IOW(ST_IOCTL_MAGIC, 0x21, struct st_program_request)

#define ST_IOCTL_PROGRAM_GET_COUNT \
    _IOR(ST_IOCTL_MAGIC, 0x22, struct st_program_count)

#define ST_IOCTL_PROGRAM_LIST \
    _IOWR(ST_IOCTL_MAGIC, 0x23, struct st_program_list_request)

/*
 * Gestione del registro dei numeri di system call x86-64.
 *
 * Le richieste vengono trasferite dallo user-space al kernel.
 */
#define ST_IOCTL_SYSCALL_ADD \
    _IOW(ST_IOCTL_MAGIC, 0x30, struct st_syscall_request)

#define ST_IOCTL_SYSCALL_REMOVE \
    _IOW(ST_IOCTL_MAGIC, 0x31, struct st_syscall_request)

#define ST_IOCTL_SYSCALL_GET_COUNT \
    _IOR(ST_IOCTL_MAGIC, 0x32, struct st_syscall_count)

#define ST_IOCTL_SYSCALL_LIST \
    _IOWR(ST_IOCTL_MAGIC, 0x33, struct st_syscall_list_request)

/*
 * Configurazione del limite globale di ammissioni.
 */
#define ST_IOCTL_MAX_SET \
    _IOW(ST_IOCTL_MAGIC, 0x40, struct st_max_config)

#define ST_IOCTL_MAX_GET \
    _IOR(ST_IOCTL_MAGIC, 0x41, struct st_max_config)

/*
 * Consultazione e reset delle statistiche.
 *
 * STATS_GET è pubblico e restituisce uno snapshot coerente.
 * STATS_RESET non trasferisce dati ed è riservato a root.
 */
#define ST_IOCTL_STATS_GET \
    _IOR(ST_IOCTL_MAGIC, 0x50, struct st_statistics_snapshot)

#define ST_IOCTL_STATS_RESET \
    _IO(ST_IOCTL_MAGIC, 0x51)

#endif
