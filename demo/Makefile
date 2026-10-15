CC ?= cc
CFLAGS += -Wall -Wextra -Wpedantic -std=c11 -O2
TARGETS := demo_worker demo_alt
.DEFAULT_GOAL := all
.PHONY: all run build clean

all: run

run: build
	bash fase1.sh
	bash fase2.sh
	bash fase3.sh
	@echo "DEMO COMPLETATA: 3 fasi."

build: $(TARGETS)
	$(MAKE) -C ../module
	$(MAKE) -C ../user

demo_worker demo_alt: worker.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) $(TARGETS) soa_demo
