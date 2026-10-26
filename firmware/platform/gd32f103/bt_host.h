#ifndef REDEV_BT_HOST_H
#define REDEV_BT_HOST_H
#include "bridge.h"
#include "pro_usb.h"
typedef struct {
    uint8_t state, error, pairing, connected, setup_stage, setup_error, led, storage_error;
    uint16_t inquiry_results, opens, closes, last_cid, last_opcode;
    uint32_t reports, published, assertions;
    uint32_t output_reports, rumble_reports;
    uint8_t last_rumble[8];
    uint8_t peer_address[6], last_report[16];
} bt_host_status_t;
extern bt_host_status_t bt_host_status;
void bt_host_init(bridge_t *bridge, pro_usb_t *usb, uint32_t now);
void bt_host_poll(uint32_t now);
void bt_host_request_pair(void);
void bt_host_request_slot(uint8_t slot);
void bt_host_shutdown(void);
void bt_host_trace_usb(uint8_t report_id, uint8_t command, uint8_t length);
void bt_host_rate_read(uint8_t page, uint8_t reply[64]);
/* Ready saved controllers in the active profile; independent of input activity. */
uint8_t bt_host_ready_mask(void);
/* Bounded metadata only: no addresses, keys or HCI/ACL payloads. Six records. */
uint8_t bt_host_trace_read(uint16_t *cursor, uint16_t *end, uint8_t out[48]);
void bt_host_receive(const uint8_t *packet, size_t length);
#endif
