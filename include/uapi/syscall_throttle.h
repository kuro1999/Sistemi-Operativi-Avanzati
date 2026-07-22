#ifndef SYSCALL_THROTTLE_UAPI_H
#define SYSCALL_THROTTLE_UAPI_H

#include <linux/ioctl.h>

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
 * Comando minimale di test.
 *
 * _IO indica che il comando:
 * - non riceve dati dallo user space;
 * - non restituisce strutture allo user space.
 */
#define ST_IOCTL_PING _IO(ST_IOCTL_MAGIC, 0x00)

#endif
