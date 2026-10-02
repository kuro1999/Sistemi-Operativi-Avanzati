#!/usr/bin/env python3
"""exit/exit_group: throttling, terminazione, diagnostica e unload.
Utente normale dopo sudo -v; modulo caricato, OFF, registri vuoti.
Al successo lascia il modulo scaricato.
"""
import os
import platform
import re
import select
import subprocess
import tempfile
import time
from pathlib import Path
from statistics_regression import ctl, snapshot

NAME = "st_exit_worker"
SOURCE = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_exit == 60 && SYS_exit_group == 231, "Richiesto x86-64");
static pthread_barrier_t barrier;

static void *idle(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&barrier);
    for (;;) pause();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[3];
    char go;
    if (argc != 3) return 90;
    long nr = strtol(argv[1], NULL, 10);
    int n = atoi(argv[2]);
    if ((nr != SYS_exit && nr != SYS_exit_group) || (n != 0 && n != 3))
        return 91;
    if (n) {
        if (pthread_barrier_init(&barrier, NULL, n + 1) != 0) return 92;
        for (int i = 0; i < n; i++)
            if (pthread_create(&threads[i], NULL, idle, NULL) != 0) return 93;
        pthread_barrier_wait(&barrier);
    }
    if (write(STDOUT_FILENO, "R", 1) != 1) return 94;
    if (read(STDIN_FILENO, &go, 1) != 1 || go != 'G') return 95;
    syscall(nr, 37);
    return 99;
}
'''


def check(ok, message):
    if not ok:
        raise RuntimeError(message)


def run_case(binary, nr, extra_threads):
    worker = None
    registered = False
    try:
        worker = subprocess.Popen(
            [str(binary), str(nr), str(extra_threads)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        check(select.select([worker.stdout], [], [], 5)[0], "Timeout startup")
        check(os.read(worker.stdout.fileno(), 1) == b"R", "Startup fallito")

        taskdir = Path(f"/proc/{worker.pid}/task")
        check(len(list(taskdir.iterdir())) == extra_threads + 1,
              "Numero thread iniziale inatteso")

        ctl("syscall-add", str(nr), root=True)
        registered = True
        ctl("max-set", "0", root=True)
        ctl("enable", root=True)
        worker.stdin.write(b"G")
        worker.stdin.flush()

        deadline = time.monotonic() + 5
        while True:
            text, _, counts, _ = snapshot(1)
            if counts == (1, 1, 0, 0, 1, 1):
                break
            check(worker.poll() is None,
                  "Worker terminato prima dell'ammissione")
            check(time.monotonic() < deadline, "Timeout waiter:\n" + text)
            time.sleep(0.01)

        time.sleep(0.10)
        text, _, counts, _ = snapshot(1)
        check(counts == (1, 1, 0, 0, 1, 1), text)
        check(worker.poll() is None, "Worker gia' terminato")
        check(len(list(taskdir.iterdir())) == extra_threads + 1,
              "Thread terminati mentre la syscall era nel limiter")
        print(f"PASS: syscall {nr}, {extra_threads + 1} thread; "
              "terminazione sospesa con MAX=0", flush=True)

        ctl("max-set", "1", root=True)
        check(worker.wait(timeout=5) == 37, "Codice di uscita diverso da 37")
        check(not taskdir.exists(),
              "Task ancora presenti dopo la terminazione")

        text, fields, counts, elapsed = snapshot(1)
        check(counts == (1, 1, 1, 0, 0, 1), text)
        check(int(fields["System call del peak"]) == nr, text)
        check(int(fields["Effective UID del peak"]) == os.geteuid(), text)
        check(fields["Programma del peak"] == NAME, text)
        peak = int(fields["Peak delay"].split()[0])
        check(0 <= peak <= round(elapsed * 1e9) + 2000, text)
        print(f"PASS: syscall {nr}; uscita 37, zero waiter, peak corretto",
              flush=True)
    finally:
        try:
            ctl("disable", root=True)
        finally:
            if worker is not None:
                if worker.poll() is None:
                    worker.kill()
                worker.wait(timeout=5)
                worker.stdin.close()
                worker.stdout.close()
            if registered:
                ctl("syscall-remove", str(nr), root=True)


def main():
    check(os.geteuid() != 0, "Eseguire come utente normale dopo sudo -v")
    check(platform.machine() == "x86_64", "Richiesto x86-64")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")
    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    subprocess.run(["sudo", "-n", "dmesg"],
                   stdout=subprocess.DEVNULL, check=True)

    with tempfile.TemporaryDirectory(prefix="st-exit-") as tmp:
        source = Path(tmp) / "worker.c"
        binary = Path(tmp) / NAME
        source.write_text(SOURCE)
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11",
             "-O2", "-pthread", str(source), "-o", str(binary)],
            check=True)

        ctl("program-add", NAME, root=True)
        try:
            for nr, threads in ((60, 0), (231, 0), (231, 3)):
                run_case(binary, nr, threads)
        finally:
            ctl("program-remove", NAME, root=True)
            ctl("max-set", "0", root=True)

    print("Tre casi superati; verifico rmmod", flush=True)
    subprocess.run(
        ["sudo", "-n", "rmmod", "syscall_throttle"],
        check=True, timeout=15)
    check(not Path("/sys/module/syscall_throttle").exists(),
          "Modulo ancora presente")

    log = subprocess.check_output(["sudo", "-n", "dmesg"], text=True)
    matches = re.findall(
        r"diagnostica nonreturning: avviate=(\d+), "
        r"completate=(\d+), ritorni_inattesi=(\d+)", log)
    check(matches, "Diagnostica nonreturning non trovata")
    started, completed, unexpected = map(int, matches[-1])
    print(f"Diagnostica: avviate={started}, completate={completed}, "
          f"ritorni_inattesi={unexpected}", flush=True)
    check(started >= 3 and completed == started and unexpected == 0,
          "Contabilizzazione nonreturning incoerente")
    print("PASS COMPLESSIVO: exit, exit_group e scaricamento; modulo scaricato",
          flush=True)


if __name__ == "__main__":
    main()
