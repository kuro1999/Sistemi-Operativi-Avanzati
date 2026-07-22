#ifndef SYSCALL_THROTTLE_MONITOR_STATE_H
#define SYSCALL_THROTTLE_MONITOR_STATE_H

#include <linux/types.h>

void st_monitor_state_init(void);
void st_monitor_state_exit(void);

void st_monitor_enable(void);
void st_monitor_disable(void);
bool st_monitor_is_enabled(void);

#endif
