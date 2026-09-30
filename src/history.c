#include "history.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APPENDF_MAX 255

void history_init(history_t *h, uint8_t *buf, size_t size) {
    assert(size != 0 && (size & (size - 1)) == 0);
    h->buf = buf;
    h->size = size;
    h->head = 0;
}

void history_append(history_t *h, const void *data, size_t len) {
    const uint8_t *src = data;
    if (len == 0)
        return;
    if (len > h->size) {
        /* Only the newest `size` bytes survive; account for the skipped ones. */
        size_t skip = len - h->size;
        src += skip;
        h->head += skip;
        len = h->size;
    }
    size_t idx = (size_t)(h->head & (h->size - 1));
    size_t first = h->size - idx;
    if (first > len)
        first = len;
    memcpy(h->buf + idx, src, first);
    if (len > first)
        memcpy(h->buf, src + first, len - first);
    h->head += len;
}

void history_appendf(history_t *h, const char *fmt, ...) {
    char tmp[APPENDF_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > APPENDF_MAX)
        n = APPENDF_MAX;
    history_append(h, tmp, (size_t)n);
}

uint64_t history_head(const history_t *h) {
    return h->head;
}

uint64_t history_oldest(const history_t *h) {
    return h->head > h->size ? h->head - h->size : 0;
}

uint64_t history_clamp(const history_t *h, uint64_t *cursor) {
    uint64_t oldest = history_oldest(h);
    if (*cursor >= oldest)
        return 0;
    uint64_t dropped = oldest - *cursor;
    *cursor = oldest;
    return dropped;
}

size_t history_peek(const history_t *h, uint64_t cursor, const uint8_t **ptr) {
    if (cursor < history_oldest(h) || cursor >= h->head)
        return 0;
    size_t idx = (size_t)(cursor & (h->size - 1));
    uint64_t avail = h->head - cursor;
    size_t contiguous = h->size - idx;
    size_t n = avail < contiguous ? (size_t)avail : contiguous;
    *ptr = h->buf + idx;
    return n;
}
