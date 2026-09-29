#include "capture.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

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

#define CAPTURE_UART uart0

#define ERR_BITS (UART_UARTIMSC_OEIM_BITS | UART_UARTIMSC_BEIM_BITS | UART_UARTIMSC_PEIM_BITS | UART_UARTIMSC_FEIM_BITS)

static uint8_t stage1[CAPTURE_STAGE1_SIZE] __attribute__((aligned(CAPTURE_STAGE1_SIZE)));
static uint dma_ch;
static uint32_t last_remaining;
static uint32_t rd;  // next unread index in stage1

// Written by the UART error IRQ, read-and-cleared by the main loop.
static volatile uint32_t irq_breaks, irq_framing, irq_parity, irq_overrun;

static void uart_error_irq(void) {
    uart_hw_t *hw = uart_get_hw(CAPTURE_UART);
    uint32_t mis = hw->mis & ERR_BITS;
    hw->icr = mis;
    // A break is also received as a framing error on a 0x00 character;
    // report it only as a break.
    if (mis & UART_UARTMIS_BEMIS_BITS) {
        irq_breaks++;
    } else if (mis & UART_UARTMIS_FEMIS_BITS) {
        irq_framing++;
    }
    if (mis & UART_UARTMIS_PEMIS_BITS) {
        irq_parity++;
    }
    if (mis & UART_UARTMIS_OEMIS_BITS) {
        irq_overrun++;
    }
}

void capture_init(void) {
    uart_init(CAPTURE_UART, PICOLOG_UART_BAUD);
    uart_set_format(CAPTURE_UART, PICOLOG_UART_DATA_BITS, PICOLOG_UART_STOP_BITS, PICOLOG_UART_PARITY);
    uart_set_hw_flow(CAPTURE_UART, false, false);
    uart_set_fifo_enabled(CAPTURE_UART, true);
    // uart_init enables the RX DREQ. Make sure DMAONERR is clear: if set,
    // the RX DMA request is masked while an error interrupt is pending.
    hw_clear_bits(&uart_get_hw(CAPTURE_UART)->dmacr, UART_UARTDMACR_DMAONERR_BITS);
    hw_set_bits(&uart_get_hw(CAPTURE_UART)->dmacr, UART_UARTDMACR_RXDMAE_BITS);

    // RX only; GP0 (TX) is left unconfigured so we never drive the target.
    // Pull-up keeps the idle-high line defined when the target is unplugged.
    // This also avoids RP2350-E9 (A2 stepping): the erratum's leakage only
    // defeats the pull-*down*; with the pull-up enabled (and the pull-down
    // disabled by gpio_pull_up) the pad is pulled out of the affected range.
    gpio_set_function(PICOLOG_UART_RX_PIN, UART_FUNCSEL_NUM(CAPTURE_UART, PICOLOG_UART_RX_PIN));
    gpio_pull_up(PICOLOG_UART_RX_PIN);

    dma_ch = (uint)dma_claim_unused_channel(true);
    dma_channel_config_t c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, CAPTURE_STAGE1_BITS);
    channel_config_set_dreq(&c, uart_get_dreq_num(CAPTURE_UART, false));
    channel_config_set_high_priority(&c, true);
    // RP2350 TRANS_COUNT has an ENDLESS mode, but in that mode the counter
    // does not decrement, so we could not tell how many bytes arrived (and
    // thus could not detect stage-1 overruns). TRIGGER_SELF also runs
    // forever (the channel re-triggers itself when the count reaches 0,
    // continuing from its current write address) and keeps a live count.
    // No DMA interrupt is enabled for this channel.
    last_remaining = CAPTURE_DMA_COUNT;
    rd = 0;
    dma_channel_configure(dma_ch, &c, stage1, &uart_get_hw(CAPTURE_UART)->dr,
                          dma_encode_transfer_count_with_self_trigger(CAPTURE_DMA_COUNT), true);

    // Line errors: DMA 8-bit reads of UARTDR discard the error bits, so
    // count them via the error interrupts instead. RX/RT interrupts stay
    // masked (the DMA handles data).
    uart_get_hw(CAPTURE_UART)->icr = ERR_BITS;
    irq_set_exclusive_handler(UART_IRQ_NUM(CAPTURE_UART), uart_error_irq);
    uart_get_hw(CAPTURE_UART)->imsc = ERR_BITS;
    irq_set_enabled(UART_IRQ_NUM(CAPTURE_UART), true);
}

static uint32_t take(volatile uint32_t *counter) {
    return __atomic_exchange_n(counter, 0u, __ATOMIC_RELAXED);
}

void capture_poll(capture_sink_fn sink, void *ctx, capture_events_t *ev) {
    uint32_t now = dma_hw->ch[dma_ch].transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
    uint32_t produced = capture_produced(last_remaining, now);
    last_remaining = now;

    if (produced > CAPTURE_STAGE1_SIZE - CAPTURE_SAFETY) {
        uint32_t skip = produced - (CAPTURE_STAGE1_SIZE - CAPTURE_SAFETY);
        rd = (rd + skip) & (CAPTURE_STAGE1_SIZE - 1u);
        produced -= skip;
        ev->lost_bytes += skip;
    }

    while (produced) {
        uint32_t n = CAPTURE_STAGE1_SIZE - rd;
        if (n > produced) {
            n = produced;
        }
        sink(stage1 + rd, n, ctx);
        rd = (rd + n) & (CAPTURE_STAGE1_SIZE - 1u);
        produced -= n;
    }

    ev->breaks += take(&irq_breaks);
    ev->framing_errors += take(&irq_framing);
    ev->parity_errors += take(&irq_parity);
    ev->uart_overruns += take(&irq_overrun);
}
