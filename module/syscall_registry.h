#ifndef SYSCALL_THROTTLE_SYSCALL_REGISTRY_H
#define SYSCALL_THROTTLE_SYSCALL_REGISTRY_H

#include <linux/types.h>

void st_syscall_registry_init(void);
void st_syscall_registry_exit(void);

int st_syscall_registry_add(unsigned int number);
int st_syscall_registry_remove(unsigned int number);

bool st_syscall_registry_contains(unsigned int number);
unsigned int st_syscall_registry_count(void);

int st_syscall_registry_snapshot(__u32 *numbers,
                                 __u32 capacity,
                                 __u32 *count);

#endif
