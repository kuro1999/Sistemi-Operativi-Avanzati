#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
start_demo
printf '\n[1/3] Selezione delle chiamate\n'

echo "  Configurazione: MAX=1, syscall getpid, programma demo_worker, EUID=$DEMO_UID."
ctl max-set 1
ctl uid-add "$DEMO_UID"
ctl program-add demo_worker
ctl syscall-add 39
"$CTL" uid-list >/dev/null
"$CTL" program-list >/dev/null
"$CTL" syscall-list >/dev/null

"$CTL" status >/dev/null
"$CTL" max-get >/dev/null
echo "  Letture senza sudo riuscite; provo una modifica (atteso rifiuto):"
if "$CTL" max-set 2; then
    echo "La modifica e' stata accettata: risultato inatteso." >&2
    exit 1
fi

ctl uid-remove "$DEMO_UID"
echo "→ OFF, tre getpid:"
run_worker "$WORKER" getpid 3
ctl enable
echo "→ ON, selezione per programma, tre getpid:"
run_worker "$WORKER" getpid 3
echo "→ Programma non registrato:"
run_worker "$ALT" getpid 2
echo "→ Syscall non registrata:"
run_worker "$WORKER" getppid 2
ctl disable
ctl program-remove demo_worker

ctl uid-add "$DEMO_UID"
ctl enable
echo "→ Selezione per EUID=$DEMO_UID:"
run_worker "$ALT" getpid 3
ctl disable
ctl uid-remove "$DEMO_UID"
ctl syscall-remove 39
"$CTL" uid-list >/dev/null
"$CTL" program-list >/dev/null
"$CTL" syscall-list >/dev/null
