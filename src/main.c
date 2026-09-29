// picolog: UART flight recorder + live console.
//
// Single-core superloop on core0. The DMA does the realtime capture work;
// everything else (copying into the history ring, USB) runs here. See
// history.h for the concurrency assumptions.
#include "capture.h"
#include "history.h"

#include "pico/stdlib.h"

#define HISTORY_SIZE (256u * 1024u)

static history_hdr_t history_hdr[2];
static uint8_t history_buf[HISTORY_SIZE];
static history_t history;

static void to_history(const uint8_t *data, size_t len, void *ctx) {
    history_append(ctx, data, len);
}

int main(void) {
    history_boot(&history, history_hdr, history_buf, HISTORY_SIZE);
    capture_init();

    for (;;) {
        capture_events_t ev = {0};
        capture_poll(to_history, &history, &ev);
    }
}
