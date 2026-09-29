#!/usr/bin/env python3
"""DISABLE con MAX=0: rilascio waiter, snapshot congelato, nuova sessione.
Utente normale dopo sudo -v; modulo caricato, monitor OFF, registri vuoti.
"""
import os
import subprocess
import time
from pathlib import Path

from statistics_regression import ctl, snapshot, WORKER


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_case(n):
    workers = []
    try:
        ctl("enable", root=True)
        report, _, counts, _ = snapshot(n)
        check(counts == (0, 0, 0, 0, 0, 0),
              "Sessione nuova con contatori non vuoti:\n" + report)

        for _ in range(n):
            workers.append(subprocess.Popen(
                [WORKER, "20"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL))

        deadline = time.monotonic() + 5
        while True:
            report, _, counts, _ = snapshot(n)
            check(all(w.poll() is None for w in workers),
                  "Un worker e' terminato con MAX=0")
            if counts == (n, n, 0, 0, n, n):
                break
            check(time.monotonic() < deadline,
                  "Non osservati tutti i waiter:\n" + report)
            time.sleep(0.02)

        print(
            f"Caso {n}: tutti i waiter sospesi; invio DISABLE",
            flush=True)

        start = time.monotonic()
        ctl("disable", root=True)
        frozen = ctl("stats")

        deadline = time.monotonic() + 5
        for worker in workers:
            code = worker.wait(
                timeout=max(0.1, deadline - time.monotonic()))
            check(code == 0, f"Worker terminato con codice {code}")

        elapsed = time.monotonic() - start
        check("disattivato" in ctl("status"), "Monitor ancora ON")
        check(ctl("stats") == frozen,
              "Snapshot modificato dopo DISABLE")

        fields = dict(
            line.split(": ", 1)
            for line in frozen.splitlines() if ": " in line)

        expected = {
            "Invocazioni rilevanti": n,
            "Invocazioni bloccate": n,
            "Attese interrotte da segnale": 0,
            "Thread attualmente bloccati": 0,
            "Picco thread bloccati": n,
        }

        check(fields["Sessione statistiche"] == "inattiva", frozen)
        for key, value in expected.items():
            check(int(fields[key]) == value, frozen)

        completed = int(fields["Invocazioni bloccate completate"])
        check(completed == n, frozen)
        check(int(fields["Peak delay"].split()[0]) > 0, frozen)
        check(int(fields["System call del peak"]) == 35, frozen)
        check(int(fields["Effective UID del peak"]) == os.geteuid(), frozen)
        check(fields["Programma del peak"] == "syscall_hook_smoke", frozen)

        average = float(fields["Media temporale thread bloccati"])
        check(0 <= average <= n + 0.000002, frozen)

        # MAX e registri invariati: OFF deve consentire nuove chiamate.
        subprocess.run(
            [WORKER, "20"], check=True, timeout=5,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)

        check(ctl("stats") == frozen,
              "Una chiamata a monitor OFF ha modificato le statistiche")

        ctl("disable", root=True)
        check(ctl("stats") == frozen,
              "DISABLE ripetuto ha modificato lo snapshot")

        print(
            f"PASS: {n} worker completati dopo DISABLE "
            f"in {elapsed:.3f} s; chiamata OFF e "
            "DISABLE ripetuto verificati",
            flush=True)
        print(frozen, flush=True)

    finally:
        try:
            ctl("disable", root=True)
        finally:
            for worker in workers:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=3)


def main():
    check(os.geteuid() != 0,
          "Eseguire come utente normale dopo sudo -v")
    check(Path("/sys/module/syscall_throttle").exists(),
          "Modulo non caricato")
    check("disattivato" in ctl("status"),
          "Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    check(Path(WORKER).is_file(),
          "Worker mancante: eseguire make -C tests")
    subprocess.run(["sudo", "-n", "true"], check=True)

    registered = []
    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))

        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))

        ctl("max-set", "0", root=True)
        initial_max = ctl("max-get")

        for n in (8, 1):
            run_case(n)
            check(ctl("max-get") == initial_max, "MAX modificato")

    finally:
        errors = []
        commands = [
            ("disable",),
            *reversed(registered),
            ("max-set", "0"),
        ]
        for command in commands:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))

        check(not errors,
              "Cleanup incompleto: " + "; ".join(errors))
        print(
            "Cleanup: monitor OFF, registri vuoti, MAX=0",
            flush=True)

    print(
        "PASS COMPLESSIVO: DISABLE, bypass OFF e nuova sessione",
        flush=True)


if __name__ == "__main__":
    main()
