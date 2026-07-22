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

#endif
