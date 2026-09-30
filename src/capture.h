#ifndef PICOLOG_CAPTURE_H
#define PICOLOG_CAPTURE_H

/*
 * Stage 1: UART0 RX -> DMA -> 32 KiB ring (no CPU involvement per byte).
 *
 * The main loop calls capture_poll() often; it hands whatever arrived since the
 * previous call to a sink (at most two spans because of the ring wrap) and
 * reports line-error events.
 */

#include <stddef.h>
#include <stdint.h>

/* The self-triggering DMA transfer count. TRANS_COUNT.COUNT is 28 bits wide; we
 * use 2^27 so differences can be taken modulo 2^27 (see capture_produced). */
#define CAPTURE_DMA_COUNT (1u << 27)
#define CAPTURE_COUNT_MASK (CAPTURE_DMA_COUNT - 1u)

#define CAPTURE_RING_BITS 15
#define CAPTURE_RING_SIZE (1u << CAPTURE_RING_BITS)
/* Headroom kept between the DMA write pointer and the bytes being copied. */
#define CAPTURE_SAFETY_MARGIN 1024u
#define CAPTURE_MAX_PENDING (CAPTURE_RING_SIZE - CAPTURE_SAFETY_MARGIN)

typedef struct {
    uint32_t brk;               /* BREAK conditions */
    uint32_t framing;           /* framing errors (not counting breaks) */
    uint32_t parity;            /* parity errors */
    uint32_t uart_overrun;      /* UART RX FIFO overruns */
    uint32_t capture_lost;      /* bytes skipped because stage 1 overflowed */
} capture_events_t;

typedef void (*capture_sink_fn)(void *ctx, const uint8_t *data, size_t len);

/* Bytes the DMA channel has transferred between two reads of its (down-counting,
 * self-reloading) transfer count register. Pure helper; both arguments are the
 * register value already masked to the COUNT field. Unambiguous as long as
 * fewer than 2^27 bytes arrive between two reads. */
static inline uint32_t capture_produced(uint32_t prev_remaining, uint32_t now_remaining) {
    return (prev_remaining - now_remaining) & CAPTURE_COUNT_MASK;
}

/* Configure UART0, the RX pin and the DMA channel, and start capturing. */
void capture_init(void);

/* Deliver newly captured bytes to `sink` and accumulate events into `*events`
 * (added to, not overwritten). Returns the number of bytes delivered. */
size_t capture_poll(capture_sink_fn sink, void *ctx, capture_events_t *events);

#endif
