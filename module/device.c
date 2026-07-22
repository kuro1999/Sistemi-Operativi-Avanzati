#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include <syscall_throttle.h>

#include "device.h"

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

static long st_device_ioctl(struct file *file,
                            unsigned int command,
                            unsigned long argument)
{
    /*
     * Questi parametri non sono ancora necessari per ST_IOCTL_PING.
     * Verranno usati dai futuri comandi ioctl.
     */
    (void)file;
    (void)argument;

    switch (command) {
    case ST_IOCTL_PING:
        pr_info("syscall_throttle: ricevuto ioctl PING\n");
        return 0;

    default:
        /*
         * ENOTTY è l'errore convenzionale restituito quando
         * un device non riconosce un comando ioctl.
         */
        return -ENOTTY;
    }
}

static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .open = st_device_open,
    .release = st_device_release,
    .unlocked_ioctl = st_device_ioctl,
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

    pr_info("syscall_throttle: device %s registrato, minor=%d\n",
            ST_DEVICE_PATH,
            st_misc_device.minor);

    return 0;
}

void st_device_exit(void)
{
    misc_deregister(&st_misc_device);
    pr_info("syscall_throttle: device deregistrato\n");
}
