#!/usr/bin/env python3
"""SIGUSR1 senza SA_RESTART e SIGKILL durante il throttling con MAX=0.
Utente normale dopo sudo -v; modulo caricato, monitor OFF e registri vuoti.
"""
import os
import signal
import subprocess
import time
from pathlib import Path

from statistics_regression import ROOT, ctl, snapshot

WORKER = str(ROOT / "tests/syscall_hook_signal")


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_case(sig):
    worker = None
    try:
        ctl("enable", root=True)
        worker = subprocess.Popen(
            [WORKER], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)

        deadline = time.monotonic() + 5
        while True:
            report, _, counts, _ = snapshot(1)
            check(worker.poll() is None,
                  "Worker terminato prima del segnale")
            if counts == (1, 1, 0, 0, 1, 1):
                break
            check(time.monotonic() < deadline,
                  "Waiter non osservato:\n" + report)
            time.sleep(0.02)

        worker.send_signal(sig)
        output, _ = worker.communicate(timeout=5)

        expected_code = 0 if sig == signal.SIGUSR1 else -int(sig)
        check(worker.returncode == expected_code,
              f"Exit code inatteso: {worker.returncode}\n{output}")

        report, fields, counts, _ = snapshot(1)
        check(counts == (1, 1, 0, 1, 0, 1), report)
        check(fields["Peak delay"] == "non disponibile", report)

        print(
            f"PASS: {sig.name}; una attesa interrotta, "
            "zero waiter residui",
            flush=True)
        if output:
            print(output, end="", flush=True)
        print(report, flush=True)

    finally:
        try:
            ctl("disable", root=True)
        finally:
            if worker is not None:
                if worker.poll() is None:
                    worker.kill()
                worker.communicate(timeout=3)


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
          "Compilare tests/syscall_hook_signal")
    subprocess.run(["sudo", "-n", "true"], check=True)

    registered = []
    try:
        ctl("program-add", "syscall_hook_signal", root=True)
        registered.append(("program-remove", "syscall_hook_signal"))

        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))

        ctl("max-set", "0", root=True)
        for sig in (signal.SIGUSR1, signal.SIGKILL):
            run_case(sig)

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
        "PASS COMPLESSIVO: SIGUSR1 senza SA_RESTART e SIGKILL",
        flush=True)


if __name__ == "__main__":
    main()
