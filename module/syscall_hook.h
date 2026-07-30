#ifndef ST_SYSCALL_HOOK_H
#define ST_SYSCALL_HOOK_H

/*
 * Installa il componente di intercettazione Ftrace.
 *
 * L'implementazione corrente intercetta __x64_sys_nanosleep,
 * verifica la rilevanza della chiamata e applica il rate limiter
 * prima dell'esecuzione della system call originale.
 */
int st_syscall_hook_init(void);

/*
 * Impedisce nuove deviazioni, rimuove l'hook e attende la
 * terminazione di tutti i wrapper già attivi.
 *
 * Il rate limiter deve essere arrestato prima di questa funzione,
 * così eventuali wrapper bloccati possono essere risvegliati.
 */
void st_syscall_hook_exit(void);

#endif
