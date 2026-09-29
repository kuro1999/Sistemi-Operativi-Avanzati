#!/usr/bin/env python3
"""Scaricamento con waiter: eseguire da utente normale dopo sudo -v.
Richiede modulo caricato, OFF, registri vuoti. Lascia il modulo scaricato.
"""
import os
import subprocess
import time
from pathlib import Path

from statistics_regression import ctl, snapshot, WORKER

N = 8
MODULE = Path("/sys/module/syscall_throttle")


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    check(os.geteuid() != 0,
          "Eseguire come utente normale dopo sudo -v")
    check(MODULE.exists(), "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)
    workers, registered = [], []
    unloading = None

    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))
        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        for _ in range(N):
            workers.append(subprocess.Popen(
                [WORKER, "20"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL))

        deadline = time.monotonic() + 5
        while True:
            report, _, counts, _ = snapshot(N)
            check(all(w.poll() is None for w in workers),
                  "Worker terminato prima dello scaricamento")
            if counts == (N, N, 0, 0, N, N):
                break
            check(time.monotonic() < deadline,
                  "Waiter mancanti:\n" + report)
            time.sleep(0.02)

        print(
            "8 waiter confermati, MAX=0; "
            "avvio rmmod senza DISABLE",
            flush=True)

        start = time.monotonic()
        unloading = subprocess.Popen(
            ["sudo", "-n", "rmmod", "syscall_throttle"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True)

        output, _ = unloading.communicate(timeout=20)
        check(unloading.returncode == 0,
              "rmmod fallito:\n" + output)

        deadline = time.monotonic() + 5
        for worker in workers:
            code = worker.wait(
                timeout=max(0.1, deadline - time.monotonic()))
            check(code == 0, "Worker terminato con errore")

        check(not MODULE.exists(),
              "Modulo ancora presente in sysfs")
        check(not Path("/dev/syscall_throttle").exists(),
              "Device ancora presente")

        print(
            "PASS: rmmod riuscito e tutti gli 8 worker completati "
            f"in {time.monotonic() - start:.3f} s",
            flush=True)

    finally:
        errors = []
        for worker in workers:
            try:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=3)
            except Exception as error:
                errors.append(str(error))

        if unloading is not None and unloading.poll() is None:
            try:
                unloading.communicate(timeout=10)
            except Exception as error:
                errors.append(
                    "Scaricamento ancora pendente: " + str(error))

        if MODULE.exists():
            if unloading is not None and unloading.poll() is None:
                errors.append(
                    "Modulo in scaricamento: non invio altre ioctl")
            else:
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
        "PASS COMPLESSIVO: scaricamento con waiter; "
        "modulo ora non caricato",
        flush=True)


if __name__ == "__main__":
    main()
