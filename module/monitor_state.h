#ifndef SYSCALL_THROTTLE_MONITOR_STATE_H
#define SYSCALL_THROTTLE_MONITOR_STATE_H

#include <linux/compiler.h>
#include <linux/jump_label.h>
#include <linux/types.h>

/*
 * Unica sorgente dello stato globale del monitor.
 *
 * La key nasce falsa: il percorso ottimizzato corrisponde quindi
 * al monitor disattivato.
 */
DECLARE_STATIC_KEY_FALSE(st_monitor_enabled_key);

static __always_inline bool
st_monitor_fast_path_enabled(void){
    return static_branch_unlikely(&st_monitor_enabled_key);
}

void st_monitor_state_init(void);
void st_monitor_state_exit(void);

void st_monitor_enable(void);
void st_monitor_disable(void);
bool st_monitor_is_enabled(void);

#endif
