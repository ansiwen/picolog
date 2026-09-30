#ifndef PICOLOG_USB_PORTS_H
#define PICOLOG_USB_PORTS_H

/*
 * Per-CDC-port behaviour: cursor handling, DTR handling and the replay state
 * machine. Hardware independent: everything that touches TinyUSB goes through
 * usb_port_ops_t, so this file is compiled and tested on the host.
 *
 * Like the history ring this is single-context code (core0 superloop).
 */

#include <stdbool.h>
#include <stdint.h>

#include "history.h"

/* After a DTR rising edge nothing is sent for this long: programs such as
 * pyserial flush the tty input right after open(), which would eat the header. */
#define USB_PORT_OPEN_DELAY_US 100000u

typedef struct {
    void *ctx;
    /* Host has configured the device AND raised DTR. Deliberately ignores USB
     * suspend (see README, "USB unplug/suspend behaviour"). */
    bool (*connected)(void *ctx, uint8_t itf);
    /* Free space in the TX FIFO. */
    uint32_t (*write_available)(void *ctx, uint8_t itf);
    /* Queue bytes; callers never pass more than write_available(). */
    uint32_t (*write)(void *ctx, uint8_t itf, const void *data, uint32_t len);
    void (*flush)(void *ctx, uint8_t itf);
    void (*clear_tx)(void *ctx, uint8_t itf);
    /* Drop everything the host sent us. */
    void (*discard_rx)(void *ctx, uint8_t itf);
} usb_port_ops_t;

typedef enum {
    USB_PORT_KIND_LIVE,   /* only new data */
    USB_PORT_KIND_REPLAY, /* history first, then live */
} usb_port_kind_t;

typedef enum {
    USB_PORT_CLOSED,
    USB_PORT_REPLAY_PENDING, /* DTR seen, waiting out the open delay */
    USB_PORT_REPLAY,         /* streaming the snapshot */
    USB_PORT_LIVE_STREAM,    /* following the head */
} usb_port_state_t;

typedef struct {
    usb_port_kind_t kind;
    uint8_t itf;
    const usb_port_ops_t *ops;
    history_t *history;

    usb_port_state_t state;
    uint64_t cursor;      /* next history offset to send */
    uint64_t replay_end;  /* end of the snapshot (REPLAY only) */
    uint64_t send_after;  /* no output before this time (us) */
} usb_port_t;

void usb_port_init(usb_port_t *p, usb_port_kind_t kind, uint8_t itf,
                   const usb_port_ops_t *ops, history_t *history);

/* Call once per main-loop iteration. `now_us` is the time since boot. */
void usb_port_task(usb_port_t *p, uint64_t now_us);

#endif
