CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static

PROGRAMS := early-cleanup-proof allocation-cleanup-proof

.PHONY: all clean

all: $(PROGRAMS)

early-cleanup-proof: early-cleanup-proof.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

allocation-cleanup-proof: allocation-cleanup-proof.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm -f $(PROGRAMS)
