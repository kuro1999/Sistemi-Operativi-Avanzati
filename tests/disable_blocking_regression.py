#!/usr/bin/env python3
"""DISABLE contabilizza il waiter senza attendere la syscall originale.
Utente normale dopo sudo -v; modulo caricato, monitor OFF, registri vuoti.
"""
import os
import platform
import socket
import subprocess
import tempfile
from pathlib import Path

from statistics_regression import ctl, CTL
from blocking_syscall_regression import SOURCE, NAME, token, wait_counts
from admitted_signal_regression import wait_pipe_read
from restart_regression import check


def run(binary):
    worker = disabling = None
    registered = []
    read_fd, write_fd = os.pipe()
    parent, child = socket.socketpair()
    try:
        worker = subprocess.Popen(
            [str(binary), "read", str(child.fileno()), str(read_fd)],
            pass_fds=(child.fileno(), read_fd),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        child.close()
        token(worker, b"R")

        ctl("program-add", NAME, root=True)
        registered.append(("program-remove", NAME))
        ctl("syscall-add", "0", root=True)
        registered.append(("syscall-remove", "0"))
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        parent.sendall(b"G")
        wait_counts((1, 1, 0, 0, 1, 1))
        print("Read sospesa nel limiter; invio DISABLE con pipe ancora vuota",
              flush=True)

        disabling = subprocess.Popen(
            ["sudo", "-n", CTL, "disable"], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        output, _ = disabling.communicate(timeout=5)
        check(disabling.returncode == 0, "DISABLE fallito:\n" + output)
        check("disattivato" in ctl("status"), "Monitor non OFF")
        wait_pipe_read(worker, read_fd)
        print("PASS: DISABLE concluso mentre read attende ancora nella pipe",
              flush=True)

        frozen = ctl("stats")
        fields = dict(line.split(": ", 1) for line in frozen.splitlines()
                      if ": " in line)
        check(fields["Sessione statistiche"] == "inattiva", frozen)
        for key, expected in {
            "Invocazioni rilevanti": 1,
            "Invocazioni bloccate": 1,
            "Invocazioni bloccate completate": 1,
            "Attese interrotte da segnale": 0,
            "Thread attualmente bloccati": 0,
            "Picco thread bloccati": 1,
            "Effective UID del peak": os.geteuid(),
        }.items():
            check(int(fields[key]) == expected, frozen)
        check(fields["Programma del peak"] == NAME, frozen)
        check(int(fields["Peak delay"].split()[0]) > 0, frozen)
        print(frozen, flush=True)

        os.write(write_fd, b"X")
        token(worker, b"D")
        check(worker.wait(timeout=3) == 0, "Read fallita")
        check(ctl("stats") == frozen,
              "La conclusione della read ha modificato lo snapshot OFF")
        print("PASS: read completata dopo il dato; snapshot finale invariato",
              flush=True)
    finally:
        errors = []
        # Se DISABLE attendesse erroneamente la read, questo la libera.
        try:
            os.write(write_fd, b"X")
        except Exception as error:
            errors.append(str(error))
        if worker is not None:
            try:
                try:
                    worker.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=3)
                worker.stdout.close()
            except Exception as error:
                errors.append(str(error))
        if disabling is not None and disabling.poll() is None:
            try:
                disabling.communicate(timeout=10)
            except Exception as error:
                errors.append("DISABLE ancora pendente: " + str(error))
        for command in [("disable",), *reversed(registered), ("max-set", "0")]:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))
        parent.close()
        child.close()
        os.close(read_fd)
        os.close(write_fd)
        check(not errors, "Cleanup incompleto: " + "; ".join(errors))
        print("Cleanup: monitor OFF, registri vuoti, MAX=0", flush=True)


def main():
    check(os.geteuid() != 0, "Eseguire come utente normale dopo sudo -v")
    check(platform.machine() == "x86_64", "Richiesto x86-64")
    check(Path("/sys/module/syscall_throttle").exists(), "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")
    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")
    subprocess.run(["sudo", "-n", "true"], check=True)

    with tempfile.TemporaryDirectory(prefix="st-disable-read-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
                        "-O2", str(source), "-o", str(binary)], check=True)
        run(binary)
    print("PASS COMPLESSIVO: DISABLE non attende la syscall originale",
          flush=True)


if __name__ == "__main__":
    main()
