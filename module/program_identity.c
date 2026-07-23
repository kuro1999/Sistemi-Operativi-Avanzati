#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/string.h>

#include <syscall_throttle.h>

#include "program_identity.h"

int st_program_get_current_name(char *name, size_t capacity)
{
    struct mm_struct *mm;
    struct file *exe_file;
    struct dentry *exe_dentry;
    struct name_snapshot snapshot;
    size_t length;
    int ret = 0;

    if (name == NULL || capacity == 0U)
        return -EINVAL;

    /*
     * Lasciamo il buffer in uno stato valido anche se una delle
     * operazioni successive fallisce.
     */
    name[0] = '\0';

    /*
     * get_task_mm() restituisce NULL per task privi di address
     * space user-space, come normalmente accade per i kernel
     * thread.
     *
     * In caso di successo acquisisce una reference sull'mm.
     */
    mm = get_task_mm(current);
    if (mm == NULL)
        return -ENOENT;

    /*
     * mm->exe_file è un puntatore RCU.
     *
     * get_file_rcu() legge il puntatore e prova ad acquisire una
     * reference stabile sulla struct file.
     */
    rcu_read_lock();
    exe_file = get_file_rcu(&mm->exe_file);
    rcu_read_unlock();

    /*
     * La reference sull'mm non è più necessaria. Se get_file_rcu()
     * è riuscita, exe_file dispone ormai di una reference propria.
     */
    mmput(mm);

    if (exe_file == NULL)
        return -ENOENT;

    exe_dentry = file_dentry(exe_file);

    /*
     * Copia in modo consistente il nome della dentry, anche in
     * presenza di una possibile rename concorrente.
     */
    take_dentry_name_snapshot(&snapshot, exe_dentry);

    length = snapshot.name.len;

    if (snapshot.name.name == NULL || length == 0U) {
        ret = -ENOENT;
        goto out_release_snapshot;
    }

    /*
     * capacity comprende anche il terminatore NUL.
     *
     * Manteniamo inoltre la semantica UAPI del registro, che
     * accetta nomi lunghi al massimo ST_PROGRAM_NAME_MAX.
     */
    if (length >= capacity ||
        length > ST_PROGRAM_NAME_MAX) {
        ret = -ENAMETOOLONG;
        goto out_release_snapshot;
    }

    memcpy(name, snapshot.name.name, length);
    name[length] = '\0';

out_release_snapshot:
    release_dentry_name_snapshot(&snapshot);
    fput(exe_file);

    return ret;
}
