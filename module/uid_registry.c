#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include "uid_registry.h"

struct st_uid_entry {
    kuid_t uid;
    struct list_head node;
};

static LIST_HEAD(st_uid_entries);
static DECLARE_RWSEM(st_uid_registry_lock);
static unsigned int st_uid_entries_count;

/* Richiede il lock del registro in lettura o scrittura.
 * Il puntatore restituito resta protetto solo finche' il lock e' acquisito. */
static struct st_uid_entry *st_uid_find_locked(kuid_t uid)
{
    struct st_uid_entry *entry;

    list_for_each_entry(entry, &st_uid_entries, node)
        if (uid_eq(entry->uid, uid))
            return entry;
    return NULL;
}

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

    down_write(&st_uid_registry_lock);

    list_for_each_entry_safe(entry, next, &st_uid_entries, node) {
        list_del(&entry->node);
        kfree(entry);
    }

    st_uid_entries_count = 0U;

    up_write(&st_uid_registry_lock);

    pr_info("syscall_throttle: registro UID rilasciato\n");
}

int st_uid_registry_add(kuid_t uid)
{
    struct st_uid_entry *new_entry;

    if (!uid_valid(uid))
        return -EINVAL;

    new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
    if (new_entry == NULL)
        return -ENOMEM;

    new_entry->uid = uid;

    down_write(&st_uid_registry_lock);

    if (st_uid_find_locked(uid) != NULL) {
        up_write(&st_uid_registry_lock);
        kfree(new_entry);
        return -EEXIST;
    }

    list_add_tail(&new_entry->node, &st_uid_entries);
    st_uid_entries_count++;

    up_write(&st_uid_registry_lock);

    return 0;
}

int st_uid_registry_remove(kuid_t uid)
{
    struct st_uid_entry *removed_entry;

    if (!uid_valid(uid))
        return -EINVAL;

    down_write(&st_uid_registry_lock);

    removed_entry = st_uid_find_locked(uid);
    if (removed_entry != NULL) {
        list_del(&removed_entry->node);
        st_uid_entries_count--;
    }

    up_write(&st_uid_registry_lock);

    if (removed_entry == NULL)
        return -ENOENT;

    kfree(removed_entry);
    return 0;
}

bool st_uid_registry_contains(kuid_t uid)
{
    bool found;

    if (!uid_valid(uid))
        return false;

    down_read(&st_uid_registry_lock);

    found = st_uid_find_locked(uid) != NULL;

    up_read(&st_uid_registry_lock);

    return found;
}

unsigned int st_uid_registry_count(void)
{
    unsigned int count;

    down_read(&st_uid_registry_lock);
    count = st_uid_entries_count;
    up_read(&st_uid_registry_lock);

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

    down_read(&st_uid_registry_lock);

    total = (__u32)st_uid_entries_count;
    *count = total;

    /*
     * Comunichiamo al chiamante la dimensione necessaria senza
     * produrre una copia parziale del registro.
     */
    if (capacity < total) {
        up_read(&st_uid_registry_lock);
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
            up_read(&st_uid_registry_lock);
            return -EOVERFLOW;
        }

        uids[index] = (__u32)numeric_uid;
        index++;
    }

    up_read(&st_uid_registry_lock);

    return 0;
}

