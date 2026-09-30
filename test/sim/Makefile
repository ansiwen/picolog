CC ?= gcc
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -I../../src

picolog_sim: picolog_sim.c ../../src/history.c ../../src/usb_ports.c \
             ../../src/history.h ../../src/usb_ports.h
	$(CC) $(CFLAGS) -o $@ picolog_sim.c ../../src/history.c ../../src/usb_ports.c

clean:
	rm -f picolog_sim

.PHONY: clean
