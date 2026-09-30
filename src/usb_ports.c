#include "usb_ports.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void usb_port_init(usb_port_t *p, uint8_t itf, bool is_replay) {
    memset(p, 0, sizeof(*p));
    p->itf = itf;
    p->is_replay = is_replay;
    p->state = USB_PORT_CLOSED;
}

__attribute__((format(printf, 2, 3))) static void set_msg(usb_port_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->msg, sizeof(p->msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(p->msg)) {
        n = sizeof(p->msg) - 1;
    }
    p->msg_len = (uint16_t)n;
    p->msg_pos = 0;
}

static void open_port(usb_port_t *p, const history_t *h, uint64_t uptime_us) {
    p->msg_len = p->msg_pos = 0;
    p->opened_at_us = uptime_us;
    if (p->is_replay) {
        p->state = USB_PORT_REPLAY_PENDING;
    } else {
        p->cursor = history_head(h);
        p->state = USB_PORT_LIVE;
    }
}

static void start_replay(usb_port_t *p, const history_t *h, uint64_t uptime_us) {
    p->cursor = history_oldest(h);
    p->replay_end = history_head(h);
    p->state = USB_PORT_REPLAY;

    uint64_t s = uptime_us / 1000000u;
    set_msg(p, "=== picolog replay: %llu bytes, uptime %llud %02u:%02u:%02u ===\r\n",
            (unsigned long long)(p->replay_end - p->cursor), (unsigned long long)(s / 86400u),
            (unsigned)(s / 3600u % 24u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
}

void usb_port_task(usb_port_t *p, const usb_port_io_t *io, const history_t *h, uint64_t uptime_us) {
    io->discard_rx(p->itf);

    if (!io->connected(p->itf)) {
        p->state = USB_PORT_CLOSED;
        return;
    }
    if (p->state == USB_PORT_CLOSED) {
        io->clear_tx(p->itf);
        open_port(p, h, uptime_us);
    }
    if (uptime_us - p->opened_at_us < USB_PORT_OPEN_DELAY_US) {
        return;
    }
    if (p->state == USB_PORT_REPLAY_PENDING) {
        start_replay(p, h, uptime_us);
    }

    for (;;) {
        uint32_t avail = io->write_available(p->itf);
        if (avail == 0) {
            break;
        }

        if (p->msg_pos < p->msg_len) {
            uint32_t n = p->msg_len - p->msg_pos;
            n = io->write(p->itf, p->msg + p->msg_pos, n < avail ? n : avail);
            if (n == 0) {
                break;
            }
            p->msg_pos = (uint16_t)(p->msg_pos + n);
            continue;
        }

        uint64_t dropped = history_clamp(h, &p->cursor);
        if (dropped) {
            set_msg(p, "\r\n[picolog: %llu bytes dropped]\r\n", (unsigned long long)dropped);
            continue;
        }

        if (p->state == USB_PORT_REPLAY && p->cursor >= p->replay_end) {
            // cursor == replay_end here unless drops skipped past it; either
            // way live continues from the cursor, so nothing is duplicated.
            set_msg(p, "\r\n=== picolog replay end, live follows ===\r\n");
            p->state = USB_PORT_LIVE;
            continue;
        }

        const uint8_t *ptr;
        uint64_t n = history_peek(h, p->cursor, &ptr);
        if (p->state == USB_PORT_REPLAY && n > p->replay_end - p->cursor) {
            n = p->replay_end - p->cursor;
        }
        if (n > avail) {
            n = avail;
        }
        if (n == 0) {
            break;
        }
        uint32_t w = io->write(p->itf, ptr, (uint32_t)n);
        if (w == 0) {
            break;
        }
        p->cursor += w;
    }

    // Flush unconditionally, not only after a write: if the FIFO filled up
    // while the host was not accepting data (e.g. USB suspend, when TinyUSB
    // does not transmit), nothing new can be written, and without this call
    // the queued data would never be sent after the host comes back.
    io->flush(p->itf);
}
