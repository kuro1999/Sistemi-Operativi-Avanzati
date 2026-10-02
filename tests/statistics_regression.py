#!/usr/bin/env python3
"""Regressione su modulo caricato, monitor OFF e registri vuoti.
Eseguire come utente normale dopo sudo -v. Lascia OFF, registri vuoti,
MAX=0 e statistiche dell'ultimo caso. Non carica/scarica il modulo.
"""
import os
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CTL = str(ROOT / "user/stctl")
WORKER = str(ROOT / "tests/syscall_hook_smoke")


def ctl(*args, root=False):
    return subprocess.check_output(
        (["sudo", "-n"] if root else []) + [CTL, *args],
        text=True, stderr=subprocess.STDOUT, timeout=10)


def snapshot(n):
    text = ctl("stats")
    f = dict(line.split(": ", 1) for line in text.splitlines() if ": " in line)
    r = int(f["Invocazioni rilevanti"])
    b = int(f["Invocazioni bloccate"])
    c = int(f["Invocazioni bloccate completate"])
    i = int(f["Attese interrotte da segnale"])
    w = int(f["Thread attualmente bloccati"])
    p = int(f["Picco thread bloccati"])
    avg = float(f["Media temporale thread bloccati"])
    elapsed = float(f["Durata osservazione"].split()[0])
    assert f["Sessione statistiche"] == "attiva", text
    assert 0 <= w <= p <= n, text
    assert c + i + w == b <= r <= n, text
    assert 0 <= avg <= p + 0.000002, text
    return text, f, (r, b, c, i, w, p), elapsed


def run_case(n):
    workers = []
    stop = threading.Event()
    # Pari: sessione stabile. Dispari: MAX_SET in corso.
    phase = 0

    def reader():
        previous, previous_phase, count = -1.0, -1, 0
        while not stop.is_set():
            before = phase
            _, _, _, elapsed = snapshot(n)
            after = phase
            # Gli invarianti di snapshot valgono anche durante il cambio.
            # La monotonia temporale vale nella stessa sessione.
            if before == after and after % 2 == 0:
                if previous_phase == after:
                    assert elapsed >= previous, "Tempo di osservazione retrogrado"
                previous, previous_phase = elapsed, after
            else:
                previous_phase = -1
            count += 1
            stop.wait(0.02)
        return count

    ctl("max-set", "0", root=True)
    ctl("enable", root=True)
    try:
        for _ in range(n):
            workers.append(subprocess.Popen(
                [WORKER, "20"], stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True))

        deadline = time.monotonic() + 5
        while True:
            text, _, counters, _ = snapshot(n)
            if counters[4] == n:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Timeout in attesa dei waiter:\n" + text)
            time.sleep(0.02)

        print(f"\nCaso {n}: tutti i waiter osservati", flush=True)

        with ThreadPoolExecutor(max_workers=2) as pool:
            readers = [pool.submit(reader) for _ in range(2)]
            try:
                time.sleep(0.3)
                phase += 1
                ctl("max-set", str(min(n, 2)), root=True)
                phase += 1

                deadline = time.monotonic() + 12
                for worker in workers:
                    _, error = worker.communicate(
                        timeout=max(0.1, deadline - time.monotonic()))
                    assert worker.returncode == 0, error
            finally:
                stop.set()

            reads = sum(future.result() for future in readers)

        text, f, counters, elapsed = snapshot(n)
        assert counters == (n, n, n, 0, 0, n), text
        assert int(f["System call del peak"]) == 35, text
        assert int(f["Effective UID del peak"]) == os.geteuid(), text
        assert f["Programma del peak"] == "syscall_hook_smoke", text
        peak = int(f["Peak delay"].split()[0])
        assert 0 <= peak <= round(elapsed * 1_000_000_000) + 2000, text
        # Con otto worker e MAX=2 servono piu' rinnovi del budget.
        if n == 8:
            assert peak >= 1_500_000_000, text
        assert reads > 0, "Nessuno snapshot concorrente"

        print(text)
        print(f"PASS: {n} processi, {reads} snapshot concorrenti", flush=True)
    finally:
        stop.set()
        try:
            ctl("disable", root=True)
        finally:
            for worker in workers:
                if worker.poll() is None:
                    worker.terminate()
                try:
                    worker.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.communicate(timeout=3)


def main():
    if not Path("/sys/module/syscall_throttle").exists():
        raise RuntimeError("Caricare prima il modulo")
    if "disattivato" not in ctl("status"):
        raise RuntimeError("Il monitor deve essere disattivato")

    for command in ("uid-count", "program-count", "syscall-count"):
        if int(ctl(command).strip().rsplit(":", 1)[1]) != 0:
            raise RuntimeError("Il test richiede registri vuoti")

    subprocess.run(["sudo", "-n", "true"], check=True)
    print("CPU disponibili:", len(os.sched_getaffinity(0)), flush=True)
    program_added = syscall_added = False

    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        program_added = True
        ctl("syscall-add", "35", root=True)
        syscall_added = True
        run_case(1)
        run_case(8)
    finally:
        errors = []
        commands = [("disable",), ("max-set", "0")]
        if syscall_added:
            commands.append(("syscall-remove", "35"))
        if program_added:
            commands.append(("program-remove", "syscall_hook_smoke"))

        for command in commands:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(f"{command}: {error}")
        if errors:
            raise RuntimeError("Cleanup incompleto: " + "; ".join(errors))

    print("PASS COMPLESSIVO: 2/2; monitor OFF, registri vuoti, MAX=0")


if __name__ == "__main__":
    main()
