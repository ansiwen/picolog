#include "usb_ports.h"

#include <stdio.h>

#define MSG_MAX 128

static const char REPLAY_END_MSG[] = "\r\n=== picolog replay end, live follows ===\r\n";

/* Queue a whole message or nothing: returns false (and sends nothing) if the TX
 * FIFO cannot take it yet, so the caller just retries on the next call. */
static bool send_atomic(usb_port_t *p, const char *msg, uint32_t len) {
    if (p->ops->write_available(p->ops->ctx, p->itf) < len)
        return false;
    p->ops->write(p->ops->ctx, p->itf, msg, len);
    return true;
}

/* If the cursor fell behind the oldest valid byte, tell the host and jump.
 * Returns false if the marker could not be queued yet. */
static bool handle_drop(usb_port_t *p) {
    uint64_t oldest = history_oldest(p->history);
    if (p->cursor >= oldest)
        return true;
    char msg[MSG_MAX];
    int n = snprintf(msg, sizeof msg, "\r\n[picolog: %llu bytes dropped]\r\n",
                     (unsigned long long)(oldest - p->cursor));
    if (!send_atomic(p, msg, (uint32_t)n))
        return false;
    history_clamp(p->history, &p->cursor);
    return true;
}

/* Take the replay snapshot and queue the header. Nothing is decided earlier than
 * this call: a snapshot taken at the DTR edge would have its oldest bytes
 * overwritten during the open delay whenever the ring is full and data flows. */
static bool start_replay(usb_port_t *p, uint64_t now_us) {
    uint64_t oldest = history_oldest(p->history);
    uint64_t end = history_head(p->history);
    uint64_t secs = now_us / 1000000u;
    char msg[MSG_MAX];
    int n = snprintf(msg, sizeof msg,
                     "=== picolog replay: %llu bytes, uptime %llud %02u:%02u:%02u ===\r\n",
                     (unsigned long long)(end - oldest),
                     (unsigned long long)(secs / 86400u),
                     (unsigned)(secs / 3600u % 24u), (unsigned)(secs / 60u % 60u),
                     (unsigned)(secs % 60u));
    if (!send_atomic(p, msg, (uint32_t)n))
        return false;
    p->cursor = oldest;
    p->replay_end = end;
    p->state = USB_PORT_REPLAY;
    return true;
}

/* Send as much history as the TX FIFO takes, up to `limit`. */
static void pump(usb_port_t *p, uint64_t limit) {
    while (p->cursor < limit) {
        const uint8_t *ptr;
        size_t n = history_peek(p->history, p->cursor, &ptr);
        if (n == 0)
            break;
        if (n > limit - p->cursor)
            n = (size_t)(limit - p->cursor);
        uint32_t avail = p->ops->write_available(p->ops->ctx, p->itf);
        if (avail == 0)
            break;
        if (n > avail)
            n = avail;
        uint32_t w = p->ops->write(p->ops->ctx, p->itf, ptr, (uint32_t)n);
        p->cursor += w;
        if (w < n)
            break;
    }
}

static void stream(usb_port_t *p) {
    for (;;) {
        if (!handle_drop(p))
            return;
        if (p->state == USB_PORT_REPLAY) {
            /* If a drop pushed the cursor past the snapshot end, that part of
             * the history is gone: cursor >= replay_end, so the replay ends
             * right here and live continues at the cursor. */
            pump(p, p->replay_end);
            if (p->cursor < p->replay_end)
                return; /* FIFO full: continue on the next call */
            if (!send_atomic(p, REPLAY_END_MSG, sizeof REPLAY_END_MSG - 1))
                return;
            p->state = USB_PORT_LIVE_STREAM;
            continue; /* live continues exactly at the replay end offset */
        }
        pump(p, history_head(p->history));
        return;
    }
}

void usb_port_init(usb_port_t *p, usb_port_kind_t kind, uint8_t itf,
                   const usb_port_ops_t *ops, history_t *history) {
    p->kind = kind;
    p->itf = itf;
    p->ops = ops;
    p->history = history;
    p->state = USB_PORT_CLOSED;
    p->cursor = 0;
    p->replay_end = 0;
    p->send_after = 0;
}

void usb_port_task(usb_port_t *p, uint64_t now_us) {
    const usb_port_ops_t *ops = p->ops;

    /* We never look at what the host sends (ModemManager probes, etc.). */
    ops->discard_rx(ops->ctx, p->itf);

    if (!ops->connected(ops->ctx, p->itf)) {
        p->state = USB_PORT_CLOSED;
    } else {
        if (p->state == USB_PORT_CLOSED) {
            /* DTR rising edge: drop stale TX data and start a new session. */
            ops->clear_tx(ops->ctx, p->itf);
            p->send_after = now_us + USB_PORT_OPEN_DELAY_US;
            if (p->kind == USB_PORT_KIND_LIVE) {
                /* The live cursor is fixed at the edge, so data arriving during
                 * the delay is still delivered. */
                p->cursor = history_head(p->history);
                p->state = USB_PORT_LIVE_STREAM;
            } else {
                p->state = USB_PORT_REPLAY_PENDING;
            }
        }
        if (now_us >= p->send_after) {
            if (p->state == USB_PORT_REPLAY_PENDING)
                start_replay(p, now_us);
            if (p->state != USB_PORT_REPLAY_PENDING)
                stream(p);
        }
    }

    /* Unconditional: while USB is suspended TinyUSB does not transmit, so the
     * FIFO fills up and nothing new can be written; without a flush on every
     * call the queued bytes would never go out after resume. */
    ops->flush(ops->ctx, p->itf);
}
