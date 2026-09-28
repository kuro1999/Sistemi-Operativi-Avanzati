#!/usr/bin/env python3
"""Deregistrazione di una syscall con un waiter e MAX=0.
Richiede modulo caricato, monitor OFF, registri vuoti e sudo -v.
Esito desiderato: il waiter prosegue dopo la deregistrazione.
"""
import subprocess
import time

from statistics_regression import ctl, snapshot, WORKER


def main():
    if "disattivato" not in ctl("status"):
        raise RuntimeError("Il monitor deve essere disattivato")

    for command in ("uid-count", "program-count", "syscall-count"):
        if int(ctl(command).strip().rsplit(":", 1)[1]) != 0:
            raise RuntimeError("Il test richiede registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)
    worker = None
    program_added = syscall_added = False

    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        program_added = True
        ctl("syscall-add", "35", root=True)
        syscall_added = True
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        worker = subprocess.Popen(
            [WORKER, "20"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        deadline = time.monotonic() + 5
        while True:
            text, _, counters, _ = snapshot(1)
            if counters[4] == 1:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Waiter non osservato:\n" + text)
            time.sleep(0.02)

        print("Thread sospeso confermato; deregistro la syscall 35.",
              flush=True)
        ctl("syscall-remove", "35", root=True)
        syscall_added = False

        try:
            output, error = worker.communicate(timeout=1.5)
        except subprocess.TimeoutExpired:
            text, _, _, _ = snapshot(1)
            print(text, flush=True)
            print(
                "FAIL: syscall deregistrata, ma il thread non è "
                "terminato entro 1,5 secondi con MAX=0.",
                flush=True
            )
            raise SystemExit(1)

        if worker.returncode != 0:
            raise RuntimeError("Worker terminato con errore:\n" + error)

        text, _, counters, _ = snapshot(1)
        assert counters == (1, 1, 1, 0, 0, 1), text
        print(output)
        print(text)
        print("PASS: la deregistrazione ha liberato il waiter.")

    finally:
        try:
            ctl("disable", root=True)
        finally:
            if worker is not None:
                try:
                    worker.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.communicate(timeout=3)

            errors = []
            commands = [("max-set", "0")]
            if syscall_added:
                commands.append(("syscall-remove", "35"))
            if program_added:
                commands.append(("program-remove", "syscall_hook_smoke"))

            for command in commands:
                try:
                    ctl(*command, root=True)
                except Exception as error:
                    errors.append(f"{command}: {error}")

            if errors:
                raise RuntimeError("Cleanup incompleto: " + "; ".join(errors))

        print("Cleanup completato: monitor OFF, registri vuoti, MAX=0.",
              flush=True)


if __name__ == "__main__":
    main()
