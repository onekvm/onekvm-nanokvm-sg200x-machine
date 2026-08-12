CC ?= cc
CFLAGS ?= -O2

.PHONY: all clean

all: onekvm-machine-nanokvm-i2c-probe

onekvm-machine-nanokvm-i2c-probe: src/onekvm-machine-nanokvm-detect.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@

clean:
	$(RM) onekvm-machine-nanokvm-i2c-probe
