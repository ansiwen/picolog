#include "history.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void history_init(history_t *h, uint8_t *buf, uint32_t size) {
    h->buf = buf;
    h->size = size;
    h->mask = size - 1;
    h->head = 0;
}

void history_append(history_t *h, const void *data, size_t len) {
    const uint8_t *src = data;
    if (len > h->size) {
        // Only the last `size` bytes can be kept.
        h->head += len - h->size;
        src += len - h->size;
        len = h->size;
    }
    uint32_t pos = (uint32_t)h->head & h->mask;
    size_t first = h->size - pos;
    if (first > len) {
        first = len;
    }
    memcpy(h->buf + pos, src, first);
    memcpy(h->buf, src + first, len - first);
    h->head += len;
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
