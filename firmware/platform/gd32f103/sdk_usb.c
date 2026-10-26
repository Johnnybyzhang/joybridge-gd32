#include "sdk_usb.h"
#include "usbd_enum.h"
#include "usbd_transc.h"
#include <string.h>

/* The SDK's descriptors API predates const. Its enumeration code only reads
 * these descriptor arrays; leave application descriptors in flash. */
static uint16_t language[] = {0x0304, 0x0409};
static uint16_t manufacturer[] = {0x0312, 'N', 'i', 'n', 't', 'e', 'n', 'd', 'o'};
static uint16_t product[] = {0x031e, 'P', 'r', 'o', ' ', 'C', 'o', 'n',
                             't',    'r', 'o', 'l', 'l', 'e', 'r'};
/* Official usbd_init populates 12 UTF16 hex digits from the GD32 unique ID. */
static uint16_t serial[13] = {0x031a};
static uint8_t *strings[] = {(uint8_t *)language, (uint8_t *)manufacturer, (uint8_t *)product,
                             (uint8_t *)serial};
static uint8_t device_descriptor[18], configuration_descriptor[41];
usb_desc sdk_usb_descriptors = {.dev_desc = device_descriptor,
                                .config_desc = configuration_descriptor,
                                .strings = strings};
static sdk_usb_t *context(usb_dev *device) {
    return device->user_data;
}

static uint8_t class_init(usb_dev *device, uint8_t config) {
    (void)config;
    sdk_usb_t *state = context(device);
    uint8_t mac[6];
    memcpy(mac, state->protocol->mac, sizeof(mac));
    pro_usb_init(state->protocol, mac);
    state->rx_pending = state->tx_busy = false;
    state->control_receiving=state->control_pending=false;
    state->idle = 0;
    state->hid_protocol = 1;
    bridge_report30(state->bridge, false, 0, state->last_input);
    usbd_ep_init(device, EP_BUF_SNG, HID_TX_ADDR,
                 (const usb_desc_ep *)(configuration_descriptor + 27));
    usbd_ep_init(device, EP_BUF_SNG, HID_RX_ADDR,
                 (const usb_desc_ep *)(configuration_descriptor + 34));
    device->ep_transc[1][TRANSC_IN] = sdk_usb_class.data_in;
    device->ep_transc[1][TRANSC_OUT] = sdk_usb_class.data_out;
    usbd_ep_recev(device, EP_OUT(1), state->rx, sizeof(state->rx));
    return USBD_OK;
}
static uint8_t class_deinit(usb_dev *device, uint8_t config) {
    (void)config;
    context(device)->rx_pending = context(device)->tx_busy = false;
    context(device)->control_receiving=context(device)->control_pending=false;
    context(device)->protocol->streaming = false;
    context(device)->protocol->imu_enabled = false;
    usbd_ep_deinit(device, EP_IN(1));
    usbd_ep_deinit(device, EP_OUT(1));
    return USBD_OK;
}
static uint8_t class_request(usb_dev *device, usb_req *req) {
    sdk_usb_t *state = context(device);
    if (req->wIndex != 0)
        return REQ_NOTSUPP;
    uint8_t *data = NULL;
    uint16_t length = 0;
    if (req->bmRequestType==0x21 && req->bRequest==9 &&
        (req->wValue>>8)==2 && req->wLength>0 && req->wLength<=64 &&
        device->cur_status==USBD_CONFIGURED) {
        uint8_t id=(uint8_t)req->wValue;
        if (state->identity==USB_ID_IDLE ? id!=0x80 : (id!=1 && id!=0x10 && id!=0x80 && id!=0x82)) return REQ_NOTSUPP;
        state->control_id=id; state->control_length=(uint8_t)req->wLength;
        state->control_receiving=true;
        usb_transc_config(&device->transc_out[0],state->control_rx,req->wLength,0);
        return REQ_SUPP;
    }
    if (req->bmRequestType == 0x81 && req->bRequest == USB_GET_DESCRIPTOR) {
        if (req->wValue == 0x2200) {
            data = (uint8_t *)(state->identity==USB_ID_IDLE ? idle_usb_report_descriptor : pro_usb_report_descriptor);
            length = state->identity==USB_ID_IDLE ? sizeof(idle_usb_report_descriptor) : (uint16_t)pro_usb_report_descriptor_size;
        } else if (req->wValue == 0x2100) {
            data = (uint8_t *)configuration_descriptor + 18;
            length = 9;
        }
    } else if (req->bmRequestType == 0xa1) {
        if (req->bRequest == 1 && req->wValue == 0x0130 && state->identity!=USB_ID_IDLE) {
            data = state->last_input;
            length = sizeof(state->last_input);
        } else if (req->bRequest == 1 && req->wValue == 0x0390 && state->identity!=USB_ID_IDLE) {
            controller_sample_t sample;
            /* Battery metadata survives short input gaps; this snapshot time
             * cannot cause a stale held button to be returned in a feature. */
            bridge_snapshot(state->bridge,state->last_input_ms,&sample);
            state->battery_feature[0]=0x90;
            state->battery_feature[1]=(uint8_t)((unsigned)sample.battery*100u/8u);
            data=state->battery_feature; length=sizeof(state->battery_feature);
        } else if (req->bRequest == 2 && req->wValue == 0) {
            data = &state->idle;
            length = 1;
        } else if (req->bRequest == 3 && req->wValue == 0) {
            data = &state->hid_protocol;
            length = 1;
        }
    } else if (req->bmRequestType == 0x21 && req->wLength == 0) {
        if (req->bRequest == 10 && (uint8_t)req->wValue == 0) {
            state->idle = (uint8_t)(req->wValue >> 8);
            return REQ_SUPP;
        }
        if (req->bRequest == 11 && req->wValue == 1)
            return REQ_SUPP;
    }
    if (data == NULL || req->wLength == 0)
        return REQ_NOTSUPP;
    usb_transc_config(&device->transc_in[0], data, USB_MIN(length, req->wLength), 0);
    return REQ_SUPP;
}
static void class_in(usb_dev *device, uint8_t endpoint) {
    if (endpoint == 1)
        context(device)->tx_busy = false;
}
static void class_out(usb_dev *device, uint8_t endpoint) {
    sdk_usb_t *state = context(device);
    if (endpoint != 1 || state->rx_pending)
        return;
    state->rx_length = device->transc_out[1].xfer_count;
    state->rx_pending = true;
    state->rx_order=state->next_order++;
    /* Leave OUT NAK until foreground processing safely queues the response. */
}
static uint8_t class_control_out(usb_dev *device) {
    sdk_usb_t *state=context(device);
    if (!state->control_receiving) return USBD_OK;
    state->control_receiving=false;
    /* Defer status ACK until foreground owns/handles this complete report.
     * This backpressures EP0 without overwriting the interrupt OUT buffer. */
    device->control.ctl_state=USBD_CTL_IDLE;
    if (device->transc_out[0].xfer_count!=state->control_length ||
        state->control_rx[0]!=state->control_id) {
        usb_stall_transc(device); return USBD_OK;
    }
    state->control_pending=true; state->control_order=state->next_order++;
    return USBD_OK;
}
usb_class sdk_usb_class = {.init = class_init,
                           .deinit = class_deinit,
                           .req_process = class_request,
                           .data_in = class_in,
                           .data_out = class_out,
                           .ctlx_out = class_control_out};

void sdk_usb_init(usb_dev *device, sdk_usb_t *state, bridge_t *bridge, pro_usb_t *protocol, usb_identity_t identity) {
    memset(device, 0, sizeof(*device));
    memset(state, 0, sizeof(*state));
    state->identity=identity;
    memcpy(device_descriptor,pro_usb_device_descriptor,sizeof(device_descriptor));
    memcpy(configuration_descriptor,pro_usb_configuration_descriptor,sizeof(configuration_descriptor));
    uint16_t pid=usb_identity_pid(identity);
    device_descriptor[10]=(uint8_t)pid; device_descriptor[11]=(uint8_t)(pid>>8);
    if (identity==USB_ID_IDLE) configuration_descriptor[25]=sizeof(idle_usb_report_descriptor);
    state->bridge = bridge;
    state->protocol = protocol;
    device->user_data = state;
    usbd_init(device, &sdk_usb_descriptors, &sdk_usb_class);
    device->ep_transc[0][TRANSC_SETUP] = sdk_usb_setup;
}

/* Bound the official standard-request dispatch table and endpoint arrays
 * before entering the SDK. Keep the vendor sources unmodified. */
static bool standard_valid(const usb_req *req) {
    uint8_t recipient = req->bmRequestType & 0x1f;
    if (req->bRequest > USB_SYNCH_FRAME || recipient > USB_RECPTYPE_EP)
        return false;
    if (recipient == USB_RECPTYPE_EP && (req->wIndex & ~0x0081u) != 0)
        return false;
    if (recipient == USB_RECPTYPE_ITF && req->wIndex != 0)
        return false;
    switch (req->bRequest) {
    case USB_GET_STATUS:
        return (req->bmRequestType & 0x80) && req->wLength == 2 && req->wValue == 0;
    case USB_CLEAR_FEATURE:
    case USB_SET_FEATURE:
        return !(req->bmRequestType & 0x80) && req->wLength == 0;
    case USB_SET_ADDRESS:
        return req->bmRequestType == 0 && req->wIndex == 0 && req->wLength == 0 &&
               req->wValue < 128;
    case USB_GET_DESCRIPTOR:
        if (req->bmRequestType == 0x81)
            return req->wValue == 0x2100 || req->wValue == 0x2200;
        if (req->bmRequestType != 0x80)
            return false;
        if ((req->wValue >> 8) == USB_DESCTYPE_STR)
            return (req->wValue & 255) < USB_STRING_COUNT;
        return req->wValue == 0x0100 || req->wValue == 0x0200;
    case USB_GET_CONFIGURATION:
        return req->bmRequestType == 0x80 && req->wValue == 0 && req->wIndex == 0 &&
               req->wLength == 1;
    case USB_SET_CONFIGURATION:
        return req->bmRequestType == 0 && req->wIndex == 0 && req->wLength == 0 && req->wValue <= 1;
    case USB_GET_INTERFACE:
        return req->bmRequestType == 0x81 && req->wValue == 0 && req->wLength == 1;
    default:
        return false;
    }
}
void sdk_usb_setup(usb_dev *device, uint8_t endpoint) {
    (void)endpoint;
    usb_req *req = &device->control.req;
    uint16_t count = device->drv_handler->ep_read((uint8_t *)req, 0, EP_BUF_SNG);
    usb_reqsta status = REQ_NOTSUPP;
    context(device)->control_receiving=context(device)->control_pending=false;
    /* An aborted prior control transfer must never leak its buffer/length. */
    usb_transc_config(&device->transc_in[0], NULL, 0, 0);
    usb_transc_config(&device->transc_out[0], NULL, 0, 0);
    device->control.ctl_zlp = 0;
    if (count == USB_SETUP_PACKET_LEN) {
        if ((req->bmRequestType & USB_REQTYPE_MASK) == USB_REQTYPE_STRD && standard_valid(req))
            status = usbd_standard_request(device, req);
        else if ((req->bmRequestType & USB_REQTYPE_MASK) == USB_REQTYPE_CLASS && req->wIndex == 0)
            status = usbd_class_request(device, req);
    }
    if (status != REQ_SUPP) {
        device->control.ctl_state = USBD_CTL_IDLE;
        usb_stall_transc(device);
    } else if (req->wLength == 0) {
        usb_ctl_status_in(device);
    } else if (req->bmRequestType & 0x80) {
        usb_ctl_data_in(device);
    } else {
        usb_ctl_data_out(device);
    }
}
void sdk_usb_poll(usb_dev *device, uint32_t now) {
    uint32_t interrupt_mask = __get_PRIMASK();
    __disable_irq();
    sdk_usb_t *state = context(device);
    if (device->cur_status == USBD_CONFIGURED && !state->tx_busy) {
        if (state->rx_pending || state->control_pending) {
            bool control=state->control_pending && (!state->rx_pending ||
                (int32_t)(state->control_order-state->rx_order)<0);
            const uint8_t *report=control ? state->control_rx : state->rx;
            size_t report_length=control ? state->control_length : state->rx_length;
            size_t length = 0;
            bool allowed=state->identity!=USB_ID_IDLE ||
                (report_length>=2 && report[0]==0x80 && report[1]>=0xf0);
            if (allowed && report_length <= sizeof(state->rx)) {
                if (state->output_handler != NULL)
                    length = state->output_handler(state->output_argument, report,
                                                   report_length, now, state->tx);
                else
                    length = pro_usb_output(state->protocol, state->bridge, report,
                                            report_length, now, state->tx);
            }
            if (control) { state->control_pending=false; state->control_reports++; }
            else { state->rx_pending=false; state->interrupt_reports++; }
            if (length) {
                state->tx_busy = true;
                usbd_ep_send(device, EP_IN(1), state->tx, (uint16_t)length);
            }
            if (control) usb_ctl_status_in(device);
            else usbd_ep_recev(device, EP_OUT(1), state->rx, sizeof(state->rx));
        } else if (state->identity!=USB_ID_IDLE && state->protocol->streaming &&
                   (state->last_sequence!=state->bridge->input_sequence ||
                    (!state->stale_report_sent && (uint32_t)(now-state->last_input_ms)>=BRIDGE_STALE_MS))) {
            bridge_report30(state->bridge, state->protocol->imu_enabled, now, state->last_input);
            memcpy(state->tx, state->last_input, sizeof(state->tx));
            /* Forward fresh samples in either IMU mode. The 1 ms IN interval
             * lets the host collect them promptly; no periodic sample replay.
             * One neutral safety report releases controls if input goes stale. */
            state->stale_report_sent=state->last_sequence==state->bridge->input_sequence;
            state->last_sequence=state->bridge->input_sequence;
            state->last_input_ms=now;
            state->tx_busy = true;
            usbd_ep_send(device, EP_IN(1), state->tx, sizeof(state->tx));
        }
    }
    __set_PRIMASK(interrupt_mask);
}

void sdk_usb_identity_poll(usb_dev *device,usb_identity_t desired,uint32_t now) {
    sdk_usb_t *state=context(device);
    if (!state->detached) {
        if (state->identity==desired) { state->identity_waiting=false; return; }
        if (!state->identity_waiting) { state->identity_waiting=true; state->identity_since=now; }
        /* Let a diagnostic command's ACK finish, but a stalled host must not
         * keep an absent gamepad registered indefinitely. */
        if (state->tx_busy && (uint32_t)(now-state->identity_since)<50) return;
        uint32_t mask=__get_PRIMASK(); __disable_irq();
        if (device->cur_status==USBD_CONFIGURED) class_deinit(device,1);
        usbd_disconnect(device); usbd_core_deinit(device);
        state->detached=true; state->attach_at=now+250;
        state->protocol->streaming=false;
        state->protocol->vibration_enabled=false;
        __set_PRIMASK(mask);
        return;
    }
    if ((int32_t)(now-state->attach_at)<0) return;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    bridge_t *bridge=state->bridge; pro_usb_t *protocol=state->protocol;
    size_t (*handler)(void *,const uint8_t *,size_t,uint32_t,uint8_t[64])=state->output_handler;
    void *argument=state->output_argument;
    sdk_usb_init(device,state,bridge,protocol,desired);
    state->output_handler=handler; state->output_argument=argument;
    usbd_connect(device);
    __set_PRIMASK(mask);
}
