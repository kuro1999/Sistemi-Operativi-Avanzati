#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include "uid_registry.h"

struct st_uid_entry {
    kuid_t uid;
    struct list_head node;
};

static LIST_HEAD(st_uid_entries);
static DEFINE_MUTEX(st_uid_registry_lock);
static unsigned int st_uid_entries_count;

void st_uid_registry_init(void)
{
    INIT_LIST_HEAD(&st_uid_entries);
    st_uid_entries_count = 0U;

    pr_info("syscall_throttle: registro UID inizializzato\n");
}

void st_uid_registry_exit(void)
{
    struct st_uid_entry *entry;
    struct st_uid_entry *next;

    mutex_lock(&st_uid_registry_lock);

    list_for_each_entry_safe(entry, next, &st_uid_entries, node) {
        list_del(&entry->node);
        kfree(entry);
    }

    st_uid_entries_count = 0U;

    mutex_unlock(&st_uid_registry_lock);

    pr_info("syscall_throttle: registro UID rilasciato\n");
}

int st_uid_registry_add(kuid_t uid)
{
    struct st_uid_entry *entry;
    struct st_uid_entry *new_entry;

    if (!uid_valid(uid))
        return -EINVAL;

    new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
    if (new_entry == NULL)
        return -ENOMEM;

    new_entry->uid = uid;

    mutex_lock(&st_uid_registry_lock);

    list_for_each_entry(entry, &st_uid_entries, node) {
        if (uid_eq(entry->uid, uid)) {
            mutex_unlock(&st_uid_registry_lock);
            kfree(new_entry);
            return -EEXIST;
        }
    }

    list_add_tail(&new_entry->node, &st_uid_entries);
    st_uid_entries_count++;

    mutex_unlock(&st_uid_registry_lock);

    return 0;
}

int st_uid_registry_remove(kuid_t uid)
{
    struct st_uid_entry *entry;
    struct st_uid_entry *next;
    struct st_uid_entry *removed_entry = NULL;

    if (!uid_valid(uid))
        return -EINVAL;

    mutex_lock(&st_uid_registry_lock);

    list_for_each_entry_safe(entry, next, &st_uid_entries, node) {
        if (uid_eq(entry->uid, uid)) {
            list_del(&entry->node);
            st_uid_entries_count--;
            removed_entry = entry;
            break;
        }
    }

    mutex_unlock(&st_uid_registry_lock);

    if (removed_entry == NULL)
        return -ENOENT;

    kfree(removed_entry);
    return 0;
}

bool st_uid_registry_contains(kuid_t uid)
{
    struct st_uid_entry *entry;
    bool found = false;

    if (!uid_valid(uid))
        return false;

    mutex_lock(&st_uid_registry_lock);

    list_for_each_entry(entry, &st_uid_entries, node) {
        if (uid_eq(entry->uid, uid)) {
            found = true;
            break;
        }
    }

    mutex_unlock(&st_uid_registry_lock);

    return found;
}

unsigned int st_uid_registry_count(void)
{
    unsigned int count;

    mutex_lock(&st_uid_registry_lock);
    count = st_uid_entries_count;
    mutex_unlock(&st_uid_registry_lock);

    return count;
}

int st_uid_registry_snapshot(__u32 *uids,
                             __u32 capacity,
                             __u32 *count)
{
    struct st_uid_entry *entry;
    __u32 total;
    __u32 index = 0U;

    if (count == NULL)
        return -EINVAL;

    if (capacity != 0U && uids == NULL)
        return -EINVAL;

    mutex_lock(&st_uid_registry_lock);

    total = (__u32)st_uid_entries_count;
    *count = total;

    /*
     * Comunichiamo al chiamante la dimensione necessaria senza
     * produrre una copia parziale del registro.
     */
    if (capacity < total) {
        mutex_unlock(&st_uid_registry_lock);
        return -ENOSPC;
    }

    list_for_each_entry(entry, &st_uid_entries, node) {
        uid_t numeric_uid;

        /*
         * Il registro conserva kuid_t. Per l'UAPI dobbiamo
         * riconvertire ogni elemento nel valore numerico
         * appartenente allo user namespace iniziale.
         */
        numeric_uid = from_kuid(&init_user_ns, entry->uid);
        if (numeric_uid == (uid_t)-1) {
            mutex_unlock(&st_uid_registry_lock);
            return -EOVERFLOW;
        }

        uids[index] = (__u32)numeric_uid;
        index++;
    }

    mutex_unlock(&st_uid_registry_lock);

    return 0;
}

