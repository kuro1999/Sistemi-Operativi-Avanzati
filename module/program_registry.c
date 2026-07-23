#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <syscall_throttle.h>

#include "program_identity.h"
#include "program_registry.h"

struct st_program_entry {
    char name[ST_PROGRAM_NAME_CAPACITY];
    struct list_head node;
};

static LIST_HEAD(st_program_entries);
static DEFINE_MUTEX(st_program_registry_lock);
static unsigned int st_program_entries_count;

static int st_program_name_validate(const char *name)
{
    size_t length;

    if (name == NULL)
        return -EINVAL;

    /*
     * strnlen() non legge oltre la capacità massima.
     *
     * Se restituisce ST_PROGRAM_NAME_CAPACITY significa che non
     * è stato trovato un terminatore NUL entro il limite previsto.
     */
    length = strnlen(name, ST_PROGRAM_NAME_CAPACITY);

    if (length == 0U ||
        length >= ST_PROGRAM_NAME_CAPACITY) {
        return -EINVAL;
    }

    /*
     * Il registro conserva esclusivamente il basename, non un
     * percorso completo o relativo.
     */
    if (memchr(name, '/', length) != NULL)
        return -EINVAL;

    return 0;
}

void st_program_registry_init(void)
{
    INIT_LIST_HEAD(&st_program_entries);
    st_program_entries_count = 0U;

    pr_info("syscall_throttle: registro programmi inizializzato\n");
}

void st_program_registry_exit(void)
{
    struct st_program_entry *entry;
    struct st_program_entry *next;

    mutex_lock(&st_program_registry_lock);

    list_for_each_entry_safe(entry,
                             next,
                             &st_program_entries,
                             node) {
        list_del(&entry->node);
        kfree(entry);
    }

    st_program_entries_count = 0U;

    mutex_unlock(&st_program_registry_lock);

    pr_info("syscall_throttle: registro programmi rilasciato\n");
}

int st_program_registry_add(const char *name)
{
    struct st_program_entry *entry;
    struct st_program_entry *new_entry;
    int ret;

    ret = st_program_name_validate(name);
    if (ret != 0)
        return ret;

    new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
    if (new_entry == NULL)
        return -ENOMEM;

    /*
     * La validazione precedente garantisce che il nome sia
     * terminato entro la capacità disponibile.
     */
    strscpy(new_entry->name,
            name,
            sizeof(new_entry->name));

    mutex_lock(&st_program_registry_lock);

    list_for_each_entry(entry, &st_program_entries, node) {
        if (strcmp(entry->name, name) == 0) {
            mutex_unlock(&st_program_registry_lock);
            kfree(new_entry);
            return -EEXIST;
        }
    }

    list_add_tail(&new_entry->node, &st_program_entries);
    st_program_entries_count++;

    mutex_unlock(&st_program_registry_lock);

    return 0;
}

int st_program_registry_remove(const char *name)
{
    struct st_program_entry *entry;
    struct st_program_entry *next;
    struct st_program_entry *removed_entry = NULL;
    int ret;

    ret = st_program_name_validate(name);
    if (ret != 0)
        return ret;

    mutex_lock(&st_program_registry_lock);

    list_for_each_entry_safe(entry,
                             next,
                             &st_program_entries,
                             node) {
        if (strcmp(entry->name, name) == 0) {
            list_del(&entry->node);
            st_program_entries_count--;
            removed_entry = entry;
            break;
        }
    }

    mutex_unlock(&st_program_registry_lock);

    if (removed_entry == NULL)
        return -ENOENT;

    kfree(removed_entry);
    return 0;
}

bool st_program_registry_contains(const char *name)
{
    struct st_program_entry *entry;
    bool found = false;

    if (st_program_name_validate(name) != 0)
        return false;

    mutex_lock(&st_program_registry_lock);

    list_for_each_entry(entry, &st_program_entries, node) {
        if (strcmp(entry->name, name) == 0) {
            found = true;
            break;
        }
    }

    mutex_unlock(&st_program_registry_lock);

    return found;
}


int st_program_registry_snapshot(struct st_program_name *programs,
                                 __u32 capacity,
                                 __u32 *count)
{
    struct st_program_entry *entry;
    __u32 required;
    __u32 index = 0U;
    ssize_t copied;

    if (count == NULL)
        return -EINVAL;

    if (capacity != 0U && programs == NULL)
        return -EINVAL;

    mutex_lock(&st_program_registry_lock);

    required = (__u32)st_program_entries_count;
    *count = required;

    /*
     * Comunichiamo al chiamante la dimensione necessaria senza
     * produrre uno snapshot parziale.
     */
    if (capacity < required) {
        mutex_unlock(&st_program_registry_lock);
        return -ENOSPC;
    }

    list_for_each_entry(entry, &st_program_entries, node) {
        /*
         * Azzera anche i byte successivi al terminatore NUL,
         * evitando di esporre dati residui della memoria kernel.
         */
        memset(&programs[index], 0, sizeof(programs[index]));

        copied = strscpy(programs[index].name,
                         entry->name,
                         sizeof(programs[index].name));

        /*
         * Non dovrebbe accadere perché i nomi presenti nel
         * registro sono già stati validati all'inserimento.
         */
        if (copied < 0) {
            mutex_unlock(&st_program_registry_lock);
            return -EOVERFLOW;
        }

        index++;
    }

    mutex_unlock(&st_program_registry_lock);
    return 0;
}


bool st_program_registry_contains_current(void)
{
    char name[ST_PROGRAM_NAME_CAPACITY];

    /*
     * Un task privo di eseguibile user-space, oppure un errore
     * nell'identificazione, non corrisponde ad alcun programma
     * registrato.
     */
    if (st_program_get_current_name(name, sizeof(name)) != 0)
        return false;

    return st_program_registry_contains(name);
}

unsigned int st_program_registry_count(void)
{
    unsigned int count;

    mutex_lock(&st_program_registry_lock);
    count = st_program_entries_count;
    mutex_unlock(&st_program_registry_lock);

    return count;
}
