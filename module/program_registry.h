#ifndef SYSCALL_THROTTLE_PROGRAM_REGISTRY_H
#define SYSCALL_THROTTLE_PROGRAM_REGISTRY_H

#include <linux/types.h>

struct st_program_name;

void st_program_registry_init(void);
void st_program_registry_exit(void);

int st_program_registry_add(const char *name);
int st_program_registry_remove(const char *name);

bool st_program_registry_contains(const char *name);
bool st_program_registry_contains_current(void);
unsigned int st_program_registry_count(void);

int st_program_registry_snapshot(struct st_program_name *programs,
                                 __u32 capacity,
                                 __u32 *count);

#endif
