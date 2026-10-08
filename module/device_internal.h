#ifndef SYSCALL_THROTTLE_DEVICE_INTERNAL_H
#define SYSCALL_THROTTLE_DEVICE_INTERNAL_H

#include <linux/types.h>

struct file;

bool st_device_is_control_command(unsigned int command);
long st_device_ioctl_dispatch(struct file *file, unsigned int command,
                              unsigned long argument);

#endif
