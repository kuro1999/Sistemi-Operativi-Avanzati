#ifndef SYSCALL_THROTTLE_PROGRAM_IDENTITY_H
#define SYSCALL_THROTTLE_PROGRAM_IDENTITY_H

#include <linux/types.h>

/*
 * Copia nel buffer il basename dell'eseguibile associato
 * al task corrente.
 *
 * Restituisce:
 *   0              nome ottenuto correttamente
 *   -EINVAL        buffer non valido
 *   -ENOENT        task privo di mm o di file eseguibile
 *   -ENAMETOOLONG  nome incompatibile con la capacità fornita
 */
int st_program_get_current_name(char *name, size_t capacity);

#endif
