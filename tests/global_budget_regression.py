#!/usr/bin/env python3
"""Budget condiviso: sudo da utente normale, modulo caricato, monitor OFF.
Richiede registri vuoti e cc. Non modifica il modulo o il Makefile.
Controlla la prima parte della finestra e il completamento dei worker.
"""
import os
import platform
import pwd
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from statistics_regression import ctl, snapshot

N, MAX = 16, 3
NAMES = ("st_budget_a", "st_budget_b")
NUMBERS = (39, 110)

SOURCE = r"""
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>

_Static_assert(SYS_getpid == 39, "ABI getpid inattesa");
_Static_assert(SYS_getppid == 110, "ABI getppid inattesa");

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    long nr;
    if (strcmp(argv[1], "39") == 0) nr = SYS_getpid;
    else if (strcmp(argv[1], "110") == 0) nr = SYS_getppid;
    else return 2;
    return syscall(nr) > 0 ? 0 : 3;
}
"""


def run(directory, uid, gid):
    workers, registered = [], []
    try:
        for name in NAMES:
            ctl("program-add", name)
            registered.append(("program-remove", name))
        for number in NUMBERS:
            ctl("syscall-add", str(number))
            registered.append(("syscall-remove", str(number)))

        ctl("max-set", "0")
        ctl("enable")

        for name in NAMES:
            for euid, egid in ((0, 0), (uid, gid)):
                for number in NUMBERS:
                    for _ in range(2):
                        workers.append(subprocess.Popen(
                            [str(directory / name), str(number)],
                            user=euid, group=egid, extra_groups=[],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL))

        deadline = time.monotonic() + 8
        while True:
            report, _, counters, _ = snapshot(N)
            if any(w.poll() is not None for w in workers):
                raise RuntimeError(
                    "Un worker e' terminato prima del rilascio")
            if counters == (N, N, 0, 0, N, N):
                break
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "Non osservati tutti i waiter:\n" + report)
            time.sleep(0.02)

        print(
            "16 waiter confermati: "
            "2 programmi x 2 EUID x 2 syscall x 2 processi",
            flush=True)

        # MAX_SET apre una nuova finestra nel limiter attuale.
        # t0 precede la ioctl: il margine temporale e' conservativo.
        t0 = time.monotonic()
        ctl("max-set", str(MAX))

        samples, latest, observed_three = 0, 0.0, False
        while True:
            report, _, counters, _ = snapshot(N)
            elapsed = time.monotonic() - t0

            # Scartiamo campioni troppo vicini al rinnovo.
            if elapsed >= 0.70:
                break

            relevant, blocked, completed, interrupted, waiting, peak = counters

            if (relevant, blocked, interrupted, peak) != (N, N, 0, N):
                raise RuntimeError("Contatori inattesi:\n" + report)

            if completed > MAX:
                raise RuntimeError(
                    "FAIL: superato il budget globale "
                    "nella prima finestra:\n" + report)

            if completed + waiting != N:
                raise RuntimeError(
                    "Contabilita' waiter incoerente:\n" + report)

            observed_three |= (
                completed == MAX and waiting == N - MAX)
            samples += 1
            latest = elapsed

            if elapsed >= 0.50:
                break
            time.sleep(0.02)

        if samples < 3 or latest < 0.30 or not observed_three:
            raise RuntimeError(
                "INCONCLUSIVO: campionamento insufficiente nella "
                "prima finestra; possibile ritardo della VM")

        print(
            f"PASS: prima parte della finestra, "
            f"3 ammissioni e 13 waiter; "
            f"{samples} snapshot, osservazione fino a {latest:.3f} s",
            flush=True)

        deadline = time.monotonic() + 12
        for worker in workers:
            code = worker.wait(
                timeout=max(0.1, deadline - time.monotonic()))
            if code != 0:
                raise RuntimeError(
                    f"Worker terminato con codice {code}")

        report, _, counters, _ = snapshot(N)
        if counters != (N, N, N, 0, 0, N):
            raise RuntimeError(
                "Statistiche finali inattese:\n" + report)

        print(
            "PASS: tutti i 16 worker completati "
            "senza altri cambi di MAX",
            flush=True)
        print(report, flush=True)

    finally:
        errors = []
        try:
            ctl("disable")
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
                ctl(*command)
            except Exception as error:
                errors.append(str(error))

        if errors:
            raise RuntimeError(
                "Cleanup incompleto: " + "; ".join(errors))

        print(
            "Cleanup: monitor OFF, registri vuoti, MAX=0",
            flush=True)


def main():
    uid = int(os.environ.get("SUDO_UID", "0"))
    if os.geteuid() != 0 or uid == 0:
        raise RuntimeError(
            "Eseguire con sudo da un utente non root")

    if platform.machine() != "x86_64":
        raise RuntimeError("Il test richiede x86-64")

    if not Path("/sys/module/syscall_throttle").exists():
        raise RuntimeError("Modulo non caricato")

    if "disattivato" not in ctl("status"):
        raise RuntimeError("Il monitor deve essere OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        if int(ctl(command).strip().rsplit(":", 1)[1]) != 0:
            raise RuntimeError("Il test richiede registri vuoti")

    with tempfile.TemporaryDirectory(prefix="st-budget-") as temporary:
        directory = Path(temporary)
        directory.chmod(0o755)

        source = directory / "worker.c"
        source.write_text(SOURCE)
        binary = directory / NAMES[0]

        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", str(source), "-o", str(binary)],
            check=True)

        binary.chmod(0o755)
        shutil.copy2(binary, directory / NAMES[1])

        print(
            f"EUID dei worker: 0 e {uid}; UID registry vuoto",
            flush=True)

        run(directory, uid, pwd.getpwuid(uid).pw_gid)

    print(
        "PASS COMPLESSIVO: budget condiviso nel caso verificato",
        flush=True)


if __name__ == "__main__":
    main()
