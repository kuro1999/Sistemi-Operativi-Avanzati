#ifndef SYSCALL_THROTTLE_UAPI_H
#define SYSCALL_THROTTLE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ST_DEVICE_NAME "syscall_throttle"
#define ST_DEVICE_PATH "/dev/" ST_DEVICE_NAME

#define ST_PROGRAM_NAME_MAX 255U
#define ST_PROGRAM_NAME_CAPACITY (ST_PROGRAM_NAME_MAX + 1U)

 // Identificatore dei comandi ioctl appartenenti al driver

#define ST_IOCTL_MAGIC 'S'

 // Stato del monitor restituito allo user-space

struct st_monitor_status {
    __u32 enabled;
};

struct st_uid_request {
    __u32 uid;
};

struct st_uid_count {
    __u32 count;
};

/*
 * Richiesta per ottenere l'elenco degli UID registrati.
 *
 * uids_ptr contiene l'indirizzo user-space di un array di __u32.
 *
 * capacity indica quanti elementi può contenere l'array.
 *
 * count viene scritto dal kernel con il numero di UID registrati.
 */
struct st_uid_list_request {
    __aligned_u64 uids_ptr;
    __u32 capacity;
    __u32 count;
};

/*
 * Richiesta relativa al nome di un programma.
 *
 * name contiene esclusivamente il basename dell'eseguibile
 */
/* Basename terminato da NUL, senza caratteri slash. */
struct st_program_request {
    char name[ST_PROGRAM_NAME_CAPACITY];
};

struct st_program_count {
    __u32 count;
};

struct st_program_name {
    char name[ST_PROGRAM_NAME_CAPACITY];
};

/*
 * Richiesta per ottenere uno snapshot dei programmi registrati.
 *
 * programs_ptr indica un array user-space di struct st_program_name.
 * capacity è il numero di elementi disponibili nell'array.
 * count viene aggiornato con il numero di programmi richiesti oppure effettivamente restituiti.
 */
struct st_program_list_request {
    __aligned_u64 programs_ptr;
    __u32 capacity;
    __u32 count;
};

/*
 * Richiesta relativa a un numero di system call x86-64.
 *
 * number contiene il numero della system call.
 */
struct st_syscall_request {
    __u32 number;
};

 // Risposta contenente il numero di system call attualmente presenti nel registro.

struct st_syscall_count {
    __u32 count;
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
 */
struct st_syscall_list_request {
    __aligned_u64 numbers_ptr;
    __u32 capacity;
    __u32 count;
};

/*
 * Configurazione del limite globale.
 *
 * max_invocations indica il numero massimo di system call
 * rilevanti ammesse durante una finestra di un secondo.
 */
struct st_max_config {
    __aligned_u64 max_invocations;
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
 * Un cambio effettivo di MAX a monitor ON apre una nuova osservazione.
 * I waiter presenti sono inclusi nei contatori relevant/blocked e nel
 * picco iniziale. Il loro delay decorre dall'inizio della nuova sessione.
 * MAX invariato non azzera; MAX_SET a monitor OFF conserva lo snapshot.
 * Budget consumato e scadenza della finestra non vengono azzerati.
 *
 * peak_delay_ns misura il massimo ritardo prima dell'esecuzione della
 * syscall, limitato alla sessione corrente per le attese trasferite.
 *
 * peak_valid indica se i campi peak_* contengono un campione
 * valido. session_active indica se la sessione è ancora aperta.
 */
struct st_statistics_snapshot {
    __aligned_u64 observation_ns;
    __aligned_u64 blocked_thread_time_ns;

    __aligned_u64 relevant_invocations;
    __aligned_u64 blocked_invocations;
    __aligned_u64 completed_blocked_invocations;
    __aligned_u64 interrupted_blocked_invocations;

    __aligned_u64 peak_delay_ns;

    __u32 current_blocked;
    __u32 peak_blocked;

    __u32 peak_euid;

    __u32 peak_valid;
    __u32 session_active;

    char peak_program[ST_PROGRAM_NAME_CAPACITY];

};

/*
 * Le ioctl di controllo del device sono escluse dal throttling,
 * ma restano soggette ai controlli LSM e ai privilegi del driver.
 */

 // Comando minimale usato per verificare la comunicazione con il driver.
/* Il bypass vale solo per comandi ST_IOCTL_* riconosciuti sul device.
 * Non comprende apertura del device o avvio del controller.
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

 // Lettura dello stato corrente.
#define ST_IOCTL_GET_STATUS \
    _IOR(ST_IOCTL_MAGIC, 0x03, struct st_monitor_status)

 // Gestione del registro UID.
#define ST_IOCTL_UID_ADD \
    _IOW(ST_IOCTL_MAGIC, 0x10, struct st_uid_request)

#define ST_IOCTL_UID_REMOVE \
    _IOW(ST_IOCTL_MAGIC, 0x11, struct st_uid_request)

 // Restituisce il numero di UID registrati.
#define ST_IOCTL_UID_GET_COUNT \
    _IOR(ST_IOCTL_MAGIC, 0x12, struct st_uid_count)

 // Restituisce l'elenco degli UID registrati.
#define ST_IOCTL_UID_LIST \
    _IOWR(ST_IOCTL_MAGIC, 0x13, struct st_uid_list_request)

 // Gestione del registro dei nomi degli eseguibili.
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
/* delete_module e registrabile: il wrapper mantiene un riferimento al
 * monitor e ne impedisce l autorimozione ordinaria. O_TRUNC viene
 * rifiutato con EPERM nel percorso intercettato, anche senza matching.
 * Per scaricare: DISABLE, poi rmmod dopo il rilascio dei riferimenti.
 */
#define ST_IOCTL_SYSCALL_ADD \
    _IOW(ST_IOCTL_MAGIC, 0x30, struct st_syscall_request)

#define ST_IOCTL_SYSCALL_REMOVE \
    _IOW(ST_IOCTL_MAGIC, 0x31, struct st_syscall_request)

#define ST_IOCTL_SYSCALL_GET_COUNT \
    _IOR(ST_IOCTL_MAGIC, 0x32, struct st_syscall_count)

#define ST_IOCTL_SYSCALL_LIST \
    _IOWR(ST_IOCTL_MAGIC, 0x33, struct st_syscall_list_request)

 // Configurazione del limite globale di ammissioni.
#define ST_IOCTL_MAX_SET \
    _IOW(ST_IOCTL_MAGIC, 0x40, struct st_max_config)

#define ST_IOCTL_MAX_GET \
    _IOR(ST_IOCTL_MAGIC, 0x41, struct st_max_config)

/* Consultazione pubblica: 0x50 ritirato per il precedente layout del peak. */
#define ST_IOCTL_STATS_GET \
    _IOR(ST_IOCTL_MAGIC, 0x52, struct st_statistics_snapshot)

/* 0x51 non riutilizzato: apparteneva al comando di reset manuale rimosso. */

#endif
