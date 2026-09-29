#!/usr/bin/env python3
"""Eseguire come utente normale dopo sudo -v, monitor OFF e registri vuoti.
Una read sospesa nella pipe non deve trattenere il budget delle nuove finestre.
"""
import os
import platform
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from statistics_regression import ctl, snapshot

NAME = "st_blocking_worker"
SOURCE = r"""
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_read == 0 && SYS_getpid == 39, "ABI inattesa");

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    int gate = atoi(argv[2]), data = atoi(argv[3]);
    char byte;

    if (write(1, "R", 1) != 1) return 3;

    /* recv non e' tra le syscall registrate nel test. */
    if (recv(gate, &byte, 1, MSG_WAITALL) != 1) return 4;

    if (strcmp(argv[1], "read") == 0) {
        if (syscall(SYS_read, data, &byte, 1) != 1 || byte != 'X')
            return 5;
    } else if (strcmp(argv[1], "pid") == 0) {
        if (syscall(SYS_getpid) <= 0) return 6;
    } else return 7;

    return write(1, "D", 1) == 1 ? 0 : 8;
}
"""


def token(worker, expected, timeout=5):
    ready, _, _ = select.select([worker.stdout], [], [], timeout)
    if not ready or os.read(worker.stdout.fileno(), 1) != expected:
        raise RuntimeError(
            f"Worker {worker.pid}: token {expected!r} non ricevuto")


def wait_counts(expected, timeout=5):
    deadline = time.monotonic() + timeout
    while True:
        report, _, counts, _ = snapshot(2)
        if counts == expected:
            return report
        if time.monotonic() >= deadline:
            raise RuntimeError(
                "Contatori attesi non raggiunti:\n" + report)
        time.sleep(0.01)


def run(binary):
    workers, gates, registered = [], [], []
    read_fd, write_fd = os.pipe()
    try:
        # Startup a monitor OFF: le letture del loader non contano.
        for mode in ("read", "pid"):
            parent, child = socket.socketpair()
            gates.append(parent)
            try:
                worker = subprocess.Popen(
                    [str(binary), mode, str(child.fileno()), str(read_fd)],
                    pass_fds=(child.fileno(), read_fd),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL)
                workers.append(worker)
            finally:
                child.close()
            token(worker, b"R")

        reader, probe = workers

        ctl("program-add", NAME, root=True)
        registered.append(("program-remove", NAME))
        for number in ("0", "39"):
            ctl("syscall-add", number, root=True)
            registered.append(("syscall-remove", number))

        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        gates[0].sendall(b"G")
        wait_counts((1, 1, 0, 0, 1, 1))
        print(
            "PASS: read inizialmente sospesa dal throttling",
            flush=True)

        t0 = time.monotonic()
        ctl("max-set", "1", root=True)
        wait_counts((1, 1, 1, 0, 0, 1))

        if select.select([reader.stdout], [], [], 0)[0]:
            raise RuntimeError(
                "La read e' terminata senza dati nella pipe")

        print(
            "PASS: read ammessa; pipe ancora vuota, "
            "zero waiter del limiter",
            flush=True)

        gates[1].sendall(b"G")
        wait_counts((2, 2, 1, 0, 1, 1))

        if time.monotonic() - t0 >= 0.70:
            raise RuntimeError(
                "INCONCLUSIVO: VM troppo lenta per osservare "
                "il consumo del budget nella prima finestra")

        print(
            "PASS: getpid in attesa; "
            "la read ha consumato l'unico posto",
            flush=True)

        # Nessun MAX_SET o DISABLE: deve intervenire il rinnovo naturale.
        token(probe, b"D")
        if probe.wait(timeout=3) != 0:
            raise RuntimeError("getpid fallita")

        report = wait_counts((2, 2, 2, 0, 0, 1))
        if select.select([reader.stdout], [], [], 0)[0]:
            raise RuntimeError(
                "La read non e' piu' in attesa dei dati")

        print(
            "PASS: getpid completata mentre read "
            "e' ancora sospesa nella pipe",
            flush=True)
        print(report, flush=True)

        os.write(write_fd, b"X")
        token(reader, b"D")
        if reader.wait(timeout=3) != 0:
            raise RuntimeError("read fallita")

        wait_counts((2, 2, 2, 0, 0, 1))
        print(
            "PASS: read completata dopo l'invio del dato",
            flush=True)

    finally:
        errors = []

        # Liberiamo prima la read reale, anche in caso di errore.
        try:
            os.write(write_fd, b"X")
        except OSError as error:
            errors.append(str(error))

        try:
            ctl("disable", root=True)
        except Exception as error:
            errors.append(str(error))

        for worker in workers:
            try:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=3)
                worker.stdout.close()
            except Exception as error:
                errors.append(str(error))

        for gate in gates:
            gate.close()

        os.close(read_fd)
        os.close(write_fd)

        for command in [*reversed(registered), ("max-set", "0")]:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))

        if errors:
            raise RuntimeError(
                "Cleanup incompleto: " + "; ".join(errors))

        print(
            "Cleanup: monitor OFF, registri vuoti, MAX=0",
            flush=True)


def main():
    if os.geteuid() == 0:
        raise RuntimeError(
            "Eseguire come utente normale dopo sudo -v")

    if platform.machine() != "x86_64":
        raise RuntimeError("Richiesto x86-64")

    if not Path("/sys/module/syscall_throttle").exists():
        raise RuntimeError("Modulo non caricato")

    if "disattivato" not in ctl("status"):
        raise RuntimeError("Richiesto monitor OFF")

    for command in ("uid-count", "program-count", "syscall-count"):
        if int(ctl(command).strip().rsplit(":", 1)[1]) != 0:
            raise RuntimeError("Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)

    with tempfile.TemporaryDirectory(prefix="st-blocking-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)

        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", str(source), "-o", str(binary)],
            check=True)

        run(binary)

    print(
        "PASS COMPLESSIVO: syscall bloccante e rinnovo del budget",
        flush=True)


if __name__ == "__main__":
    main()
