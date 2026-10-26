#include "bridge.h"
#include "pro_usb.h"
/* Offline-only startup fixture. The flash tool only accepts hid-diagnostic. */
volatile uint32_t diagnostic_status;
uint32_t diagnostic_data = 0x12345678u;
uint8_t diagnostic_report[64];
static bridge_t bridge;
static pro_usb_t usb;
void app_main(void) {
    static const uint8_t test_mac[6] = {2, 0, 0, 0, 0, 1};
    bridge_init(&bridge, 0);
    pro_usb_init(&usb, test_mac);
    bridge_report30(&bridge, usb.imu_enabled, 0, diagnostic_report);
    diagnostic_status = diagnostic_data == 0x12345678u ? 0x52454459u : 0xbad00001u;
    for (;;)
        __asm volatile("wfi");
}
