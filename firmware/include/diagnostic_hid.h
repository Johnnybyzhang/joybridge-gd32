#ifndef REDEV_DIAGNOSTIC_HID_H
#define REDEV_DIAGNOSTIC_HID_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DIAGNOSTIC_VERSION 0x00010029u
typedef struct {
    uint8_t uid[12];
    bool boot_requested;
    uint32_t boot_requested_ms;
} diagnostic_hid_t;
/* Report 80 / F0: ping. 80 / F1: return to original loader.
 * Both require 8BDI at bytes 2..5; F1 additionally requires BOOT at 6..9.
 * Bytes 10..13 are an echoed host nonce. No command writes flash. */
size_t diagnostic_hid_output(diagnostic_hid_t *state, const uint8_t *report, size_t length,
                             uint32_t now, uint8_t reply[64]);
#endif
