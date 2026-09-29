#!/usr/bin/env python3
"""SA_RESTART durante il throttling: read con dato disponibile, MAX=0.
Utente normale dopo sudo -v; modulo caricato, monitor OFF e registri vuoti.
"""
import os
import platform
import select
import signal
import socket
import subprocess
import tempfile
from pathlib import Path

from statistics_regression import ctl, snapshot
from blocking_syscall_regression import token, wait_counts

NAME = "st_restart_worker"
SOURCE = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_read == 0, "ABI inattesa");
static volatile sig_atomic_t received;

static void handler(int sig)
{
    int saved = errno;
    (void)sig;
    received = 1;
    if (write(1, "S", 1) != 1) _exit(9);
    errno = saved;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int gate = atoi(argv[1]), data = atoi(argv[2]);
    struct sigaction action = {0};
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGUSR1, &action, NULL) == -1) return 3;

    char byte;
    if (write(1, "R", 1) != 1) return 4;
    if (recv(gate, &byte, 1, MSG_WAITALL) != 1) return 5;

    /* Una sola chiamata C: nessun ciclo di retry in user space. */
    long result = syscall(SYS_read, data, &byte, 1);
    if (result != 1 || byte != 'X' || !received) return 6;
    return write(1, "D", 1) == 1 ? 0 : 7;
}
"""


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(binary):
    worker = None
    registered = []
    read_fd, write_fd = os.pipe()
    parent, child = socket.socketpair()
    try:
        os.write(write_fd, b"X")

        # Completiamo il caricamento del worker a monitor OFF.
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
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)

        parent.sendall(b"G")
        wait_counts((1, 1, 0, 0, 1, 1))
        print(
            "PASS: read sospesa dal limiter "
            "nonostante il dato disponibile",
            flush=True)

        worker.send_signal(signal.SIGUSR1)
        token(worker, b"S")
        wait_counts((2, 2, 0, 1, 1, 1))

        check(worker.poll() is None,
              "Worker terminato con MAX=0")
        check(not select.select([worker.stdout], [], [], 0)[0],
              "read ritornata prima dell'ammissione")
        check(bool(select.select([read_fd], [], [], 0)[0]),
              "Dato consumato mentre MAX=0")

        print(
            "PASS: handler eseguito; read riavviata "
            "e nuovamente in attesa; dato ancora nella pipe",
            flush=True)

        ctl("max-set", "1", root=True)
        token(worker, b"D")
        check(worker.wait(timeout=3) == 0,
              "Worker terminato con errore")

        report = wait_counts((2, 2, 1, 1, 0, 1))
        check(not select.select([read_fd], [], [], 0)[0],
              "Il byte non e' stato consumato")

        _, fields, _, _ = snapshot(2)
        check(int(fields["System call del peak"]) == 0, report)
        check(int(fields["Effective UID del peak"]) == os.geteuid(),
              report)
        check(fields["Programma del peak"] == NAME, report)

        print(
            "PASS: con MAX=1 la read completa e consuma il byte",
            flush=True)
        print(report, flush=True)

    finally:
        errors = []
        try:
            os.write(write_fd, b"X")
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

    with tempfile.TemporaryDirectory(prefix="st-restart-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)

        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", str(source), "-o", str(binary)],
            check=True)
        run(binary)

    print(
        "PASS COMPLESSIVO: SA_RESTART durante il throttling",
        flush=True)


if __name__ == "__main__":
    main()
