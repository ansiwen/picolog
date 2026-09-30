CC ?= gcc
CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
         -fno-sanitize-recover=undefined -I../../src

TESTS = test_history test_capture test_usb_ports

# `make` builds and runs the tests; `make build` only builds them.
all: check

build: $(TESTS)

test_history: test_history.c ../../src/history.c ../../src/history.h
	$(CC) $(CFLAGS) -o $@ test_history.c ../../src/history.c

test_capture: test_capture.c ../../src/capture.h
	$(CC) $(CFLAGS) -o $@ test_capture.c

test_usb_ports: test_usb_ports.c ../../src/usb_ports.c ../../src/usb_ports.h \
                ../../src/history.c ../../src/history.h
	$(CC) $(CFLAGS) -o $@ test_usb_ports.c ../../src/usb_ports.c ../../src/history.c

check: build
	./test_history
	./test_capture
	./test_usb_ports

clean:
	rm -f $(TESTS)

.PHONY: all build check clean
