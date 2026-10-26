#ifndef REDEV_SDK_USB_H
#define REDEV_SDK_USB_H
#include "pro_usb.h"
#include "usb_identity.h"
#include "usbd_core.h"

typedef struct {
    usb_identity_t identity;
    bool detached, identity_waiting;
    uint32_t identity_since, attach_at;
    bridge_t *bridge;
    pro_usb_t *protocol;
    uint8_t rx[64], control_rx[64], tx[64], last_input[64];
    uint8_t battery_feature[2];
    volatile uint16_t rx_length;
    volatile bool rx_pending, tx_busy;
    volatile bool control_receiving, control_pending;
    uint8_t control_id, control_length;
    uint32_t next_order, rx_order, control_order;
    uint32_t control_reports, interrupt_reports;
    uint8_t idle, hid_protocol;
    uint32_t last_sequence, last_input_ms;
    bool stale_report_sent;
    size_t (*output_handler)(void *argument, const uint8_t *report, size_t length, uint32_t now,
                             uint8_t reply[64]);
    void *output_argument;
} sdk_usb_t;

extern usb_desc sdk_usb_descriptors;
extern usb_class sdk_usb_class;
/* Call with the USB IRQ disabled and initialized bridge/protocol objects.
 * Wraps official usbd_init and installs the bounded SETUP callback afterward.
 * Clock, pull-up, IRQ routing and real low-level driver remain board work. */
void sdk_usb_init(usb_dev *device, sdk_usb_t *context, bridge_t *bridge, pro_usb_t *protocol, usb_identity_t identity);
/* Foreground, nonblocking detach/re-enumeration; Bluetooth keeps running. */
void sdk_usb_identity_poll(usb_dev *device, usb_identity_t desired, uint32_t now);
void sdk_usb_setup(usb_dev *device, uint8_t endpoint);
/* Foreground only. Uses a short, bounded critical section to synchronize with
 * the SDK IRQ callbacks. Responses retain the OUT packet until IN is free. */
void sdk_usb_poll(usb_dev *device, uint32_t now);
#endif
