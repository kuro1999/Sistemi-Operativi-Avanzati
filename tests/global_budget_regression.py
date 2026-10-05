#!/usr/bin/env python3
"""Budget condiviso: sudo da utente normale, modulo caricato, monitor OFF.
Richiede registri vuoti e cc. Non modifica il modulo o il Makefile.
Controlla la prima parte della finestra e il completamento dei worker.
"""
import os
import platform
import pwd
import select
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
    char gate;
    if (write(STDOUT_FILENO, "R", 1) != 1) return 4;
    if (read(STDIN_FILENO, &gate, 1) != 1 || gate != 'G') return 5;
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

        ctl("max-set", str(MAX))

        for name in NAMES:
            for euid, egid in ((0, 0), (uid, gid)):
                for number in NUMBERS:
                    for _ in range(2):
                        workers.append(subprocess.Popen(
                            [str(directory / name), str(number)],
                            user=euid, group=egid, extra_groups=[],
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL))

        # Preparazione fuori dalla finestra: ogni worker attende il gate.
        for worker in workers:
            ready, _, _ = select.select([worker.stdout], [], [], 5)
            if not ready or os.read(worker.stdout.fileno(), 1) != b"R":
                raise RuntimeError("Worker non pronto al gate")
        if any(worker.poll() is not None for worker in workers):
            raise RuntimeError("Worker terminato prima di ENABLE")
        print("16 worker pronti: 2 programmi x 2 EUID x 2 syscall x 2 processi",
              flush=True)

        # ENABLE da OFF avvia la finestra. MAX resta 3 per tutta la prova.
        # Il timestamp precedente alla ioctl rende il margine conservativo.
        t0 = time.monotonic()
        ctl("enable")
        for worker in workers:
            worker.stdin.write(b"G")
            worker.stdin.flush()
            worker.stdin.close()

        expected = (N, N - MAX, 0, 0, N - MAX, N - MAX)
        samples, first, latest = 0, None, 0.0
        while True:
            report, _, counters, _ = snapshot(N)
            codes = [worker.poll() for worker in workers]
            elapsed = time.monotonic() - t0
            # Non attribuire alla prima finestra dati raccolti troppo tardi.
            if elapsed >= 0.70:
                break
            if any(code not in (None, 0) for code in codes):
                raise RuntimeError("Worker terminato con errore")
            finished = sum(code == 0 for code in codes)
            if finished > MAX:
                raise RuntimeError("FAIL: oltre 3 syscall completate prima "
                                   "del rinnovo della finestra\n" + report)
            if counters == expected and finished == MAX:
                if first is None:
                    first = elapsed
                samples += 1
                latest = elapsed
                if latest >= 0.50 and latest - first >= 0.20 and samples >= 3:
                    break
            elif first is not None:
                raise RuntimeError("FAIL: budget o waiter cambiati prima "
                                   "del rinnovo\n" + report)
            time.sleep(0.02)
        if (first is None or samples < 3 or latest < 0.50
                or latest - first < 0.20):
            raise RuntimeError("INCONCLUSIVO: osservazione insufficiente "
                               "entro la prima finestra; ripetere il test")
        print(f"PASS: 3 ammissioni globali e 13 waiter; {samples} snapshot "
              f"stabili fino a {latest:.3f} s da prima di ENABLE", flush=True)

        deadline = time.monotonic() + 12
        for worker in workers:
            code = worker.wait(
                timeout=max(0.1, deadline - time.monotonic()))
            if code != 0:
                raise RuntimeError(
                    f"Worker terminato con codice {code}")

        report, _, counters, _ = snapshot(N)
        if counters != (N, N - MAX, N - MAX, 0, 0, N - MAX):
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
