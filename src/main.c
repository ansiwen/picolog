#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "capture.h"
#include "history.h"

#define HISTORY_SIZE (256u * 1024u)
#define MARKER_MIN_INTERVAL_US 100000u

static uint8_t history_buf[HISTORY_SIZE];
static history_t history;

static void sink_to_history(void *ctx, const uint8_t *data, size_t len) {
    history_append(ctx, data, len);
}

int main(void) {
    history_init(&history, history_buf, sizeof history_buf);
    capture_init();
    capture_events_t ev = {0};
    for (;;)
        capture_poll(sink_to_history, &history, &ev);
}
