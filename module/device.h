#ifndef SYSCALL_THROTTLE_DEVICE_H
#define SYSCALL_THROTTLE_DEVICE_H

#include <linux/types.h>

/*
 * Solo process context. true significa richiesta gestita, anche se
 * result contiene un errore. false richiede il percorso ordinario.
 */
bool st_device_try_control_ioctl(unsigned int fd, unsigned int command,
                                 unsigned long argument, long *result);

int st_device_init(void);
void st_device_exit(void);

#endif
