# Host-side unit tests for the hardware-independent firmware modules.
#   make -C test/unit          build and run all tests
CC ?= cc
CFLAGS ?= -O1 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -I../../src -fsanitize=address,undefined -fno-omit-frame-pointer
SRC := ../../src

TESTS := test_history test_usb_ports

.PHONY: all check clean
all: check

check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_history: test_history.c $(SRC)/history.c $(SRC)/history.h
	$(CC) $(CFLAGS) -o $@ test_history.c $(SRC)/history.c

test_usb_ports: test_usb_ports.c $(SRC)/usb_ports.c $(SRC)/usb_ports.h $(SRC)/history.c $(SRC)/history.h
	$(CC) $(CFLAGS) -o $@ test_usb_ports.c $(SRC)/usb_ports.c $(SRC)/history.c

clean:
	rm -f $(TESTS)
