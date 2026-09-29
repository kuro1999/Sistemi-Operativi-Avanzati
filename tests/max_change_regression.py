#!/usr/bin/env python3
"""MAX aumentato/ridotto conserva le ammissioni della finestra.
Utente normale dopo sudo -v; modulo caricato, OFF e registri vuoti.
"""
import os
import subprocess
import time

from statistics_regression import ctl, snapshot, WORKER


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_case(initial, new, waiting):
    workers = []
    total = initial + waiting

    def spawn():
        worker = subprocess.Popen(
            [WORKER, "20"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        workers.append(worker)
        return worker

    ctl("max-set", str(initial), root=True)
    try:
        start = time.monotonic()
        ctl("enable", root=True)

        first = [spawn() for _ in range(initial)]
        for worker in first:
            check(worker.wait(timeout=3) == 0, "Worker iniziale fallito")

        text, _, counts, _ = snapshot(total)
        check(counts == (initial, 0, 0, 0, 0, 0), text)

        for _ in range(waiting):
            spawn()

        expected_initial = (total, waiting, 0, 0, waiting, waiting)
        while True:
            text, _, counts, _ = snapshot(total)
            check(time.monotonic() - start < 0.40,
                  "INCONCLUDENTE: preparazione troppo lenta; ripetere")
            if counts == expected_initial:
                break
            time.sleep(0.005)

        print(f"MAX {initial} -> {new}: {initial} chiamate ammesse, "
              f"{waiting} sospese", flush=True)
        ctl("max-set", str(new), root=True)
        check(time.monotonic() - start < 0.45,
              "INCONCLUDENTE: MAX_SET troppo tardivo; ripetere")

        released = min(waiting, max(0, new - initial))
        expected = (
            total, waiting, released, 0, waiting - released, waiting
        )

        # Attendiamo l'effetto dell'eventuale aumento, restando
        # ben prima della scadenza della prima finestra.
        while True:
            text, _, counts, _ = snapshot(total)
            check(time.monotonic() - start < 0.55,
                  "INCONCLUDENTE: osservazione tardiva; ripetere")
            check(counts[2] <= released,
                  "FAIL: MAX_SET ha restituito budget gia' consumato:\n"
                  + text)
            if counts == expected:
                break
            time.sleep(0.005)

        samples = 0
        while time.monotonic() - start < 0.70:
            text, _, counts, _ = snapshot(total)
            if time.monotonic() - start >= 0.70:
                break
            check(counts == expected,
                  "FAIL: ammissioni inattese nella prima finestra:\n" + text)
            samples += 1
            time.sleep(0.01)

        check(samples >= 3,
              "INCONCLUDENTE: campioni insufficienti; ripetere")
        print(f"PASS: {released} attese concluse, "
              f"{waiting - released} ancora sospese; "
              f"{samples} snapshot validi", flush=True)

        # Nessun altro MAX_SET: i rimanenti devono procedere
        # grazie al rinnovo periodico del budget.
        for worker in workers:
            check(worker.wait(timeout=4) == 0, "Worker fallito")
        text, _, counts, _ = snapshot(total)
        check(counts == (total, waiting, waiting, 0, 0, waiting), text)
        print("PASS: tutti i worker completati senza ulteriori modifiche",
              flush=True)
    finally:
        try:
            ctl("disable", root=True)
        finally:
            for worker in workers:
                if worker.poll() is None:
                    worker.terminate()
                try:
                    worker.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=3)


def main():
    check(os.geteuid() != 0, "Eseguire come utente normale dopo sudo -v")
    check("disattivato" in ctl("status"), "Richiesto monitor OFF")
    for command in ("uid-count", "program-count", "syscall-count"):
        check(int(ctl(command).strip().rsplit(":", 1)[1]) == 0,
              "Richiesti registri vuoti")

    registered = []
    try:
        ctl("program-add", "syscall_hook_smoke", root=True)
        registered.append(("program-remove", "syscall_hook_smoke"))
        ctl("syscall-add", "35", root=True)
        registered.append(("syscall-remove", "35"))

        run_case(initial=1, new=2, waiting=2)
        run_case(initial=3, new=1, waiting=1)
    finally:
        errors = []
        for command in [
            ("disable",), *reversed(registered), ("max-set", "0")
        ]:
            try:
                ctl(*command, root=True)
            except Exception as error:
                errors.append(str(error))
        check(not errors, "Cleanup incompleto: " + "; ".join(errors))
        print("Cleanup: monitor OFF, registri vuoti, MAX=0", flush=True)

    print("PASS COMPLESSIVO: aumento e riduzione conservano il consumo")


if __name__ == "__main__":
    main()
