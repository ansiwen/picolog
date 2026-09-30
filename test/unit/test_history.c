#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                           \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                     \
    } while (0)

#define SZ 16

static uint8_t storage[SZ];
static history_t h;

/* Read the whole valid range [from, head) via history_peek. */
static size_t read_all(uint64_t from, uint8_t *out, size_t cap) {
    size_t total = 0;
    uint64_t c = from;
    const uint8_t *p;
    size_t n;
    while ((n = history_peek(&h, c, &p)) > 0) {
        if (total + n > cap)
            return (size_t)-1;
        memcpy(out + total, p, n);
        total += n;
        c += n;
    }
    return total;
}

static void test_init(void) {
    history_init(&h, storage, SZ);
    CHECK(history_head(&h) == 0);
    CHECK(history_oldest(&h) == 0);
    const uint8_t *p;
    CHECK(history_peek(&h, 0, &p) == 0);
    uint64_t c = 0;
    CHECK(history_clamp(&h, &c) == 0 && c == 0);
}

static void test_simple(void) {
    history_init(&h, storage, SZ);
    history_append(&h, "hello", 5);
    CHECK(history_head(&h) == 5);
    CHECK(history_oldest(&h) == 0);
    uint8_t out[SZ];
    CHECK(read_all(0, out, sizeof out) == 5 && memcmp(out, "hello", 5) == 0);
    CHECK(read_all(2, out, sizeof out) == 3 && memcmp(out, "llo", 3) == 0);
    history_append(&h, "", 0);
    CHECK(history_head(&h) == 5);
    const uint8_t *p;
    CHECK(history_peek(&h, 5, &p) == 0);
    CHECK(history_peek(&h, 6, &p) == 0);
}

static void test_wraparound(void) {
    history_init(&h, storage, SZ);
    history_append(&h, "0123456789", 10);
    history_append(&h, "abcdefghij", 10); /* head 20, oldest 4 */
    CHECK(history_head(&h) == 20);
    CHECK(history_oldest(&h) == 4);
    uint8_t out[SZ];
    CHECK(read_all(4, out, sizeof out) == 16);
    CHECK(memcmp(out, "456789abcdefghij", 16) == 0);
    /* peek must split at the physical end of the array */
    const uint8_t *p;
    size_t n = history_peek(&h, 4, &p);
    CHECK(n == 12 && p == storage + 4);
    n = history_peek(&h, 16, &p);
    CHECK(n == 4 && p == storage);
    /* stale cursor */
    CHECK(history_peek(&h, 3, &p) == 0);
    uint64_t c = 1;
    CHECK(history_clamp(&h, &c) == 3 && c == 4);
    CHECK(history_clamp(&h, &c) == 0 && c == 4);
    c = 4;
    CHECK(history_clamp(&h, &c) == 0);
}

static void test_exact_fill(void) {
    history_init(&h, storage, SZ);
    history_append(&h, "0123456789abcdef", 16);
    CHECK(history_oldest(&h) == 0);
    uint8_t out[SZ];
    CHECK(read_all(0, out, sizeof out) == 16 && memcmp(out, "0123456789abcdef", 16) == 0);
    history_append(&h, "X", 1);
    CHECK(history_oldest(&h) == 1);
    CHECK(read_all(1, out, sizeof out) == 16 && memcmp(out, "123456789abcdefX", 16) == 0);
}

static void test_larger_than_ring(void) {
    history_init(&h, storage, SZ);
    history_append(&h, "AB", 2);
    const char big[] = "0123456789abcdefghijklmnopqrstuvwxyz"; /* 36 */
    history_append(&h, big, 36);
    CHECK(history_head(&h) == 38);
    CHECK(history_oldest(&h) == 22);
    uint8_t out[SZ];
    CHECK(read_all(22, out, sizeof out) == 16);
    CHECK(memcmp(out, big + 20, 16) == 0);
    /* Different physical alignment for the same situation */
    history_append(&h, "Z", 1);
    history_append(&h, big, 36);
    CHECK(history_head(&h) == 75);
    CHECK(read_all(history_oldest(&h), out, sizeof out) == 16);
    CHECK(memcmp(out, big + 20, 16) == 0);
}

static void test_dropped_detection(void) {
    history_init(&h, storage, SZ);
    uint64_t cursor = 0;
    history_append(&h, "0123456789", 10);
    CHECK(history_clamp(&h, &cursor) == 0);
    for (int i = 0; i < 3; i++)
        history_append(&h, "0123456789", 10); /* head 40 */
    CHECK(history_oldest(&h) == 24);
    CHECK(history_clamp(&h, &cursor) == 24 && cursor == 24);
}

static void test_big_offsets(void) {
    uint8_t out[SZ];
    const uint64_t bases[] = {0xFFFFFFF8ull, 1ull << 32, (1ull << 40) + 5};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        history_init(&h, storage, SZ);
        h.head = bases[i];
        history_append(&h, "0123456789", 10);
        history_append(&h, "abcdefghij", 10);
        CHECK(history_head(&h) == bases[i] + 20);
        CHECK(history_oldest(&h) == bases[i] + 4);
        CHECK(read_all(history_oldest(&h), out, sizeof out) == 16);
        CHECK(memcmp(out, "456789abcdefghij", 16) == 0);
        uint64_t c = bases[i];
        CHECK(history_clamp(&h, &c) == 4 && c == bases[i] + 4);
    }
}

static void test_appendf(void) {
    static uint8_t big[1024];
    history_init(&h, big, sizeof big);
    history_appendf(&h, "\r\n[picolog: %d bytes dropped]\r\n", 42);
    const char *want = "\r\n[picolog: 42 bytes dropped]\r\n";
    CHECK(history_head(&h) == strlen(want));
    CHECK(memcmp(big, want, strlen(want)) == 0);

    /* truncation to 255 bytes */
    history_init(&h, big, sizeof big);
    char longs[400];
    memset(longs, 'x', sizeof longs - 1);
    longs[sizeof longs - 1] = 0;
    history_appendf(&h, "%s", longs);
    CHECK(history_head(&h) == 255);
    CHECK(big[0] == 'x' && big[254] == 'x');

    /* exactly 255 fits untouched, 256 is cut */
    history_init(&h, big, sizeof big);
    history_appendf(&h, "%.255s", longs);
    CHECK(history_head(&h) == 255);
    history_init(&h, big, sizeof big);
    history_appendf(&h, "%.256s", longs);
    CHECK(history_head(&h) == 255);

    /* empty output appends nothing */
    history_init(&h, big, sizeof big);
    history_appendf(&h, "%s", "");
    CHECK(history_head(&h) == 0);
}

int main(void) {
    test_init();
    test_simple();
    test_wraparound();
    test_exact_fill();
    test_larger_than_ring();
    test_dropped_detection();
    test_big_offsets();
    test_appendf();
    printf("test_history: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
