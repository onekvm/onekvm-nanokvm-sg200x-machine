CC ?= cc
CFLAGS ?= -O2

.PHONY: all clean test-boot-splash

all: onekvm-machine-nanokvm-i2c-probe

onekvm-machine-nanokvm-i2c-probe: src/onekvm-machine-nanokvm-detect.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@

test-boot-splash:
	@work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
	    tests/boot-splash-test.c $(LDFLAGS) -o "$$work/test" && "$$work/test"

clean:
	$(RM) onekvm-machine-nanokvm-i2c-probe
