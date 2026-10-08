#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "device.h"
#include "monitor_state.h"
#include "program_registry.h"
#include "rate_limiter.h"
#include "statistics.h"
#include "syscall_hook.h"
#include "syscall_registry.h"
#include "uid_registry.h"

static int __init syscall_throttle_init(void)
{
    int ret;

    /*
    * Inizializziamo tutti i sottosistemi prima di installare
    * l'hook, affinché i wrapper delle syscall trovino
    * le strutture dati già pronte all'uso.
    */

    st_monitor_state_init();
    st_uid_registry_init();
    st_program_registry_init();
    st_syscall_registry_init();
    st_rate_limiter_init();
    st_statistics_init();

    ret = st_syscall_hook_init();
    if (ret != 0)
        goto fail_syscall_hook;

    ret = st_device_init();
    if (ret != 0)
        goto fail_device;

    pr_info("syscall_throttle: modulo caricato\n");
    return 0;

fail_device:
    /*
     * Rendiamo prima inutilizzabile il rate limiter e
     * risvegliamo eventuali utilizzatori, poi rimuoviamo
     * definitivamente l'hook.
     */
    st_rate_limiter_exit();
    st_syscall_hook_exit();
    st_statistics_exit();
    goto fail_common;

fail_syscall_hook:
    st_rate_limiter_exit();
    st_statistics_exit();

fail_common:
    st_syscall_registry_exit();
    st_program_registry_exit();
    st_uid_registry_exit();
    st_monitor_state_exit();

    return ret;
}

static void __exit syscall_throttle_exit(void)
{
    /*
     * Prima impediamo l'arrivo di nuovi comandi user-space.
     *
     * Successivamente rimuoviamo l'hook e attendiamo la
     * terminazione di eventuali wrapper già attivi, prima di
     * rilasciare le strutture consultabili dal percorso syscall.
     */
    st_device_exit();

    /*
     * Il rate limiter deve entrare in shutdown prima che
     * st_syscall_hook_exit() attenda i wrapper attivi.
     *
     * In questo modo eventuali wrapper bloccati vengono
     * risvegliati e osservano ST_RATE_LIMITER_SHUTDOWN.
     */
    st_rate_limiter_exit();
    st_syscall_hook_exit();
    st_statistics_exit();

    st_syscall_registry_exit();
    st_program_registry_exit();
    st_uid_registry_exit();
    st_monitor_state_exit();

    pr_info("syscall_throttle: modulo rimosso\n");
}

module_init(syscall_throttle_init);
module_exit(syscall_throttle_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kuro1999");
MODULE_DESCRIPTION("Linux Kernel Module per il throttling delle system call");
MODULE_VERSION("0.1");
