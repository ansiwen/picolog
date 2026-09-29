// Stage-2 history ring: a power-of-two byte ring addressed by monotonic
// 64-bit offsets, with a persistence header that survives Pico resets.
//
// This module is hardware-independent so it can be unit-tested on the host
// (see test/unit). The firmware places the header and data buffer in
// __uninitialized_ram; the tests use ordinary arrays.
//
// Concurrency: there is exactly one producer (the capture path) and all
// consumers (the USB ports) run in the same core0 superloop. Nothing here is
// called from interrupt context, so no locking is needed. Do not call these
// functions from an IRQ handler or from core1 without adding locking.
#ifndef PICOLOG_HISTORY_H
#define PICOLOG_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HISTORY_MAGIC   0x474c4350u  // "PCLG" little-endian
#define HISTORY_VERSION 1u

// The header is committed after at most this many appended bytes. A reset
// in the middle of an append can therefore corrupt at most this many of the
// oldest bytes, which history_boot() excludes from the restored history.
#define HISTORY_COMMIT_CHUNK 512u

// One copy of the persistent header. Two copies are kept (A/B) and written
// alternately, so a reset while one copy is being written leaves the other
// intact. The newest valid copy (by seq) wins on boot.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;         // data buffer size in bytes
    uint32_t reset_count;  // Pico resets since the history was initialized
    uint32_t seq;          // incremented on every commit
    uint32_t reserved0;
    uint64_t head;         // total bytes ever written (monotonic)
    uint32_t crc;          // CRC32 over all preceding fields
    uint32_t reserved1;
} history_hdr_t;

typedef struct {
    history_hdr_t *hdr;  // two persistent header copies
    uint8_t *buf;
    uint32_t size;
    uint32_t mask;
    uint64_t head;
    // Offsets below floor are invalid even if they are within `size` of
    // head (possible corruption from an interrupted append before a reset).
    uint64_t floor;
    uint32_t reset_count;
    uint32_t seq;
    uint8_t active;  // index of the header copy holding the latest commit
} history_t;

typedef enum {
    HISTORY_BOOT_FRESH,     // no valid header: history initialized empty
    HISTORY_BOOT_RESTORED,  // valid header: history kept, appending continues
} history_boot_t;

// Attach to (or initialize) a history. `hdr` must point to two header
// copies; `size` must be a power of two and at least 2*HISTORY_COMMIT_CHUNK.
// On restore, reset_count is incremented; on fresh init it is 0.
history_boot_t history_boot(history_t *h, history_hdr_t hdr[2], uint8_t *buf, uint32_t size);

void history_append(history_t *h, const void *data, size_t len);

// printf-style append, for in-band markers. Output is truncated to 255 bytes.
void history_appendf(history_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline uint64_t history_head(const history_t *h) { return h->head; }

// Oldest offset that still holds valid data.
static inline uint64_t history_oldest(const history_t *h) {
    uint64_t oldest = h->head > h->size ? h->head - h->size : 0;
    return oldest > h->floor ? oldest : h->floor;
}

static inline uint32_t history_reset_count(const history_t *h) { return h->reset_count; }

// If *cursor is older than the oldest valid byte, move it to the oldest
// valid byte and return how many bytes were skipped; otherwise return 0.
uint64_t history_clamp(const history_t *h, uint64_t *cursor);

// Return the number of contiguous readable bytes at `cursor` (up to head or
// the physical end of the buffer, whichever is first) and point *ptr at
// them. `cursor` must be >= history_oldest() (call history_clamp first).
size_t history_peek(const history_t *h, uint64_t cursor, const uint8_t **ptr);

// Standard CRC-32 (IEEE 802.3, reflected, as used by zlib).
uint32_t history_crc32(const void *data, size_t len);

#endif
