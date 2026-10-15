#!/usr/bin/env bash
# Supporto condiviso: le operazioni delle prove rimangono nei tre script.
set -euo pipefail
DEMO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$DEMO_DIR")"
CTL="$ROOT_DIR/user/stctl"
WORKER="$DEMO_DIR/demo_worker"
ALT="$DEMO_DIR/demo_alt"
DEMO_UID="$(id -u)"

ctl() { sudo -n "$CTL" "$@" >/dev/null; }
run_worker() { timeout --kill-after=2s 12s "$@"; }

cleanup() {
    local status=$?
    trap - EXIT
    if ! ctl disable >/dev/null; then status=1; fi
    # Il monitor OFF libera i worker ancora sospesi; ogni worker ha anche un timeout.
    wait || true
    rm -rf -- "$DEMO_TMP"
    if (( status == 0 )); then
        if sudo -n timeout --kill-after=5s 15s rmmod syscall_throttle &&
           [[ ! -d /sys/module/syscall_throttle ]]; then
            : # Scaricamento riuscito; nessuna conferma ripetuta.
        else
            echo "Errore nello scaricamento: controllare sudo dmesg." >&2
            status=1
        fi
    else
        echo "Demo interrotta: modulo lasciato caricato per l'ispezione." >&2
    fi
    exit "$status"
}

start_demo() {
    if (( DEMO_UID == 0 )); then echo "Esegui la demo senza sudo." >&2; exit 1; fi
    if [[ -d /sys/module/syscall_throttle ]]; then
        echo "Modulo gia' caricato: esegui make unload prima della demo." >&2
        exit 1
    fi
    sudo -v
    DEMO_TMP="$(mktemp -d)"
    if ! sudo -n insmod "$ROOT_DIR/module/syscall_throttle.ko"; then
        rm -rf -- "$DEMO_TMP"
        exit 1
    fi
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

wait_ready() {
    # Al massimo cinque secondi per l'avvio del programma che legge dalla pipe.
    for ((attempt=0; attempt<100; attempt++)); do
        [[ -f "$DEMO_TMP/ready" ]] && return 0
        sleep 0.05
    done
    echo "Il worker non e' pronto entro cinque secondi." >&2
    return 1
}

# Riassume l'output del controller senza modificare le statistiche lette.
stats() {
    "$CTL" stats | awk -F': ' '
        /^Thread attualmente bloccati:/ {blocked=$2}
        /^Picco thread bloccati:/ {peak=$2}
        /^Media temporale thread bloccati:/ {average=$2}
        /^Peak delay:/ {delay=$2}
        /^Effective UID del peak:/ {uid=$2}
        /^Programma del peak:/ {program=$2}
        END {printf "  Sospesi=%s, picco=%s, media=%.3f; ritardo max=%s; EUID=%s, programma=%s\n",
                    blocked, peak, average, delay, uid, program}'
}
