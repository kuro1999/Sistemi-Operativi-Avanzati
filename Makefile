.DEFAULT_GOAL := all
.PHONY: all load build unload demo clean check-user

all: load

check-user:
	@test "$$(id -u)" -ne 0 || { echo "Esegui make da utente normale, senza sudo."; exit 1; }

build:
	$(MAKE) -C module
	$(MAKE) -C user

load: check-user
	$(MAKE) build
	@if test -d /sys/module/syscall_throttle; then \
		echo "Modulo gia' caricato: configurazione conservata; il codice in memoria non viene aggiornato."; \
	else \
		sudo insmod module/syscall_throttle.ko && \
		echo "Modulo caricato; monitor disattivato e pronto per la configurazione."; \
	fi

unload: check-user
	$(MAKE) -C user
	@if test -d /sys/module/syscall_throttle; then \
		sudo ./user/stctl disable && \
		sudo rmmod syscall_throttle && \
		echo "Modulo scaricato."; \
	else \
		echo "Modulo gia' scaricato."; \
	fi

demo: check-user
	$(MAKE) -C demo

clean:
	$(MAKE) -C module clean
	$(MAKE) -C user clean
	$(MAKE) -C demo clean
