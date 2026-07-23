#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include <syscall_throttle.h>

#include "device.h"
#include "monitor_state.h"
#include "program_registry.h"
#include "uid_registry.h"

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


static long st_ioctl_uid_add(unsigned long argument)
{
    struct st_uid_request request;
    kuid_t uid;
    int ret;

    /*
     * Copiamo la richiesta dalla memoria del processo chiamante
     * verso una struttura locale nella memoria kernel.
     */
    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: UID_ADD fallito per pid=%d: "
                "puntatore user non valido\n",
                current->pid);
        return -EFAULT;
    }

    /*
     * I campi riservati devono essere zero. Questo permette di
     * riconoscere richieste incompatibili con la versione corrente
     * dell'interfaccia.
     */
    if (request.reserved != 0U) {
        pr_warn("syscall_throttle: UID_ADD rifiutato: "
                "campo reserved non nullo\n");
        return -EINVAL;
    }

    /*
     * L'UID ricevuto è un numero user-space. Lo convertiamo nel
     * tipo kuid_t usato internamente dal kernel.
     *
     * Il registro è globale al modulo, quindi interpretiamo il
     * valore rispetto allo user namespace iniziale.
     */
    uid = make_kuid(&init_user_ns, request.uid);
    if (!uid_valid(uid)) {
        pr_warn("syscall_throttle: UID_ADD rifiutato: "
                "UID %u non valido\n",
                request.uid);
        return -EINVAL;
    }

    ret = st_uid_registry_add(uid);

    if (ret == 0) {
        pr_info("syscall_throttle: UID %u registrato\n",
                request.uid);
    } else if (ret == -EEXIST) {
        pr_warn("syscall_throttle: UID %u già registrato\n",
                request.uid);
    } else if (ret == -ENOMEM) {
        pr_err("syscall_throttle: memoria insufficiente "
               "durante la registrazione dell'UID %u\n",
               request.uid);
    }

    return ret;
}


static long st_ioctl_uid_remove(unsigned long argument)
{
    struct st_uid_request request;
    kuid_t uid;
    int ret;

    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: UID_REMOVE fallito per pid=%d: "
                "puntatore user non valido\n",
                current->pid);
        return -EFAULT;
    }

    if (request.reserved != 0U) {
        pr_warn("syscall_throttle: UID_REMOVE rifiutato: "
                "campo reserved non nullo\n");
        return -EINVAL;
    }

    uid = make_kuid(&init_user_ns, request.uid);
    if (!uid_valid(uid)) {
        pr_warn("syscall_throttle: UID_REMOVE rifiutato: "
                "UID %u non valido\n",
                request.uid);
        return -EINVAL;
    }

    ret = st_uid_registry_remove(uid);

    if (ret == 0) {
        pr_info("syscall_throttle: UID %u rimosso\n",
                request.uid);
    } else if (ret == -ENOENT) {
        pr_warn("syscall_throttle: impossibile rimuovere UID %u: "
                "non registrato\n",
                request.uid);
    }

    return ret;
}


static long st_ioctl_uid_get_count(unsigned long argument)
{
    struct st_uid_count result = {
        .count = (__u32)st_uid_registry_count(),
        .reserved = 0U,
    };

    if (copy_to_user((void __user *)argument,
                     &result,
                     sizeof(result)) != 0) {
        pr_warn("syscall_throttle: UID_GET_COUNT fallito per pid=%d: "
                "puntatore user non valido\n",
                current->pid);
        return -EFAULT;
    }

    pr_info("syscall_throttle: UID_GET_COUNT da pid=%d: "
            "%u UID registrati\n",
            current->pid,
            (unsigned int)result.count);

    return 0;
}


static long st_ioctl_uid_list(unsigned long argument)
{
    struct st_uid_list_request request;
    __u32 *uids = NULL;
    __u32 required;
    __u32 actual = 0U;
    int ret;

    /*
     * La struttura principale si trova nello spazio di indirizzamento
     * del processo chiamante.
     */
    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: UID_LIST fallito per pid=%d: "
                "richiesta user non valida\n",
                current->pid);
        return -EFAULT;
    }

    if (request.reserved[0] != 0U ||
        request.reserved[1] != 0U) {
        pr_warn("syscall_throttle: UID_LIST rifiutato: "
                "campi reserved non nulli\n");
        return -EINVAL;
    }

    /*
     * Se lo user-space dichiara una capacità non nulla, deve anche
     * fornire l'indirizzo dell'array nel quale copiare gli UID.
     */
    if (request.capacity != 0U &&
        request.uids_ptr == 0U) {
        pr_warn("syscall_throttle: UID_LIST rifiutato: "
                "array user mancante\n");
        return -EINVAL;
    }

    /*
     * Allochiamo in base alla dimensione reale del registro,
     * non alla capacità dichiarata dallo user-space.
     *
     * In questo modo un utente non privilegiato non può provocare
     * una grande allocazione passando una capacity arbitrariamente
     * elevata.
     */
    required = (__u32)st_uid_registry_count();

    if (request.capacity < required) {
        request.count = required;

        if (copy_to_user((void __user *)argument,
                         &request,
                         sizeof(request)) != 0) {
            return -EFAULT;
        }

        pr_info("syscall_throttle: UID_LIST richiede capacità %u, "
                "ricevuta %u\n",
                (unsigned int)required,
                (unsigned int)request.capacity);

        return -ENOSPC;
    }

    if (required != 0U) {
        uids = kcalloc(required, sizeof(*uids), GFP_KERNEL);
        if (uids == NULL)
            return -ENOMEM;
    }

    /*
     * Lo snapshot copia la lista nell'array kernel mantenendo il
     * mutex soltanto durante l'accesso al registro.
     */
    ret = st_uid_registry_snapshot(uids, required, &actual);

    /*
     * Il registro potrebbe essere cresciuto tra il conteggio
     * precedente e la creazione dello snapshot.
     */
    if (ret == -ENOSPC) {
        request.count = actual;

        if (copy_to_user((void __user *)argument,
                         &request,
                         sizeof(request)) != 0) {
            kfree(uids);
            return -EFAULT;
        }

        pr_info("syscall_throttle: UID_LIST deve essere ripetuto: "
                "nuova capacità richiesta %u\n",
                (unsigned int)actual);

        kfree(uids);
        return -ENOSPC;
    }

    if (ret != 0) {
        kfree(uids);
        return ret;
    }

    /*
     * A questo punto il mutex del registro è già stato rilasciato.
     * Possiamo quindi accedere alla memoria user-space senza
     * bloccare UID_ADD, UID_REMOVE o altre consultazioni.
     */
    if (actual != 0U &&
        copy_to_user(u64_to_user_ptr(request.uids_ptr),
                     uids,
                     actual * sizeof(*uids)) != 0) {
        pr_warn("syscall_throttle: UID_LIST fallito per pid=%d: "
                "array user non valido\n",
                current->pid);
        kfree(uids);
        return -EFAULT;
    }

    request.count = actual;

    /*
     * Restituiamo anche la struttura aggiornata, contenente il
     * numero effettivo di UID copiati.
     */
    if (copy_to_user((void __user *)argument,
                     &request,
                     sizeof(request)) != 0) {
        kfree(uids);
        return -EFAULT;
    }

    pr_info("syscall_throttle: UID_LIST da pid=%d: "
            "%u UID restituiti\n",
            current->pid,
            (unsigned int)actual);

    kfree(uids);
    return 0;
}


static long st_ioctl_program_add(unsigned long argument)
{
    struct st_program_request request;
    int ret;

    /*
     * Copiamo l'intera struttura in memoria kernel.
     * Non dereferenziamo direttamente il puntatore user-space.
     */
    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: PROGRAM_ADD fallito per pid=%d: "
                "richiesta user non valida\n",
                current->pid);
        return -EFAULT;
    }

    if (request.reserved[0] != 0U ||
        request.reserved[1] != 0U) {
        pr_warn("syscall_throttle: PROGRAM_ADD rifiutato: "
                "campi reserved non nulli\n");
        return -EINVAL;
    }

    /*
     * Il registro valida:
     * - nome non vuoto;
     * - terminazione NUL entro la capacità;
     * - assenza del carattere '/';
     * - assenza di duplicati.
     */
    ret = st_program_registry_add(request.name);

    if (ret == 0) {
        pr_info("syscall_throttle: programma '%s' registrato\n",
                request.name);
    } else if (ret == -EEXIST) {
        pr_warn("syscall_throttle: programma '%s' già registrato\n",
                request.name);
    } else if (ret == -EINVAL) {
        /*
         * Non stampiamo request.name: in caso di mancata
         * terminazione NUL, usarlo con %%s non sarebbe sicuro.
         */
        pr_warn("syscall_throttle: PROGRAM_ADD rifiutato: "
                "nome non valido\n");
    } else if (ret == -ENOMEM) {
        pr_err("syscall_throttle: memoria insufficiente durante "
               "la registrazione di un programma\n");
    }

    return ret;
}


static long st_ioctl_program_remove(unsigned long argument)
{
    struct st_program_request request;
    int ret;

    /*
     * La richiesta viene prima copiata interamente in memoria
     * kernel. Il puntatore user-space non viene dereferenziato
     * direttamente.
     */
    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: PROGRAM_REMOVE fallito per pid=%d: "
                "richiesta user non valida\n",
                current->pid);
        return -EFAULT;
    }

    if (request.reserved[0] != 0U ||
        request.reserved[1] != 0U) {
        pr_warn("syscall_throttle: PROGRAM_REMOVE rifiutato: "
                "campi reserved non nulli\n");
        return -EINVAL;
    }

    ret = st_program_registry_remove(request.name);

    if (ret == 0) {
        pr_info("syscall_throttle: programma '%s' rimosso\n",
                request.name);
    } else if (ret == -ENOENT) {
        /*
         * -ENOENT implica che il nome è stato validato dal
         * registro, quindi è sicuro stamparlo con %s.
         */
        pr_warn("syscall_throttle: programma '%s' non registrato\n",
                request.name);
    } else if (ret == -EINVAL) {
        /*
         * Un nome non terminato da NUL non deve essere stampato
         * direttamente come stringa.
         */
        pr_warn("syscall_throttle: PROGRAM_REMOVE rifiutato: "
                "nome non valido\n");
    }

    return ret;
}


static long st_ioctl_program_get_count(unsigned long argument)
{
    struct st_program_count response = {
        .count = st_program_registry_count(),
        .reserved = 0U,
    };

    /*
     * Il conteggio viene prodotto dal kernel e copiato nella
     * struttura indicata dallo user-space.
     */
    if (copy_to_user((void __user *)argument,
                     &response,
                     sizeof(response)) != 0) {
        pr_warn("syscall_throttle: PROGRAM_GET_COUNT fallito "
                "per pid=%d: destinazione user non valida\n",
                current->pid);
        return -EFAULT;
    }

    pr_info("syscall_throttle: PROGRAM_GET_COUNT da pid=%d: "
            "%u programmi registrati\n",
            current->pid,
            response.count);

    return 0;
}


static long st_ioctl_program_list(unsigned long argument)
{
    struct st_program_list_request request;
    struct st_program_name *programs = NULL;
    __u32 required;
    __u32 actual = 0U;
    size_t bytes;
    int ret;

    /*
     * Copiamo prima la descrizione del buffer user-space in una
     * struttura locale kernel.
     */
    if (copy_from_user(&request,
                       (const void __user *)argument,
                       sizeof(request)) != 0) {
        pr_warn("syscall_throttle: PROGRAM_LIST fallito per pid=%d: "
                "richiesta user non valida\n",
                current->pid);
        return -EFAULT;
    }

    if (request.reserved[0] != 0U ||
        request.reserved[1] != 0U) {
        pr_warn("syscall_throttle: PROGRAM_LIST rifiutato: "
                "campi reserved non nulli\n");
        return -EINVAL;
    }

    /*
     * Una capacità positiva richiede un puntatore user-space
     * non nullo.
     */
    if (request.capacity != 0U &&
        request.programs_ptr == 0U) {
        pr_warn("syscall_throttle: PROGRAM_LIST rifiutato: "
                "puntatore nullo con capacità positiva\n");
        return -EINVAL;
    }

    /*
     * La dimensione dell'allocazione viene ricavata dal registro
     * kernel e non dalla capacità dichiarata dallo user-space.
     */
    required = (__u32)st_program_registry_count();
    request.count = required;

    /*
     * Se il buffer dichiarato è troppo piccolo, restituiamo la
     * dimensione necessaria senza produrre una lista parziale.
     */
    if (request.capacity < required) {
        if (copy_to_user((void __user *)argument,
                         &request,
                         sizeof(request)) != 0) {
            return -EFAULT;
        }

        return -ENOSPC;
    }

    /*
     * Registro vuoto: non occorre allocare né copiare un array.
     */
    if (required == 0U) {
        if (copy_to_user((void __user *)argument,
                         &request,
                         sizeof(request)) != 0) {
            return -EFAULT;
        }

        pr_info("syscall_throttle: PROGRAM_LIST da pid=%d: "
                "0 programmi restituiti\n",
                current->pid);
        return 0;
    }

    programs = kcalloc(required,
                       sizeof(*programs),
                       GFP_KERNEL);
    if (programs == NULL)
        return -ENOMEM;

    /*
     * Il registro potrebbe cambiare tra la lettura del conteggio
     * e l'acquisizione del mutex nello snapshot.
     */
    ret = st_program_registry_snapshot(programs,
                                       required,
                                       &actual);

    if (ret == -ENOSPC) {
        /*
         * Il registro è cresciuto: comunichiamo la nuova
         * dimensione necessaria allo user-space.
         */
        request.count = actual;

        if (copy_to_user((void __user *)argument,
                         &request,
                         sizeof(request)) != 0) {
            kfree(programs);
            return -EFAULT;
        }

        kfree(programs);
        return -ENOSPC;
    }

    if (ret != 0) {
        pr_err("syscall_throttle: impossibile creare lo snapshot "
               "dei programmi: errore=%d\n",
               ret);
        kfree(programs);
        return ret;
    }

    request.count = actual;
    bytes = (size_t)actual * sizeof(*programs);

    /*
     * Lo snapshot ha già rilasciato il mutex. La copia verso lo
     * user-space non blocca quindi il registro.
     */
    if (actual != 0U &&
        copy_to_user(u64_to_user_ptr(request.programs_ptr),
                     programs,
                     bytes) != 0) {
        pr_warn("syscall_throttle: PROGRAM_LIST fallito per pid=%d: "
                "array user non valido\n",
                current->pid);
        kfree(programs);
        return -EFAULT;
    }

    kfree(programs);

    /*
     * Restituiamo anche la struttura aggiornata, in particolare
     * il numero effettivo di elementi copiati.
     */
    if (copy_to_user((void __user *)argument,
                     &request,
                     sizeof(request)) != 0) {
        return -EFAULT;
    }

    pr_info("syscall_throttle: PROGRAM_LIST da pid=%d: "
            "%u programmi restituiti\n",
            current->pid,
            actual);

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

    case ST_IOCTL_UID_ADD:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: UID_ADD rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        return st_ioctl_uid_add(argument);

    case ST_IOCTL_UID_REMOVE:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: UID_REMOVE rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        return st_ioctl_uid_remove(argument);

    case ST_IOCTL_UID_GET_COUNT:
        return st_ioctl_uid_get_count(argument);

    case ST_IOCTL_UID_LIST:
        return st_ioctl_uid_list(argument);

    case ST_IOCTL_PROGRAM_ADD:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: PROGRAM_ADD rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        return st_ioctl_program_add(argument);

    case ST_IOCTL_PROGRAM_REMOVE:
        if (!st_caller_is_root()) {
            pr_warn("syscall_throttle: PROGRAM_REMOVE rifiutato: "
                    "pid=%d euid=%u\n",
                    current->pid,
                    __kuid_val(current_euid()));
            return -EPERM;
        }

        return st_ioctl_program_remove(argument);

    case ST_IOCTL_PROGRAM_GET_COUNT:
        return st_ioctl_program_get_count(argument);

    case ST_IOCTL_PROGRAM_LIST:
        return st_ioctl_program_list(argument);

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
