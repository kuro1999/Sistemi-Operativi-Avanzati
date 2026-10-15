#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
start_demo
printf '\n[2/3] Budget e attese\n'
echo "  Configurazione: MAX=2, demo_worker e demo_alt, syscall getpid e getppid."
ctl program-add demo_worker
ctl program-add demo_alt
ctl syscall-add 39
ctl syscall-add 110
ctl max-set 2
ctl enable

echo "→ Sei processi concorrenti: osservare le durate con budget condiviso."
pids=()
for ((i=0; i<3; i++)); do
    run_worker "$WORKER" getpid & pids+=("$!")
    run_worker "$ALT" getppid & pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done
stats
ctl disable

ctl max-set 0
ctl enable
echo "→ MAX=0, quattro processi: statistiche dopo circa 1,5 secondi."
pids=()
for ((i=0; i<4; i++)); do
    run_worker "$WORKER" getpid & pids+=("$!")
done
sleep 0.5
"$CTL" stats >/dev/null
sleep 1
stats
echo "→ Disattivo il monitor e attendo i risultati dei quattro processi."
ctl disable
for pid in "${pids[@]}"; do wait "$pid"; done
stats
