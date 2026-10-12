CC ?= cc
CPPFLAGS += -I../user
CFLAGS += -Wall -Wextra -Wpedantic -std=c11 -O2
TARGETS := soa_demo demo_worker demo_alt
.PHONY: all clean
all: $(TARGETS)
soa_demo: demo.c protocol.h ../user/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ demo.c
demo_worker demo_alt: worker.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ worker.c
clean:
	$(RM) $(TARGETS)
