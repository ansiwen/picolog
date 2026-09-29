// Stage 1: UART RX -> DMA -> 32 KiB hardware ring, plus UART line-error
// counting from the UART error interrupt.
#ifndef PICOLOG_CAPTURE_H
#define PICOLOG_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define CAPTURE_STAGE1_BITS 15u  // DMA ring wrap maxes out at 2^15 bytes
#define CAPTURE_STAGE1_SIZE (1u << CAPTURE_STAGE1_BITS)

// The DMA channel runs in TRIGGER_SELF mode with this reload count, so the
// live TRANS_COUNT keeps decrementing and wraps every CAPTURE_DMA_COUNT
// bytes. Bytes produced between two polls = (prev - now) mod COUNT, which is
// unambiguous as long as polls are less than COUNT bytes apart (2^27 bytes is
// >90 s even at 12 Mbaud; the 2 s watchdog guarantees far more frequent polls).
#define CAPTURE_DMA_COUNT_BITS 27u
#define CAPTURE_DMA_COUNT (1u << CAPTURE_DMA_COUNT_BITS)

// If more than (STAGE1_SIZE - SAFETY) bytes are pending, the oldest are
// skipped so the DMA cannot overwrite what we are copying while we copy it.
#define CAPTURE_SAFETY 1024u

static inline uint32_t capture_produced(uint32_t prev_remaining, uint32_t now_remaining) {
    return (prev_remaining - now_remaining) & (CAPTURE_DMA_COUNT - 1u);
}

typedef struct {
    uint32_t breaks;
    uint32_t framing_errors;
    uint32_t parity_errors;
    uint32_t uart_overruns;  // UART RX FIFO overrun (DMA did not keep up)
    uint64_t lost_bytes;     // stage-1 overrun (main loop did not keep up)
} capture_events_t;

typedef void (*capture_sink_fn)(const uint8_t *data, size_t len, void *ctx);

void capture_init(void);

// Hand all bytes received since the last call to `sink` (in up to two
// spans) and add any line errors / overruns seen since the last call to *ev.
void capture_poll(capture_sink_fn sink, void *ctx, capture_events_t *ev);

#endif
