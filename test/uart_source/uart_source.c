/* Test signal source for picolog: prints the time since boot on UART0 TX
 * every UART_SOURCE_PERIOD_MS, 8N1, 3.3 V logic. One line looks like
 *
 *     [     12.300] t=12300 ms
 *
 * The timestamp is in milliseconds, so gaps or duplicates in the capture are easy to spot. */

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

/* UART_SOURCE_* are set from CMake options. */
#ifndef UART_SOURCE_BAUD
#define UART_SOURCE_BAUD 115200
#endif
#ifndef UART_SOURCE_TX_PIN
#define UART_SOURCE_TX_PIN 0
#endif
#ifndef UART_SOURCE_PERIOD_MS
#define UART_SOURCE_PERIOD_MS 100
#endif

#define UART_ID uart0

int main(void) {
    uart_init(UART_ID, UART_SOURCE_BAUD);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UART_ID, false, false);
    gpio_set_function(UART_SOURCE_TX_PIN, UART_FUNCSEL_NUM(UART_ID, UART_SOURCE_TX_PIN));

    /* Absolute deadlines: the period does not drift by the time a line takes to send. */
    absolute_time_t next = get_absolute_time();
    for (;;) {
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        char line[48];
        int n = snprintf(line, sizeof line, "[%6lu.%03lu] t=%lu ms\r\n",
                         (unsigned long)(ms / 1000), (unsigned long)(ms % 1000),
                         (unsigned long)ms);
        uart_write_blocking(UART_ID, (const uint8_t *)line, (size_t)n);

        next = delayed_by_ms(next, UART_SOURCE_PERIOD_MS);
        sleep_until(next);
    }
}
