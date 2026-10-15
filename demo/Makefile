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

	sudo -n insmod ../module/syscall_throttle.ko
	status=0
	sudo -n timeout --kill-after=10s 120s ./soa_demo || status=$$?
	if test "$$status" -ne 0; then
	    echo "Demo non completata: codice $$status (3=inconclusiva, 124=timeout)."
	    echo "Nessuno scaricamento automatico dopo il fallimento; controllare sudo dmesg."
	    exit "$$status"
	fi

	echo ""
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

	echo "  Verifica finale: modulo scaricato."
	echo "  PASS FASE 3"
	echo "DEMO SUPERATA: 3 fasi."

build: $(TARGETS)
	$(MAKE) -C ../module
	$(MAKE) -C ../user

soa_demo: demo.c protocol.h ../user/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ demo.c
demo_worker demo_alt: worker.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ worker.c
clean:
	$(RM) $(TARGETS)
