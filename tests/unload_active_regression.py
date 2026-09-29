#!/usr/bin/env python3
"""Scaricamento con read gia' ammessa; lascia il modulo scaricato.
Utente normale dopo sudo -v; modulo caricato, OFF e registri vuoti.
"""
import os
import platform
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from statistics_regression import ctl
from blocking_syscall_regression import SOURCE, NAME, token, wait_counts
from admitted_signal_regression import wait_pipe_read
from restart_regression import check

MODULE = Path("/sys/module/syscall_throttle")
DEVICE = Path("/dev/syscall_throttle")


def run(binary):
    worker = unloading = None
    registered = []
    read_fd, write_fd = os.pipe()
    parent, child = socket.socketpair()

    try:
        worker = subprocess.Popen(
            [str(binary), "read", str(child.fileno()), str(read_fd)],
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

        unloading = subprocess.Popen(
            ["sudo", "-n", "rmmod", "syscall_throttle"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True)

        deadline = time.monotonic() + 10
        while True:
            if unloading.poll() is not None:
                output, _ = unloading.communicate()
                raise RuntimeError(
                    "rmmod terminato prima del rilascio della read; "
                    f"codice={unloading.returncode}\n{output}")

            check(MODULE.exists(),
                  "Modulo rimosso mentre read e' ancora attiva")
            state = (MODULE / "initstate").read_text().strip()
            if state == "going" and not DEVICE.exists():
                break

            check(time.monotonic() < deadline,
                  "Scaricamento non avviato")
            time.sleep(0.02)

        check(not select.select([worker.stdout], [], [], 0.30)[0],
              "Read terminata prima dell'invio del dato")
        check(unloading.poll() is None and MODULE.exists(),
              "Scaricamento completato con read ancora attiva")
        wait_pipe_read(worker, read_fd)

        print(
            "PASS: modulo in stato going, device rimosso, "
            "rmmod ancora pendente con read attiva",
            flush=True)

        start = time.monotonic()
        os.write(write_fd, b"X")
        token(worker, b"D")
        check(worker.wait(timeout=5) == 0,
              "Read terminata con errore")

        output, _ = unloading.communicate(timeout=20)
        check(unloading.returncode == 0,
              "rmmod fallito:\n" + output)
        check(not MODULE.exists() and not DEVICE.exists(),
              "Modulo o device ancora presenti")

        print(
            "PASS: read e rmmod completati dopo l'invio del dato "
            f"in {time.monotonic() - start:.3f} s",
            flush=True)

    finally:
        errors = []

        # Prima consentiamo alla syscall originale di uscire.
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

        if unloading is not None and unloading.poll() is None:
            try:
                unloading.communicate(timeout=20)
            except Exception as error:
                errors.append(
                    "Scaricamento ancora pendente: " + str(error))

        if MODULE.exists():
            if (MODULE / "initstate").read_text().strip() == "going":
                errors.append("Modulo ancora in stato going")
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

        parent.close()
        child.close()
        os.close(read_fd)
        os.close(write_fd)
        check(not errors,
              "Cleanup incompleto: " + "; ".join(errors))


def main():
    check(os.geteuid() != 0,
          "Eseguire come utente normale dopo sudo -v")
    check(platform.machine() == "x86_64", "Richiesto x86-64")
    check(MODULE.exists(), "Modulo non caricato")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)

    with tempfile.TemporaryDirectory(prefix="st-unload-active-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)

        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", str(source), "-o", str(binary)],
            check=True)
        run(binary)

    print(
        "PASS COMPLESSIVO: scaricamento con syscall attiva; "
        "modulo scaricato",
        flush=True)


if __name__ == "__main__":
    main()
