#!/usr/bin/env python3
"""delete_module: bersaglio vuoto, pin, autorimozione e unload OFF.
Eseguire con sudo; richiede monitor scaricato. Al successo scarica entrambi.
"""
import errno
import os
import platform
import select
import subprocess
import tempfile
import time
from pathlib import Path
from statistics_regression import ROOT, ctl, snapshot

MONITOR = "syscall_throttle"
FIXTURE = "st_delete_fixture"
NAME = "st_delete_worker"
MODROOT = Path("/sys/module")

WORKER_SOURCE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_delete_module == 176, "Richiesto x86-64");

int main(int argc, char **argv)
{
    char go;
    if (argc != 2) return 90;
    if (write(1, "R", 1) != 1) return 91;
    if (read(0, &go, 1) != 1 || go != 'G') return 92;
    errno = 0;
    long result = syscall(SYS_delete_module, argv[1], O_NONBLOCK);
    int saved_errno = errno;
    printf("%ld %d\n", result, saved_errno);
    return 0;
}
'''

MODULE_SOURCE = r'''
#include <linux/init.h>
#include <linux/module.h>

static int __init fixture_init(void) { return 0; }
static void __exit fixture_exit(void) { }

module_init(fixture_init);
module_exit(fixture_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bersaglio vuoto per il test delete_module");
'''


def check(ok, message):
    if not ok:
        raise RuntimeError(message)


def refcount():
    return int((MODROOT / MONITOR / "refcnt").read_text())


def run_case(binary, target, expected):
    worker = None
    ctl("max-set", "0")
    ctl("enable")
    base = refcount()
    try:
        worker = subprocess.Popen(
            [str(binary), target],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        check(select.select([worker.stdout], [], [], 5)[0],
              "Timeout startup")
        check(os.read(worker.stdout.fileno(), 1) == b"R",
              "Startup fallito")
        worker.stdin.write(b"G")
        worker.stdin.flush()

        deadline = time.monotonic() + 5
        while True:
            report, _, counts, _ = snapshot(1)
            if counts == (1, 1, 0, 0, 1, 1):
                break
            check(worker.poll() is None,
                  "Worker terminato prima del blocco")
            check(time.monotonic() < deadline,
                  "Timeout waiter:\n" + report)
            time.sleep(0.01)

        time.sleep(0.10)
        report, _, counts, _ = snapshot(1)
        check(counts == (1, 1, 0, 0, 1, 1), report)
        check((MODROOT / target).exists(),
              "Bersaglio rimosso con MAX=0")
        check(refcount() >= base + 1,
              "Riferimento al monitor non osservato")
        print(f"PASS: {target}; MAX=0 blocca la syscall, pin presente",
              flush=True)

        ctl("max-set", "1")
        output, error = worker.communicate(timeout=8)
        check(worker.returncode == 0, repr(error))
        result = tuple(map(int, output.split()))
        check(result == expected,
              f"Risultato inatteso: {result}; atteso {expected}")
        check((MODROOT / target).exists() == (target == MONITOR),
              "Stato del bersaglio inatteso")

        report, fields, counts, elapsed = snapshot(1)
        check(counts == (1, 1, 1, 0, 0, 1), report)
        check(int(fields["Effective UID del peak"]) == 0, report)
        check(fields["Programma del peak"] == NAME, report)
        peak = int(fields["Peak delay"].split()[0])
        check(0 <= peak <= round(elapsed * 1e9) + 2000, report)

        deadline = time.monotonic() + 3
        while refcount() != base:
            check(time.monotonic() < deadline,
                  "Riferimento al monitor non rilasciato")
            time.sleep(0.01)
        print(f"PASS: {target}; risultato={result}, "
              "zero waiter, pin rilasciato", flush=True)
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
    check(not (MODROOT / FIXTURE).exists(),
          "Il modulo di prova esiste gia'")
    build = Path("/lib/modules") / platform.release() / "build"
    check(build.is_dir(), "Header kernel non disponibili")

    with tempfile.TemporaryDirectory(prefix="st-delete-") as tmp:
        folder = Path(tmp)
        source = folder / "worker.c"
        binary = folder / NAME
        source.write_text(WORKER_SOURCE)
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11", "-O2",
             str(source), "-o", str(binary)], check=True)
        (folder / (FIXTURE + ".c")).write_text(MODULE_SOURCE)
        (folder / "Makefile").write_text("obj-m += " + FIXTURE + ".o\n")
        subprocess.run(
            ["make", "-C", str(build), "M=" + str(folder), "modules"],
            check=True)

        monitor_loaded = fixture_loaded = False
        registered = []
        unload_attempted = False
        try:
            subprocess.run(
                ["insmod", str(ROOT / "module/syscall_throttle.ko")],
                check=True, timeout=30)
            monitor_loaded = True
            subprocess.run(
                ["insmod", str(folder / (FIXTURE + ".ko"))], check=True)
            fixture_loaded = True

            ctl("program-add", NAME)
            registered.append(("program-remove", NAME))
            ctl("syscall-add", "176")
            registered.append(("syscall-remove", "176"))

            run_case(binary, FIXTURE, (0, 0))
            run_case(binary, MONITOR, (-1, errno.EWOULDBLOCK))

            # 176 resta registrata: verifichiamo proprio il bypass OFF.
            # In caso di errore qui non ripetiamo automaticamente rmmod.
            unload_attempted = True
            subprocess.run(["rmmod", MONITOR], check=True, timeout=15)
            monitor_loaded = False
            print("PASS: rmmod da OFF con syscall 176 ancora registrata",
                  flush=True)
        finally:
            if monitor_loaded and not unload_attempted:
                ctl("disable")
                for command in reversed(registered):
                    ctl(*command)
                ctl("max-set", "0")
            if fixture_loaded and (MODROOT / FIXTURE).exists():
                subprocess.run(["rmmod", FIXTURE], check=True, timeout=15)
            if monitor_loaded and not unload_attempted:
                subprocess.run(["rmmod", MONITOR], check=True, timeout=15)

        check(not (MODROOT / MONITOR).exists(), "Monitor ancora caricato")
        check(not (MODROOT / FIXTURE).exists(),
              "Modulo di prova ancora caricato")

    print("PASS COMPLESSIVO: rimozione bersaglio, autorimozione protetta, "
          "unload OFF; entrambi i moduli scaricati", flush=True)


if __name__ == "__main__":
    main()
