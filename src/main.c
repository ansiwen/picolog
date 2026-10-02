/*
 * picolog: UART flight recorder + live console.
 *
 * Single-core superloop: DMA captures the UART in the background, the loop moves
 * bytes into the history ring, turns UART line errors into in-band markers and
 * serves the two USB CDC ports.
 */
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "capture.h"
#include "history.h"
#include "selftest_tx.h"
#include "usb_ports.h"

#define HISTORY_SIZE (256u * 1024u)
#define MARKER_MIN_INTERVAL_US 100000u /* at most one marker emission per 100 ms */

#define ITF_LIVE 0
#define ITF_REPLAY 1

static uint8_t history_buf[HISTORY_SIZE];
static history_t history;

/* ---- TinyUSB glue for usb_ports ------------------------------------------ */

/* "Connected" = configured by a host AND DTR set. Deliberately not
 * tud_cdc_n_connected(): that one also requires !suspended, but the RP2350
 * TinyUSB port forces VBUS detect on, so an unplug looks like a suspend, and a
 * host that sleeps with a terminal open suspends too. Ignoring suspend keeps the
 * session across those. A real replug resets the bus, which clears the
 * configuration and DTR, so it still starts a new session. */
static bool glue_connected(void *ctx, uint8_t itf) {
    (void)ctx;
    return tud_mounted() && (tud_cdc_n_get_line_state(itf) & 1u);
}
static uint32_t glue_write_available(void *ctx, uint8_t itf) {
    (void)ctx;
    return tud_cdc_n_write_available(itf);
}
static uint32_t glue_write(void *ctx, uint8_t itf, const void *data, uint32_t len) {
    (void)ctx;
    return tud_cdc_n_write(itf, data, len);
}
static void glue_flush(void *ctx, uint8_t itf) {
    (void)ctx;
    tud_cdc_n_write_flush(itf);
}
static void glue_clear_tx(void *ctx, uint8_t itf) {
    (void)ctx;
    tud_cdc_n_write_clear(itf);
}
static void glue_discard_rx(void *ctx, uint8_t itf) {
    (void)ctx;
    tud_cdc_n_read_flush(itf);
}

static const usb_port_ops_t usb_ops = {
    .ctx = NULL,
    .connected = glue_connected,
    .write_available = glue_write_available,
    .write = glue_write,
    .flush = glue_flush,
    .clear_tx = glue_clear_tx,
    .discard_rx = glue_discard_rx,
};

static usb_port_t port_live;
static usb_port_t port_replay;

/* ---- capture -> history ------------------------------------------------------ */

static void sink_to_history(void *ctx, const uint8_t *data, size_t len) {
    history_append(ctx, data, len);
}

/* ---- markers ----------------------------------------------------------------- */

static capture_events_t pending_events;
static uint64_t last_marker_us;

static void emit_count_marker(const char *what, uint32_t count) {
    if (count == 0)
        return;
    if (count == 1)
        history_appendf(&history, "\r\n[picolog: %s]\r\n", what);
    else
        history_appendf(&history, "\r\n[picolog: %s x%lu]\r\n", what, (unsigned long)count);
}

/* Markers go in after the data of the same poll, so their placement is only
 * approximate. Accumulate between emissions so a wrong baud rate (a stream of
 * framing errors) cannot flood the history. */
static void emit_markers(uint64_t now_us) {
    capture_events_t *e = &pending_events;
    if (!(e->brk | e->framing | e->parity | e->uart_overrun | e->capture_lost))
        return;
    if (now_us - last_marker_us < MARKER_MIN_INTERVAL_US)
        return;
    last_marker_us = now_us;
    emit_count_marker("BREAK", e->brk);
    emit_count_marker("framing error", e->framing);
    emit_count_marker("parity error", e->parity);
    emit_count_marker("UART FIFO overrun", e->uart_overrun);
    if (e->capture_lost)
        history_appendf(&history, "\r\n[picolog: capture overrun, %lu bytes lost]\r\n",
                        (unsigned long)e->capture_lost);
    *e = (capture_events_t){0};
}

int main(void) {
    history_init(&history, history_buf, sizeof history_buf);
    capture_init();
    usb_port_init(&port_live, USB_PORT_KIND_LIVE, ITF_LIVE, &usb_ops, &history);
    usb_port_init(&port_replay, USB_PORT_KIND_REPLAY, ITF_REPLAY, &usb_ops, &history);
    tud_init(BOARD_TUD_RHPORT);
    selftest_tx_start(); /* empty unless built with PICOLOG_SELFTEST_TX */

    for (;;) {
        tud_task();
        capture_poll(sink_to_history, &history, &pending_events);
        uint64_t now = time_us_64();
        emit_markers(now);
        usb_port_task(&port_live, now);
        usb_port_task(&port_replay, now);
    }
}
