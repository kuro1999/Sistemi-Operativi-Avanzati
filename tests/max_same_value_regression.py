#!/usr/bin/env python3
"""Ripetere MAX_SET con lo stesso valore non deve rinnovare il budget.
Utente normale dopo sudo -v; modulo caricato, OFF e registri vuoti.
"""
import os
import subprocess
import time

from statistics_regression import ctl, snapshot, WORKER


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    check(os.geteuid() != 0, "Eseguire come utente normale dopo sudo -v")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")
    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    workers = []
    registered = []
    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))
        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))
        ctl("max-set", "1", root=True)

        # Timestamp precedente a ENABLE: limite conservativo per
        # osservare soltanto la prima parte della prima finestra.
        start = time.monotonic()
        ctl("enable", root=True)

        first = subprocess.Popen(
            [WORKER, "20"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        workers.append(first)
        check(first.wait(timeout=3) == 0, "Primo worker fallito")
        text, _, counts, _ = snapshot(2)
        check(counts == (1, 0, 0, 0, 0, 0), text)
        print("Prima chiamata completata: budget MAX=1 consumato",
              flush=True)

        second = subprocess.Popen(
            [WORKER, "20"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        workers.append(second)

        while True:
            text, _, counts, _ = snapshot(2)
            check(time.monotonic() - start < 0.40,
                  "INCONCLUDENTE: preparazione troppo lenta; ripetere il test")
            if counts == (2, 1, 0, 0, 1, 1):
                break
            check(second.poll() is None,
                  "Secondo worker terminato prima del blocco atteso:\n" + text)
            time.sleep(0.005)

        print("Seconda chiamata sospesa; ripeto MAX_SET(1)", flush=True)
        ctl("max-set", "1", root=True)
        check(time.monotonic() - start < 0.45,
              "INCONCLUDENTE: MAX_SET troppo tardivo; ripetere il test")

        samples = 0
        while time.monotonic() - start < 0.70:
            text, _, counts, _ = snapshot(2)
            elapsed = time.monotonic() - start
            if elapsed >= 0.70:
                break
            check(counts == (2, 1, 0, 0, 1, 1),
                  "FAIL: ripetere MAX_SET(1) ha liberato la seconda "
                  f"chiamata entro {elapsed:.3f} s da prima di ENABLE.\n"
                  + text)
            samples += 1
            time.sleep(0.01)

        check(samples >= 3,
              "INCONCLUDENTE: campioni insufficienti; ripetere il test")
        print(f"PASS: budget non rinnovato; {samples} snapshot validi",
              flush=True)

        check(second.wait(timeout=3) == 0, "Secondo worker fallito")
        text, _, counts, _ = snapshot(2)
        check(counts == (2, 1, 1, 0, 0, 1), text)
        print("PASS: seconda chiamata completata al rinnovo del budget",
              flush=True)
        print(text, flush=True)
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

    print("PASS COMPLESSIVO: MAX_SET invariato conserva il budget")


if __name__ == "__main__":
    main()
