#ifndef REDEV_USB_IDENTITY_H
#define REDEV_USB_IDENTITY_H
#include <stdint.h>
#include "bridge.h"
typedef enum { USB_ID_IDLE, USB_ID_PRO, USB_ID_JOYCONS } usb_identity_t;
#define USB_PID_PRO 0x2009u
#define USB_PID_JOYCONS 0x200eu
/* Local development identity, not a Nintendo-assigned product. */
#define USB_PID_IDLE 0x20ffu
usb_identity_t usb_identity_for_ready(uint8_t slot, uint8_t ready_mask);
usb_identity_t usb_identity_for_input(const bridge_t *bridge,uint8_t ready_mask);
uint16_t usb_identity_pid(usb_identity_t identity);
extern const uint8_t idle_usb_report_descriptor[29];
#endif
