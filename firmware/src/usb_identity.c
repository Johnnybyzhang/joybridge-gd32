#include "usb_identity.h"
/* Vendor application collection only: no joystick axes, buttons, or hat.
 * The same guarded diagnostic reports and bootloader return remain usable. */
const uint8_t idle_usb_report_descriptor[29]={
    0x06,0x00,0xff,0x09,0x01,0xa1,0x01,
    0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,
    0x85,0x81,0x09,0x01,0x81,0x02,
    0x85,0x80,0x09,0x02,0x91,0x02,0xc0};
usb_identity_t usb_identity_for_ready(uint8_t slot,uint8_t ready_mask) {
    if (!ready_mask) return USB_ID_IDLE;
    return slot==0 ? USB_ID_PRO : USB_ID_JOYCONS;
}
uint16_t usb_identity_pid(usb_identity_t identity) {
    return identity==USB_ID_PRO ? USB_PID_PRO : identity==USB_ID_JOYCONS ? USB_PID_JOYCONS : USB_PID_IDLE;
}
usb_identity_t usb_identity_for_input(const bridge_t *bridge,uint8_t ready_mask) {
    /* Completing HID setup is not evidence of a valid input report. Do not
     * attach a gamepad until the selected session has published real input. */
    for (unsigned i=0;i<2;i++)
        if (!bridge->live[i].valid) ready_mask&=(uint8_t)~(1u<<i);
    return usb_identity_for_ready(bridge->settings.active_slot,ready_mask);
}
