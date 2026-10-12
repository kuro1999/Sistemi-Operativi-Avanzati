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
	sudo -n insmod ../module/syscall_throttle.ko
	sudo -n timeout --kill-after=10s 120s ./soa_demo
	sudo -n rmmod syscall_throttle
	@echo "Demo superata; modulo scaricato."

build: $(TARGETS)
	$(MAKE) -C ../module
	$(MAKE) -C ../user

soa_demo: demo.c protocol.h ../user/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ demo.c
demo_worker demo_alt: worker.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ worker.c
clean:
	$(RM) $(TARGETS)
