#include "selftest_tx.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "selftest_line.h"

/* PICOLOG_UART_* (format of the captured UART, reused so both sides always
 * match) and PICOLOG_SELFTEST_* come from CMake. */
#define TX_UART uart1

/* Core 1 has its own alarm pool, so the timer IRQ runs on core 1. Using
 * sleep_until() here would go through the default pool, whose IRQ lives on core 0
 * and would interrupt it on every tick. */
static alarm_pool_t *pool;
static repeating_timer_t timer;

static bool __not_in_flash_func(tick)(repeating_timer_t *t) {
    (void)t;
    uart_hw_t *hw = uart_get_hw(TX_UART);
    /* Skip a line instead of blocking or sending half of it: only start when the
     * TX FIFO has drained (a line fits into the 32-entry FIFO). Happens only if
     * the period is shorter than the time a line takes at this baud rate. */
    if (!(hw->fr & UART_UARTFR_TXFE_BITS))
        return true;

    char line[SELFTEST_LINE_MAX];
    size_t n = selftest_format_line(line, time_us_64() / 1000);
    for (size_t i = 0; i < n && uart_is_writable(TX_UART); i++)
        hw->dr = (uint8_t)line[i];
    return true;
}

static void core1_main(void) {
    uart_init(TX_UART, PICOLOG_UART_BAUD);
    uart_set_hw_flow(TX_UART, false, false);
    uart_set_format(TX_UART, PICOLOG_UART_DATA_BITS, PICOLOG_UART_STOP_BITS, PICOLOG_UART_PARITY);
    uart_set_fifo_enabled(TX_UART, true);
    gpio_set_function(PICOLOG_SELFTEST_TX_PIN, UART_FUNCSEL_NUM(TX_UART, PICOLOG_SELFTEST_TX_PIN));

    pool = alarm_pool_create_with_unused_hardware_alarm(1);
    /* Negative delay: period measured from the previous deadline, no drift. */
    alarm_pool_add_repeating_timer_ms(pool, -(int32_t)PICOLOG_SELFTEST_PERIOD_MS, tick, NULL,
                                      &timer);
    for (;;)
        __wfi();
}

void selftest_tx_start(void) {
    multicore_launch_core1(core1_main);
}
