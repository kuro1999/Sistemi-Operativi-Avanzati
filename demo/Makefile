.ONESHELL:
.SHELLFLAGS := -eu -c

CC ?= cc
CPPFLAGS += -I../user
CFLAGS += -Wall -Wextra -Wpedantic -std=c11 -O2
TARGETS := soa_demo demo_worker demo_alt
.DEFAULT_GOAL := all
.PHONY: all run build clean

all: run

# Eseguire come utente normale: sudo serve solo per le operazioni privilegiate.
run:
	@test "$$(id -u)" -ne 0 || { echo "Esegui make da utente normale, senza sudo."; exit 1; }
	@test ! -d /sys/module/syscall_throttle || { echo "Modulo gia' caricato: scaricalo prima di avviare la demo."; exit 1; }
	$(MAKE) build
	sudo -v
	# Eseguito dal Makefile dalla directory demo, dopo build e sudo -v.
	set -eu

	inconclusive() {
	    echo "INCONCLUSIVA: $$*" >&2
	    exit 3
	}

	# Memorizza il log precedente: un vecchio risultato non prova questa esecuzione.
	before=$$(sudo -n dmesg --raw) ||
	    inconclusive "impossibile leggere dmesg; modulo non caricato dalla demo."
	old_load=$$(printf '%s\n' "$$before" | awk '/syscall_throttle: modulo caricato/ {line=$$0} END {print line}')

	sudo -n insmod ../module/syscall_throttle.ko
	status=0
	sudo -n timeout --kill-after=10s 120s ./soa_demo || status=$$?
	if test "$$status" -ne 0; then
	    echo "Demo non completata: codice $$status (3=inconclusiva, 124=timeout)."
	    echo "Nessuno scaricamento automatico dopo il fallimento; controllare sudo dmesg."
	    exit "$$status"
	fi

	echo "Verifica finale fase 6: scaricamento ordinario dopo OFF e chiusura del device."
	status=0
	sudo -n timeout --kill-after=5s 15s rmmod syscall_throttle || status=$$?
	if test "$$status" -ne 0; then
	    echo "FAIL: scaricamento finale non completato (codice $$status)."
	    exit "$$status"
	fi
	test ! -d /sys/module/syscall_throttle || {
	    echo "FAIL: modulo ancora presente."
	    exit 1
	}

	# Il modulo emette i contatori durante lo scaricamento, non prima.
	after=$$(sudo -n dmesg --raw) ||
	    inconclusive "modulo scaricato, ma diagnostica kernel non leggibile."
	new_load=$$(printf '%s\n' "$$after" | awk '/syscall_throttle: modulo caricato/ {line=$$0} END {print line}')
	test -n "$$new_load" && test "$$new_load" != "$$old_load" ||
	    inconclusive "manca il nuovo messaggio di caricamento nel log kernel."
	diagnostic=$$(printf '%s\n' "$$after" | awk '
	    /syscall_throttle: modulo caricato/ {seen=1; line=""}
	    seen && /syscall_throttle: diagnostica nonreturning:/ {line=$$0}
	    END {print line}')
	test -n "$$diagnostic" ||
	    inconclusive "nessuna diagnostica nonreturning dopo il nuovo caricamento."

	echo "Prova dal log kernel di questa esecuzione:"
	printf '  %s\n' "$$diagnostic"
	counts=$$(printf '%s\n' "$$diagnostic" | sed -n 's/.*diagnostica nonreturning: avviate=\([0-9][0-9]*\), completate=\([0-9][0-9]*\), ritorni_inattesi=\([0-9][0-9]*\)[[:space:]]*$$/\1 \2 \3/p')
	# Il formato appena validato contiene soltanto tre numeri separati da spazi.
	set -- $$counts
	test "$$#" -eq 3 || inconclusive "formato della diagnostica non riconosciuto."
	printf '  Attesi:   avviate=2, completate=2, ritorni_inattesi=0\n'
	printf '  Osservati: avviate=%s, completate=%s, ritorni_inattesi=%s\n' "$$1" "$$2" "$$3"
	if test "$$1" -ne 2 || test "$$2" -ne 2 || test "$$3" -ne 0; then
	    echo "FAIL: contatori nonreturning diversi dai valori attesi."
	    exit 1
	fi
	echo "PASS: contatori kernel coerenti con exit ed exit_group e modulo scaricato."
	echo "DEMO SUPERATA: 6 fasi, scaricamento e diagnostica kernel verificati."

build: $(TARGETS)
	$(MAKE) -C ../module
	$(MAKE) -C ../user

soa_demo: demo.c protocol.h ../user/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ demo.c
demo_worker demo_alt: worker.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ worker.c
clean:
	$(RM) $(TARGETS)
