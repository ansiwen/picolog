#include "history.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

uint32_t history_crc32(const void *data, size_t len) {
    // Nibble-table CRC-32: small table, ~4x faster than bit-by-bit.
    static const uint32_t tbl[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tbl[crc & 0xf];
        crc = (crc >> 4) ^ tbl[crc & 0xf];
    }
    return ~crc;
}

static uint32_t hdr_crc(const history_hdr_t *hd) {
    return history_crc32(hd, offsetof(history_hdr_t, crc));
}

static bool hdr_valid(const history_hdr_t *hd, uint32_t size) {
    return hd->magic == HISTORY_MAGIC && hd->version == HISTORY_VERSION && hd->size == size &&
           hd->crc == hdr_crc(hd);
}

static void commit(history_t *h) {
    // Make sure the data bytes are stored before the header that covers them.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint8_t next = h->active ^ 1u;
    history_hdr_t *hd = &h->hdr[next];
    h->seq++;
    hd->magic = HISTORY_MAGIC;
    hd->version = HISTORY_VERSION;
    hd->size = h->size;
    hd->reset_count = h->reset_count;
    hd->seq = h->seq;
    hd->reserved0 = 0;
    hd->head = h->head;
    hd->reserved1 = 0;
    hd->crc = hdr_crc(hd);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    h->active = next;
}

history_boot_t history_boot(history_t *h, history_hdr_t hdr[2], uint8_t *buf, uint32_t size) {
    h->hdr = hdr;
    h->buf = buf;
    h->size = size;
    h->mask = size - 1;

    bool va = hdr_valid(&hdr[0], size);
    bool vb = hdr_valid(&hdr[1], size);
    int pick = -1;
    if (va && vb) {
        // Serial-number comparison so seq wraparound is handled.
        pick = (int32_t)(hdr[1].seq - hdr[0].seq) > 0 ? 1 : 0;
    } else if (va) {
        pick = 0;
    } else if (vb) {
        pick = 1;
    }

    if (pick < 0) {
        h->head = 0;
        h->floor = 0;
        h->reset_count = 0;
        h->seq = 0;
        h->active = 1;  // first commit goes to copy 0
        memset(hdr, 0, 2 * sizeof(*hdr));
        commit(h);
        return HISTORY_BOOT_FRESH;
    }

    const history_hdr_t *hd = &hdr[pick];
    h->head = hd->head;
    h->seq = hd->seq;
    h->active = (uint8_t)pick;
    h->reset_count = hd->reset_count + 1;
    // An append that was interrupted by the reset may have overwritten up to
    // HISTORY_COMMIT_CHUNK bytes past the committed head, i.e. the oldest
    // bytes of the ring. Exclude them.
    h->floor = h->head + HISTORY_COMMIT_CHUNK > size ? h->head + HISTORY_COMMIT_CHUNK - size : 0;
    commit(h);
    return HISTORY_BOOT_RESTORED;
}

void history_append(history_t *h, const void *data, size_t len) {
    const uint8_t *src = data;
    while (len) {
        size_t n = len < HISTORY_COMMIT_CHUNK ? len : HISTORY_COMMIT_CHUNK;
        uint32_t pos = (uint32_t)h->head & h->mask;
        size_t first = h->size - pos;
        if (first > n) {
            first = n;
        }
        memcpy(h->buf + pos, src, first);
        memcpy(h->buf, src + first, n - first);
        h->head += n;
        src += n;
        len -= n;
        commit(h);
    }
}

void history_appendf(history_t *h, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    history_append(h, tmp, (size_t)n);
}

uint64_t history_clamp(const history_t *h, uint64_t *cursor) {
    uint64_t oldest = history_oldest(h);
    if (*cursor >= oldest) {
        return 0;
    }
    uint64_t dropped = oldest - *cursor;
    *cursor = oldest;
    return dropped;
}

size_t history_peek(const history_t *h, uint64_t cursor, const uint8_t **ptr) {
    if (cursor >= h->head || cursor < history_oldest(h)) {
        *ptr = NULL;
        return 0;
    }
    uint32_t pos = (uint32_t)cursor & h->mask;
    uint64_t avail = h->head - cursor;
    uint32_t to_end = h->size - pos;
    *ptr = h->buf + pos;
    return avail < to_end ? (size_t)avail : to_end;
}
