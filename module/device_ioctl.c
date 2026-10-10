#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include <syscall_throttle.h>
#include "device_internal.h"
#include "monitor_state.h"
#include "program_registry.h"
#include "rate_limiter.h"
#include "statistics.h"
#include "syscall_registry.h"
#include "uid_registry.h"

/* Serializza ENABLE, DISABLE, MAX_SET e RESET. I registri hanno lock propri. */
static DEFINE_MUTEX(st_policy_lock);

static long st_ioctl_ping(unsigned long argument)
{
    (void)argument;
    return 0;
}

static long st_ioctl_enable(unsigned long argument)
{
    int ret = 0;

    (void)argument;
    mutex_lock(&st_policy_lock);
    if (!st_monitor_is_enabled()) {
        /* Pubblica ON soltanto dopo aver preparato limiter e statistiche. */
        ret = st_rate_limiter_start();
        if (!ret) {
            st_statistics_session_start();
            st_monitor_enable();
        }
    }
    mutex_unlock(&st_policy_lock);
    return ret;
}

static long st_ioctl_disable(unsigned long argument)
{
    (void)argument;
    mutex_lock(&st_policy_lock);
    if (st_monitor_is_enabled()) {
        /* OFF -> risveglio waiter -> raccolta ritardi e snapshot congelato.
         * Il mutex impedisce ENABLE/RESET durante timer stop e drain.
         */
        st_monitor_disable();
        st_rate_limiter_stop();
        st_statistics_session_stop();
    }
    mutex_unlock(&st_policy_lock);
    return 0;
}

static long st_ioctl_get_status(unsigned long argument)
{
    struct st_monitor_status response = {
        .enabled = st_monitor_is_enabled() ? 1U : 0U,
    };

    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

static long st_ioctl_max_set(unsigned long argument)
{
    struct st_max_config request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved[0] || request.reserved[1])
        return -EINVAL;

    /* Serializza anche la nuova osservazione statistica al cambio di MAX. */
    mutex_lock(&st_policy_lock);
    st_rate_limiter_set_max(request.max_invocations);
    mutex_unlock(&st_policy_lock);
    return 0;
}

static long st_ioctl_max_get(unsigned long argument)
{
    struct st_max_config response = {
        .max_invocations = st_rate_limiter_get_max(),
    };

    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

static long st_ioctl_stats_get(unsigned long argument)
{
    struct st_statistics_snapshot response;

    st_statistics_get_snapshot(&response);
    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

static long st_ioctl_stats_reset(unsigned long argument)
{
    int ret;

    (void)argument;
    mutex_lock(&st_policy_lock);
    ret = st_statistics_reset();
    mutex_unlock(&st_policy_lock);
    return ret;
}

static long st_ioctl_uid_update(unsigned long argument, bool add)
{
    struct st_uid_request request;
    kuid_t uid;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved)
        return -EINVAL;
    /* Il registro globale interpreta gli UID nel namespace iniziale. */
    uid = make_kuid(&init_user_ns, request.uid);
    if (!uid_valid(uid))
        return -EINVAL;
    return add ? st_uid_registry_add(uid) : st_uid_registry_remove(uid);
}

static long st_ioctl_uid_add(unsigned long argument)
{
    return st_ioctl_uid_update(argument, true);
}

static long st_ioctl_uid_remove(unsigned long argument)
{
    return st_ioctl_uid_update(argument, false);
}

static long st_ioctl_program_update(unsigned long argument, bool add)
{
    struct st_program_request request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved[0] || request.reserved[1])
        return -EINVAL;
    /* Il registro valida il nome, inclusa la terminazione NUL. */
    return add ? st_program_registry_add(request.name)
               : st_program_registry_remove(request.name);
}

static long st_ioctl_program_add(unsigned long argument)
{
    return st_ioctl_program_update(argument, true);
}

static long st_ioctl_program_remove(unsigned long argument)
{
    return st_ioctl_program_update(argument, false);
}

static long st_ioctl_syscall_update(unsigned long argument, bool add)
{
    struct st_syscall_request request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved)
        return -EINVAL;
    return add ? st_syscall_registry_add(request.number)
               : st_syscall_registry_remove(request.number);
}

static long st_ioctl_syscall_add(unsigned long argument)
{
    return st_ioctl_syscall_update(argument, true);
}

static long st_ioctl_syscall_remove(unsigned long argument)
{
    return st_ioctl_syscall_update(argument, false);
}

static long st_ioctl_uid_get_count(unsigned long argument)
{
    struct st_uid_count response = { .count = st_uid_registry_count() };

    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

static long st_ioctl_program_get_count(unsigned long argument)
{
    struct st_program_count response = { .count = st_program_registry_count() };

    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

static long st_ioctl_syscall_get_count(unsigned long argument)
{
    struct st_syscall_count response = { .count = st_syscall_registry_count() };

    return copy_to_user((void __user *)argument, &response, sizeof(response))
        ? -EFAULT : 0;
}

enum st_list_kind { ST_LIST_UID, ST_LIST_PROGRAM, ST_LIST_SYSCALL };

/* Alloca sul conteggio reale; snapshot e copie user-space restano separati.
 * ENOSPC comunica la dimensione richiesta anche se il registro cresce.
 */
static long st_ioctl_list_reply(unsigned long argument, void *request, size_t request_size,
    __u32 capacity, __u32 *count, __u64 user_items, enum st_list_kind kind)
{
    void *items = NULL;
    size_t item_size = kind == ST_LIST_PROGRAM ? sizeof(struct st_program_name) : sizeof(__u32);
    __u32 actual = 0, required = kind == ST_LIST_UID ? st_uid_registry_count() :
        kind == ST_LIST_PROGRAM ? st_program_registry_count() : st_syscall_registry_count();
    int ret = -ENOSPC;

    *count = required;
    if (capacity < required)
        goto reply;
    ret = 0;
    /* Solo UID esegue lo snapshot anche con conteggio iniziale zero. */
    if (!required && kind != ST_LIST_UID)
        goto reply;
    if (required) {
        items = kcalloc(required, item_size, GFP_KERNEL);
        if (!items)
            return -ENOMEM;
    }
    ret = kind == ST_LIST_UID ? st_uid_registry_snapshot(items, required, &actual) :
        kind == ST_LIST_PROGRAM ? st_program_registry_snapshot(items, required, &actual) :
        st_syscall_registry_snapshot(items, required, &actual);
    if (ret && ret != -ENOSPC)
        goto out;
    *count = actual;
    if (!ret && actual &&
        copy_to_user(u64_to_user_ptr(user_items), items, (size_t)actual * item_size)) {
        ret = -EFAULT;
        goto out;
    }
reply:
    if (copy_to_user((void __user *)argument, request, request_size))
        ret = -EFAULT;
out:
    kfree(items);
    return ret;
}

static long st_ioctl_uid_list(unsigned long argument)
{
    struct st_uid_list_request request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved[0] || request.reserved[1] ||
        (request.capacity && !request.uids_ptr))
        return -EINVAL;
    return st_ioctl_list_reply(argument, &request, sizeof(request), request.capacity,
        &request.count, request.uids_ptr, ST_LIST_UID);
}

static long st_ioctl_program_list(unsigned long argument)
{
    struct st_program_list_request request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved[0] || request.reserved[1] ||
        (request.capacity && !request.programs_ptr))
        return -EINVAL;
    return st_ioctl_list_reply(argument, &request, sizeof(request), request.capacity,
        &request.count, request.programs_ptr, ST_LIST_PROGRAM);
}

static long st_ioctl_syscall_list(unsigned long argument)
{
    struct st_syscall_list_request request;

    if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (request.reserved[0] || request.reserved[1] ||
        (request.capacity && !request.numbers_ptr))
        return -EINVAL;
    return st_ioctl_list_reply(argument, &request, sizeof(request), request.capacity,
        &request.count, request.numbers_ptr, ST_LIST_SYSCALL);
}

struct st_ioctl_entry {
    unsigned int command;
    long (*handler)(unsigned long argument);
    bool requires_root;
    bool changes_policy;
};

/* Comandi esatti: non basta controllare magic, numero o direzione. */
static const struct st_ioctl_entry st_ioctl_commands[] = {
    { ST_IOCTL_PING,              st_ioctl_ping,              false, false },
    { ST_IOCTL_ENABLE,            st_ioctl_enable,            true,  false },
    { ST_IOCTL_DISABLE,           st_ioctl_disable,           true,  false },
    { ST_IOCTL_GET_STATUS,        st_ioctl_get_status,        false, false },
    { ST_IOCTL_UID_ADD,           st_ioctl_uid_add,           true,  true  },
    { ST_IOCTL_UID_REMOVE,        st_ioctl_uid_remove,        true,  true  },
    { ST_IOCTL_UID_GET_COUNT,     st_ioctl_uid_get_count,     false, false },
    { ST_IOCTL_UID_LIST,          st_ioctl_uid_list,          false, false },
    { ST_IOCTL_PROGRAM_ADD,       st_ioctl_program_add,       true,  true  },
    { ST_IOCTL_PROGRAM_REMOVE,    st_ioctl_program_remove,    true,  true  },
    { ST_IOCTL_PROGRAM_GET_COUNT, st_ioctl_program_get_count, false, false },
    { ST_IOCTL_PROGRAM_LIST,      st_ioctl_program_list,      false, false },
    { ST_IOCTL_SYSCALL_ADD,       st_ioctl_syscall_add,       true,  true  },
    { ST_IOCTL_SYSCALL_REMOVE,    st_ioctl_syscall_remove,    true,  true  },
    { ST_IOCTL_SYSCALL_GET_COUNT, st_ioctl_syscall_get_count, false, false },
    { ST_IOCTL_SYSCALL_LIST,      st_ioctl_syscall_list,      false, false },
    { ST_IOCTL_MAX_SET,           st_ioctl_max_set,           true,  false },
    { ST_IOCTL_MAX_GET,           st_ioctl_max_get,           false, false },
    { ST_IOCTL_STATS_GET,         st_ioctl_stats_get,         false, false },
    { ST_IOCTL_STATS_RESET,       st_ioctl_stats_reset,       true,  false },
};

static const struct st_ioctl_entry *st_ioctl_find(unsigned int command)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(st_ioctl_commands); i++)
        if (st_ioctl_commands[i].command == command)
            return &st_ioctl_commands[i];
    return NULL;
}

bool st_device_is_control_command(unsigned int command)
{
    return st_ioctl_find(command) != NULL;
}

long st_device_ioctl_dispatch(struct file *file, unsigned int command,
                              unsigned long argument)
{
    const struct st_ioctl_entry *entry = st_ioctl_find(command);
    long ret;

    (void)file;
    if (!entry)
        return -ENOTTY;
    if (entry->requires_root && !uid_eq(current_euid(), GLOBAL_ROOT_UID))
        return -EPERM;

    ret = entry->handler(argument);
    /* Il gestore ha rilasciato i lock del registro prima della notifica. */
    if (!ret && entry->changes_policy)
        st_rate_limiter_policy_changed();
    return ret;
}
