# Syscall Throttling Linux Kernel Module

Progetto individuale di Advanced Operating Systems (and System Security),
Università di Roma Tor Vergata, A.A. 2025-2026.

Il modulo espone `/dev/syscall_throttle` e controlla le syscall native x86-64
se il numero è registrato e corrisponde almeno uno tra basename dell'eseguibile
ed effective UID. Il budget MAX è globale e condiviso.

La [guida corrente](docs/COMPORTAMENTO_E_TEST.md) descrive comportamento,
scelte progettuali, test e verifiche residue. I documenti `STATO_SVILUPPO*.md`
sono fotografie storiche delle milestone.

## Compilazione

Dalla radice del repository, con gli header del kernel in esecuzione:

```bash
make -C module
make -C user
make -C tests
```

Ambiente verificato: Ubuntu 24.04.3 LTS, kernel `7.0.0-28-generic`, x86-64,
VM con due CPU. La portabilità verso altri kernel non è attestata dai test.

## Esempio minimo

Richiede modulo inizialmente scaricato. Il worker invoca esplicitamente
nanosleep x86-64 (35); viene selezionato soltanto l'eseguibile di prova.

```bash
sudo insmod module/syscall_throttle.ko
./user/stctl ping
sudo ./user/stctl program-add syscall_hook_smoke
sudo ./user/stctl syscall-add 35
sudo ./user/stctl max-set 1
sudo ./user/stctl enable
./tests/syscall_hook_smoke 20
./user/stctl stats
sudo ./user/stctl disable
sudo ./user/stctl syscall-remove 35
sudo ./user/stctl program-remove syscall_hook_smoke
sudo rmmod syscall_throttle
```

Una singola chiamata entro il budget può non richiedere attesa e quindi
non produrre un peak delay valido.

## Struttura

- `module/`: device, registri, limiter, statistiche e hook Ftrace.
- `include/uapi/`: interfaccia ioctl condivisa.
- `user/`: controller `stctl`.
- `tests/`: programmi e regressioni.
- `scripts/`: script di supporto; consultarli prima dell'uso.
- `docs/`: guida corrente e documenti storici.
