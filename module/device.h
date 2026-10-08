#ifndef SYSCALL_THROTTLE_DEVICE_H
#define SYSCALL_THROTTLE_DEVICE_H

#include <linux/types.h>

/*
 * Solo process context. true indica richiesta gestita, anche in errore;
 * false richiede il percorso ordinario.
 */
bool st_device_try_control_ioctl(unsigned int fd, unsigned int command,
                                 unsigned long argument, long *result);
int st_device_init(void);
void st_device_exit(void);

#endif
