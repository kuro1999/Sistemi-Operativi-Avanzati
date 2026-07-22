#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "device.h"

static int __init syscall_throttle_init(void)
{
    int ret;

    ret = st_device_init();
    if (ret != 0)
        return ret;

    pr_info("syscall_throttle: modulo caricato\n");
    return 0;
}

static void __exit syscall_throttle_exit(void)
{
    st_device_exit();
    pr_info("syscall_throttle: modulo rimosso\n");
}

module_init(syscall_throttle_init);
module_exit(syscall_throttle_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kuro1999");
MODULE_DESCRIPTION("Linux Kernel Module per il throttling delle system call");
MODULE_VERSION("0.1");
