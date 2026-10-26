#include "radio.h"
#include "gd32_sdk.h"
#include "h4.h"
#include "hci_uart.h"
#include "rda5876.h"
#include "../../generated/rda_tables.h"
#include <string.h>

radio_status_t radio_status;
static h4_parser_t parser;
void radio_parser_errors(uint32_t errors[2]) { errors[0]=parser.bad_type; errors[1]=parser.oversized; }
static bool start_pending, query_waiting;
static uint8_t requested_variant, query_index;
static uint32_t due, query_sent, init_started;
static uint16_t query_opcode;
static void (*host_packet_handler)(const uint8_t *, size_t);
void radio_set_packet_handler(void (*handler)(const uint8_t *, size_t)) { host_packet_handler=handler; }
static const uint16_t queries[] = {0x0c03, 0x1001, 0x1009, 0x1005, 0x1003,
                                  0x0c13, 0x0c24, 0x0c1a};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void fail(uint8_t error) { radio_status.error = error; radio_status.phase = RADIO_ERROR; }
static void delay_half_clock(void) {
    /* A conservative software I2C clock; no periodic interrupt needed. */
    for (volatile unsigned n = 0; n < 80; n++)
        __NOP();
}
static void scl(bool high) {
    if (high) gpio_bit_set(GPIOB, GPIO_PIN_10); else gpio_bit_reset(GPIOB, GPIO_PIN_10);
    delay_half_clock();
}
static void sda(bool high) {
    if (high) gpio_bit_set(GPIOB, GPIO_PIN_11); else gpio_bit_reset(GPIOB, GPIO_PIN_11);
    delay_half_clock();
}
static bool i2c_byte(uint8_t value) {
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_11);
    for (unsigned bit = 0; bit < 8; bit++) {
        sda((value & 0x80) != 0); scl(true); scl(false); value <<= 1;
    }
    gpio_init(GPIOB, GPIO_MODE_IPU, GPIO_OSPEED_10MHZ, GPIO_PIN_11);
    scl(true);
    bool ack = gpio_input_bit_get(GPIOB, GPIO_PIN_11) == RESET;
    scl(false);
    gpio_bit_set(GPIOB, GPIO_PIN_11);
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_11);
    return ack;
}
static bool rf_write(uint8_t reg, uint16_t value) {
    uint8_t bytes[4]; rda5876_rf_bytes(reg, value, bytes);
    sda(true); scl(true); sda(false); scl(false);
    bool ok = true;
    for (unsigned i = 0; i < 4 && ok; i++) ok = i2c_byte(bytes[i]);
    sda(false); scl(true); sda(true); scl(false);
    if (ok) radio_status.i2c_writes++;
    return ok;
}
static void packet_received(void *argument, const uint8_t *packet, size_t length) {
    (void)argument;
    radio_status.events++;
    radio_status.last_event_length = (uint8_t)(length < 64 ? length : 64);
    memcpy(radio_status.last_event, packet, radio_status.last_event_length);
    if (radio_status.phase == RADIO_READY && host_packet_handler) {
        host_packet_handler(packet, length);
        return;
    }
    if (radio_status.phase != RADIO_QUERY || !query_waiting || length < 7 ||
        packet[0] != 4 || packet[1] != 0x0e || le16(packet + 4) != query_opcode)
        return;
    if (packet[6] != 0) { fail(3); return; }
    switch (query_opcode) {
    case 0x1001:
        if (length < 15) { fail(4); return; }
        radio_status.hci_version = packet[7];
        radio_status.manufacturer = le16(packet + 11);
        radio_status.lmp_subversion = le16(packet + 13);
        break;
    case 0x1009:
        if (length < 13) { fail(4); return; }
        memcpy(radio_status.address, packet + 7, 6);
        break;
    case 0x1005:
        if (length < 14) { fail(4); return; }
        radio_status.acl_length = le16(packet + 7);
        radio_status.acl_buffers = le16(packet + 10);
        if (radio_status.acl_length == 0 || radio_status.acl_buffers == 0) { fail(5); return; }
        break;
    case 0x1003:
        if (length < 15) { fail(4); return; }
        memcpy(radio_status.features, packet + 7, 8);
        break;
    default: break;
    }
    query_waiting = false;
    query_index++;
    if (query_index == sizeof(queries) / sizeof(queries[0])) radio_status.phase = RADIO_READY;
}
void radio_request_start(uint8_t variant) {
    if (variant <= 1) { requested_variant = variant; start_pending = true; }
}
static void phase(uint8_t next, uint32_t now, uint32_t delay) {
    radio_status.phase = next; radio_status.row = 0; due = now + delay;
}
static bool hci_word(uint32_t address, uint32_t value, uint8_t mode, uint32_t now) {
    uint8_t command[14];
    rda5876_write_word(address, value, mode, command);
    if (!hci_uart_send(command, sizeof(command))) return false;
    radio_status.hci_writes++; due = now + 10;
    return true;
}
static void hci_table(const rda_word_t *rows, size_t count, uint8_t next, uint32_t now) {
    if (radio_status.row == count) { phase(next, now, 10); return; }
    const rda_word_t *row = &rows[radio_status.row];
    if (hci_word(row->address, row->value, 0, now)) radio_status.row++;
}
void radio_poll(uint32_t now, const uint8_t identity[6]) {
    if (start_pending) {
        start_pending = false;
        init_started = now;
        memset(&radio_status, 0, sizeof(radio_status));
        radio_status.variant = requested_variant;
        query_waiting = false; query_index = 0;
        h4_init(&parser, packet_received, NULL);
        if (!hci_uart_init(115200)) { fail(1); return; }
        rcu_periph_clock_enable(RCU_GPIOB);
        gpio_bit_set(GPIOB, GPIO_PIN_10 | GPIO_PIN_11);
        gpio_bit_reset(GPIOB, GPIO_PIN_13);
        gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_13);
        for (unsigned i = 0; i < 30; i++) { scl(false); scl(true); }
        phase(RADIO_POWER_LOW, now, 50);
    }
    if (radio_status.phase == RADIO_OFF || radio_status.phase == RADIO_ERROR) return;
    if (radio_status.phase != RADIO_READY && (uint32_t)(now - init_started) >= 10000) {
        fail(8); return;
    }
    uint8_t received[256]; size_t count;
    while ((count = hci_uart_read(received, sizeof(received))) != 0)
        h4_feed(&parser, received, count);
    if (hci_uart_status.fault) { fail(6); return; }
    if ((int32_t)(now - due) < 0 || !hci_uart_tx_idle()) return;
    switch (radio_status.phase) {
    case RADIO_POWER_LOW: phase(RADIO_RF_FIRST, now, 0); break;
    case RADIO_RF_FIRST:
    case RADIO_RF_SECOND:
        if (radio_status.row < sizeof(rda_rf_i2c) / sizeof(rda_rf_i2c[0])) {
            const rda_word_t *row = &rda_rf_i2c[radio_status.row];
            if (!rf_write((uint8_t)row->address, (uint16_t)row->value)) { fail(2); break; }
            radio_status.row++; due = now + 1;
        } else if (radio_status.phase == RADIO_RF_FIRST) {
            gpio_bit_reset(GPIOB, GPIO_PIN_13);
            phase(RADIO_POWER_PAUSE, now, 50);
        } else phase(RADIO_CORE, now, 20);
        break;
    case RADIO_POWER_PAUSE:
        gpio_bit_set(GPIOB, GPIO_PIN_13); phase(RADIO_POWER_RELEASE, now, 50); break;
    case RADIO_POWER_RELEASE: phase(RADIO_RF_SECOND, now, 0); break;
    case RADIO_CORE:
        hci_table(rda_core_initial, sizeof(rda_core_initial) / sizeof(rda_core_initial[0]), RADIO_RF_TAIL, now); break;
    case RADIO_RF_TAIL:
        if (radio_status.row < 2) {
            if (!rf_write(0x30, radio_status.row == 0 ? 0x129 : 0x12b)) { fail(2); break; }
            radio_status.row++; due = now + 10;
        } else phase(RADIO_CONDITIONAL, now, 10);
        break;
    case RADIO_PATCH:
        hci_table(rda_patches, sizeof(rda_patches) / sizeof(rda_patches[0]), RADIO_IDENTITY, now); break;
    case RADIO_CONDITIONAL:
        hci_table(rda_controller_config, sizeof(rda_controller_config) / sizeof(rda_controller_config[0]), RADIO_CONDITIONAL_TAIL, now);
        break;
    case RADIO_CONDITIONAL_TAIL:
        if (radio_status.row<sizeof(rda_vendor_setup)/sizeof(rda_vendor_setup[0])) {
            if (hci_uart_send(rda_vendor_setup[radio_status.row],sizeof(rda_vendor_setup[0]))) {
                radio_status.hci_writes++; radio_status.row++; due=now+10;
            }
        } else phase(RADIO_PATCH,now,10);
        break;
    case RADIO_IDENTITY: {
        uint32_t value;
        if (radio_status.row == 0)
            value = (uint32_t)identity[0] | ((uint32_t)identity[1] << 8) | ((uint32_t)identity[2] << 16) | ((uint32_t)identity[3] << 24);
        else if (radio_status.row == 1) value = identity[4] | ((uint32_t)identity[5] << 8);
        else value = 6;
        uint32_t address = radio_status.row == 0 ? 0x80000044 : radio_status.row == 1 ? 0x80000048 : 0x80000040;
        if (hci_word(address, value, 0x80, now) && ++radio_status.row == 3) phase(RADIO_BAUD, now, 10);
        break;
    }
    case RADIO_BAUD:
        if (radio_status.row < 2) {
            uint8_t commands[2][14]; rda5876_baud_packets(commands);
            if (hci_uart_send(commands[radio_status.row], 14)) {
                radio_status.hci_writes++; radio_status.row++; due = now + 10;
            }
        } else phase(RADIO_SET_BAUD, now, 10);
        break;
    case RADIO_SET_BAUD:
        if (hci_uart_set_baud(1500000)) {
            h4_init(&parser, packet_received, NULL);
            phase(RADIO_QUERY, now, 50);
        }
        break;
    case RADIO_QUERY:
        if (query_waiting) {
            if ((uint32_t)(now - query_sent) >= 2000) fail(7);
        } else {
            query_opcode = queries[query_index];
            uint8_t command[252] = {1, (uint8_t)query_opcode, (uint8_t)(query_opcode >> 8), 0};
            if (query_opcode == 0x0c13) {
                command[3] = 248;
                memcpy(command + 4, "Nintendo Switch", sizeof("Nintendo Switch") - 1);
            } else if (query_opcode == 0x0c24) {
                command[3] = 3; command[4] = 8; command[5] = 0x25;
            } else if (query_opcode == 0x0c1a) {
                command[3] = 1; command[4] = 3; /* diagnostic inquiry + page scan */
            }
            if (hci_uart_send(command, 4u + command[3])) {
                query_waiting = true; query_sent = now; radio_status.hci_writes++;
            }
        }
        break;
    default: break;
    }
}
