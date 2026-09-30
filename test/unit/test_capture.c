#include "capture.h"

#include <stdio.h>

static int failures, checks;
#define CHECK(cond)                                                           \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                     \
    } while (0)

int main(void) {
    /* The counter counts down from 2^27 and reloads itself. */
    CHECK(capture_produced(CAPTURE_DMA_COUNT, CAPTURE_DMA_COUNT) == 0);
    CHECK(capture_produced(CAPTURE_DMA_COUNT, CAPTURE_DMA_COUNT - 1) == 1);
    CHECK(capture_produced(1000, 900) == 100);
    CHECK(capture_produced(1000, 1000) == 0);
    /* reload: counter hit 0 and restarted at 2^27 */
    CHECK(capture_produced(10, CAPTURE_DMA_COUNT - 5) == 15);
    CHECK(capture_produced(1, CAPTURE_DMA_COUNT) == 1);
    CHECK(capture_produced(0, CAPTURE_DMA_COUNT - 1) == 1);
    CHECK(capture_produced(CAPTURE_DMA_COUNT, 0) == 0); /* 0 and 2^27 are the same phase */
    /* larger than the ring is still exact */
    CHECK(capture_produced(CAPTURE_DMA_COUNT, CAPTURE_DMA_COUNT - 100000) == 100000);
    CHECK(CAPTURE_MAX_PENDING == 32768u - 1024u);
    printf("test_capture: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
