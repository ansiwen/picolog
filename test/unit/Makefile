CC ?= gcc
CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
         -fno-sanitize-recover=undefined -I../../src

all: test_history test_usb_ports

test_history: test_history.c ../../src/history.c ../../src/history.h
	$(CC) $(CFLAGS) -o $@ test_history.c ../../src/history.c

test_usb_ports: test_usb_ports.c ../../src/usb_ports.c ../../src/usb_ports.h \
                ../../src/history.c ../../src/history.h
	$(CC) $(CFLAGS) -o $@ test_usb_ports.c ../../src/usb_ports.c ../../src/history.c

check: all
	./test_history
	./test_usb_ports

clean:
	rm -f test_history test_usb_ports

.PHONY: all check clean
