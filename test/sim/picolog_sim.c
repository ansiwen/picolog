/*
 * picolog host simulator.
 *
 *   picolog_sim DIR [baud] [history_size]
 *
 * Runs the real history.c and usb_ports.c against pseudo-terminals:
 *   DIR/uart    write here to play the target's serial output
 *   DIR/live    "live" CDC port
 *   DIR/replay  "replay" CDC port
 * (symlinks to the pty slave devices). Prints "ready" on stdout when they exist.
 *
 * A port counts as "DTR asserted" while its pty slave is open (the master then
 * has no POLLHUP). UART input is paced to the baud rate (10 bits per byte);
 * without pacing tests would overflow the ring unrealistically fast. Each port
 * has a 1 KiB TX FIFO like CFG_TUD_CDC_TX_BUFSIZE, drained by flush() into the
 * pty, so a reader that does not read makes the FIFO fill up and the ring drop
 * data exactly like on the device.
 *
 * Not modelled: DMA, UART error handling and BREAK.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "usb_ports.h"

#define TX_FIFO_SIZE 1024

typedef struct {
    int master;
    uint8_t fifo[TX_FIFO_SIZE];
    size_t fifo_len;
} sim_port_t;

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Open a pty pair, put it in raw mode, publish the slave under `link`. The slave
 * is opened and closed once so the master reports POLLHUP until a real user
 * opens it (Linux does not report it before the first open). */
static int make_pty(const char *dir, const char *name) {
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
        perror("pty");
        exit(1);
    }
    const char *sname = ptsname(m);
    int s = open(sname, O_RDWR | O_NOCTTY);
    if (s < 0) {
        perror("open slave");
        exit(1);
    }
    struct termios t;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);
    close(s);

    char link[512];
    snprintf(link, sizeof link, "%s/%s", dir, name);
    unlink(link);
    if (symlink(sname, link) < 0) {
        perror("symlink");
        exit(1);
    }
    return m;
}

static bool slave_open(int master) {
    struct pollfd p = {.fd = master, .events = 0};
    if (poll(&p, 1, 0) < 0)
        return false;
    return !(p.revents & POLLHUP);
}

/* ---- usb_ports ops on top of the pty ports ----------------------------------- */

static sim_port_t ports[2];

static bool op_connected(void *ctx, uint8_t itf) {
    (void)ctx;
    return slave_open(ports[itf].master);
}
static uint32_t op_write_available(void *ctx, uint8_t itf) {
    (void)ctx;
    return (uint32_t)(TX_FIFO_SIZE - ports[itf].fifo_len);
}
static uint32_t op_write(void *ctx, uint8_t itf, const void *data, uint32_t len) {
    (void)ctx;
    sim_port_t *p = &ports[itf];
    size_t room = TX_FIFO_SIZE - p->fifo_len;
    if (len > room)
        len = (uint32_t)room;
    memcpy(p->fifo + p->fifo_len, data, len);
    p->fifo_len += len;
    return len;
}
static void op_flush(void *ctx, uint8_t itf) {
    (void)ctx;
    sim_port_t *p = &ports[itf];
    if (p->fifo_len == 0)
        return;
    ssize_t n = write(p->master, p->fifo, p->fifo_len);
    if (n > 0) {
        memmove(p->fifo, p->fifo + n, p->fifo_len - (size_t)n);
        p->fifo_len -= (size_t)n;
    }
}
static void op_clear_tx(void *ctx, uint8_t itf) {
    (void)ctx;
    ports[itf].fifo_len = 0;
}
static void op_discard_rx(void *ctx, uint8_t itf) {
    (void)ctx;
    char tmp[256];
    while (read(ports[itf].master, tmp, sizeof tmp) > 0)
        ;
}

static const usb_port_ops_t ops = {
    .connected = op_connected,
    .write_available = op_write_available,
    .write = op_write,
    .flush = op_flush,
    .clear_tx = op_clear_tx,
    .discard_rx = op_discard_rx,
};

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s DIR [baud] [history_size]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    unsigned long baud = argc > 2 ? strtoul(argv[2], NULL, 0) : 115200;
    size_t hsize = argc > 3 ? strtoul(argv[3], NULL, 0) : 256u * 1024u;
    if (baud == 0 || hsize == 0 || (hsize & (hsize - 1))) {
        fprintf(stderr, "baud must be > 0, history_size a power of two\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    history_t hist;
    history_init(&hist, malloc(hsize), hsize);

    int uart = make_pty(dir, "uart");
    ports[0].master = make_pty(dir, "live");
    ports[1].master = make_pty(dir, "replay");

    usb_port_t live, replay;
    usb_port_init(&live, USB_PORT_KIND_LIVE, 0, &ops, &hist);
    usb_port_init(&replay, USB_PORT_KIND_REPLAY, 1, &ops, &hist);

    printf("ready\n");
    fflush(stdout);

    uint64_t start = now_us();
    uint64_t last = start;
    double credit = 0; /* bytes the UART may still deliver */
    while (!stop) {
        uint64_t t = now_us();
        credit += (double)(t - last) * (double)baud / 10e6;
        last = t;
        if (credit > 4096)
            credit = 4096; /* no unbounded burst after a stall */
        size_t allow = (size_t)credit;
        if (allow > 0) {
            uint8_t buf[4096];
            ssize_t n = read(uart, buf, allow < sizeof buf ? allow : sizeof buf);
            if (n > 0) {
                history_append(&hist, buf, (size_t)n);
                credit -= (double)n;
            }
        }
        usb_port_task(&live, t - start);
        usb_port_task(&replay, t - start);
        struct timespec nap = {0, 200000}; /* 0.2 ms */
        nanosleep(&nap, NULL);
    }
    return 0;
}
