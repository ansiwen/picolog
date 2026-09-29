// USB descriptors: composite device with two CDC-ACM functions.
//   interface 0/1: "picolog live"   (typically /dev/ttyACM0)
//   interface 2/3: "picolog replay" (typically /dev/ttyACM1)
#include "pico/unique_id.h"
#include "tusb.h"

#ifndef PICOLOG_USB_VID
#define PICOLOG_USB_VID 0x1209  // pid.codes test VID
#endif
#ifndef PICOLOG_USB_PID
#define PICOLOG_USB_PID 0x0001  // pid.codes test PID
#endif

enum {
    ITF_NUM_CDC_LIVE = 0,
    ITF_NUM_CDC_LIVE_DATA,
    ITF_NUM_CDC_REPLAY,
    ITF_NUM_CDC_REPLAY_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_LIVE,
    STRID_CDC_REPLAY,
    STRID_COUNT
};

#define EPNUM_CDC_LIVE_NOTIF 0x81
#define EPNUM_CDC_LIVE_OUT 0x02
#define EPNUM_CDC_LIVE_IN 0x82
#define EPNUM_CDC_REPLAY_NOTIF 0x83
#define EPNUM_CDC_REPLAY_OUT 0x04
#define EPNUM_CDC_REPLAY_IN 0x84

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // IAD is required for composite CDC.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PICOLOG_USB_VID,
    .idProduct = PICOLOG_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_LIVE, STRID_CDC_LIVE, EPNUM_CDC_LIVE_NOTIF, 8, EPNUM_CDC_LIVE_OUT,
                       EPNUM_CDC_LIVE_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_REPLAY, STRID_CDC_REPLAY, EPNUM_CDC_REPLAY_NOTIF, 8, EPNUM_CDC_REPLAY_OUT,
                       EPNUM_CDC_REPLAY_IN, 64),
};
_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length");

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const strings[STRID_COUNT] = {
    [STRID_MANUFACTURER] = "picolog",
    [STRID_PRODUCT] = "picolog UART recorder",
    [STRID_SERIAL] = serial_str,
    [STRID_CDC_LIVE] = "picolog live",
    [STRID_CDC_REPLAY] = "picolog replay",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[32 + 1];

    if (!serial_str[0]) {
        // Unique ID of the flash chip, as hex.
        pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
    }

    size_t len;
    if (index == STRID_LANGID) {
        desc_str[1] = 0x0409;  // English (US)
        len = 1;
    } else {
        if (index >= STRID_COUNT || !strings[index]) {
            return NULL;
        }
        const char *s = strings[index];
        for (len = 0; len < 32 && s[len]; len++) {
            desc_str[1 + len] = (uint8_t)s[len];
        }
    }
    // First element: length in bytes (incl. header) and descriptor type.
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
