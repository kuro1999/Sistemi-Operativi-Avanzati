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
 * Identificatore dei comandi ioctl appartenenti al nostro driver.
 */
#define ST_IOCTL_MAGIC 'S'

/*
 * Stato del monitor restituito allo user-space.
 *
 * Non usiamo bool nell'UAPI: __u32 ha una dimensione fissa e rende
 * l'interfaccia binaria più prevedibile.
 */
struct st_monitor_status {
    __u32 enabled;
    __u32 reserved;
};

/*
 * Richiesta relativa a un UID.
 *
 * uid contiene il valore numerico dell'UID nello user-space.
 * reserved deve essere impostato a zero.
 */
struct st_uid_request {
    __u32 uid;
    __u32 reserved;
};

/*
 * Numero di UID attualmente presenti nel registro.
 */
struct st_uid_count {
    __u32 count;
    __u32 reserved;
};

/*
 * Richiesta per ottenere l'elenco degli UID registrati.
 *
 * uids_ptr contiene l'indirizzo user-space di un array di __u32.
 * capacity indica quanti elementi può contenere l'array.
 * count viene scritto dal kernel con il numero di UID registrati.
 * reserved deve essere inizializzato a zero.
 */
struct st_uid_list_request {
    __aligned_u64 uids_ptr;
    __u32 capacity;
    __u32 count;
    __u32 reserved[2];
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

#endif
