// Host-side unit tests for src/usb_ports.c with a fake USB I/O layer.
#include "usb_ports.h"

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

#define SIZE 4096u

// ---- fake USB ----

typedef struct {
    bool connected;
    uint32_t avail_per_task;  // how many bytes the "host" accepts per task call
    uint32_t avail_left;
    char out[1 << 20];
    size_t out_len;
    int clear_tx_calls;
    int discard_rx_calls;
    int flush_calls;
} fake_port_t;

static fake_port_t fake[2];

static bool f_connected(uint8_t itf) { return fake[itf].connected; }
static uint32_t f_write_available(uint8_t itf) { return fake[itf].avail_left; }
static uint32_t f_write(uint8_t itf, const void *buf, uint32_t len) {
    fake_port_t *f = &fake[itf];
    if (len > f->avail_left) {
        len = f->avail_left;
    }
    if (f->out_len + len >= sizeof(f->out)) {
        abort();
    }
    memcpy(f->out + f->out_len, buf, len);
    f->out_len += len;
    f->out[f->out_len] = 0;  // keep it a C string for strstr
    f->avail_left -= len;
    return len;
}
static void f_flush(uint8_t itf) { fake[itf].flush_calls++; }
static void f_clear_tx(uint8_t itf) { fake[itf].clear_tx_calls++; }
static void f_discard_rx(uint8_t itf) { fake[itf].discard_rx_calls++; }

static const usb_port_io_t io = {f_connected, f_write_available, f_write, f_flush, f_clear_tx, f_discard_rx};

static void task(usb_port_t *p, const history_t *h) {
    fake[p->itf].avail_left = fake[p->itf].avail_per_task;
    usb_port_task(p, &io, h, 90061000000ull);  // 1d 01:01:01
}

static void reset_fakes(void) {
    memset(fake, 0, sizeof(fake));
    fake[0].avail_per_task = fake[1].avail_per_task = 1u << 30;
}

// ---- history ----

static history_hdr_t hdr[2];
static uint8_t buf[SIZE];

static void fresh(history_t *h) {
    memset(hdr, 0, sizeof(hdr));
    history_boot(h, hdr, buf, SIZE);
}

// Deterministic printable "log" text with sequence numbers.
static uint64_t line_no;
static void append_lines(history_t *h, int n) {
    char tmp[64];
    for (int i = 0; i < n; i++) {
        int len = snprintf(tmp, sizeof(tmp), "line %06llu\r\n", (unsigned long long)line_no++);
        history_append(h, tmp, (size_t)len);
    }
}

// Copy [from, to) of the history into dst (must still be valid).
static size_t history_slice(const history_t *h, uint64_t from, uint64_t to, char *dst) {
    size_t total = 0;
    while (from < to) {
        const uint8_t *p;
        size_t n = history_peek(h, from, &p);
        if (n > to - from) {
            n = (size_t)(to - from);
        }
        memcpy(dst + total, p, n);
        total += n;
        from += n;
    }
    return total;
}

// Remove one occurrence of `needle` from the fake output, returning its
// position or -1.
static long cut(fake_port_t *f, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= f->out_len; i++) {
        if (memcmp(f->out + i, needle, nl) == 0) {
            memmove(f->out + i, f->out + i + nl, f->out_len - i - nl);
            f->out_len -= nl;
            f->out[f->out_len] = 0;
            return (long)i;
        }
    }
    return -1;
}

// ---- tests ----

static void test_live_shows_only_new_data(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    usb_port_t p;
    usb_port_init(&p, 0, false);

    history_append(&h, "old", 3);
    task(&p, &h);  // not connected
    CHECK(fake[0].out_len == 0);
    CHECK(p.state == USB_PORT_CLOSED);

    fake[0].connected = true;
    task(&p, &h);
    CHECK(fake[0].out_len == 0);  // "old" is not shown
    CHECK(fake[0].clear_tx_calls == 1);

    history_append(&h, "new", 3);
    task(&p, &h);
    CHECK(fake[0].out_len == 3 && memcmp(fake[0].out, "new", 3) == 0);
    CHECK(fake[0].flush_calls >= 1);

    // DTR low: stop sending.
    fake[0].connected = false;
    task(&p, &h);
    history_append(&h, "gone", 4);
    task(&p, &h);
    CHECK(fake[0].out_len == 3);

    // Reopen: again only new data.
    fake[0].connected = true;
    task(&p, &h);
    history_append(&h, "again", 5);
    task(&p, &h);
    CHECK(fake[0].out_len == 8 && memcmp(fake[0].out, "newagain", 8) == 0);
    CHECK(fake[0].clear_tx_calls == 2);
    CHECK(fake[0].discard_rx_calls > 0);
}

static void test_replay_then_live_no_gap_no_dup(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    line_no = 0;
    append_lines(&h, 400);  // ~5 KB > SIZE: ring has wrapped
    uint64_t oldest = history_oldest(&h);
    CHECK(oldest > 0);

    usb_port_t p;
    usb_port_init(&p, 1, true);
    fake[1].connected = true;
    fake[1].avail_per_task = 97;  // slow-ish reader, odd chunking

    // Keep producing while replaying, but slower than the reader.
    for (int i = 0; i < 2000; i++) {
        task(&p, &h);
        if (i % 3 == 0) {
            append_lines(&h, 1);
        }
    }
    // Drain.
    for (int i = 0; i < 1000; i++) {
        task(&p, &h);
    }
    CHECK(p.state == USB_PORT_LIVE);
    CHECK(p.cursor == history_head(&h));

    fake_port_t *f = &fake[1];
    // Header comes first, with the byte count and uptime/reset count.
    char want_hdr[160];
    snprintf(want_hdr, sizeof(want_hdr),
             "=== picolog replay: %llu bytes, uptime 1d 01:01:01, resets since power-on 0 ===\r\n",
             (unsigned long long)(p.replay_end - oldest));
    CHECK(cut(f, want_hdr) == 0);
    long end_pos = cut(f, "\r\n=== picolog replay end, live follows ===\r\n");
    CHECK(end_pos == (long)(p.replay_end - oldest));
    CHECK(strstr(f->out, "dropped") == NULL);

    // What remains must be exactly the history from `oldest` to head.
    static char want[1 << 20];
    size_t n = history_slice(&h, oldest > history_oldest(&h) ? oldest : history_oldest(&h), history_head(&h), want);
    // The oldest bytes may have been overwritten by now; compare the tail.
    CHECK(f->out_len >= n);
    CHECK(memcmp(f->out + (f->out_len - n), want, n) == 0);

    // Line numbers must be strictly consecutive across the replay/live seam.
    unsigned long long prev = 0;
    bool first = true;
    int lines = 0;
    for (char *s = f->out; (s = strstr(s, "line ")) != NULL; s += 5) {
        unsigned long long v = strtoull(s + 5, NULL, 10);
        if (!first && v != prev + 1) {
            fprintf(stderr, "gap/dup: %llu after %llu\n", v, prev);
            failures++;
            break;
        }
        first = false;
        prev = v;
        lines++;
    }
    CHECK(prev == line_no - 1);
    CHECK(lines > 400);
}

static void test_replay_every_reconnect(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    history_append(&h, "abc", 3);
    usb_port_t p;
    usb_port_init(&p, 1, true);
    for (int round = 0; round < 2; round++) {
        fake[1].out_len = 0;
        fake[1].out[0] = 0;
        fake[1].connected = true;
        task(&p, &h);
        CHECK(p.state == USB_PORT_LIVE);
        CHECK(cut(&fake[1], "=== picolog replay: 3 bytes") == 0);
        CHECK(cut(&fake[1], "\r\n=== picolog replay end, live follows ===\r\n") > 0);
        CHECK(strstr(fake[1].out, "abc") != NULL);
        fake[1].connected = false;
        task(&p, &h);
        CHECK(p.state == USB_PORT_CLOSED);
    }
}

static void test_replay_empty_history(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    usb_port_t p;
    usb_port_init(&p, 1, true);
    fake[1].connected = true;
    task(&p, &h);
    CHECK(p.state == USB_PORT_LIVE);
    CHECK(cut(&fake[1], "=== picolog replay: 0 bytes") == 0);
    CHECK(cut(&fake[1], "\r\n=== picolog replay end, live follows ===\r\n") >= 0);
    history_append(&h, "x", 1);
    task(&p, &h);
    CHECK(fake[1].out_len > 0 && fake[1].out[fake[1].out_len - 1] == 'x');
}

static void test_slow_live_reader_drops(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    usb_port_t p;
    usb_port_init(&p, 0, false);
    fake[0].connected = true;
    task(&p, &h);
    uint64_t start = p.cursor;

    // Host stops reading entirely while 3*SIZE bytes arrive.
    fake[0].avail_per_task = 0;
    line_no = 0;
    for (int i = 0; i < 100; i++) {
        append_lines(&h, 10);
        task(&p, &h);  // must not stall or crash
    }
    CHECK(fake[0].out_len == 0);
    uint64_t expected_drop = history_oldest(&h) - start;
    CHECK(expected_drop > 0);

    fake[0].avail_per_task = 1u << 30;
    task(&p, &h);
    char want[80];
    snprintf(want, sizeof(want), "\r\n[picolog: %llu bytes dropped]\r\n", (unsigned long long)expected_drop);
    CHECK(cut(&fake[0], want) == 0);
    CHECK(fake[0].out_len == SIZE);
    CHECK(p.cursor == history_head(&h));
}

static void test_drop_during_replay(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    line_no = 0;
    append_lines(&h, 400);
    usb_port_t p;
    usb_port_init(&p, 1, true);
    fake[1].connected = true;
    fake[1].avail_per_task = 50;
    task(&p, &h);  // header partially sent
    // Flood: more than the whole ring arrives while the reader is slow.
    append_lines(&h, 800);
    fake[1].avail_per_task = 1u << 30;
    task(&p, &h);
    CHECK(p.state == USB_PORT_LIVE);
    CHECK(strstr(fake[1].out, "bytes dropped]") != NULL);
    CHECK(strstr(fake[1].out, "=== picolog replay end, live follows ===") != NULL);
    CHECK(p.cursor == history_head(&h));
    // The header must still be complete (it is not subject to drops).
    CHECK(strncmp(fake[1].out, "=== picolog replay: ", 20) == 0);
    CHECK(strstr(fake[1].out, "resets since power-on 0 ===\r\n") != NULL);
}

static void test_ports_independent(void) {
    reset_fakes();
    history_t h;
    fresh(&h);
    history_append(&h, "hist", 4);
    usb_port_t live, replay;
    usb_port_init(&live, 0, false);
    usb_port_init(&replay, 1, true);
    fake[0].connected = fake[1].connected = true;
    task(&live, &h);
    task(&replay, &h);
    history_append(&h, "NEW", 3);
    task(&live, &h);
    task(&replay, &h);
    CHECK(fake[0].out_len == 3 && memcmp(fake[0].out, "NEW", 3) == 0);
    CHECK(strstr(fake[1].out, "hist") != NULL);
    CHECK(fake[1].out_len >= 3 && memcmp(fake[1].out + fake[1].out_len - 3, "NEW", 3) == 0);

    // Closing one does not affect the other.
    fake[0].connected = false;
    task(&live, &h);
    history_append(&h, "Z", 1);
    task(&live, &h);
    task(&replay, &h);
    CHECK(fake[0].out_len == 3);
    CHECK(fake[1].out[fake[1].out_len - 1] == 'Z');
}

int main(void) {
    test_live_shows_only_new_data();
    test_replay_then_live_no_gap_no_dup();
    test_replay_every_reconnect();
    test_replay_empty_history();
    test_slow_live_reader_drops();
    test_drop_during_replay();
    test_ports_independent();
    if (failures) {
        fprintf(stderr, "test_usb_ports: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_usb_ports: all tests passed\n");
    return 0;
}
