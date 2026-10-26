#include "radio.h"
#include "hci_uart.h"
#include "gd32_sdk.h"
#include <string.h>

/* Real radio state machine/H4 parser, synthetic standard HCI controller. */
volatile hci_uart_status_t hci_uart_status;
volatile uint32_t radio_test_status;
/* Exercise initialized RAM as well as BSS in this standalone fixture. */
volatile uint32_t radio_fixture_data = 0x5876;
static uint8_t response[64];
static size_t response_size;
static unsigned vendor_commands, vendor_setup_commands, standard_commands;
static bool malformed;
bool hci_uart_init(uint32_t baud) { hci_uart_status.baud = baud; return true; }
bool hci_uart_set_baud(uint32_t baud) { hci_uart_status.baud = baud; return true; }
bool hci_uart_tx_idle(void) { return true; }
size_t hci_uart_read(uint8_t *data, size_t capacity) {
    size_t count = response_size < capacity ? response_size : capacity;
    memcpy(data, response, count); response_size = 0; return count;
}
bool hci_uart_send(const uint8_t *command, size_t length) {
    if (length < 4 || command[0] != 1 || length != (size_t)command[3] + 4) { malformed = true; return false; }
    uint16_t opcode = command[1] | ((uint16_t)command[2] << 8);
    if (opcode == 0xfd02) { vendor_commands++; return true; }
    if (opcode == 0xfc72) {
        if (length!=9 || (command[4]!=5 && command[4]!=1)) malformed=true;
        vendor_setup_commands++; return true;
    }
    standard_commands++;
    memset(response, 0, sizeof(response));
    response[0] = 4; response[1] = 0x0e; response[3] = 1;
    response[4] = command[1]; response[5] = command[2]; response_size = 7;
    switch (opcode) {
    case 0x1001:
        response_size = 15; response[7] = 4; response[10] = 4;
        response[11] = 0x57; response[13] = 0x76; response[14] = 0x58; break;
    case 0x1009:
        response_size = 13; memcpy(response + 7, "\x05\x04\x03\x02\x01\x02", 6); break;
    case 0x1005:
        response_size = 14; response[7] = 0xfd; response[8] = 3; response[10] = 8; break;
    case 0x1003:
        response_size = 15; response[7] = 0xff; response[8] = 0xff; break;
    case 0x0c13:
        if (memcmp(command + 4, "Nintendo Switch", 16) != 0) malformed = true;
        break;
    default: break;
    }
    response[2] = (uint8_t)(response_size - 3);
    return true;
}
void app_main(void) {
    static const uint8_t identity[] = {2,1,2,3,4,5};
    radio_request_start(0);
    for (uint32_t now = 0; now < 5000 && radio_status.phase != RADIO_READY; now += 10)
        radio_poll(now, identity);
    radio_test_status = radio_fixture_data == 0x5876 && !malformed && vendor_commands == 132 &&
        vendor_setup_commands == 2 && standard_commands == 8 &&
        radio_status.phase == RADIO_READY && radio_status.i2c_writes == 96 &&
        radio_status.acl_length == 1021 && radio_status.acl_buffers == 8 &&
        radio_status.manufacturer == 0x57 && radio_status.lmp_subversion == 0x5876 &&
        hci_uart_status.baud == 1500000 ? 0x52444159 : 0xbad00000 | radio_status.phase;
    for (;;) __WFI();
}
