// picolog: UART flight recorder + live console.
//
// Single-core superloop on core0. The DMA does the realtime capture work;
// everything else (copying into the history ring, USB) runs here. See
// history.h for the concurrency assumptions.
#include "capture.h"
#include "history.h"
#include "usb_ports.h"

#include "hardware/structs/powman.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include <stdio.h>

#define HISTORY_SIZE (256u * 1024u)

// Line-error markers are aggregated and emitted at most this often, so a
// misconfigured baud rate (a framing error on every byte) cannot flood the
// history with markers.
#define MARKER_INTERVAL_US 100000u

#ifndef PICOLOG_WATCHDOG_MS
#define PICOLOG_WATCHDOG_MS 2000
#endif

// Persistent state: placed in __uninitialized_ram so crt0 does not zero it,
// which lets the history survive a watchdog or soft reset. The RP2350
// bootrom keeps its own state in boot RAM / USB RAM and does not clear main
// SRAM for a normal (non-secure, no load map) flash image; history_boot()
// validates the header CRC anyway, so any corruption just yields a fresh,
// empty history.
typedef struct {
    history_hdr_t hdr[2];
    uint8_t buf[HISTORY_SIZE];
} persist_t;
static persist_t __uninitialized_ram(persist);

static history_t history;

static usb_port_t port_live;
static usb_port_t port_replay;

static bool io_connected(uint8_t itf) { return tud_cdc_n_connected(itf); }
static uint32_t io_write_available(uint8_t itf) { return tud_cdc_n_write_available(itf); }
static uint32_t io_write(uint8_t itf, const void *buf, uint32_t len) { return tud_cdc_n_write(itf, buf, len); }
static void io_flush(uint8_t itf) { tud_cdc_n_write_flush(itf); }
static void io_clear_tx(uint8_t itf) { tud_cdc_n_write_clear(itf); }
static void io_discard_rx(uint8_t itf) { tud_cdc_n_read_flush(itf); }

static const usb_port_io_t tusb_io = {
    .connected = io_connected,
    .write_available = io_write_available,
    .write = io_write,
    .flush = io_flush,
    .clear_tx = io_clear_tx,
    .discard_rx = io_discard_rx,
};

static void to_history(const uint8_t *data, size_t len, void *ctx) {
    history_append(ctx, data, len);
}

static void marker_count(const char *what, uint32_t n) {
    if (n == 1) {
        history_appendf(&history, "\r\n[picolog: %s]\r\n", what);
    } else if (n > 1) {
        history_appendf(&history, "\r\n[picolog: %s x%lu]\r\n", what, (unsigned long)n);
    }
}

static void emit_markers(capture_events_t *ev, uint64_t now_us) {
    static bool emitted_before;
    static uint64_t last_us;
    if (!ev->breaks && !ev->framing_errors && !ev->parity_errors && !ev->uart_overruns && !ev->lost_bytes) {
        return;
    }
    if (emitted_before && now_us - last_us < MARKER_INTERVAL_US) {
        return;  // keep accumulating
    }
    emitted_before = true;
    last_us = now_us;

    if (ev->lost_bytes) {
        history_appendf(&history, "\r\n[picolog: capture overrun, %llu bytes lost]\r\n",
                        (unsigned long long)ev->lost_bytes);
    }
    marker_count("UART FIFO overrun", ev->uart_overruns);
    marker_count("BREAK", ev->breaks);
    marker_count("framing error", ev->framing_errors);
    marker_count("parity error", ev->parity_errors);
    *ev = (capture_events_t){0};
}

// Watchdog SCRATCH0..3 are free for application use (the bootrom and SDK
// use SCRATCH4..7). They are cleared by chip-level resets but preserved over
// system/subsystem resets, which tells us whether POWMAN CHIP_RESET (which
// only records chip-level resets) is describing *this* reset or an older one.
#define BOOT_SEEN_SCRATCH 0
#define BOOT_SEEN_MAGIC 0x706c6f67u  // "plog"

static void describe_reset(char *out, size_t n) {
    uint32_t wd = watchdog_hw->reason;
    uint32_t chip = powman_hw->chip_reset;
    bool chip_level = watchdog_hw->scratch[BOOT_SEEN_SCRATCH] != BOOT_SEEN_MAGIC;
    watchdog_hw->scratch[BOOT_SEEN_SCRATCH] = BOOT_SEEN_MAGIC;

    out[0] = 0;
    // WATCHDOG_REASON is cleared by every chip-level reset and by a warm
    // (SYSRESETREQ / debugger) reset, so if it is set it is current.
    if (wd & WATCHDOG_REASON_TIMER_BITS) {
        snprintf(out, n, "watchdog");
        return;
    }
    if (wd & WATCHDOG_REASON_FORCE_BITS) {
        snprintf(out, n, "watchdog-forced");
        return;
    }
    if (!chip_level) {
        // Core warm reset without a chip-level reset: Arm SYSRESETREQ
        // (e.g. software reset via AIRCR) or a debugger reset.
        snprintf(out, n, "soft");
        return;
    }
    static const struct {
        uint32_t bits;
        const char *name;
    } causes[] = {
        {POWMAN_CHIP_RESET_HAD_POR_BITS, "por"},
        {POWMAN_CHIP_RESET_HAD_BOR_BITS, "brownout"},
        {POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS, "run-pin"},
        {POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS, "debugger"},
        {POWMAN_CHIP_RESET_HAD_RESCUE_BITS, "rescue"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS | POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
             POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS | POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS,
         "watchdog"},
        {POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS, "powerdown"},
        {POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS, "glitch"},
        {POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS, "debugger-sysreset"},
    };
    size_t len = 0;
    for (size_t i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
        if ((chip & causes[i].bits) && len + 1 < n) {
            int w = snprintf(out + len, n - len, "%s%s", len ? "+" : "", causes[i].name);
            if (w > 0) {
                len += (size_t)w;
            }
        }
    }
    if (len == 0) {
        snprintf(out, n, "unknown");
    }
}

int main(void) {
    char reason[64];
    describe_reset(reason, sizeof(reason));

    history_boot_t boot = history_boot(&history, persist.hdr, persist.buf, HISTORY_SIZE);
    history_appendf(&history, "\r\n[picolog: boot, reset reason %s, history %s]\r\n", reason,
                    boot == HISTORY_BOOT_RESTORED ? "kept" : "cleared");

    capture_init();

    usb_port_init(&port_live, 0, false);
    usb_port_init(&port_replay, 1, true);
    tud_init(BOARD_TUD_RHPORT);

    watchdog_enable(PICOLOG_WATCHDOG_MS, true);

    capture_events_t ev = {0};
    for (;;) {
        watchdog_update();
        tud_task();

        // Data first, then markers, so a marker lands after the bytes that
        // were received before the event was noticed.
        capture_poll(to_history, &history, &ev);
        uint64_t now = time_us_64();
        emit_markers(&ev, now);

        usb_port_task(&port_live, &tusb_io, &history, now);
        usb_port_task(&port_replay, &tusb_io, &history, now);
    }
}
