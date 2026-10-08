#include <linux/jump_label.h>
#include <linux/kernel.h>

#include "monitor_state.h"

/*
 * Stato globale del monitor rappresentato tramite static key.
 *
 * FALSE:
 * il monitor è disattivato e i jump-label site seguono il
 * percorso rapido.
 *
 * TRUE:
 * il monitor è attivo e i jump-label site entrano nella policy.
 */
DEFINE_STATIC_KEY_FALSE(st_monitor_enabled_key);

void st_monitor_state_exit(void)
{
    /*
     * Durante l'unload il monitor può essere ancora logicamente
     * attivo. Ripristiniamo quindi la key falsa prima che il
     * modulo venga definitivamente rimosso.
     */
    if (st_monitor_is_enabled())
        static_branch_disable(
            &st_monitor_enabled_key);

    pr_info("syscall_throttle: stato del monitor rilasciato\n");
}

void st_monitor_enable(void)
{
    static_branch_enable(
        &st_monitor_enabled_key);

    pr_info("syscall_throttle: monitor attivato\n");
}

void st_monitor_disable(void)
{
    static_branch_disable(
        &st_monitor_enabled_key);

    pr_info("syscall_throttle: monitor disattivato\n");
}

bool st_monitor_is_enabled(void)
{
    /*
     * API generale usata dal control plane.
     *
     * I due fast path delle system call usano invece direttamente
     * st_monitor_fast_path_enabled(), evitando questa chiamata.
     */
    return static_branch_unlikely(
        &st_monitor_enabled_key);
}
