#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#include <asm/unistd.h>

#include "syscall_registry.h"

/*
 * NR_syscalls deriva dagli header x86 del kernel contro cui
 * il modulo viene compilato.
 *
 * Nel kernel attualmente utilizzato vale 472, quindi gli indici
 * validi sono compresi tra 0 e 471.
 */
#define ST_SYSCALL_LIMIT NR_syscalls

/*
 * Ogni bit rappresenta lo stato di registrazione del numero
 * di system call corrispondente.
 */
static DECLARE_BITMAP(st_syscall_bitmap, ST_SYSCALL_LIMIT);

static DEFINE_MUTEX(st_syscall_registry_lock);
static unsigned int st_syscall_entries_count;

static bool st_syscall_number_is_valid(unsigned int number)
{
    return number < ST_SYSCALL_LIMIT;
}

void st_syscall_registry_init(void)
{
    /*
     * L'inizializzazione è esplicita anche se una variabile
     * statica partirebbe già azzerata.
     */
    bitmap_zero(st_syscall_bitmap, ST_SYSCALL_LIMIT);
    st_syscall_entries_count = 0U;

    pr_info("syscall_throttle: registro system call inizializzato, "
            "limite=%u\n",
            (unsigned int)ST_SYSCALL_LIMIT);
}

void st_syscall_registry_exit(void)
{
    /*
     * Il bitmap non contiene allocazioni dinamiche. È comunque
     * azzerato per lasciare il componente in uno stato definito.
     */
    bitmap_zero(st_syscall_bitmap, ST_SYSCALL_LIMIT);
    st_syscall_entries_count = 0U;

    pr_info("syscall_throttle: registro system call rilasciato\n");
}

int st_syscall_registry_add(unsigned int number)
{
    if (!st_syscall_number_is_valid(number))
        return -EINVAL;

    mutex_lock(&st_syscall_registry_lock);

    if (test_bit(number, st_syscall_bitmap)) {
        mutex_unlock(&st_syscall_registry_lock);
        return -EEXIST;
    }

    /*
     * set_bit() rende atomico l'aggiornamento del singolo bit
     * rispetto ai lettori lockless che usano test_bit().
     */
    set_bit(number, st_syscall_bitmap);
    st_syscall_entries_count++;

    mutex_unlock(&st_syscall_registry_lock);

    return 0;
}

int st_syscall_registry_remove(unsigned int number)
{
    if (!st_syscall_number_is_valid(number))
        return -EINVAL;

    mutex_lock(&st_syscall_registry_lock);

    if (!test_bit(number, st_syscall_bitmap)) {
        mutex_unlock(&st_syscall_registry_lock);
        return -ENOENT;
    }

    clear_bit(number, st_syscall_bitmap);
    st_syscall_entries_count--;

    mutex_unlock(&st_syscall_registry_lock);

    return 0;
}

bool st_syscall_registry_contains(unsigned int number)
{
    if (!st_syscall_number_is_valid(number))
        return false;

    /*
     * Questa lettura non acquisisce il mutex.
     *
     * Le modifiche usano set_bit() e clear_bit(), quindi un
     * lettore concorrente osserverà il valore precedente oppure
     * quello successivo, entrambi stati validi del registro.
     */
    return test_bit(number, st_syscall_bitmap);
}

unsigned int st_syscall_registry_count(void)
{
    unsigned int count;

    mutex_lock(&st_syscall_registry_lock);
    count = st_syscall_entries_count;
    mutex_unlock(&st_syscall_registry_lock);

    return count;
}

int st_syscall_registry_snapshot(__u32 *numbers,
                                 __u32 capacity,
                                 __u32 *count)
{
    unsigned long number;
    __u32 required;
    __u32 index = 0U;

    if (count == NULL)
        return -EINVAL;

    mutex_lock(&st_syscall_registry_lock);

    /*
     * Bitmap e contatore vengono letti sotto lo stesso mutex
     * usato da add() e remove(). Lo snapshot rappresenta quindi
     * uno stato consistente del registro.
     */
    required = (__u32)st_syscall_entries_count;
    *count = required;

    /*
     * Non produciamo liste parziali. Il chiamante riceve la
     * dimensione necessaria e può ripetere l'operazione con
     * un buffer più grande.
     */
    if (capacity < required) {
        mutex_unlock(&st_syscall_registry_lock);
        return -ENOSPC;
    }

    /*
     * Per un registro vuoto non è necessario che numbers punti
     * a un buffer valido.
     */
    if (required == 0U) {
        mutex_unlock(&st_syscall_registry_lock);
        return 0;
    }

    if (numbers == NULL) {
        mutex_unlock(&st_syscall_registry_lock);
        return -EINVAL;
    }

    /*
     * for_each_set_bit() visita i bit impostati in ordine
     * crescente. L'array risultante è quindi già ordinato.
     */
    for_each_set_bit(number,
                     st_syscall_bitmap,
                     ST_SYSCALL_LIMIT) {
        /*
         * Il controllo è difensivo: count e bitmap dovrebbero
         * essere sempre coerenti perché protetti dallo stesso
         * mutex.
         */
        if (index >= required) {
            mutex_unlock(&st_syscall_registry_lock);
            return -EOVERFLOW;
        }

        numbers[index] = (__u32)number;
        index++;
    }

    /*
     * Una differenza indicherebbe una violazione dell'invariante
     * tra il contatore e il numero di bit impostati.
     */
    if (index != required) {
        mutex_unlock(&st_syscall_registry_lock);
        return -EOVERFLOW;
    }

    mutex_unlock(&st_syscall_registry_lock);

    return 0;
}

