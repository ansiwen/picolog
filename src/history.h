// Stage-2 history ring: a power-of-two byte ring addressed by monotonic
// 64-bit offsets.
//
// This module is hardware-independent so it can be unit-tested on the host
// (see test/unit).
//
// Concurrency: there is exactly one producer (the capture path) and all
// consumers (the USB ports) run in the same core0 superloop. Nothing here is
// called from interrupt context, so no locking is needed. Do not call these
// functions from an IRQ handler or from core1 without adding locking.
#ifndef PICOLOG_HISTORY_H
#define PICOLOG_HISTORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t size;
    uint32_t mask;
    uint64_t head;  // total bytes ever written (monotonic)
} history_t;

// Initialize an empty history. `size` must be a power of two.
void history_init(history_t *h, uint8_t *buf, uint32_t size);

void history_append(history_t *h, const void *data, size_t len);

// printf-style append, for in-band markers. Output is truncated to 255 bytes.
void history_appendf(history_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline uint64_t history_head(const history_t *h) { return h->head; }

// Oldest offset that still holds valid data.
static inline uint64_t history_oldest(const history_t *h) { return h->head > h->size ? h->head - h->size : 0; }

// If *cursor is older than the oldest valid byte, move it to the oldest
// valid byte and return how many bytes were skipped; otherwise return 0.
uint64_t history_clamp(const history_t *h, uint64_t *cursor);

// Return the number of contiguous readable bytes at `cursor` (up to head or
// the physical end of the buffer, whichever is first) and point *ptr at
// them. `cursor` must be >= history_oldest() (call history_clamp first).
size_t history_peek(const history_t *h, uint64_t cursor, const uint8_t **ptr);

#endif
