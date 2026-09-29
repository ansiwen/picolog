// Host-side unit tests for src/history.c.
#include "history.h"

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

#define SIZE 1024u  // smallest allowed: 2 * HISTORY_COMMIT_CHUNK

static history_hdr_t hdr[2];
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
    memset(hdr, 0xa5, sizeof(hdr));  // garbage, like SRAM after power-on
    memset(buf, 0x5a, sizeof(buf));
    CHECK(history_boot(h, hdr, buf, SIZE) == HISTORY_BOOT_FRESH);
}

static void test_crc(void) {
    CHECK(history_crc32("123456789", 9) == 0xcbf43926u);
    CHECK(history_crc32("", 0) == 0);
}

static void test_fresh_boot(void) {
    history_t h;
    fresh(&h);
    CHECK(history_head(&h) == 0);
    CHECK(history_oldest(&h) == 0);
    CHECK(history_reset_count(&h) == 0);
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

static void test_restore(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 300);

    history_t h2;  // simulate a reset: new RAM state object, same buffers
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h2) == 300);
    CHECK(history_reset_count(&h2) == 1);
    // head + chunk <= size: nothing could have been overwritten.
    CHECK(history_oldest(&h2) == 0);
    CHECK(verify_pattern(&h2, 0));

    history_t h3;
    CHECK(history_boot(&h3, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_reset_count(&h3) == 2);
    append_pattern(&h3, 50);
    CHECK(verify_pattern(&h3, 0));
}

static void test_restore_floor(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 5000);

    history_t h2;
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h2) == 5000);
    // The oldest HISTORY_COMMIT_CHUNK bytes are excluded after a restore.
    CHECK(history_oldest(&h2) == 5000 - SIZE + HISTORY_COMMIT_CHUNK);
    CHECK(verify_pattern(&h2, history_oldest(&h2)));

    // As head advances, the floor stops mattering.
    append_pattern(&h2, SIZE);
    CHECK(history_oldest(&h2) == history_head(&h2) - SIZE);
    CHECK(verify_pattern(&h2, history_oldest(&h2)));
}

static void test_interrupted_append(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 1500);
    // Simulate a reset after data bytes were written but before the header
    // commit: scribble over the next chunk's worth of ring bytes.
    uint64_t head = history_head(&h);
    for (uint32_t i = 0; i < HISTORY_COMMIT_CHUNK; i++) {
        buf[(head + i) % SIZE] = 0xee;
    }
    history_t h2;
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h2) == head);
    CHECK(verify_pattern(&h2, history_oldest(&h2)));
}

static void test_corrupt_one_header(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 100);
    uint64_t prev_head = history_head(&h);
    history_append(&h, "x", 1);  // this commit lands in the other copy

    // Corrupt the latest copy (as if reset hit mid-commit).
    h.hdr[h.active].head ^= 0x10;
    history_t h2;
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h2) == prev_head);
    CHECK(verify_pattern(&h2, 0));
}

static void test_corrupt_both_headers(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 100);
    hdr[0].crc ^= 1;
    hdr[1].magic = 0;
    history_t h2;
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_FRESH);
    CHECK(history_head(&h2) == 0);
    CHECK(history_reset_count(&h2) == 0);
}

static void test_size_mismatch(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 100);
    static uint8_t bigger[2 * SIZE];
    history_t h2;
    CHECK(history_boot(&h2, hdr, bigger, 2 * SIZE) == HISTORY_BOOT_FRESH);
}

static void test_version_mismatch(void) {
    history_t h;
    fresh(&h);
    append_pattern(&h, 100);
    for (int i = 0; i < 2; i++) {
        hdr[i].version = HISTORY_VERSION + 1;
        hdr[i].crc = history_crc32(&hdr[i], offsetof(history_hdr_t, crc));
    }
    history_t h2;
    CHECK(history_boot(&h2, hdr, buf, SIZE) == HISTORY_BOOT_FRESH);
}

// Rewrite both header copies to describe `head` with the given seqs.
static void forge_headers(uint64_t head0, uint32_t seq0, uint64_t head1, uint32_t seq1) {
    uint64_t heads[2] = {head0, head1};
    uint32_t seqs[2] = {seq0, seq1};
    for (int i = 0; i < 2; i++) {
        memset(&hdr[i], 0, sizeof(hdr[i]));
        hdr[i].magic = HISTORY_MAGIC;
        hdr[i].version = HISTORY_VERSION;
        hdr[i].size = SIZE;
        hdr[i].reset_count = 7;
        hdr[i].seq = seqs[i];
        hdr[i].head = heads[i];
        hdr[i].crc = history_crc32(&hdr[i], offsetof(history_hdr_t, crc));
    }
}

static void test_seq_wraparound(void) {
    forge_headers(111, 0xffffffffu, 222, 0);
    history_t h;
    CHECK(history_boot(&h, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h) == 222);  // seq 0 is newer than 0xffffffff
    CHECK(history_reset_count(&h) == 8);

    forge_headers(333, 5, 444, 4);
    CHECK(history_boot(&h, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h) == 333);
}

static void test_64bit_offsets(void) {
    // Start just below 2^32 so appends cross the 32-bit boundary.
    uint64_t start = (1ull << 32) - 700;
    forge_headers(start, 1, start - 1, 0);
    history_t h;
    CHECK(history_boot(&h, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
    CHECK(history_head(&h) == start);
    uint64_t base = history_head(&h);
    append_pattern(&h, 3 * SIZE);
    CHECK(history_head(&h) == base + 3 * SIZE);
    CHECK(history_head(&h) > (1ull << 32));
    CHECK(history_oldest(&h) == history_head(&h) - SIZE);
    CHECK(verify_pattern(&h, history_oldest(&h)));

    uint64_t cur = start;
    CHECK(history_clamp(&h, &cur) == history_oldest(&h) - start);

    // And far beyond: 2^40.
    forge_headers(1ull << 40, 10, 0, 9);
    CHECK(history_boot(&h, hdr, buf, SIZE) == HISTORY_BOOT_RESTORED);
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
    test_crc();
    test_fresh_boot();
    test_simple_append();
    test_wraparound();
    test_append_larger_than_ring();
    test_dropped_detection();
    test_restore();
    test_restore_floor();
    test_interrupted_append();
    test_corrupt_one_header();
    test_corrupt_both_headers();
    test_size_mismatch();
    test_version_mismatch();
    test_seq_wraparound();
    test_64bit_offsets();
    test_appendf();
    if (failures) {
        fprintf(stderr, "test_history: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_history: all tests passed\n");
    return 0;
}
