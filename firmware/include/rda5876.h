#ifndef REDEV_RDA5876_H
#define REDEV_RDA5876_H
#include <stddef.h>
#include <stdint.h>
#define RDA_HCI_START_BAUD 115200u
#define RDA_HCI_RUN_BAUD 1500000u
/* Emit recovered vendor memory-write command. Mode is 0 for setup and 0x80
 * for the recovered baud sequence. There are no local-MCU memory writes. */
size_t rda5876_write_word(uint32_t controller_address, uint32_t value, uint8_t mode,
                          uint8_t out[14]);
void rda5876_baud_packets(uint8_t out[2][14]);
/* RF write transaction: address byte 0x2c, register, big-endian word. */
void rda5876_rf_bytes(uint8_t reg, uint16_t value, uint8_t out[4]);
#endif
