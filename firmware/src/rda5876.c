#include "rda5876.h"
static void put32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (uint8_t)(value >> (8u * i));
}
size_t rda5876_write_word(uint32_t address, uint32_t value, uint8_t mode, uint8_t out[14]) {
    if (mode != 0 && mode != 0x80)
        return 0;
    out[0] = 1;
    out[1] = 2;
    out[2] = 0xfd;
    out[3] = 10;
    out[4] = mode;
    out[5] = 1;
    put32(out + 6, address);
    put32(out + 10, value);
    return 14;
}
void rda5876_baud_packets(uint8_t out[2][14]) {
    rda5876_write_word(0x80000060u, RDA_HCI_RUN_BAUD, 0x80, out[0]);
    rda5876_write_word(0x80000040u, 0x0602b300u, 0x80, out[1]);
}
void rda5876_rf_bytes(uint8_t reg, uint16_t value, uint8_t out[4]) {
    out[0] = 0x2c;
    out[1] = reg;
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}
