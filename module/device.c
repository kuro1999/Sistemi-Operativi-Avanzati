#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "device.h"

#define ST_DEVICE_NAME "syscall_throttle"

static int st_device_open(struct inode *inode, struct file *file)
{
    pr_info("syscall_throttle: device aperto\n");
    return 0;
}

static int st_device_release(struct inode *inode, struct file *file)
{
    pr_info("syscall_throttle: device chiuso\n");
    return 0;
}

/*
 * Tabella delle operazioni supportate dal character device.
 *
 * In seguito aggiungeremo:
 *     .unlocked_ioctl = st_device_ioctl
 */
static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .open = st_device_open,
    .release = st_device_release,
};

static struct miscdevice st_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = ST_DEVICE_NAME,
    .fops = &st_file_operations,
    .mode = 0666,
};

int st_device_init(void)
{
    int ret;

    ret = misc_register(&st_misc_device);
    if (ret != 0) {
        pr_err("syscall_throttle: registrazione device fallita: %d\n",
               ret);
        return ret;
    }

    pr_info("syscall_throttle: device /dev/%s registrato, minor=%d\n",
            ST_DEVICE_NAME,
            st_misc_device.minor);

    return 0;
}

void st_device_exit(void)
{
    misc_deregister(&st_misc_device);
    pr_info("syscall_throttle: device deregistrato\n");
}
