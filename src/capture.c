#include "capture.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

/* PICOLOG_UART_* are set from CMake options. */
#ifndef PICOLOG_UART_BAUD
#define PICOLOG_UART_BAUD 115200
#endif
#ifndef PICOLOG_UART_RX_PIN
#define PICOLOG_UART_RX_PIN 1
#endif
#ifndef PICOLOG_UART_DATA_BITS
#define PICOLOG_UART_DATA_BITS 8
#endif
#ifndef PICOLOG_UART_STOP_BITS
#define PICOLOG_UART_STOP_BITS 1
#endif
#ifndef PICOLOG_UART_PARITY
#define PICOLOG_UART_PARITY UART_PARITY_NONE
#endif

#define UART_ID uart0
#define UART_IRQ UART0_IRQ
#define ERR_IRQ_BITS (UART_UARTIMSC_FEIM_BITS | UART_UARTIMSC_PEIM_BITS | \
                      UART_UARTIMSC_BEIM_BITS | UART_UARTIMSC_OEIM_BITS)

/* DMA ring target. Must be aligned to its own size for the write ring. */
static uint8_t stage1[CAPTURE_RING_SIZE] __attribute__((aligned(CAPTURE_RING_SIZE)));

static uint dma_chan;
static uint32_t prev_remaining; /* transfer count at the previous poll */
static uint32_t read_pos;       /* running read position in stage1 (mod 2^32) */

/* Counters written by the error IRQ, drained by the main loop. */
static uint32_t err_break, err_framing, err_parity, err_overrun;

static inline uint32_t dma_remaining(void) {
    return dma_channel_hw_addr(dma_chan)->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
}

/* DMA reads of UARTDR throw the per-character error bits away, so line errors
 * are taken from the UART's own error interrupts (RX/RT stay masked). */
static void uart_error_irq(void) {
    uart_hw_t *hw = uart_get_hw(UART_ID);
    uint32_t mis = hw->mis & ERR_IRQ_BITS;
    hw->icr = mis;
    if (mis & UART_UARTMIS_BEMIS_BITS)
        __atomic_fetch_add(&err_break, 1, __ATOMIC_RELAXED);
    else if (mis & UART_UARTMIS_FEMIS_BITS) /* a break also raises FE: don't count twice */
        __atomic_fetch_add(&err_framing, 1, __ATOMIC_RELAXED);
    if (mis & UART_UARTMIS_PEMIS_BITS)
        __atomic_fetch_add(&err_parity, 1, __ATOMIC_RELAXED);
    if (mis & UART_UARTMIS_OEMIS_BITS)
        __atomic_fetch_add(&err_overrun, 1, __ATOMIC_RELAXED);
}

void capture_init(void) {
    uart_init(UART_ID, PICOLOG_UART_BAUD);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, PICOLOG_UART_DATA_BITS, PICOLOG_UART_STOP_BITS, PICOLOG_UART_PARITY);
    uart_set_fifo_enabled(UART_ID, true);

    /* RX only: GP0/TX is never muxed to the UART, so it cannot drive the target.
     * Idle is high; the pull-up also avoids RP2350-E9 (a floating input is held
     * near 2.2 V by leakage and defeats a pull-down; the pull-up still works). */
    gpio_set_function(PICOLOG_UART_RX_PIN, UART_FUNCSEL_NUM(UART_ID, PICOLOG_UART_RX_PIN));
    gpio_pull_up(PICOLOG_UART_RX_PIN);

    /* RXDMAE only, DMAONERR explicitly clear: if set, the UART masks the RX DMA
     * request while an error interrupt is pending and capture would stall on the
     * first framing error. (uart_init() enables TXDMAE too; we never use it.) */
    uart_get_hw(UART_ID)->dmacr = UART_UARTDMACR_RXDMAE_BITS;

    /* Error interrupts only. */
    uart_get_hw(UART_ID)->icr = 0x7ff;
    uart_get_hw(UART_ID)->imsc = ERR_IRQ_BITS;
    irq_set_exclusive_handler(UART_IRQ, uart_error_irq);
    irq_set_enabled(UART_IRQ, true);

    /* One channel, TRIGGER_SELF: re-triggers itself forever, continues at its
     * current write address, and keeps a live down-counter. ENDLESS mode would
     * also run forever but its counter does not decrement, so we could not tell
     * how many bytes arrived. */
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, CAPTURE_RING_BITS);
    channel_config_set_dreq(&c, uart_get_dreq(UART_ID, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(dma_chan, &c, stage1, &uart_get_hw(UART_ID)->dr,
                          dma_encode_transfer_count_with_self_trigger(CAPTURE_DMA_COUNT),
                          false);
    prev_remaining = CAPTURE_DMA_COUNT & DMA_CH0_TRANS_COUNT_COUNT_BITS;
    read_pos = 0;
    dma_channel_start(dma_chan);
}

size_t capture_poll(capture_sink_fn sink, void *ctx, capture_events_t *events) {
    uint32_t now = dma_remaining();
    uint32_t pending = capture_produced(prev_remaining, now);
    prev_remaining = now;

    if (pending > CAPTURE_MAX_PENDING) {
        /* Never let the DMA catch up with the bytes we are about to copy. */
        uint32_t skip = pending - CAPTURE_MAX_PENDING;
        events->capture_lost += skip;
        read_pos += skip;
        pending = CAPTURE_MAX_PENDING;
    }

    size_t delivered = 0;
    __compiler_memory_barrier();
    while (pending > 0) {
        uint32_t idx = read_pos & (CAPTURE_RING_SIZE - 1);
        uint32_t n = CAPTURE_RING_SIZE - idx;
        if (n > pending)
            n = pending;
        sink(ctx, stage1 + idx, n);
        read_pos += n;
        pending -= n;
        delivered += n;
    }

    events->brk += __atomic_exchange_n(&err_break, 0, __ATOMIC_RELAXED);
    events->framing += __atomic_exchange_n(&err_framing, 0, __ATOMIC_RELAXED);
    events->parity += __atomic_exchange_n(&err_parity, 0, __ATOMIC_RELAXED);
    events->uart_overrun += __atomic_exchange_n(&err_overrun, 0, __ATOMIC_RELAXED);
    return delivered;
}
