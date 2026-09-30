// picolog: UART flight recorder + live console.
//
// Single-core superloop on core0. The DMA does the realtime capture work;
// everything else (copying into the history ring, USB) runs here. See
// history.h for the concurrency assumptions.
#include "capture.h"
#include "history.h"
#include "usb_ports.h"

#include "pico/stdlib.h"
#include "tusb.h"

#define HISTORY_SIZE (256u * 1024u)

// Line-error markers are aggregated and emitted at most this often, so a
// misconfigured baud rate (a framing error on every byte) cannot flood the
// history with markers.
#define MARKER_INTERVAL_US 100000u

static uint8_t history_buf[HISTORY_SIZE];
static history_t history;

static usb_port_t port_live;
static usb_port_t port_replay;

// "Connected" = configured by a host and DTR asserted. Unlike
// tud_cdc_n_connected() this deliberately ignores USB suspend: the RP2350
// TinyUSB port forces VBUS detect on, so an unplugged cable looks like a
// suspend, and a host going to sleep with the terminal still open suspends
// the bus too. Keeping the session across a suspend means a resumed terminal
// simply continues (with a "dropped" marker if needed) instead of getting a
// second replay. A real replug always starts a new session because the
// host's bus reset clears the configuration and DTR (usbd_reset/cdcd_reset).
// While suspended TinyUSB does not transmit, so the TX FIFO just fills up.
static bool io_connected(uint8_t itf) { return tud_mounted() && (tud_cdc_n_get_line_state(itf) & 0x1u); }
static uint32_t io_write_available(uint8_t itf) { return tud_cdc_n_write_available(itf); }
static uint32_t io_write(uint8_t itf, const void *buf, uint32_t len) { return tud_cdc_n_write(itf, buf, len); }
static void io_flush(uint8_t itf) { tud_cdc_n_write_flush(itf); }
static void io_clear_tx(uint8_t itf) { tud_cdc_n_write_clear(itf); }
static void io_discard_rx(uint8_t itf) { tud_cdc_n_read_flush(itf); }

static const usb_port_io_t tusb_io = {
    .connected = io_connected,
    .write_available = io_write_available,
    .write = io_write,
    .flush = io_flush,
    .clear_tx = io_clear_tx,
    .discard_rx = io_discard_rx,
};

static void to_history(const uint8_t *data, size_t len, void *ctx) {
    history_append(ctx, data, len);
}

static void marker_count(const char *what, uint32_t n) {
    if (n == 1) {
        history_appendf(&history, "\r\n[picolog: %s]\r\n", what);
    } else if (n > 1) {
        history_appendf(&history, "\r\n[picolog: %s x%lu]\r\n", what, (unsigned long)n);
    }
}

static void emit_markers(capture_events_t *ev, uint64_t now_us) {
    static bool emitted_before;
    static uint64_t last_us;
    if (!ev->breaks && !ev->framing_errors && !ev->parity_errors && !ev->uart_overruns && !ev->lost_bytes) {
        return;
    }
    if (emitted_before && now_us - last_us < MARKER_INTERVAL_US) {
        return;  // keep accumulating
    }
    emitted_before = true;
    last_us = now_us;

    if (ev->lost_bytes) {
        history_appendf(&history, "\r\n[picolog: capture overrun, %llu bytes lost]\r\n",
                        (unsigned long long)ev->lost_bytes);
    }
    marker_count("UART FIFO overrun", ev->uart_overruns);
    marker_count("BREAK", ev->breaks);
    marker_count("framing error", ev->framing_errors);
    marker_count("parity error", ev->parity_errors);
    *ev = (capture_events_t){0};
}

int main(void) {
    history_init(&history, history_buf, HISTORY_SIZE);

    capture_init();

    usb_port_init(&port_live, 0, false);
    usb_port_init(&port_replay, 1, true);
    tud_init(BOARD_TUD_RHPORT);

    capture_events_t ev = {0};
    for (;;) {
        tud_task();

        // Data first, then markers, so a marker lands after the bytes that
        // were received before the event was noticed.
        capture_poll(to_history, &history, &ev);
        uint64_t now = time_us_64();
        emit_markers(&ev, now);

        usb_port_task(&port_live, &tusb_io, &history, now);
        usb_port_task(&port_replay, &tusb_io, &history, now);
    }
}
