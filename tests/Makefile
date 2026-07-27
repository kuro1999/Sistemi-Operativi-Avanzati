.RECIPEPREFIX := >

CC ?= cc

CPPFLAGS += -I../include/uapi
CFLAGS += -Wall -Wextra -Wpedantic -std=c11 -O2
LDLIBS += -pthread

TARGETS := policy_transition_stress

.PHONY: all clean

all: $(TARGETS)

%: %.c
>$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
>$(RM) $(TARGETS)
