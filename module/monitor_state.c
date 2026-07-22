#include <linux/compiler.h>
#include <linux/kernel.h>

#include "monitor_state.h"

/*
 * Stato globale del monitor.
 *
 * La variabile resta privata a questo file: gli altri componenti
 * devono utilizzare le funzioni dichiarate in monitor_state.h.
 */
static bool st_monitor_enabled; //static impedisce modifiche esterne lo vede solo il monitor per accedere devono usare l'api esposta

void st_monitor_state_init(void)
{
    WRITE_ONCE(st_monitor_enabled, false);
    pr_info("syscall_throttle: monitor inizializzato come disattivato\n");
}

void st_monitor_state_exit(void)
{
    WRITE_ONCE(st_monitor_enabled, false);
    pr_info("syscall_throttle: stato del monitor rilasciato\n");
}

void st_monitor_enable(void)
{
    WRITE_ONCE(st_monitor_enabled, true);
    pr_info("syscall_throttle: monitor attivato\n");
}

void st_monitor_disable(void)
{
    WRITE_ONCE(st_monitor_enabled, false);
    pr_info("syscall_throttle: monitor disattivato\n");
}

bool st_monitor_is_enabled(void)
{
    return READ_ONCE(st_monitor_enabled);
}
