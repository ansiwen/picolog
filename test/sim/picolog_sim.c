// Host simulator: runs the real history ring and USB port logic on Linux,
// with pseudo-terminals standing in for the hardware:
//
//   <dir>/uart    write here = bytes arriving on the Pico's UART RX
//   <dir>/live    the live CDC port
//   <dir>/replay  the replay CDC port
//
// A port counts as "DTR asserted" while some process has its pty open
// (Linux reports POLLHUP on the master while no slave fd is open).
// Used to exercise test/hw/test_integration.py without hardware; it does not
// model DMA, UART errors or resets. UART input is paced to the given baud
// rate (8N1, 10 bits per byte) like the real line.
//
//   ./picolog_sim /tmp/picolog-sim [baud] [history_size]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "usb_ports.h"

static int master_fd[2];  // live, replay

static int make_pty(const char *link) {
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0 || grantpt(fd) < 0 || unlockpt(fd) < 0) {
        perror("pty");
        exit(1);
    }
    const char *name = ptsname(fd);
    // Raw mode on the slave side (termios persists while the master is open).
    int s = open(name, O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);
    close(s);
    unlink(link);
    if (symlink(name, link) < 0) {
        perror("symlink");
        exit(1);
    }
    return fd;
}

static bool io_connected(uint8_t itf) {
    struct pollfd p = {.fd = master_fd[itf], .events = 0};
    poll(&p, 1, 0);
    return !(p.revents & POLLHUP);
}
static uint32_t io_write_available(uint8_t itf) {
    (void)itf;
    return 4096;  // real limit found by the non-blocking write
}
static uint32_t io_write(uint8_t itf, const void *buf, uint32_t len) {
    ssize_t n = write(master_fd[itf], buf, len);
    return n > 0 ? (uint32_t)n : 0;
}
static void io_flush(uint8_t itf) { (void)itf; }
static void io_clear_tx(uint8_t itf) { tcflush(master_fd[itf], TCIFLUSH); }
static void io_discard_rx(uint8_t itf) {
    char tmp[256];
    while (read(master_fd[itf], tmp, sizeof(tmp)) > 0) {
    }
}

static const usb_port_io_t io = {io_connected, io_write_available, io_write, io_flush, io_clear_tx, io_discard_rx};

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s DIR [baud] [history_size]\n", argv[0]);
        return 2;
    }
    double bytes_per_us = (argc > 2 ? strtod(argv[2], NULL) : 115200.0) / 10.0 / 1e6;
    uint32_t size = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 256u * 1024u;
    if (size < 2 * HISTORY_COMMIT_CHUNK || (size & (size - 1))) {
        fprintf(stderr, "history size must be a power of two >= %u\n", 2 * HISTORY_COMMIT_CHUNK);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    mkdir(argv[1], 0755);
    char path[4096];
    snprintf(path, sizeof(path), "%s/uart", argv[1]);
    int uart = make_pty(path);
    snprintf(path, sizeof(path), "%s/live", argv[1]);
    master_fd[0] = make_pty(path);
    snprintf(path, sizeof(path), "%s/replay", argv[1]);
    master_fd[1] = make_pty(path);

    // Keep the uart pty "connected" so writes from the generator never EIO.
    int uart_keep = open(ptsname(uart), O_RDWR | O_NOCTTY);
    (void)uart_keep;

    static history_hdr_t hdr[2];
    uint8_t *buf = malloc(size);
    history_t h;
    history_boot(&h, hdr, buf, size);
    history_appendf(&h, "\r\n[picolog: boot, reset reason sim, history cleared]\r\n");

    usb_port_t live, replay;
    usb_port_init(&live, 0, false);
    usb_port_init(&replay, 1, true);
    uint64_t t0 = now_us();
    uint64_t last = t0;
    double allowance = 0;  // bytes the "UART" may deliver right now
    fprintf(stderr, "picolog_sim: ready in %s\n", argv[1]);

    for (;;) {
        uint64_t t = now_us();
        allowance += (double)(t - last) * bytes_per_us;
        last = t;
        if (allowance > 4096) {
            allowance = 4096;
        }
        uint8_t tmp[4096];
        ssize_t n = 0;
        if (allowance >= 1) {
            n = read(uart, tmp, (size_t)allowance);
            if (n > 0) {
                allowance -= (double)n;
            }
        }
        if (n > 0) {
            history_append(&h, tmp, (size_t)n);
        } else if (n < 0 && errno != EAGAIN && errno != EIO) {
            perror("uart read");
            return 1;
        }
        usb_port_task(&live, &io, &h, now_us() - t0);
        usb_port_task(&replay, &io, &h, now_us() - t0);
        if (n <= 0) {
            usleep(200);
        }
    }
}
