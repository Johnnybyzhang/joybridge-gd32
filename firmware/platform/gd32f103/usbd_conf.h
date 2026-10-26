#ifndef REDEV_USBD_CONF_H
#define REDEV_USBD_CONF_H
#include "gd32_sdk.h"
/* GigaDevice USBD (packet-memory peripheral), not the F105/107 USBFS core.
 * PB15 active-low pull-up control is established by the supplied bootloader
 * at 080022f4. Runtime replaces the SDK's active-high pull-up callback. */
#define USB_PULLUP GPIOB
#define USB_PULLUP_PIN GPIO_PIN_15
#define USBD_CFG_MAX_NUM 1U
#define USBD_ITF_MAX_NUM 1U
#define EP_COUNT 2U
#define USB_STRING_COUNT 4U
#define BTABLE_OFFSET 0U
#define EP0_RX_ADDR 0x40U
#define EP0_TX_ADDR 0x80U
#define HID_RX_ADDR 0xc0U
#define HID_TX_ADDR 0x100U
#endif
