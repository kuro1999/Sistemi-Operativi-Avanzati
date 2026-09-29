#!/usr/bin/env python3
"""Deregistrazioni concorrenti con MAX=0; richiede OFF e registri vuoti.
Eseguire come utente normale dopo sudo -v.
"""
import os
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from statistics_regression import ctl, snapshot, WORKER

N, ROUNDS, CHANGES = 4, 5, 10
VALUES = {"program": "syscall_hook_smoke", "syscall": "35"}


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    check(os.geteuid() != 0,
          "Eseguire come utente normale dopo sudo -v")
    check(Path("/sys/module/syscall_throttle").exists(),
          "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)
    present = {kind: False for kind in VALUES}
    workers = []

    def change(kind, add):
        ctl(
            kind + ("-add" if add else "-remove"),
            VALUES[kind], root=True)
        present[kind] = add

    def spawn():
        worker = subprocess.Popen(
            [WORKER, "20"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        workers.append(worker)
        return worker

    try:
        change("program", True)
        ctl("max-set", "0", root=True)

        for round_number in range(1, ROUNDS + 1):
            workers = []
            change("syscall", True)
            ctl("enable", root=True)

            for _ in range(N):
                spawn()

            deadline = time.monotonic() + 5
            while True:
                report, _, counts, _ = snapshot(N + 1)
                check(all(w.poll() is None for w in workers),
                      "Worker terminato prima delle modifiche")
                if counts == (N, N, 0, 0, N, N):
                    break
                check(time.monotonic() < deadline,
                      "Waiter mancanti:\n" + report)
                time.sleep(0.01)

            barrier = threading.Barrier(3)
            stop = threading.Event()

            def toggle(kind):
                barrier.wait(timeout=5)
                for _ in range(CHANGES):
                    change(kind, False)
                    change(kind, True)

            def reader():
                samples = 0
                while not stop.is_set():
                    snapshot(N + 1)
                    samples += 1
                    stop.wait(0.01)
                return samples

            with ThreadPoolExecutor(max_workers=3) as pool:
                reading = pool.submit(reader)
                updating = [
                    pool.submit(toggle, kind) for kind in VALUES
                ]
                try:
                    barrier.wait(timeout=5)
                    for future in updating:
                        future.result(timeout=30)
                finally:
                    stop.set()
                samples = reading.result(timeout=12)

            check(samples > 0,
                  "Nessuno snapshot durante le modifiche")

            # Programma e syscall sono di nuovo registrati.
            # Verifichiamo il blocco di una nuova chiamata.
            sentinel = spawn()
            deadline = time.monotonic() + 5
            while True:
                report, _, counts, _ = snapshot(N + 1)
                check(sentinel.poll() is None,
                      "Nuovo worker passato con MAX=0")
                if (
                    counts[0] == N + 1
                    and counts[1] == N + 1
                    and counts[4] >= 1
                ):
                    break
                check(time.monotonic() < deadline,
                      "Nuovo waiter non osservato:\n" + report)
                time.sleep(0.01)

            # Nessun cambio di MAX e nessun DISABLE per liberarli.
            change("syscall", False)

            deadline = time.monotonic() + 5
            for worker in workers:
                code = worker.wait(
                    timeout=max(0.1, deadline - time.monotonic()))
                check(code == 0, "Worker terminato con errore")

            report, _, counts, _ = snapshot(N + 1)
            check(
                counts[:5] == (N + 1, N + 1, N + 1, 0, 0),
                report)
            check(N <= counts[5] <= N + 1, report)

            print(
                f"PASS giro {round_number}/{ROUNDS}: "
                f"{4 * CHANGES} modifiche, {samples} snapshot, "
                f"{N + 1} worker completati; MAX sempre 0",
                flush=True)
            ctl("disable", root=True)

    finally:
        errors = []
        try:
            ctl("disable", root=True)
        except Exception as error:
            errors.append(str(error))

        for worker in workers:
            try:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=3)
            except Exception as error:
                errors.append(str(error))

        for kind in VALUES:
            if present[kind]:
                try:
                    change(kind, False)
                except Exception as error:
                    errors.append(str(error))

        try:
            ctl("max-set", "0", root=True)
        except Exception as error:
            errors.append(str(error))

        check(not errors,
              "Cleanup incompleto: " + "; ".join(errors))
        print(
            "Cleanup: monitor OFF, registri vuoti, MAX=0",
            flush=True)

    print(
        "PASS COMPLESSIVO: modifiche concorrenti programma/syscall",
        flush=True)


if __name__ == "__main__":
    main()
