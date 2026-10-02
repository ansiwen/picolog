#ifndef PICOLOG_SELFTEST_TX_H
#define PICOLOG_SELFTEST_TX_H

/*
 * Optional built-in UART test source (CMake option PICOLOG_SELFTEST_TX, off by
 * default): core 1 sends a timestamp line on UART1 TX every
 * PICOLOG_SELFTEST_PERIOD_MS. Jumper that pin to the capture RX pin and picolog
 * records its own test data.
 *
 * With the option off this is an empty inline function: no code, no data.
 */

#ifdef PICOLOG_SELFTEST_TX
void selftest_tx_start(void);
#else
static inline void selftest_tx_start(void) {}
#endif

#endif
