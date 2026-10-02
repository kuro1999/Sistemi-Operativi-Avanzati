#!/usr/bin/env python3
"""SA_RESTART dopo ammissione: segnale durante la read reale su pipe vuota.
Utente normale dopo sudo -v; monitor OFF e registri vuoti.
"""
import os
import platform
import select
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from statistics_regression import ctl
from blocking_syscall_regression import token, wait_counts
from restart_regression import SOURCE, NAME, check


def wait_pipe_read(worker, fd):
    deadline = time.monotonic() + 5
    channel = call = ""
    while time.monotonic() < deadline:
        check(worker.poll() is None,
              "Worker terminato prima del segnale")

        channel = Path(
            f"/proc/{worker.pid}/wchan").read_text().strip()
        call = Path(
            f"/proc/{worker.pid}/syscall").read_text().strip()
        parts = call.split()

        if (
            "pipe_read" in channel
            and len(parts) >= 2
            and parts[0] == "0"
            and int(parts[1], 0) == fd
        ):
            print(
                f"Read reale confermata: wchan={channel}, fd={fd}",
                flush=True)
            return

        time.sleep(0.01)

    raise RuntimeError(
        "INCONCLUSIVO: read reale non identificata; "
        f"wchan={channel!r}, syscall={call!r}")


def run(binary):
    worker = None
    registered = []
    read_fd, write_fd = os.pipe()
    parent, child = socket.socketpair()

    try:
        worker = subprocess.Popen(
            [str(binary), str(child.fileno()), str(read_fd)],
            pass_fds=(child.fileno(), read_fd),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        child.close()
        token(worker, b"R")

        ctl("program-add", NAME, root=True)
        registered.append(("program-remove", NAME))
        ctl("syscall-add", "0", root=True)
        registered.append(("syscall-remove", "0"))
        ctl("max-set", "1", root=True)
        ctl("enable", root=True)

        parent.sendall(b"G")
        wait_counts((1, 0, 0, 0, 0, 0))
        wait_pipe_read(worker, read_fd)
        print(
            "PASS: prima read ammessa senza attesa nel limiter",
            flush=True)

        # Il nuovo MAX non revoca la read gia' ammessa.
        ctl("max-set", "0", root=True)
        wait_pipe_read(worker, read_fd)

        worker.send_signal(signal.SIGUSR1)
        token(worker, b"S")
        wait_counts((1, 1, 0, 0, 1, 1))
        print(
            "PASS: read riavviata e bloccata da MAX=0; "
            "zero interruzioni delle attese del limiter",
            flush=True)

        os.write(write_fd, b"X")
        check(not select.select([worker.stdout], [], [], 0.10)[0],
              "Read completata con MAX=0")
        check(bool(select.select([read_fd], [], [], 0)[0]),
              "Byte consumato con MAX=0")
        wait_counts((1, 1, 0, 0, 1, 1))

        print(
            "PASS: il dato disponibile non permette "
            "di aggirare il limiter",
            flush=True)

        ctl("max-set", "1", root=True)
        token(worker, b"D")
        check(worker.wait(timeout=3) == 0,
              "Worker terminato con errore")
        check(not select.select([read_fd], [], [], 0)[0],
              "Byte non consumato")

        print(wait_counts((1, 1, 1, 0, 0, 1)), flush=True)
        print(
            "PASS: read riavviata completata dopo nuova ammissione",
            flush=True)

    finally:
        errors = []
        try:
            os.write(write_fd, b"X")
        except Exception as error:
            errors.append(str(error))

        try:
            ctl("disable", root=True)
        except Exception as error:
            errors.append(str(error))

        if worker is not None:
            try:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=3)
                worker.stdout.close()
            except Exception as error:
                errors.append(str(error))

        parent.close()
        child.close()
        os.close(read_fd)
        os.close(write_fd)

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

    with tempfile.TemporaryDirectory(prefix="st-admitted-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)

        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", str(source), "-o", str(binary)],
            check=True)
        run(binary)

    print(
        "PASS COMPLESSIVO: segnale durante la syscall gia' ammessa",
        flush=True)


if __name__ == "__main__":
    main()
