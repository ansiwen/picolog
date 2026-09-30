#ifndef PICOLOG_TUSB_CONFIG_H
#define PICOLOG_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

/* CFG_TUSB_MCU / CFG_TUSB_OS come from the pico-sdk. */
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUD_ENDPOINT0_SIZE 64

/* Two CDC-ACM interfaces: 0 = live, 1 = replay. */
#define CFG_TUD_CDC 2
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

/* We ignore everything the host sends, so RX is tiny; TX holds a whole replay
 * header plus data so the main loop can queue in bigger pieces. */
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif
