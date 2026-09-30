#ifndef PICOLOG_HISTORY_H
#define PICOLOG_HISTORY_H

/*
 * Stage-2 history ring: the flight-recorder buffer.
 *
 * Hardware independent. The ring size must be a power of two so indices can be
 * masked. Positions are absolute, monotonically increasing 64-bit offsets: the
 * byte at offset `o` lives at buf[o & (size - 1)] while `o >= history_oldest()`.
 *
 * Concurrency: there is exactly one producer and all consumers run in the same
 * single-threaded (core0 superloop) context, so no locking is used. Do not call
 * these functions from an interrupt handler or from another core.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t size;   /* power of two */
    uint64_t head; /* total bytes ever appended == offset of the next byte */
} history_t;

/* `size` must be a power of two. The ring starts empty. */
void history_init(history_t *h, uint8_t *buf, size_t size);

/* Append bytes. If len > size only the last `size` bytes are kept (head still
 * advances by the full len). */
void history_append(history_t *h, const void *data, size_t len);

/* printf-style append, output truncated to 255 bytes. Used for markers. */
void history_appendf(history_t *h, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Offset one past the newest byte. */
uint64_t history_head(const history_t *h);

/* Offset of the oldest byte still valid: max(0, head - size). */
uint64_t history_oldest(const history_t *h);

/* If *cursor points at overwritten data, move it to the oldest valid byte and
 * return the number of bytes skipped, otherwise return 0. */
uint64_t history_clamp(const history_t *h, uint64_t *cursor);

/* Contiguous readable bytes starting at `cursor`, up to head or the end of the
 * backing array. Returns 0 if there is nothing to read or the cursor is stale
 * (older than the oldest valid byte). *ptr is set only when the result is > 0. */
size_t history_peek(const history_t *h, uint64_t cursor, const uint8_t **ptr);

#endif
