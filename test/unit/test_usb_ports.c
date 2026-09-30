/*
 * usb_ports tests with a fake TinyUSB layer.
 *
 * Fake time advances by USB_PORT_OPEN_DELAY_US per task call (unless a test
 * passes its own step), so a pending open resolves on the next call. The fake
 * TX FIFO accepts `room` bytes per task call, which models the host draining
 * the endpoint; room = 0 models a host that reads nothing.
 */
#include "usb_ports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                           \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                     \
    } while (0)

#define RING 1024u
#define OUT_MAX (1u << 20)

typedef struct {
    bool connected;
    uint32_t room;    /* bytes the FIFO accepts until the next refill */
    uint32_t refill;  /* room restored at the start of every task call */
    uint8_t *out;
    size_t out_len;
    int flushes, clears, discards;
} fake_t;

static bool f_connected(void *ctx, uint8_t itf) { (void)itf; return ((fake_t *)ctx)->connected; }
static uint32_t f_avail(void *ctx, uint8_t itf) { (void)itf; return ((fake_t *)ctx)->room; }
static uint32_t f_write(void *ctx, uint8_t itf, const void *d, uint32_t len) {
    (void)itf;
    fake_t *f = ctx;
    if (len > f->room)
        len = f->room;
    if (f->out_len + len > OUT_MAX) {
        printf("fake out overflow\n");
        exit(2);
    }
    memcpy(f->out + f->out_len, d, len);
    f->out_len += len;
    f->room -= len;
    return len;
}
static void f_flush(void *ctx, uint8_t itf) { (void)itf; ((fake_t *)ctx)->flushes++; }
static void f_clear(void *ctx, uint8_t itf) { (void)itf; ((fake_t *)ctx)->clears++; }
static void f_discard(void *ctx, uint8_t itf) { (void)itf; ((fake_t *)ctx)->discards++; }

typedef struct {
    history_t hist;
    uint8_t ring[RING];
    fake_t fake[2];
    usb_port_ops_t ops[2];
    usb_port_t port[2]; /* [0] live, [1] replay */
    uint64_t now;
    uint64_t next_byte; /* generator state: byte value = offset % 251 */
} rig_t;

static uint8_t pat(uint64_t off) { return (uint8_t)(off % 251); }

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof *r);
    history_init(&r->hist, r->ring, RING);
    for (int i = 0; i < 2; i++) {
        r->fake[i].out = malloc(OUT_MAX);
        r->fake[i].refill = 1u << 20;
        r->ops[i] = (usb_port_ops_t){
            .ctx = &r->fake[i], .connected = f_connected, .write_available = f_avail,
            .write = f_write, .flush = f_flush, .clear_tx = f_clear, .discard_rx = f_discard};
    }
    usb_port_init(&r->port[0], USB_PORT_KIND_LIVE, 0, &r->ops[0], &r->hist);
    usb_port_init(&r->port[1], USB_PORT_KIND_REPLAY, 2, &r->ops[1], &r->hist);
    r->now = 1000000000ull; /* not zero, so uptime formatting sees something */
}

static void rig_free(rig_t *r) {
    free(r->fake[0].out);
    free(r->fake[1].out);
}

static void produce(rig_t *r, size_t n) {
    uint8_t tmp[4096];
    while (n) {
        size_t k = n > sizeof tmp ? sizeof tmp : n;
        for (size_t i = 0; i < k; i++)
            tmp[i] = pat(r->next_byte++);
        history_append(&r->hist, tmp, k);
        n -= k;
    }
}

/* One main-loop iteration for one port. */
static void task_dt(rig_t *r, int i, uint64_t dt) {
    r->now += dt;
    r->fake[i].room = r->fake[i].refill;
    usb_port_task(&r->port[i], r->now);
}
static void task(rig_t *r, int i) { task_dt(r, i, USB_PORT_OPEN_DELAY_US); }

static void tasks(rig_t *r, int i, int n) {
    while (n--)
        task(r, i);
}

static void set_dtr(rig_t *r, int i, bool on) { r->fake[i].connected = on; }
static void clear_out(rig_t *r, int i) { r->fake[i].out_len = 0; }

#define LIVE 0
#define REPL 1
#define OUT(r, i) ((r)->fake[i].out)
#define LEN(r, i) ((r)->fake[i].out_len)

static const char END_MSG[] = "\r\n=== picolog replay end, live follows ===\r\n";

static bool has(const uint8_t *buf, size_t len, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(buf + i, s, n) == 0)
            return true;
    return false;
}

/* Verify that buf[0..len) is pattern bytes for offsets [start, start+len). */
static bool is_pattern(const uint8_t *buf, size_t len, uint64_t start) {
    for (size_t i = 0; i < len; i++)
        if (buf[i] != pat(start + i))
            return false;
    return true;
}

/* Parse a replay session output: header, payload, end marker, live tail.
 * Returns false if the structure is wrong. */
typedef struct {
    unsigned long long claimed; /* N from the header */
    size_t payload_off, payload_len; /* history part */
    size_t live_off, live_len;       /* after the end marker */
    char header[128];
} replay_t;

static bool parse_replay(const uint8_t *buf, size_t len, replay_t *out) {
    const char *pre = "=== picolog replay: ";
    if (len < strlen(pre) || memcmp(buf, pre, strlen(pre)) != 0)
        return false;
    const uint8_t *eol = memchr(buf, '\n', len);
    if (!eol)
        return false;
    size_t hl = (size_t)(eol - buf) + 1;
    if (hl >= sizeof out->header)
        return false;
    memcpy(out->header, buf, hl);
    out->header[hl] = 0;
    if (sscanf(out->header, "=== picolog replay: %llu bytes, uptime", &out->claimed) != 1)
        return false;
    size_t el = strlen(END_MSG);
    for (size_t i = hl; i + el <= len; i++) {
        if (memcmp(buf + i, END_MSG, el) == 0) {
            out->payload_off = hl;
            out->payload_len = i - hl;
            out->live_off = i + el;
            out->live_len = len - out->live_off;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */

static void test_live_only_new_data(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 300); /* history before the terminal opens: must not be shown */
    tasks(&r, LIVE, 3);
    CHECK(LEN(&r, LIVE) == 0); /* not connected: silent */
    set_dtr(&r, LIVE, true);
    task(&r, LIVE);
    CHECK(r.fake[LIVE].clears == 1);
    CHECK(LEN(&r, LIVE) == 0); /* nothing yet: within the open delay or nothing new */
    uint64_t before = r.next_byte;
    produce(&r, 100);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 100);
    CHECK(is_pattern(OUT(&r, LIVE), 100, before));
    /* stream more */
    produce(&r, 50);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 150);
    CHECK(is_pattern(OUT(&r, LIVE), 150, before));
    /* DTR low stops sending */
    set_dtr(&r, LIVE, false);
    produce(&r, 60);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 150);
    /* reopening starts at "now" again, again without old data */
    set_dtr(&r, LIVE, true);
    clear_out(&r, LIVE);
    uint64_t again = r.next_byte;
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 0);
    produce(&r, 10);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 10 && is_pattern(OUT(&r, LIVE), 10, again));
    CHECK(r.fake[LIVE].clears == 2);
    rig_free(&r);
}

static void test_open_delay(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 100);
    set_dtr(&r, REPL, true);
    task_dt(&r, REPL, 1000); /* edge seen */
    task_dt(&r, REPL, 50000); /* 50 ms later */
    CHECK(LEN(&r, REPL) == 0);
    task_dt(&r, REPL, 49000); /* 99 ms after the edge */
    CHECK(LEN(&r, REPL) == 0);
    task_dt(&r, REPL, 1000); /* 100 ms: go */
    CHECK(LEN(&r, REPL) > 0);
    rig_free(&r);

    /* Live: data arriving during the delay is delivered once the delay is over. */
    rig_init(&r);
    set_dtr(&r, LIVE, true);
    task_dt(&r, LIVE, 1000);
    produce(&r, 20);
    task_dt(&r, LIVE, 10000);
    CHECK(LEN(&r, LIVE) == 0);
    task_dt(&r, LIVE, 100000);
    CHECK(LEN(&r, LIVE) == 20 && is_pattern(OUT(&r, LIVE), 20, 0));
    rig_free(&r);
}

static void test_replay_basic(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 500);
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 4);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.claimed == 500);
    CHECK(rp.payload_len == 500 && is_pattern(OUT(&r, REPL) + rp.payload_off, 500, 0));
    CHECK(rp.live_len == 0);
    /* uptime: now = 1000 s + a few 0.1 s steps -> "0d 00:16:40" */
    CHECK(has((uint8_t *)rp.header, strlen(rp.header), "uptime 0d 00:16:4"));
    CHECK(has((uint8_t *)rp.header, strlen(rp.header), " ===\r\n"));
    CHECK(strstr(rp.header, "[picolog") == NULL);
    /* then live follows without duplicates */
    produce(&r, 40);
    task(&r, REPL);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.live_len == 40 && is_pattern(OUT(&r, REPL) + rp.live_off, 40, 500));
    CHECK(r.fake[REPL].flushes >= 5);
    rig_free(&r);
}

static void test_replay_uptime_format(void) {
    rig_t r;
    rig_init(&r);
    r.now = (uint64_t)(2 * 86400 + 3 * 3600 + 4 * 60 + 5) * 1000000ull;
    set_dtr(&r, REPL, true);
    task_dt(&r, REPL, 0);
    task_dt(&r, REPL, USB_PORT_OPEN_DELAY_US);
    /* header is generated when sending starts: 100 ms later still in second 5 */
    CHECK(has(OUT(&r, REPL), LEN(&r, REPL), "=== picolog replay: 0 bytes, uptime 2d 03:04:05 ===\r\n"));
    rig_free(&r);
}

static void test_replay_empty_history(void) {
    rig_t r;
    rig_init(&r);
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 3);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.claimed == 0 && rp.payload_len == 0 && rp.live_len == 0);
    /* and it now follows live */
    produce(&r, 5);
    task(&r, REPL);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.live_len == 5 && is_pattern(OUT(&r, REPL) + rp.live_off, 5, 0));
    rig_free(&r);
}

static void test_replay_full_ring_and_wrap(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, RING * 3 + 77); /* wrapped several times */
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 4);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.claimed == RING);
    CHECK(rp.payload_len == RING);
    CHECK(is_pattern(OUT(&r, REPL) + rp.payload_off, RING, r.next_byte - RING));
    CHECK(!has(OUT(&r, REPL), LEN(&r, REPL), "dropped"));
    rig_free(&r);
}

/* Data arrives during the open delay and while a slow replay is being sent
 * (ring far from full, so nothing is dropped): the seam must be exact. */
static void test_seam_no_gap_no_dup(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 600);
    r.fake[REPL].refill = 64; /* slow host: replay takes many calls */
    set_dtr(&r, REPL, true);
    task(&r, REPL);       /* edge */
    produce(&r, 100);     /* arrives during the open delay -> part of the snapshot */
    for (int i = 0; i < 40; i++) {
        task(&r, REPL);
        produce(&r, 5);   /* arrives during the replay -> live part */
    }
    r.fake[REPL].refill = 1u << 20;
    tasks(&r, REPL, 3);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(!has(OUT(&r, REPL), LEN(&r, REPL), "dropped"));
    /* header claims the snapshot size taken when sending started */
    CHECK(rp.claimed == 700);
    CHECK(rp.payload_len == 700 && is_pattern(OUT(&r, REPL) + rp.payload_off, 700, 0));
    /* everything after the end marker continues exactly at offset 700 */
    CHECK(rp.live_len == r.next_byte - 700);
    CHECK(is_pattern(OUT(&r, REPL) + rp.live_off, rp.live_len, 700));
    rig_free(&r);
}

/* With a full ring and data flowing during the delay, the snapshot taken when
 * sending starts must not begin with a dropped marker. */
static void test_late_snapshot_full_ring(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, RING * 2);
    set_dtr(&r, REPL, true);
    task(&r, REPL); /* edge */
    produce(&r, 500); /* overwrites the oldest bytes during the delay */
    task(&r, REPL);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(!has(OUT(&r, REPL), LEN(&r, REPL), "dropped"));
    CHECK(rp.claimed == RING);
    CHECK(is_pattern(OUT(&r, REPL) + rp.payload_off, RING, r.next_byte - RING));
    rig_free(&r);
}

static void test_reconnect_replays_again(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 300);
    replay_t a, b;
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 3);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &a));
    size_t alen = LEN(&r, REPL);
    uint8_t *first = malloc(alen);
    memcpy(first, OUT(&r, REPL), alen);
    set_dtr(&r, REPL, false);
    task(&r, REPL);
    clear_out(&r, REPL);
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 3);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &b));
    CHECK(a.claimed == 300 && b.claimed == 300);
    CHECK(b.payload_len == 300 && is_pattern(OUT(&r, REPL) + b.payload_off, 300, 0));
    CHECK(r.fake[REPL].clears == 2);
    /* third time with more data */
    set_dtr(&r, REPL, false);
    task(&r, REPL);
    produce(&r, 100);
    clear_out(&r, REPL);
    set_dtr(&r, REPL, true);
    tasks(&r, REPL, 3);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &b));
    CHECK(b.claimed == 400 && b.payload_len == 400);
    free(first);
    rig_free(&r);
}

static void test_slow_live_reader_exact_drop_count(void) {
    rig_t r;
    rig_init(&r);
    set_dtr(&r, LIVE, true);
    tasks(&r, LIVE, 2);
    r.fake[LIVE].refill = 0; /* host reads nothing */
    produce(&r, 100);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 0);
    produce(&r, RING * 2); /* cursor (0) is now far behind */
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == 0);
    r.fake[LIVE].refill = 1u << 20;
    uint64_t oldest = history_oldest(&r.hist);
    CHECK(oldest == 100 + RING);
    task(&r, LIVE);
    char want[64];
    snprintf(want, sizeof want, "\r\n[picolog: %llu bytes dropped]\r\n", (unsigned long long)oldest);
    size_t wl = strlen(want);
    CHECK(LEN(&r, LIVE) == wl + RING);
    CHECK(memcmp(OUT(&r, LIVE), want, wl) == 0);
    CHECK(is_pattern(OUT(&r, LIVE) + wl, RING, oldest));
    /* marker goes to the port only, never into the history */
    CHECK(history_head(&r.hist) == r.next_byte);
    /* no second marker */
    produce(&r, 10);
    task(&r, LIVE);
    CHECK(LEN(&r, LIVE) == wl + RING + 10);
    rig_free(&r);
}

static void test_drop_marker_waits_for_room(void) {
    rig_t r;
    rig_init(&r);
    set_dtr(&r, LIVE, true);
    tasks(&r, LIVE, 2);
    r.fake[LIVE].refill = 0;
    produce(&r, RING * 2);
    task(&r, LIVE);
    r.fake[LIVE].refill = 10; /* too small for the marker: nothing may be sent */
    tasks(&r, LIVE, 3);
    CHECK(LEN(&r, LIVE) == 0);
    r.fake[LIVE].refill = 1u << 20;
    task(&r, LIVE);
    CHECK(has(OUT(&r, LIVE), LEN(&r, LIVE), "\r\n[picolog: 1024 bytes dropped]\r\n"));
    CHECK(LEN(&r, LIVE) > RING);
    rig_free(&r);
}

static void test_drop_during_replay(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, RING); /* full ring */
    r.fake[REPL].refill = 100; /* very slow */
    set_dtr(&r, REPL, true);
    task(&r, REPL);
    for (int i = 0; i < 4; i++)
        task(&r, REPL); /* ~100 bytes per call after the header */
    size_t sent_before = LEN(&r, REPL);
    CHECK(sent_before > 0 && sent_before < RING);
    produce(&r, RING / 2); /* overwrites what the replay has not sent yet */
    r.fake[REPL].refill = 1u << 20;
    tasks(&r, REPL, 3);
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(has(OUT(&r, REPL), LEN(&r, REPL), "bytes dropped]"));
    /* Everything after the marker is contiguous and ends at the head. */
    const uint8_t *buf = OUT(&r, REPL);
    size_t len = LEN(&r, REPL);
    const char *m = "\r\n[picolog: ";
    size_t mpos = 0;
    for (size_t i = 0; i + strlen(m) <= len; i++)
        if (memcmp(buf + i, m, strlen(m)) == 0) { mpos = i; break; }
    unsigned long long n = 0;
    CHECK(sscanf((const char *)buf + mpos, "\r\n[picolog: %llu bytes dropped]\r\n", &n) == 1);
    /* bytes before the marker: header + first part of the history from offset 0 */
    size_t hdr = strlen(rp.header);
    CHECK(is_pattern(buf + hdr, mpos - hdr, 0));
    uint64_t sent_hist = mpos - hdr;
    CHECK(sent_hist + n == history_oldest(&r.hist));
    size_t mlen = (size_t)snprintf(NULL, 0, "\r\n[picolog: %llu bytes dropped]\r\n", n);
    size_t rest = len - mpos - mlen;
    size_t el = strlen(END_MSG);
    CHECK(has(buf + mpos + mlen, rest, END_MSG));
    uint64_t oldest = history_oldest(&r.hist);
    /* Strong check: strip the end marker and the remainder is one continuous run. */
    uint8_t *tmp = malloc(rest);
    size_t k = 0;
    size_t i = 0;
    while (i < rest) {
        if (i + el <= rest && memcmp(buf + mpos + mlen + i, END_MSG, el) == 0) {
            i += el;
            continue;
        }
        tmp[k++] = buf[mpos + mlen + i];
        i++;
    }
    CHECK(k == r.next_byte - oldest);
    CHECK(is_pattern(tmp, k, oldest));
    free(tmp);
    rig_free(&r);
}

static void test_everything_lost_during_replay(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 200);
    r.fake[REPL].refill = 70; /* header (~55 bytes) fits, little else per call */
    set_dtr(&r, REPL, true);
    task(&r, REPL);
    task(&r, REPL);
    produce(&r, RING * 4); /* whole snapshot overwritten */
    r.fake[REPL].refill = 1u << 20;
    tasks(&r, REPL, 4);
    const uint8_t *buf = OUT(&r, REPL);
    size_t len = LEN(&r, REPL);
    CHECK(has(buf, len, "bytes dropped]"));
    CHECK(has(buf, len, END_MSG));
    /* after the end marker the live data continues exactly at the oldest byte */
    const uint8_t *e = NULL;
    for (size_t i = 0; i + strlen(END_MSG) <= len; i++)
        if (memcmp(buf + i, END_MSG, strlen(END_MSG)) == 0) { e = buf + i + strlen(END_MSG); break; }
    CHECK(e != NULL);
    if (e) {
        size_t tail = len - (size_t)(e - buf);
        CHECK(tail == RING && is_pattern(e, tail, history_oldest(&r.hist)));
    }
    rig_free(&r);
}

static void test_ports_independent(void) {
    rig_t r;
    rig_init(&r);
    produce(&r, 200);
    set_dtr(&r, LIVE, true);
    set_dtr(&r, REPL, true);
    for (int i = 0; i < 3; i++) {
        task(&r, LIVE);
        task(&r, REPL);
    }
    uint64_t at = r.next_byte;
    produce(&r, 30);
    task(&r, LIVE);
    task(&r, REPL);
    CHECK(LEN(&r, LIVE) == 30 && is_pattern(OUT(&r, LIVE), 30, at));
    replay_t rp;
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp));
    CHECK(rp.payload_len == 200 && rp.live_len == 30);
    /* closing one leaves the other running */
    set_dtr(&r, LIVE, false);
    produce(&r, 10);
    task(&r, LIVE);
    task(&r, REPL);
    CHECK(LEN(&r, LIVE) == 30);
    CHECK(parse_replay(OUT(&r, REPL), LEN(&r, REPL), &rp) && rp.live_len == 40);
    rig_free(&r);
}

static void test_flush_and_housekeeping(void) {
    rig_t r;
    rig_init(&r);
    /* not connected: still flushes and drains RX */
    tasks(&r, LIVE, 3);
    CHECK(r.fake[LIVE].flushes == 3 && r.fake[LIVE].discards == 3);
    /* connected but the host accepts nothing: still flushes */
    set_dtr(&r, LIVE, true);
    r.fake[LIVE].refill = 0;
    produce(&r, 100);
    tasks(&r, LIVE, 5);
    CHECK(r.fake[LIVE].flushes == 8 && r.fake[LIVE].discards == 8);
    CHECK(LEN(&r, LIVE) == 0);
    /* the other port's counters are untouched */
    CHECK(r.fake[REPL].flushes == 0);
    rig_free(&r);
}

int main(void) {
    test_live_only_new_data();
    test_open_delay();
    test_replay_basic();
    test_replay_uptime_format();
    test_replay_empty_history();
    test_replay_full_ring_and_wrap();
    test_seam_no_gap_no_dup();
    test_late_snapshot_full_ring();
    test_reconnect_replays_again();
    test_slow_live_reader_exact_drop_count();
    test_drop_marker_waits_for_room();
    test_drop_during_replay();
    test_everything_lost_during_replay();
    test_ports_independent();
    test_flush_and_housekeeping();
    printf("test_usb_ports: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
