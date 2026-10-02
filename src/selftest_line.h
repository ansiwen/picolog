#ifndef PICOLOG_SELFTEST_LINE_H
#define PICOLOG_SELFTEST_LINE_H

/*
 * One line of the built-in self-test source (see selftest_tx.c), hardware
 * independent so it can be unit tested on the host:
 *
 *     [     12.300] t=12300 ms\r\n
 *
 * The time since boot appears as seconds and as milliseconds, so gaps and
 * duplicates in a capture are easy to spot. Hand-rolled instead of snprintf to
 * keep printf out of the binary.
 */

#include <stddef.h>
#include <stdint.h>

/* Longest possible line: "[" + 17 + "." + 3 + "] t=" + 20 + " ms\r\n". */
#define SELFTEST_LINE_MAX 64

/* Formats the line for `ms` into buf (no terminating NUL) and returns its length. */
size_t selftest_format_line(char *buf, uint64_t ms);

#endif
