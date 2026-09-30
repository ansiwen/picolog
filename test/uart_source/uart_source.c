/*
 * UART test source: sends one line with the time since boot every
 * SOURCE_PERIOD_MS on UART0 TX, e.g.
 *
 *   [    12.300]
 *   [    12.400]
 *
 * Wire GP0 (pin 1) to the picolog Pico's GP1 (pin 2) and connect the grounds.
 * Both boards are 3.3 V, so no level shifting is needed.
 */

#include <stdint.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

/* SOURCE_UART_* are set from CMake options. */
#ifndef SOURCE_UART_BAUD
#define SOURCE_UART_BAUD 115200
#endif
#ifndef SOURCE_UART_TX_PIN
#define SOURCE_UART_TX_PIN 0
#endif
#ifndef SOURCE_UART_DATA_BITS
#define SOURCE_UART_DATA_BITS 8
#endif
#ifndef SOURCE_UART_STOP_BITS
#define SOURCE_UART_STOP_BITS 1
#endif
#ifndef SOURCE_UART_PARITY
#define SOURCE_UART_PARITY UART_PARITY_NONE
#endif
#ifndef SOURCE_PERIOD_MS
#define SOURCE_PERIOD_MS 100
#endif

#define UART_ID uart0

int main(void) {
    uart_init(UART_ID, SOURCE_UART_BAUD);
    uart_set_format(UART_ID, SOURCE_UART_DATA_BITS, SOURCE_UART_STOP_BITS, SOURCE_UART_PARITY);
    gpio_set_function(SOURCE_UART_TX_PIN, UART_FUNCSEL_NUM(UART_ID, SOURCE_UART_TX_PIN));

    /* Schedule against absolute deadlines so the period does not drift by the
     * time spent formatting and sending. */
    absolute_time_t next = make_timeout_time_ms(SOURCE_PERIOD_MS);

    for (;;) {
        sleep_until(next);
        /* Advance from the previous deadline, not from now. */
        next = delayed_by_ms(next, SOURCE_PERIOD_MS);

        uint64_t ms = to_us_since_boot(get_absolute_time()) / 1000;
        char line[32];
        int n = snprintf(line, sizeof line, "[%7lu.%03u]\r\n",
                         (unsigned long)(ms / 1000), (unsigned)(ms % 1000));
        if (n > 0 && (size_t)n < sizeof line) {
            /* Blocks while the 32-byte TX FIFO is full; at low baud rates this
             * can exceed the period, in which case lines simply go out late. */
            uart_write_blocking(UART_ID, (const uint8_t *)line, (size_t)n);
        }
    }
}
