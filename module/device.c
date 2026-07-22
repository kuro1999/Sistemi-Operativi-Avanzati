#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include <syscall_throttle.h>

#include "device.h"
#include "monitor_state.h"

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
 * La traccia richiede che le modifiche alla configurazione siano
 * consentite esclusivamente a un thread con effective UID pari a 0.
 */
static bool st_caller_is_root(void)
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static long st_ioctl_get_status(unsigned long argument)
{
    bool enabled = st_monitor_is_enabled();

    struct st_monitor_status status = {
        .enabled = enabled ? 1U : 0U,
        .reserved = 0U,
    };

    if (copy_to_user((void __user *)argument,
                     &status,
                     sizeof(status)) != 0) {
        pr_warn("syscall_throttle: GET_STATUS fallito per pid=%d: "
                "puntatore user non valido\n",
                current->pid);
        return -EFAULT;
    }

    pr_info("syscall_throttle: GET_STATUS da pid=%d: monitor %s\n",
            current->pid,
            enabled ? "attivo" : "disattivato");

    return 0;
}

static long st_device_ioctl(struct file *file,
                            unsigned int command,
                            unsigned long argument)
{
    (void)file;

    switch (command) {
    case ST_IOCTL_PING:
        pr_info("syscall_throttle: ricevuto ioctl PING\n");
        return 0;

    case ST_IOCTL_ENABLE:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: ENABLE rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        st_monitor_enable();
        return 0;

    case ST_IOCTL_DISABLE:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: DISABLE rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        st_monitor_disable();
        return 0;

    case ST_IOCTL_GET_STATUS:
        return st_ioctl_get_status(argument);

    default:
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
