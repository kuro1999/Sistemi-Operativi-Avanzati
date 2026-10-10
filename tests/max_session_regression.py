#!/usr/bin/env python3
"""Sessioni su MAX_SET: trasferimento, delay, MAX invariato e OFF.
Utente normale dopo sudo -v; modulo caricato, OFF e registri vuoti.
"""
import os
import subprocess
import time
from pathlib import Path

from statistics_regression import ctl, snapshot, WORKER

N = 8


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def peak_in_session(text, fields, elapsed):
    check(fields["Peak delay"] != "non disponibile", text)
    peak = int(fields["Peak delay"].split()[0])
    # Durata osservazione e' stampata con sei decimali.
    check(0 <= peak <= round(elapsed * 1_000_000_000) + 2000,
          "FAIL: il peak comprende tempo precedente alla sessione:\n" + text)
    check(int(fields["Effective UID del peak"]) == os.geteuid(), text)
    check(fields["Programma del peak"] == "syscall_hook_smoke", text)


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while True:
        result = snapshot(N)
        if predicate(result[2]):
            return result
        check(time.monotonic() < deadline, "Timeout:\n" + result[0])
        time.sleep(0.01)


def main():
    check(os.geteuid() != 0, "Eseguire come utente normale dopo sudo -v")
    check(Path("/sys/module/syscall_throttle").exists(), "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")
    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")
    subprocess.run(["sudo", "-n", "true"], check=True)

    workers = []
    registered = []
    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))
        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        for _ in range(N):
            workers.append(subprocess.Popen(
                [WORKER, "20"], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL))
        wait_for(lambda c: c == (N, N, 0, 0, N, N))
        time.sleep(0.35)
        _, _, _, old_elapsed = snapshot(N)

        ctl("max-set", "1", root=True)
        text, fields, counts, elapsed = wait_for(lambda c: c[2] >= 1)
        check(counts[0:2] == (N, N) and counts[3] == 0, text)
        check(counts[4] > 0,
              "INCONCLUDENTE: nessun waiter rimasto; ripetere")
        peak_in_session(text, fields, elapsed)
        print("PASS: MAX 0 -> 1 trasferisce i waiter; peak limitato "
              "alla nuova sessione", flush=True)
        print(f"Durata precedente: {old_elapsed:.6f} s; "
              f"nuova osservazione: {elapsed:.6f} s", flush=True)

        # Il numero trasferito dipende dalle ammissioni prima di MAX=0.
        ctl("max-set", "0", root=True)
        text, fields, counts, elapsed = snapshot(N)
        carried = counts[4]
        check(0 < carried < N,
              "INCONCLUDENTE: nessun trasferimento osservabile; ripetere")
        expected = (carried, carried, 0, 0, carried, carried)
        check(counts == expected, text)
        check(fields["Peak delay"] == "non disponibile", text)
        print(f"PASS: MAX 1 -> 0 apre una nuova sessione con "
              f"{carried} waiter e senza il vecchio peak", flush=True)

        # MAX=0 mantiene sospesi i waiter. I worker gia' ammessi
        # devono invece terminare la loro nanosleep.
        deadline = time.monotonic() + 3
        while sum(w.poll() is None for w in workers) != carried:
            check(time.monotonic() < deadline,
                  "Processi residui diversi dai waiter contabilizzati")
            time.sleep(0.01)

        time.sleep(0.20)
        _, _, _, before_same = snapshot(N)
        ctl("max-set", "0", root=True)
        text, fields, counts, after_same = snapshot(N)
        check(counts == expected and after_same >= before_same, text)
        check(fields["Peak delay"] == "non disponibile", text)
        print("PASS: MAX invariato conserva sessione e waiter", flush=True)

        ctl("disable", root=True)
        for worker in workers:
            check(worker.wait(timeout=5) == 0,
                  "Worker terminato con errore")

        frozen = ctl("stats")
        fields = dict(line.split(": ", 1) for line in frozen.splitlines()
                      if ": " in line)
        check(fields["Sessione statistiche"] == "inattiva", frozen)
        for key, value in {
            "Invocazioni rilevanti": carried,
            "Invocazioni bloccate": carried,
            "Invocazioni bloccate completate": carried,
            "Attese interrotte da segnale": 0,
            "Thread attualmente bloccati": 0,
            "Picco thread bloccati": carried,
        }.items():
            check(int(fields[key]) == value, frozen)

        elapsed = float(fields["Durata osservazione"].split()[0])
        peak_in_session(frozen, fields, elapsed)
        check(int(fields["Peak delay"].split()[0]) >= 190_000_000, frozen)
        print("PASS: DISABLE completa le attese trasferite", flush=True)
        print(frozen, flush=True)

        ctl("max-set", "7", root=True)
        check(ctl("stats") == frozen, "MAX_SET da OFF altera lo snapshot")
        print("PASS: cambio MAX da OFF conserva lo snapshot", flush=True)
    finally:
        errors = []
        try:
            ctl("disable", root=True)
        except Exception as error:
            errors.append(str(error))

        for worker in workers:
            try:
                if worker.poll() is None:
                    worker.terminate()
                try:
                    worker.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=3)
            except Exception as error:
                errors.append(str(error))

        for command in [*reversed(registered), ("max-set", "0")]:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))
        check(not errors, "Cleanup incompleto: " + "; ".join(errors))
        print("Cleanup: monitor OFF, registri vuoti, MAX=0", flush=True)

    print("PASS COMPLESSIVO: sessioni statistiche al cambio di MAX",
          flush=True)


if __name__ == "__main__":
    main()
