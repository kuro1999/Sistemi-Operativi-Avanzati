#ifndef ST_SYSCALL_HOOK_H
#define ST_SYSCALL_HOOK_H

/*
 * Installa gli hook Ftrace per le syscall native x86-64.
 * Le chiamate registrate vengono deviate al wrapper quando il
 * monitor e attivo; il wrapper applica policy e rate limiter.
 */
int st_syscall_hook_init(void);

/*
 * Impedisce nuove deviazioni, rimuove gli hook e attende i wrapper
 * attivi. Il tracepoint di exit/exit_group resta fino al loro termine.
 *
 * Arrestare prima il rate limiter per risvegliare i waiter.
 */
void st_syscall_hook_exit(void);

#endif
