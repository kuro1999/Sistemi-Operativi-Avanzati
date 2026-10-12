CC ?= gcc

CPPFLAGS += -I.
CFLAGS += -Wall -Wextra -Wpedantic -std=c11 -O2

TARGET := stctl
SOURCES := stctl.c

all: $(TARGET)

$(TARGET): $(SOURCES) syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -f $(TARGET)

.PHONY: all clean
