#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "device.h"
#include "monitor_state.h"
#include "program_registry.h"
#include "uid_registry.h"

static int __init syscall_throttle_init(void)
{
    int ret;

    st_monitor_state_init();
    st_uid_registry_init();
    st_program_registry_init();

    ret = st_device_init();
    if (ret != 0) {
        st_program_registry_exit();
        st_uid_registry_exit();
        st_monitor_state_exit();
        return ret;
    }

    pr_info("syscall_throttle: modulo caricato\n");
    return 0;
}

static void __exit syscall_throttle_exit(void)
{
    /*
     * Prima impediamo l'arrivo di nuovi comandi user-space,
     * poi rilasciamo i registri e lo stato interno.
     */
    st_device_exit();
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
