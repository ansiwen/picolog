#include <stdint.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#ifndef PICOLOG_USB_VID
#define PICOLOG_USB_VID 0x1209
#endif
#ifndef PICOLOG_USB_PID
#define PICOLOG_USB_PID 0x0001
#endif

/* Interface numbers: 0/1 = live (control/data), 2/3 = replay. */
enum {
    ITF_NUM_LIVE = 0,
    ITF_NUM_LIVE_DATA,
    ITF_NUM_REPLAY,
    ITF_NUM_REPLAY_DATA,
    ITF_NUM_TOTAL
};

/* Endpoints: live notif 0x81, out 0x02, in 0x82; replay notif 0x83, out 0x04, in 0x84. */
#define EPNUM_LIVE_NOTIF 0x81
#define EPNUM_LIVE_OUT 0x02
#define EPNUM_LIVE_IN 0x82
#define EPNUM_REPLAY_NOTIF 0x83
#define EPNUM_REPLAY_OUT 0x04
#define EPNUM_REPLAY_IN 0x84

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_LIVE,
    STRID_REPLAY,
};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* Composite device using an Interface Association Descriptor per CDC port. */
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

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

/* Bus powered, 100 mA (the board really runs from VSYS; USB only carries data). */
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_LIVE, STRID_LIVE, EPNUM_LIVE_NOTIF, 8, EPNUM_LIVE_OUT,
                       EPNUM_LIVE_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_REPLAY, STRID_REPLAY, EPNUM_REPLAY_NOTIF, 8, EPNUM_REPLAY_OUT,
                       EPNUM_REPLAY_IN, 64),
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char *const string_table[] = {
    [STRID_MANUFACTURER] = "picolog",
    [STRID_PRODUCT] = "picolog UART recorder",
    [STRID_LIVE] = "picolog live",
    [STRID_REPLAY] = "picolog replay",
};

/* A string descriptor is a 2-byte header followed by UTF-16LE text. */
static uint16_t desc_str[1 + 32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    const char *str;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    size_t chr_count;

    if (index == STRID_LANGID) {
        desc_str[1] = 0x0409; /* English (US) */
        chr_count = 1;
    } else {
        if (index == STRID_SERIAL) {
            pico_get_unique_board_id_string(serial, sizeof serial);
            str = serial;
        } else if (index < sizeof string_table / sizeof string_table[0] && string_table[index]) {
            str = string_table[index];
        } else {
            return NULL;
        }
        chr_count = strlen(str);
        if (chr_count > 31)
            chr_count = 31;
        for (size_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = (uint8_t)str[i];
    }

    /* First element: length in bytes (including the header) and descriptor type. */
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
