#ifndef REDEV_RADIO_H
#define REDEV_RADIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { RADIO_OFF, RADIO_POWER_LOW, RADIO_RF_FIRST, RADIO_POWER_PAUSE,
       RADIO_POWER_RELEASE, RADIO_RF_SECOND, RADIO_CORE, RADIO_RF_TAIL,
       RADIO_PATCH, RADIO_CONDITIONAL, RADIO_CONDITIONAL_TAIL, RADIO_IDENTITY,
       RADIO_BAUD, RADIO_SET_BAUD, RADIO_QUERY, RADIO_READY, RADIO_ERROR };
typedef struct {
    uint8_t phase, variant, error, hci_version;
    uint16_t row, i2c_writes, hci_writes, events, manufacturer, lmp_subversion;
    uint16_t acl_length, acl_buffers;
    uint8_t address[6], features[8];
    uint8_t last_event[64], last_event_length;
} radio_status_t;
extern radio_status_t radio_status;
void radio_request_start(uint8_t variant);
void radio_set_packet_handler(void (*handler)(const uint8_t *, size_t));
void radio_poll(uint32_t now, const uint8_t identity[6]);
void radio_parser_errors(uint32_t errors[2]);
#endif
