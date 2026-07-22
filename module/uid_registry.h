#ifndef SYSCALL_THROTTLE_UID_REGISTRY_H
#define SYSCALL_THROTTLE_UID_REGISTRY_H

#include <linux/types.h>
#include <linux/uidgid.h>

void st_uid_registry_init(void);
void st_uid_registry_exit(void);

int st_uid_registry_add(kuid_t uid);
int st_uid_registry_remove(kuid_t uid);

bool st_uid_registry_contains(kuid_t uid);
unsigned int st_uid_registry_count(void);

int st_uid_registry_snapshot(__u32 *uids,
                             __u32 capacity,
                             __u32 *count);

#endif
