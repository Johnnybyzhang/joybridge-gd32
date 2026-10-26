#include "sdk_usb.h"
#include "diagnostic_hid.h"
#include "usbd_lld_core.h"
#include <string.h>

/* Offline fixture: real vendor enumeration, core and transaction code with
 * only the peripheral handler replaced. Never use this as receiver firmware. */
usb_core_drv usbd_core;
static usb_dev device;
static sdk_usb_t state;
static bridge_t bridge;
static pro_usb_t protocol;
static usb_req setup_packet;
static uint8_t packet[64];
static uint16_t packet_size;
static unsigned sends, receives, stalls, detach_count, attach_count;
static diagnostic_hid_t diag;
static void mock_pullup(FlagStatus enabled) { if (enabled==SET) attach_count++; else detach_count++; }
static size_t diag_output(void *arg,const uint8_t *r,size_t n,uint32_t now,uint8_t out[64]) {
    return diagnostic_hid_output(arg,r,n,now,out);
}
volatile uint32_t sdk_usb_test_status;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            sdk_usb_test_status = __LINE__;                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void mock_init(void) {}
static void mock_setup(usb_dev *dev, uint8_t kind, uint32_t address, const usb_desc_ep *desc) {
    (void)kind;
    (void)address;
    usb_transc *transc = EP_DIR(desc->bEndpointAddress) ? dev->transc_in : dev->transc_out;
    transc[EP_ID(desc->bEndpointAddress)].max_len = desc->wMaxPacketSize;
}
static void mock_write(uint8_t *data, uint8_t endpoint, uint16_t length) {
    (void)endpoint;
    packet_size = length;
    if (length <= sizeof(packet) && length != 0)
        memcpy(packet, data, length);
    sends++;
}
static uint16_t mock_read(uint8_t *data, uint8_t endpoint, uint8_t kind) {
    (void)endpoint;
    (void)kind;
    memcpy(data, &setup_packet, sizeof(setup_packet));
    return sizeof(setup_packet);
}
static void mock_receive(usb_dev *dev, uint8_t endpoint) {
    (void)dev;
    (void)endpoint;
    receives++;
}
static void mock_disable(usb_dev *dev, uint8_t endpoint) {
    (void)dev;
    (void)endpoint;
}
static void mock_stall(usb_dev *dev, uint8_t endpoint) {
    (void)dev;
    (void)endpoint;
    stalls++;
}
struct _usb_handler usbd_drv_handler = {.init = mock_init,
                                        .deinit = mock_init,
                                        .dp_pullup = mock_pullup,
                                        .ep_setup = mock_setup,
                                        .ep_write = mock_write,
                                        .ep_read = mock_read,
                                        .ep_rx_enable = mock_receive,
                                        .ep_disable = mock_disable,
                                        .ep_stall_set = mock_stall};

static void setup(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    setup_packet = (usb_req){.bmRequestType = type,
                             .bRequest = request,
                             .wValue = value,
                             .wIndex = index,
                             .wLength = length};
    device.ep_transc[0][TRANSC_SETUP](&device, 0);
}
static void input_complete(void) {
    device.ep_transc[1][TRANSC_IN](&device, 1);
}
static void output(const uint8_t *data, uint16_t length) {
    if (length <= sizeof(state.rx))
        memcpy(state.rx, data, length);
    device.transc_out[1].xfer_count = length;
    device.ep_transc[1][TRANSC_OUT](&device, 1);
}
static void run_tests(void) {
    static const uint8_t mac[] = {2, 0, 0, 0, 0, 1};
    bridge_init(&bridge, NULL);
    pro_usb_init(&protocol, mac);
    sdk_usb_init(&device, &state, &bridge, &protocol, USB_ID_PRO);
    device.transc_in[0].max_len = device.transc_out[0].max_len = 64;
    device.cur_status = USBD_DEFAULT;
    setup(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18);
    CHECK(packet_size == 18 && packet[8] == 0x7e && packet[9] == 5 && packet[10] == 9 &&
          packet[11] == 0x20);
    setup(0, USB_SET_ADDRESS, 1, 0, 0);
    setup(0, USB_SET_CONFIGURATION, 1, 0, 0);
    CHECK(device.cur_status == USBD_CONFIGURED && receives == 1);
    CHECK(device.transc_out[1].max_len == 64 && device.transc_in[1].max_len == 64);
    setup(0x81, USB_GET_DESCRIPTOR, 0x2200, 0, 0xffff);
    size_t descriptor_offset=0;
    for (;;) {
        CHECK(packet_size<=64 && packet_size>0);
        CHECK(memcmp(packet,pro_usb_report_descriptor+descriptor_offset,packet_size)==0);
        descriptor_offset+=packet_size;
        if (!device.transc_in[0].xfer_len) break;
        /* Same continuation as the real low-level IN-completion handler. */
        usbd_ep_send(&device,0,device.transc_in[0].xfer_buf,device.transc_in[0].xfer_len);
    }
    CHECK(descriptor_offset==pro_usb_report_descriptor_size);
    setup(0x81, USB_GET_DESCRIPTOR, 0x2100, 0, 9);
    CHECK(packet_size == 9 && packet[1] == 0x21);
    /* Standard Battery Strength is a read-only percentage feature, independent
     * of Nintendo report parsing and retained across stale control input. */
    bridge.settings.slots[0].kind=SLOT_PRO; bridge.live[0].valid=true;
    for (unsigned level=0;level<=8;level+=2) {
        bridge.live[0].sample.battery=(uint8_t)level;
        setup(0xa1,1,0x0390,0,64);
        CHECK(packet_size==2 && packet[0]==0x90 && packet[1]==level*100/8);
    }
    bridge.settings.active_slot=1; bridge.settings.slots[1].kind=SLOT_JOYCONS;
    bridge.live[1].valid=true; bridge.live[1].sample.battery=2;
    setup(0xa1,1,0x0390,0,2);
    CHECK(packet_size==2 && packet[1]==25); /* lower of the connected pair */
    bridge_init(&bridge,NULL);
    unsigned before = stalls;
    setup(0x80, 0xff, 0, 0, 64);             /* vendor dispatch-table bound */
    setup(0x82, USB_GET_STATUS, 0, 0xff, 2); /* endpoint-array bound */
    setup(0x80, USB_GET_DESCRIPTOR, 0x03ff, 0x409, 255);
    setup(0x80, USB_GET_DESCRIPTOR, 0, 0, 64);
    setup(0x21, 9, 0x0201, 0, 65); /* oversized SET_REPORT cannot receive */
    CHECK(stalls == before + 5 && device.transc_out[0].xfer_len == 0);
    before=stalls;
    setup(0x21,9,0x0390,0,2);
    CHECK(stalls==before+1); /* battery status is never host writable */

    static const uint8_t handshake[] = {0x80, 1};
    unsigned prior_sends = sends, prior_receives = receives;
    output(handshake, sizeof(handshake));
    CHECK(state.rx_pending && receives == prior_receives);
    sdk_usb_poll(&device, 10);
    CHECK(state.tx_busy && !state.rx_pending && packet[0] == 0x81 && packet[1] == 1);
    CHECK(sends == prior_sends + 1 && receives == prior_receives + 1);
    output(handshake, sizeof(handshake));
    sdk_usb_poll(&device, 11);
    CHECK(state.rx_pending && sends == prior_sends + 1); /* cannot overwrite pending IN */
    input_complete();
    sdk_usb_poll(&device, 12);
    CHECK(!state.rx_pending && sends == prior_sends + 2);
    input_complete();
    output(handshake, 65); /* corrupt low-level length is also bounded */
    sdk_usb_poll(&device, 13);
    CHECK(!state.rx_pending && !state.tx_busy);

    /* Windows HidD_SetOutputReport uses control SET_REPORT. Real SDK OUT0
     * dispatch must defer its status ACK until foreground consumes the data. */
    uint8_t rumble[10]={0x10,0,0x28,0x61,0x42,0x50,0x4c,0x41,0x40,0x48};
    setup(0x21,9,0x0210,0,sizeof(rumble));
    CHECK(state.control_receiving && device.transc_out[0].xfer_len==10);
    memcpy(state.control_rx,rumble,sizeof(rumble)); device.transc_out[0].xfer_count=10;
    prior_sends=sends;
    device.ep_transc[0][TRANSC_OUT](&device,0);
    CHECK(state.control_pending && sends==prior_sends); /* no premature status */
    sdk_usb_poll(&device,14);
    CHECK(!state.control_pending && state.control_reports==1 && sends==prior_sends+1);
    CHECK(memcmp(protocol.rumble,rumble+2,8)==0);
    uint32_t sequence=protocol.rumble_sequence;
    output(rumble,10);
    setup(0x21,9,0x0210,0,10);
    memcpy(state.control_rx,rumble,10); state.control_rx[2]=0x4c;
    device.transc_out[0].xfer_count=10;
    device.ep_transc[0][TRANSC_OUT](&device,0);
    sdk_usb_poll(&device,15);
    CHECK(state.control_pending && protocol.rumble[0]==0x28 && protocol.rumble_sequence==sequence+1);
    sdk_usb_poll(&device,16);
    CHECK(!state.control_pending && protocol.rumble[0]==0x4c && protocol.rumble_sequence==sequence+2);
    setup(0x21,9,0x0210,0,10);
    memcpy(state.control_rx,rumble,10); device.transc_out[0].xfer_count=9;
    before=stalls;
    device.ep_transc[0][TRANSC_OUT](&device,0);
    CHECK(stalls==before+1 && !state.control_pending); /* short data cannot leak */
    setup(0x21,9,0x0210,0,10);
    CHECK(state.control_receiving);
    setup(0xa1,2,0,0,1); /* replacement SETUP aborts incomplete OUT cleanly */
    CHECK(!state.control_receiving && !state.control_pending);

    protocol.streaming = true;
    CHECK(device.desc->config_desc[33]==1); /* full-speed IN polling: 1 ms */
    bridge.input_sequence=1;
    sdk_usb_poll(&device, 20);
    CHECK(state.tx_busy && packet_size == 64 && packet[0] == 0x30);
    prior_sends=sends;
    sdk_usb_poll(&device,20);
    CHECK(sends==prior_sends); /* never overwrite an IN transfer */
    input_complete();
    prior_sends = sends;
    sdk_usb_poll(&device, 20); /* USB completion can precede the next 10 ms tick */
    CHECK(sends == prior_sends); /* no replay even with IMU disabled */
    bridge.input_sequence++;
    sdk_usb_poll(&device,21);
    CHECK(sends==prior_sends+1); input_complete();
    protocol.imu_enabled=true;
    bridge.input_sequence++;
    sdk_usb_poll(&device,36); input_complete();
    prior_sends=sends;
    sdk_usb_poll(&device,44);
    CHECK(sends==prior_sends); /* Never replay an old IMU batch as new motion. */
    bridge.input_sequence++;
    sdk_usb_poll(&device,45);
    CHECK(sends==prior_sends+1); input_complete();
    prior_sends=sends;
    sdk_usb_poll(&device,45+BRIDGE_STALE_MS);
    CHECK(sends==prior_sends+1); input_complete();
    sdk_usb_poll(&device,45+2*BRIDGE_STALE_MS);
    CHECK(sends==prior_sends+1); /* one stale safety report, no idle stream */
    (void)device.class_core->deinit(&device, 1);
    CHECK(!protocol.streaming && !protocol.imu_enabled);

    state.output_handler=diag_output; state.output_argument=&diag;
    CHECK(usb_identity_for_ready(0,0)==USB_ID_IDLE && usb_identity_for_ready(1,0)==USB_ID_IDLE);
    CHECK(usb_identity_for_ready(0,1)==USB_ID_PRO && usb_identity_for_ready(1,3)==USB_ID_JOYCONS);
    CHECK(usb_identity_for_ready(1,1)==USB_ID_JOYCONS);
    CHECK(usb_identity_for_input(&bridge,3)==USB_ID_IDLE);
    bridge.live[0].valid=true;
    CHECK(usb_identity_for_input(&bridge,1)==USB_ID_PRO);
    CHECK(usb_identity_for_input(&bridge,2)==USB_ID_IDLE);
    bridge.settings.active_slot=1;
    CHECK(usb_identity_for_input(&bridge,1)==USB_ID_JOYCONS);
    bridge.live[0].valid=false;
    CHECK(usb_identity_for_input(&bridge,3)==USB_ID_IDLE);
    bridge.settings.active_slot=0;
    state.tx_busy=true;
    sdk_usb_identity_poll(&device,USB_ID_IDLE,100);
    CHECK(!state.detached);
    sdk_usb_identity_poll(&device,USB_ID_IDLE,149);
    CHECK(!state.detached);
    sdk_usb_identity_poll(&device,USB_ID_IDLE,150);
    CHECK(state.detached && detach_count==1 && !protocol.streaming && !protocol.vibration_enabled);
    sdk_usb_identity_poll(&device,USB_ID_IDLE,399);
    CHECK(state.detached);
    sdk_usb_identity_poll(&device,USB_ID_IDLE,400);
    /* Host bus reset initializes EP0 in the physical low-level driver. */
    device.transc_in[0].max_len=device.transc_out[0].max_len=64;
    device.cur_status=USBD_DEFAULT;
    CHECK(!state.detached && state.identity==USB_ID_IDLE && attach_count==1);
    CHECK(state.output_handler==diag_output && state.output_argument==&diag);
    CHECK(device.desc->dev_desc[10]==0xff && device.desc->dev_desc[11]==0x20);
    CHECK(device.desc->config_desc[25]==sizeof(idle_usb_report_descriptor));
    setup(0x81,USB_GET_DESCRIPTOR,0x2200,0,64);
    CHECK(packet_size==sizeof(idle_usb_report_descriptor));
    CHECK(memcmp(packet,idle_usb_report_descriptor,packet_size)==0);
    CHECK(packet[0]==0x06 && packet[1]==0 && packet[2]==0xff);
    setup(0,USB_SET_ADDRESS,3,0,0); setup(0,USB_SET_CONFIGURATION,1,0,0);
    CHECK(device.cur_status==USBD_CONFIGURED);
    protocol.streaming=true;
    prior_sends=sends; sdk_usb_poll(&device,401); CHECK(sends==prior_sends);
    output(handshake,sizeof(handshake)); sdk_usb_poll(&device,402); CHECK(sends==prior_sends);
    uint8_t boot[64]={0x80,0xf1,'8','B','D','I','B','O','O','T',1,2,3,4};
    output(boot,sizeof(boot)); sdk_usb_poll(&device,403);
    CHECK(diag.boot_requested && packet[0]==0x81 && packet[1]==0xf1);
    input_complete();
    sdk_usb_identity_poll(&device,USB_ID_JOYCONS,500);
    CHECK(state.detached);
    sdk_usb_identity_poll(&device,USB_ID_JOYCONS,750);
    /* Host bus reset initializes EP0 in the physical low-level driver. */
    device.transc_in[0].max_len=device.transc_out[0].max_len=64;
    device.cur_status=USBD_DEFAULT;
    CHECK(state.identity==USB_ID_JOYCONS && device.desc->dev_desc[10]==0x0e);
    CHECK(device.desc->config_desc[25]==pro_usb_report_descriptor_size && !state.tx_busy && !state.rx_pending);
    CHECK(!protocol.streaming && !protocol.vibration_enabled);
    setup(0,USB_SET_ADDRESS,4,0,0); setup(0,USB_SET_CONFIGURATION,1,0,0);
    sdk_usb_identity_poll(&device,USB_ID_PRO,800);
    sdk_usb_identity_poll(&device,USB_ID_PRO,1050);
    /* Host bus reset initializes EP0 in the physical low-level driver. */
    device.transc_in[0].max_len=device.transc_out[0].max_len=64;
    device.cur_status=USBD_DEFAULT;
    CHECK(state.identity==USB_ID_PRO && device.desc->dev_desc[10]==0x09);
    CHECK(detach_count==3 && attach_count==3);
    sdk_usb_test_status = 0x53444b59;
}
void app_main(void) {
    run_tests();
    for (;;)
        __asm volatile("wfi");
}
