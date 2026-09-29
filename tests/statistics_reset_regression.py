#!/usr/bin/env python3
"""Reset: EPERM, EBUSY con waiter, azzeramento ON/OFF e nuovi eventi.
Utente normale dopo sudo -v; modulo caricato, monitor OFF, registri vuoti.
"""
import errno
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

from statistics_regression import ctl, snapshot, WORKER

# UAPI x86-64: ST_IOCTL_STATS_RESET = _IO('S', 0x51).
RESET_CODE = """
import fcntl, os
fd = os.open('/dev/syscall_throttle', os.O_RDWR)
try:
    try:
        fcntl.ioctl(fd, (ord('S') << 8) | 0x51)
        result = 0
    except OSError as error:
        result = error.errno
    print(result)
finally:
    os.close(fd)
"""


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def reset(expected, root=True):
    command = (
        (["sudo", "-n"] if root else [])
        + [sys.executable, "-c", RESET_CODE]
    )
    actual = int(subprocess.check_output(
        command, text=True, timeout=10).strip())
    check(actual == expected,
          f"STATS_RESET: errno atteso={expected}, ottenuto={actual}")


def wait_counts(n, expected):
    deadline = time.monotonic() + 5
    while True:
        report, _, counts, _ = snapshot(n)
        if counts == expected:
            return report
        check(time.monotonic() < deadline,
              "Contatori attesi non raggiunti:\n" + report)
        time.sleep(0.02)


def finish(workers):
    deadline = time.monotonic() + 5
    for worker in workers:
        code = worker.wait(
            timeout=max(0.1, deadline - time.monotonic()))
        check(code == 0, "Worker terminato con errore")


def main():
    check(os.geteuid() != 0,
          "Eseguire come utente normale dopo sudo -v")
    check(platform.machine() == "x86_64", "Richiesto x86-64")
    check(Path("/sys/module/syscall_throttle").exists(),
          "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)
    workers, registered = [], []

    def spawn():
        worker = subprocess.Popen(
            [WORKER, "20"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        workers.append(worker)
        return worker

    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))
        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        for _ in range(4):
            spawn()
        wait_counts(4, (4, 4, 0, 0, 4, 4))

        reset(errno.EPERM, root=False)
        reset(errno.EBUSY)
        wait_counts(4, (4, 4, 0, 0, 4, 4))
        check(all(w.poll() is None for w in workers),
              "Reset rifiutato ma worker terminati")
        print(
            "PASS: EPERM non-root, EBUSY root; quattro waiter conservati",
            flush=True)

        ctl("max-set", "4", root=True)
        finish(workers)
        wait_counts(4, (4, 4, 4, 0, 0, 4))

        max_before = ctl("max-get")
        reset(0)
        report = wait_counts(4, (0, 0, 0, 0, 0, 0))
        _, fields, _, _ = snapshot(4)
        check(fields["Peak delay"] == "non disponibile", report)
        check(ctl("status").strip() == "Monitor: attivo",
              "Monitor non ON")
        check(ctl("max-get") == max_before,
              "Reset ha modificato MAX")
        print(
            "PASS: reset ON riuscito; contatori azzerati, MAX conservato",
            flush=True)

        ctl("max-set", "0", root=True)
        new_worker = spawn()
        wait_counts(1, (1, 1, 0, 0, 1, 1))
        ctl("max-set", "1", root=True)
        finish([new_worker])
        print(wait_counts(1, (1, 1, 1, 0, 0, 1)), flush=True)
        print(
            "PASS: nuova chiamata conteggiata senza i vecchi eventi",
            flush=True)

        ctl("disable", root=True)
        reset(0)
        report = ctl("stats")
        fields = dict(
            line.split(": ", 1)
            for line in report.splitlines() if ": " in line)

        check(fields["Sessione statistiche"] == "inattiva", report)
        for key in (
            "Invocazioni rilevanti",
            "Invocazioni bloccate",
            "Invocazioni bloccate completate",
            "Attese interrotte da segnale",
            "Thread attualmente bloccati",
            "Picco thread bloccati",
        ):
            check(int(fields[key]) == 0, report)

        check(fields["Peak delay"] == "non disponibile", report)
        check(ctl("stats") == report, "Snapshot OFF non stabile")
        print(
            "PASS: reset OFF riuscito, snapshot vuoto e stabile",
            flush=True)

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

        for command in [*reversed(registered), ("max-set", "0")]:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))

        check(not errors,
              "Cleanup incompleto: " + "; ".join(errors))
        print(
            "Cleanup: monitor OFF, registri vuoti, MAX=0",
            flush=True)

    print("PASS COMPLESSIVO: reset statistiche", flush=True)


if __name__ == "__main__":
    main()
