#ifndef REDEV_PRO_USB_H
#define REDEV_PRO_USB_H
#include "bridge.h"
enum { PRO_EVENT_RUMBLE = 1, PRO_EVENT_IMU = 2, PRO_EVENT_PLAYER_LED = 4 };
typedef struct {
    uint8_t mac[6], rumble[8], player_leds;
    uint8_t events;
    uint32_t rumble_sequence;
    uint32_t home_sequence;
    uint8_t home_led[38];
    bool imu_enabled, vibration_enabled, streaming, usb_only;
} pro_usb_t;
extern const uint8_t pro_usb_device_descriptor[18];
extern const uint8_t pro_usb_configuration_descriptor[41];
extern const uint8_t pro_usb_report_descriptor[];
extern const size_t pro_usb_report_descriptor_size;
void pro_usb_init(pro_usb_t *usb, const uint8_t mac[6]);
/* Entire USB OUT report including ID. Returns response size, or zero for
 * no response/malformed packet. Caller owns a 64-byte response buffer and
 * must backpressure OUT until any response is safely queued. */
size_t pro_usb_output(pro_usb_t *usb, const bridge_t *bridge, const uint8_t *report, size_t length,
                      uint32_t now, uint8_t response[64]);
/* Virtual, read-only calibration flash. Never exposes real MCU flash/keys. */
bool pro_usb_spi_read(uint32_t address, uint8_t *out, size_t size);
#endif
