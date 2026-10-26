#include "diagnostic_hid.h"
#include <string.h>
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(value >> (i * 8));
}
size_t diagnostic_hid_output(diagnostic_hid_t *state, const uint8_t *report, size_t length,
                             uint32_t now, uint8_t reply[64]) {
    if (length < 14 || length > 64 || report[0] != 0x80 ||
        (report[1] != 0xf0 && report[1] != 0xf1) || memcmp(report + 2, "8BDI", 4) != 0)
        return 0;
    if (report[1] == 0xf1 && memcmp(report + 6, "BOOT", 4) != 0)
        return 0;
    if (report[1] == 0xf1 && !state->boot_requested) {
        state->boot_requested = true;
        state->boot_requested_ms = now;
    }
    memset(reply, 0, 64);
    reply[0] = 0x81;
    reply[1] = report[1];
    memcpy(reply + 2, "8BDI", 4);
    reply[6] = 1;
    reply[7] = state->boot_requested ? 1 : 0;
    put32(reply + 8, now);
    put32(reply + 12, DIAGNOSTIC_VERSION);
    put32(reply + 16, 48000000);
    memcpy(reply + 20, state->uid, sizeof(state->uid));
    memcpy(reply + 32, report + 10, 4);
    return 64;
}
