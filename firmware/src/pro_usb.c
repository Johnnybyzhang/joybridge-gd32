#include "pro_usb.h"
#include <string.h>

/* Describe the actual Nintendo 0x30 full-mode byte layout to generic HID too.
 * The original Nintendo descriptor describes a different button/axis layout:
 * using it here exposes the changing timer and battery as phantom controls.
 * Keep the joystick application and Nintendo report IDs for HIDAPI/Steam. */
const uint8_t pro_usb_report_descriptor[] = {
    0x05,0x01,0x09,0x04,0xa1,0x01,0x85,0x30,0x15,0x00,
    /* Timer and packed battery/connection flags are not joystick controls. */
    0x75,0x08,0x95,0x02,0x81,0x03,
    /* Actual buttons in bytes 3..5. Rail/reserved bits are constant. */
    0x05,0x09,0x25,0x01,0x45,0x01,0x75,0x01,
    0x19,0x01,0x29,0x04,0x95,0x04,0x81,0x02,
    0x95,0x02,0x81,0x03,
    0x19,0x05,0x29,0x06,0x81,0x02,
    0x19,0x07,0x29,0x0c,0x95,0x06,0x81,0x02,
    0x95,0x02,0x81,0x03,
    0x19,0x0d,0x29,0x10,0x95,0x04,0x81,0x02,
    0x95,0x02,0x81,0x03,
    0x19,0x11,0x29,0x12,0x81,0x02,
    /* Four packed 12-bit stick axes, bytes 6..11. No fabricated hat field. */
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,
    0x26,0xff,0x0f,0x46,0xff,0x0f,0x75,0x0c,0x95,0x04,0x81,0x02,
    /* Vibration status, IMU and padding remain Nintendo protocol data. */
    0x75,0x08,0x95,0x34,0x81,0x03,
    0x06,0x00,0xff,0x26,0xff,0x00,0x46,0xff,0x00,
    0x85,0x21,0x09,0x01,0x75,0x08,0x95,0x3f,0x81,0x03,
    0x85,0x81,0x09,0x02,0x75,0x08,0x95,0x3f,0x81,0x03,
    0x85,0x01,0x09,0x03,0x75,0x08,0x95,0x3f,0x91,0x83,
    0x85,0x10,0x09,0x04,0x75,0x08,0x95,0x3f,0x91,0x83,
    0x85,0x80,0x09,0x05,0x75,0x08,0x95,0x3f,0x91,0x83,
    0x85,0x82,0x09,0x06,0x75,0x08,0x95,0x3f,0x91,0x83,
    /* Standard read-only Battery Strength feature, an unsigned percentage.
     * A full-width field avoids Windows scaled-value sign-extension errors
     * seen at level 4 when describing Nintendo's packed 3-bit battery field. */
    0x05,0x06,0x09,0x20,0x85,0x90,0x15,0x00,0x25,0x64,
    0x35,0x00,0x45,0x64,0x75,0x08,0x95,0x01,0xb1,0x02,0xc0};
_Static_assert(sizeof(pro_usb_report_descriptor)<256,"Report descriptor length fits HID descriptor");
const size_t pro_usb_report_descriptor_size = sizeof(pro_usb_report_descriptor);
const uint8_t pro_usb_device_descriptor[18] = {18,   1,    0x00, 0x02, 0, 0, 0, 64, 0x7e,
                                               0x05, 0x09, 0x20, 1,    2, 1, 2, 3,  1};
const uint8_t pro_usb_configuration_descriptor[41] = {9,
                                                      2,
                                                      41,
                                                      0,
                                                      1,
                                                      1,
                                                      0,
                                                      0x80,
                                                      50, /* bus powered, 100mA */
                                                      9,
                                                      4,
                                                      0,
                                                      0,
                                                      2,
                                                      3,
                                                      0,
                                                      0,
                                                      0,
                                                      9,
                                                      0x21,
                                                      0x11,
                                                      0x01,
                                                      0,
                                                      1,
                                                      0x22,
                                                      (uint8_t)sizeof(pro_usb_report_descriptor),
                                                      0,
                                                      7,
                                                      5,
                                                      0x81,
                                                      3,
                                                      64,
                                                      0,
                                                      1, /* collect fresh BT input on the next USB frame */
                                                      7,
                                                      5,
                                                      0x01,
                                                      3,
                                                      64,
                                                      0,
                                                      8};
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void stick_pack(uint8_t *p, uint16_t x, uint16_t y) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)((x >> 8) | ((y & 15u) << 4));
    p[2] = (uint8_t)(y >> 4);
}
bool pro_usb_spi_read(uint32_t address, uint8_t *out, size_t n) {
    /* Explicit safe virtual window and bounded arithmetic. */
    if (n > 29u || address < 0x6000u || address > 0x80ffu || n > 0x8100u - address)
        return false;
    memset(out, 0xff, n);
    uint8_t factory[0xa0];
    memset(factory, 0xff, sizeof(factory));
    factory[0x12] = 0x03; /* Pro Controller */
    factory[0x1b] = 0;    /* body/button colors available */
    for (unsigned axis = 0; axis < 3; ++axis) {
        put16(factory + 0x20 + axis * 2, 0);     /* accel origin */
        put16(factory + 0x26 + axis * 2, 16384); /* 4g scale */
        put16(factory + 0x2c + axis * 2, 0);     /* gyro bias */
        put16(factory + 0x32 + axis * 2, 13371); /* virtual gyro scale */
    }
    /* Left: positive range, center, negative range. Right: center, negative,
     * positive. Both virtual sticks use symmetric 1536-count travel. */
    stick_pack(factory + 0x3d, 1536, 1536);
    stick_pack(factory + 0x40, 2048, 2048);
    stick_pack(factory + 0x43, 1536, 1536);
    stick_pack(factory + 0x46, 2048, 2048);
    stick_pack(factory + 0x49, 1536, 1536);
    stick_pack(factory + 0x4c, 1536, 1536);
    memset(factory + 0x50, 0x32, 12); /* neutral body, buttons and grips */
    for (unsigned axis = 0; axis < 3; ++axis)
        put16(factory + 0x80 + axis * 2, 0);
    /* Device stick parameters: deadzone 160, range ratio 0xfff. */
    stick_pack(factory + 0x86, 160, 4095);
    stick_pack(factory + 0x98, 160, 4095);
    for (size_t i = 0; i < n; ++i)
        if (address + i >= 0x6000u && address + i < 0x60a0u)
            out[i] = factory[address + i - 0x6000u];
    return true;
}
void pro_usb_init(pro_usb_t *u, const uint8_t mac[6]) {
    memset(u, 0, sizeof(*u));
    memcpy(u->mac, mac, 6);
    static const uint8_t stop[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    memcpy(u->rumble, stop, 8);
    u->player_leds=1;
}
size_t pro_usb_output(pro_usb_t *u, const bridge_t *b, const uint8_t *r, size_t n, uint32_t now,
                      uint8_t out[64]) {
    if (n < 1u || n > 64u)
        return 0;
    if ((r[0] == 0x01 && n < 11u) || (r[0] == 0x10 && n < 10u))
        return 0;
    memset(out, 0, 64);
    if (r[0] == 0x80) {
        if (n < 2u)
            return 0;
        out[0] = 0x81;
        out[1] = r[1];
        switch (r[1]) {
        case 0x01:
            out[3] = 0x03;
            for (unsigned i = 0; i < 6; ++i)
                out[4 + i] = u->mac[5u - i];
            return 64;
        case 0x02:
        case 0x03:
            return 64;
        case 0x04:
            u->usb_only = true;
            return 0;
        case 0x05:
            u->usb_only = false;
            return 0;
        default:
            return 0;
        }
    }
    if ((r[0] == 0x01 || r[0] == 0x10) && n >= 10u) {
        memcpy(u->rumble, r + 2, 8);
        ++u->rumble_sequence;
        u->events |= PRO_EVENT_RUMBLE;
    }
    if (r[0] == 0x10 || r[0] != 0x01 || n < 11u)
        return 0;
    bridge_report30(b, false, now, out);
    out[0] = 0x21;
    out[14] = r[10];
    switch (r[10]) {
    case 0x00:
        out[13] = 0x80;
        out[15] = 3;
        break;
    case 0x02:
        out[13] = 0x82;
        out[15] = 3;
        out[16] = 0x48;
        out[17] = 3;
        out[18] = 2;
        memcpy(out + 19, u->mac, 6);
        out[25] = 1;
        out[26] = 1;
        break;
    case 0x03:
        if (n < 12u || r[11] != 0x30)
            break;
        u->streaming = true;
        out[13] = 0x80;
        break;
    case 0x10:
        if (n < 16u || r[15] == 0 || !pro_usb_spi_read(read32(r + 11), out + 20, r[15]))
            break;
        out[13] = 0x90;
        memcpy(out + 15, r + 11, 5);
        break;
    case 0x30:
        if (n < 12u)
            break;
        u->player_leds = r[11];
        u->events |= PRO_EVENT_PLAYER_LED;
        out[13] = 0x80;
        break;
    case 0x31:
        out[13] = 0xb0;
        out[15] = u->player_leds;
        break;
    case 0x38:
        if (n < 15u) break;
        memset(u->home_led,0,sizeof(u->home_led));
        memcpy(u->home_led,r+11,n-11u<sizeof(u->home_led) ? n-11u : sizeof(u->home_led));
        ++u->home_sequence;
        out[13]=0x80;
        break;
    case 0x40:
        if (n < 12u || r[11] > 1u)
            break;
        u->imu_enabled = r[11] != 0;
        u->events |= PRO_EVENT_IMU;
        out[13] = 0x80;
        break;
    case 0x41:
        /* Canonical range only until the physical sensor adapter exists.
         * Never ACK a sensitivity we cannot actually provide. */
        if (n >= 15u && r[11] == 3 && r[12] == 0 && r[13] == 1 && r[14] == 1)
            out[13] = 0x80;
        break;
    case 0x48:
        if (n < 12u || r[11] > 1u)
            break;
        u->vibration_enabled = r[11] != 0;
        ++u->rumble_sequence;
        u->events |= PRO_EVENT_RUMBLE;
        out[13] = 0x80;
        break;
    default:
        break; /* Explicit NACK. SPI writes/erase/MCU modes not implemented. */
    }
    return 64;
}
