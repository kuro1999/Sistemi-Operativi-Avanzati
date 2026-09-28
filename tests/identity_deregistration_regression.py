#!/usr/bin/env python3
"""Eseguire con sudo da un utente non root, su monitor OFF e registri vuoti.
Il controller resta root; solo il worker assume UID/GID dell'utente.
"""
import os
import pwd
import subprocess
import time

from statistics_regression import ctl, snapshot, WORKER


def run_case(label, identities, order, uid, gid):
    registered = []
    worker = None

    def drop_privileges():
        os.setgroups([])
        os.setgid(gid)
        os.setuid(uid)

    def remove(item):
        kind, value = item
        ctl(kind + "-remove", value)
        registered.remove(item)

    try:
        for item in [("syscall", "35"), *identities]:
            ctl(item[0] + "-add", item[1])
            registered.append(item)
        ctl("max-set", "0")
        ctl("enable")
        worker = subprocess.Popen(
            [WORKER, "20"], preexec_fn=drop_privileges,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        deadline = time.monotonic() + 5
        while True:
            text, _, counters, _ = snapshot(1)
            if counters[4] == 1:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Waiter non osservato:\n" + text)
            time.sleep(0.02)

        remove(order[0])
        if len(order) == 2:
            deadline = time.monotonic() + 0.3
            while time.monotonic() < deadline:
                text, _, counters, _ = snapshot(1)
                assert counters == (1, 1, 0, 0, 1, 1), text
                assert worker.poll() is None, "Worker liberato troppo presto"
                time.sleep(0.02)
            print(label + ": blocco mantenuto con un criterio residuo",
                  flush=True)
            remove(order[1])

        _, error = worker.communicate(timeout=1.5)
        assert worker.returncode == 0, error
        text, fields, counters, _ = snapshot(1)
        assert counters == (1, 1, 1, 0, 0, 1), text
        assert int(fields["Effective UID del peak"]) == uid, text
        assert fields["Programma del peak"] == "syscall_hook_smoke", text
        assert int(fields["System call del peak"]) == 35, text
        print("PASS: " + label, flush=True)
    finally:
        errors = []
        try:
            ctl("disable")
        except Exception as error:
            errors.append(str(error))
        if worker is not None:
            try:
                worker.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.communicate(timeout=3)
        for kind, value in reversed(registered):
            try:
                ctl(kind + "-remove", value)
            except Exception as error:
                errors.append(str(error))
        try:
            ctl("max-set", "0")
        except Exception as error:
            errors.append(str(error))
        if errors:
            raise RuntimeError("Cleanup incompleto: " + "; ".join(errors))


def main():
    uid = int(os.environ.get("SUDO_UID", "0"))
    if os.geteuid() != 0 or uid == 0:
        raise RuntimeError("Eseguire con sudo da un utente non root")
    gid = pwd.getpwuid(uid).pw_gid
    if "disattivato" not in ctl("status"):
        raise RuntimeError("Il monitor deve essere disattivato")
    for command in ("uid-count", "program-count", "syscall-count"):
        if int(ctl(command).strip().rsplit(":", 1)[1]) != 0:
            raise RuntimeError("Il test richiede registri vuoti")

    program = ("program", "syscall_hook_smoke")
    user = ("uid", str(uid))
    cases = [
        ("solo programma", [program], [program]),
        ("solo UID", [user], [user]),
        ("OR: rimuovi programma, poi UID", [program, user], [program, user]),
        ("OR: rimuovi UID, poi programma", [program, user], [user, program]),
    ]
    print(f"Controller EUID=0, worker EUID={uid}", flush=True)
    for label, identities, order in cases:
        run_case(label, identities, order, uid, gid)
    print("PASS COMPLESSIVO: 4/4; monitor OFF, registri vuoti, MAX=0")


if __name__ == "__main__":
    main()
