#include "selftest_line.h"

#include <stdio.h>
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

static void expect(uint64_t ms, const char *want) {
    char buf[SELFTEST_LINE_MAX];
    size_t n = selftest_format_line(buf, ms);
    size_t want_len = strlen(want);
    CHECK(n <= SELFTEST_LINE_MAX);
    CHECK(n == want_len);
    if (n == want_len && n <= SELFTEST_LINE_MAX) {
        CHECK(memcmp(buf, want, n) == 0);
        if (memcmp(buf, want, n) != 0)
            printf("  got \"%.*s\"\n", (int)n, buf);
    }
}

int main(void) {
    expect(0, "[      0.000] t=0 ms\r\n");
    expect(7, "[      0.007] t=7 ms\r\n");
    expect(100, "[      0.100] t=100 ms\r\n");
    expect(999, "[      0.999] t=999 ms\r\n");
    expect(1000, "[      1.000] t=1000 ms\r\n");
    expect(12300, "[     12.300] t=12300 ms\r\n");
    expect(9999999999ull, "[9999999.999] t=9999999999 ms\r\n");
    /* wider than the 7-character seconds field: grows, never truncates */
    expect(12345678901ull, "[12345678.901] t=12345678901 ms\r\n");
    /* 2^64-1 ms: the longest possible line */
    expect(UINT64_MAX, "[18446744073709551.615] t=18446744073709551615 ms\r\n");
    printf("test_selftest_line: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
