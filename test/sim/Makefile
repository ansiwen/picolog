# Host simulator for running test/hw/test_integration.py without hardware.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -I../../src
SRC := ../../src

picolog_sim: picolog_sim.c $(SRC)/history.c $(SRC)/usb_ports.c $(SRC)/history.h $(SRC)/usb_ports.h
	$(CC) $(CFLAGS) -o $@ picolog_sim.c $(SRC)/history.c $(SRC)/usb_ports.c

clean:
	rm -f picolog_sim
