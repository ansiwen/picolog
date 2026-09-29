// Per-port cursor logic for the two CDC ports.
//
//   live   (CDC 0): on DTR rise, start at the current head and stream new data.
//   replay (CDC 1): on DTR rise, send a header, the full history, an end
//                   marker, then continue live from exactly where the
//                   replay ended (no gap, no duplicate).
//
// A reader that falls behind the oldest valid byte is moved forward and gets
// a "[picolog: N bytes dropped]" marker. Bytes from the host are discarded.
//
// Hardware-independent: all USB access goes through usb_port_io_t so the
// logic can be unit-tested on the host. Like the history, this runs only in
// the core0 superloop.
#ifndef PICOLOG_USB_PORTS_H
#define PICOLOG_USB_PORTS_H

#include <stdbool.h>
#include <stdint.h>

#include "history.h"

typedef struct {
    bool (*connected)(uint8_t itf);  // mounted and DTR asserted
    uint32_t (*write_available)(uint8_t itf);
    uint32_t (*write)(uint8_t itf, const void *buf, uint32_t len);
    void (*flush)(uint8_t itf);
    void (*clear_tx)(uint8_t itf);    // drop stale queued TX data
    void (*discard_rx)(uint8_t itf);  // drop anything the host sent
} usb_port_io_t;

typedef enum {
    USB_PORT_CLOSED,
    USB_PORT_REPLAY,
    USB_PORT_LIVE,
} usb_port_state_t;

typedef struct {
    uint8_t itf;
    bool is_replay;
    usb_port_state_t state;
    uint64_t cursor;
    uint64_t replay_end;
    uint16_t msg_len;
    uint16_t msg_pos;
    char msg[160];  // pending out-of-band text (header, markers)
} usb_port_t;

void usb_port_init(usb_port_t *p, uint8_t itf, bool is_replay);

// Handle DTR edges and push as much pending data as the port accepts.
void usb_port_task(usb_port_t *p, const usb_port_io_t *io, const history_t *h, uint64_t uptime_us);

#endif
