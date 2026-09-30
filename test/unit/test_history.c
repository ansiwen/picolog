// Host-side unit tests for src/history.c.
#include "history.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define SIZE 1024u

static uint8_t buf[SIZE];

static uint8_t pattern(uint64_t off) { return (uint8_t)(off * 7u + (off >> 8) + 3u); }

static void append_pattern(history_t *h, uint64_t count) {
    uint8_t tmp[333];  // odd size so chunks straddle the wrap point
    while (count) {
        size_t n = count < sizeof(tmp) ? (size_t)count : sizeof(tmp);
        for (size_t i = 0; i < n; i++) {
            tmp[i] = pattern(history_head(h) + i);
        }
        history_append(h, tmp, n);
        count -= n;
    }
}

// Read [from, head) through history_peek and compare against pattern().
static bool verify_pattern(const history_t *h, uint64_t from) {
    uint64_t cur = from;
    while (cur < history_head(h)) {
        const uint8_t *p;
        size_t n = history_peek(h, cur, &p);
        if (n == 0) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            if (p[i] != pattern(cur + i)) {
                fprintf(stderr, "mismatch at offset %llu\n", (unsigned long long)(cur + i));
                return false;
            }
        }
        cur += n;
    }
    return cur == history_head(h);
}

static void fresh(history_t *h) {
    memset(buf, 0x5a, sizeof(buf));
    history_init(h, buf, SIZE);
}

static void test_init(void) {
    history_t h;
    fresh(&h);
    CHECK(history_head(&h) == 0);
    CHECK(history_oldest(&h) == 0);
    const uint8_t *p;
    CHECK(history_peek(&h, 0, &p) == 0);
}

static void test_simple_append(void) {
    history_t h;
    fresh(&h);
    history_append(&h, "hello", 5);
    CHECK(history_head(&h) == 5);
    const uint8_t *p;
    size_t n = history_peek(&h, 0, &p);
    CHECK(n == 5 && memcmp(p, "hello", 5) == 0);
    n = history_peek(&h, 2, &p);
    CHECK(n == 3 && memcmp(p, "llo", 3) == 0);
    CHECK(history_peek(&h, 5, &p) == 0);
}

static void test_wraparound(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 3000);
    CHECK(history_head(&h) == 3000);
    CHECK(history_oldest(&h) == 3000 - SIZE);
    CHECK(verify_pattern(&h, history_oldest(&h)));

    // Peek stops at the physical end of the buffer.
    const uint8_t *p;
    uint64_t oldest = history_oldest(&h);
    size_t n = history_peek(&h, oldest, &p);
    CHECK(n == SIZE - (oldest % SIZE));
    CHECK(p == buf + (oldest % SIZE));
}

static void test_append_larger_than_ring(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 10);
    uint8_t big[5000];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = pattern(10 + i);
    }
    history_append(&h, big, sizeof(big));
    CHECK(history_head(&h) == 5010);
    CHECK(verify_pattern(&h, history_oldest(&h)));
}

static void test_dropped_detection(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 100);
    uint64_t cur = 40;
    CHECK(history_clamp(&h, &cur) == 0 && cur == 40);

    append_pattern(&h, 2000);  // head = 2100, oldest = 1076
    CHECK(history_clamp(&h, &cur) == 1076 - 40);
    CHECK(cur == 1076);
    CHECK(history_clamp(&h, &cur) == 0);

    // Peek refuses stale cursors instead of returning overwritten data.
    const uint8_t *p;
    CHECK(history_peek(&h, 1075, &p) == 0);
}

static void test_64bit_offsets(void) {
    // Pretend the recorder has been running for a long time: start just
    // below 2^32 so appends cross the 32-bit boundary.
    history_t h;
    fresh(&h);
    uint64_t start = (1ull << 32) - 700;
    h.head = start;
    append_pattern(&h, 3 * SIZE);
    CHECK(history_head(&h) == start + 3 * SIZE);
    CHECK(history_head(&h) > (1ull << 32));
    CHECK(history_oldest(&h) == history_head(&h) - SIZE);
    CHECK(verify_pattern(&h, history_oldest(&h)));

    uint64_t cur = start;
    CHECK(history_clamp(&h, &cur) == history_oldest(&h) - start);

    // And far beyond: 2^40.
    h.head = 1ull << 40;
    append_pattern(&h, 2 * SIZE);
    CHECK(verify_pattern(&h, history_oldest(&h)));
}

static void test_appendf(void) {
    history_t h;
    fresh(&h);
    history_appendf(&h, "[picolog: %llu bytes dropped]", (unsigned long long)12345678901ull);
    const char *want = "[picolog: 12345678901 bytes dropped]";
    const uint8_t *p;
    size_t n = history_peek(&h, 0, &p);
    CHECK(n == strlen(want) && memcmp(p, want, n) == 0);

    // Long output is truncated, not overflowed.
    char longstr[600];
    memset(longstr, 'a', sizeof(longstr) - 1);
    longstr[sizeof(longstr) - 1] = 0;
    uint64_t before = history_head(&h);
    history_appendf(&h, "%s", longstr);
    CHECK(history_head(&h) - before == 255);
}

int main(void) {
    test_init();
    test_simple_append();
    test_wraparound();
    test_append_larger_than_ring();
    test_dropped_detection();
    test_64bit_offsets();
    test_appendf();
    if (failures) {
        fprintf(stderr, "test_history: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_history: all tests passed\n");
    return 0;
}
