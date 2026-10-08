#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/security.h>

#include <syscall_throttle.h>
#include "device.h"
#include "device_internal.h"

static const struct file_operations st_file_operations = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = st_device_ioctl_dispatch,
};

bool st_device_try_control_ioctl(unsigned int fd, unsigned int command,
                                 unsigned long argument, long *result)
{
    struct file *file;
    long ret;

    if (!result || !st_device_is_control_command(command))
        return false;

    file = fget(fd);
    if (!file)
        return false;
    if (file->f_op != &st_file_operations) {
        fput(file);
        return false;
    }

    /* Usa lo stesso file referenziato anche in caso di close/dup2 concorrenti.
     * Le ioctl private riconosciute mantengono il controllo LSM ordinario.
     */
    ret = security_file_ioctl(file, command, argument);
    if (!ret)
        ret = st_device_ioctl_dispatch(file, command, argument);
    if (ret == -ENOIOCTLCMD)
        ret = -ENOTTY;

    fput(file);
    *result = ret;
    return true;
}

static struct miscdevice st_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = ST_DEVICE_NAME,
    .fops = &st_file_operations,
    .mode = 0666,
};

int st_device_init(void)
{
    int ret = misc_register(&st_misc_device);

    if (ret) {
        pr_err("syscall_throttle: registrazione device fallita: %d\n", ret);
        return ret;
    }
    pr_info("syscall_throttle: device %s registrato, minor=%d\n",
            ST_DEVICE_PATH, st_misc_device.minor);
    return 0;
}

void st_device_exit(void)
{
    misc_deregister(&st_misc_device);
    pr_info("syscall_throttle: device deregistrato\n");
}
