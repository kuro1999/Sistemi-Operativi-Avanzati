#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
start_demo
printf '\n[3/3] Syscall particolari e rimozione\n'
echo "  Configurazione iniziale: MAX=1, demo_worker, syscall read."
ctl program-add demo_worker
ctl syscall-add 0
ctl max-set 1

for monitor in OFF ON; do
    rm -f "$DEMO_TMP/ready" "$DEMO_TMP/start"
    run_worker "$WORKER" pipe "$DEMO_TMP/ready" "$DEMO_TMP/start" & pid=$!
    wait_ready
    if [[ "$monitor" == ON ]]; then ctl enable; fi
    echo "→ Pipe con monitor $monitor: due byte disponibili dopo circa 200 ms."
    touch "$DEMO_TMP/start"
    wait "$pid"
    "$CTL" stats >/dev/null
    ctl disable
done
echo "  Confronto: la seconda read ha gia' il dato; con ON puo' attendere il budget."
ctl syscall-remove 0

for call in exit exit_group; do
    if [[ "$call" == exit ]]; then number=60; else number=231; fi
    ctl syscall-add "$number"
    ctl max-set 0
    ctl enable
    run_worker "$WORKER" "$call" & pid=$!
    sleep 0.3
    "$CTL" stats >/dev/null
    ctl max-set 1
    status=0
    wait "$pid" || status=$?
    echo "  $call: MAX=0 → MAX=1; stato processo=$status (42 scelto dal figlio)."
    # Un timeout o un errore non deve essere presentato come completamento regolare.
    [[ "$status" == 42 ]] || exit 1
    ctl disable
    ctl syscall-remove "$number"
done

ctl syscall-add 176
ctl max-set 1
ctl enable
echo "→ Provo a rimuovere syscall_throttle: root, monitor ON, MAX=1, delete_module registrata."
echo "  Attesi: normale EWOULDBLOCK (kernel, riferimento attivo); forzata EPERM (hook)."
echo "  Device chiuso; demo_worker registrato, demo_alt non registrato."
for mode in delete force; do
    for program in "$WORKER" "$ALT"; do
        refs=$(cat /sys/module/syscall_throttle/refcnt)
        result=$(sudo -n timeout --kill-after=2s 12s "$program" "$mode")
        state=$("$CTL" status)
        printf '  %s / %s: %s; ref iniziali=%s; %s\n' \
            "$mode" "$(basename "$program")" "$result" "$refs" "$state"
    done
done
ctl disable
