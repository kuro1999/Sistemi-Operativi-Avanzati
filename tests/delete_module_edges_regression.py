#!/usr/bin/env python3
"""delete_module: force, identita' esclusa, SIGUSR1 e SIGKILL.
Eseguire con sudo e monitor scaricato. Usa un bersaglio inesistente.
"""
import errno
import os
import platform
import select
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path
from statistics_regression import ROOT, ctl, snapshot

NAME = "st_delete_edges"
MONITOR = "syscall_throttle"
MODROOT = Path("/sys/module")

SOURCE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_delete_module == 176, "Richiesto x86-64");
static volatile sig_atomic_t received;
static void handler(int sig) { (void)sig; received = 1; }

int main(int argc, char **argv)
{
    char go;
    struct sigaction sa = {0};
    if (argc != 3) return 90;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) return 91;
    if (write(1, "R", 1) != 1) return 92;
    if (read(0, &go, 1) != 1 || go != 'G') return 93;
    unsigned int flags = (unsigned int)strtoul(argv[2], NULL, 10);
    errno = 0;
    long result = syscall(SYS_delete_module, argv[1], flags);
    int saved_errno = errno;
    printf("%ld %d %d\n", result, saved_errno, (int)received);
    return 0;
}
'''


def check(ok, message):
    if not ok:
        raise RuntimeError(message)


def refcount():
    return int((MODROOT / MONITOR / "refcnt").read_text())


def run_case(binary, target, mode):
    worker = None
    ctl("max-set", "0")
    ctl("enable")
    base = refcount()
    flags = os.O_NONBLOCK
    if mode in ("force", "unmatched"):
        flags |= os.O_TRUNC

    try:
        worker = subprocess.Popen(
            [str(binary), target, str(flags)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        check(select.select([worker.stdout], [], [], 5)[0],
              "Timeout startup")
        check(os.read(worker.stdout.fileno(), 1) == b"R",
              "Startup fallito")
        worker.stdin.write(b"G")
        worker.stdin.flush()

        if mode != "unmatched":
            deadline = time.monotonic() + 5
            while True:
                text, _, counts, _ = snapshot(1)
                if counts == (1, 1, 0, 0, 1, 1):
                    break
                check(worker.poll() is None,
                      "Worker terminato prima del blocco")
                check(time.monotonic() < deadline,
                      "Timeout waiter:\n" + text)
                time.sleep(0.01)

            check(refcount() >= base + 1,
                  "Pin non osservato durante il blocco")

            if mode == "force":
                ctl("max-set", "1")
            elif mode == "usr1":
                worker.send_signal(signal.SIGUSR1)
            else:
                worker.kill()

        output, error = worker.communicate(timeout=5)
        if mode == "kill":
            check(worker.returncode == -signal.SIGKILL,
                  "Worker non terminato da SIGKILL")
        else:
            check(worker.returncode == 0, repr(error))
            expected = (
                (-1, errno.EINTR, 1) if mode == "usr1"
                else (-1, errno.EPERM, 0)
            )
            check(tuple(map(int, output.split())) == expected,
                  "Risultato inatteso: " + repr(output))

        text, fields, counts, _ = snapshot(1)
        expected_counts = {
            "force": (1, 1, 1, 0, 0, 1),
            "unmatched": (0, 0, 0, 0, 0, 0),
            "usr1": (1, 1, 0, 1, 0, 1),
            "kill": (1, 1, 0, 1, 0, 1),
        }[mode]
        check(counts == expected_counts, text)
        if mode != "force":
            check(fields["Peak delay"] == "non disponibile", text)

        check(not (MODROOT / target).exists(), "Bersaglio inatteso")
        deadline = time.monotonic() + 3
        while refcount() != base:
            check(time.monotonic() < deadline, "Pin non rilasciato")
            time.sleep(0.01)

        print(f"PASS: {mode}; risultato e statistiche corretti, "
              "pin rilasciato", flush=True)
    finally:
        if worker is not None:
            if worker.poll() is None:
                worker.kill()
            worker.communicate(timeout=5)
        ctl("disable")


def main():
    check(os.geteuid() == 0, "Eseguire con sudo")
    check(platform.machine() == "x86_64", "Richiesto x86-64")
    check(not (MODROOT / MONITOR).exists(),
          "Richiesto monitor scaricato")
    target = "st_absent_" + str(os.getpid())
    check(not (MODROOT / target).exists(),
          "Il bersaglio deve essere inesistente")

    with tempfile.TemporaryDirectory(prefix="st-delete-edges-") as tmp:
        folder = Path(tmp)
        source = folder / "worker.c"
        binary = folder / NAME
        other = folder / "st_delete_other"
        source.write_text(SOURCE)
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11", "-O2",
             str(source), "-o", str(binary)], check=True)
        shutil.copy2(binary, other)

        subprocess.run(
            ["insmod", str(ROOT / "module/syscall_throttle.ko")],
            check=True, timeout=30)
        registered = []
        unload_attempted = False
        try:
            ctl("program-add", NAME)
            registered.append(("program-remove", NAME))
            ctl("syscall-add", "176")
            registered.append(("syscall-remove", "176"))

            for mode in ("force", "unmatched", "usr1", "kill"):
                run_case(
                    other if mode == "unmatched" else binary,
                    target, mode)

            unload_attempted = True
            subprocess.run(["rmmod", MONITOR], check=True, timeout=15)
        finally:
            if not unload_attempted:
                ctl("disable")
                for command in reversed(registered):
                    ctl(*command)
                ctl("max-set", "0")
                subprocess.run(["rmmod", MONITOR], check=True, timeout=15)

        check(not (MODROOT / MONITOR).exists(),
              "Monitor ancora caricato")

    print("PASS COMPLESSIVO: force e segnali; modulo scaricato",
          flush=True)


if __name__ == "__main__":
    main()
