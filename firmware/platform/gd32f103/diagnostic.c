#include "board.h"
#include "bt_host.h"
#include "diagnostic_hid.h"
#include "hardware.h"
#include "hci_uart.h"
#include "radio.h"
#include "sdk_usb.h"
#include "settings_flash.h"
#include "usbd_lld_core.h"
#include "usbd_lld_int.h"
#include <string.h>

/* HID-accessible bring-up firmware. USB uses the official GigaDevice drivers. */
static usb_dev device;
static sdk_usb_t usb_state;
static bridge_t bridge;
static pro_usb_t protocol;
static diagnostic_hid_t diagnostic;
static uint8_t radio_identity[6];
static uint8_t led_test_channel;
static uint32_t led_test_until;
/* PB9 is red, PB8 green; PB5 is the remaining blue RGB channel. */
static const uint8_t profile_leds[2]={LED_CHANNEL_PB5,LED_CHANNEL_PB8};
static volatile uint32_t milliseconds;
volatile uint32_t diagnostic_status;
static uint16_t product[] = {0x032c, '8', 'B', 'i', 't', 'D', 'o', ' ', 'H', 'I', 'D',
                             ' ',    'D', 'i', 'a', 'g', 'n', 'o', 's', 't', 'i', 'c'};
static uint16_t manufacturer[] = {0x031a, '8', 'B', 'i', 't', 'D', 'o',
                                  ' ',    'R', 'e', 'd', 'e', 'v'};

void SysTick_Handler(void) {
    milliseconds += 10;
}
uint32_t diagnostic_millis(void) {
    /* Interpolate the existing 100 Hz tick with the hardware down-counter.
     * USB/idle IRQ wakeups need 1 ms resolution, not another periodic IRQ. */
    uint32_t base,remaining;
    do { base=milliseconds; remaining=SysTick->VAL; } while (base!=milliseconds);
    return base+(SysTick->LOAD-remaining)/48000u;
}
void USBD_LP_CAN0_RX0_IRQHandler(void) {
    usbd_isr();
}
void USBD_HP_CAN0_TX_IRQHandler(void) {
    usbd_int_hpst();
}
void Default_Handler(void) {
    diagnostic_enter_bootloader();
}

static size_t output(void *argument, const uint8_t *report, size_t length, uint32_t now,
                     uint8_t reply[64]) {
    if (length && length<=64 &&
        !(report[0]==0x80 && length>=2 && report[1]>=0xf0)) {
        uint8_t command=report[0]==1 && length>10 ? report[10] :
                        report[0]==0x80 && length>1 ? report[1] : 0;
        bt_host_trace_usb(report[0],command,(uint8_t)length);
    }
    if (length>=15 && length<=64 && report[0]==0x80 && report[1]==0xfa &&
        memcmp(report+2,"8BDI",4)==0 && report[14]<=8) {
        memset(reply,0,64); reply[0]=0x81; reply[1]=0xfa; memcpy(reply+2,"8BDI",4);
        if (report[14]!=2) bt_host_rate_read(report[14],reply);
        if (!report[14]) {
            uint32_t errors[2]; radio_parser_errors(errors); memcpy(reply+48,errors,8);
            memcpy(reply+56,(const void *)&hci_uart_status.uart_errors,4);
            memcpy(reply+60,(const void *)&hci_uart_status.dma_errors,4);
        } else if (report[14]==2) {
            uint32_t words[]={hci_uart_status.idle_wakes,hci_uart_status.idle_intervals,
                hci_uart_status.idle_total_us,hci_uart_status.idle_max_us,hci_uart_status.rx_high_water};
            memcpy(reply+8,words,sizeof(words));
        }
        memcpy(reply+32,report+10,4); return 64;
    }
    if (length>=14 && length<=64 && report[0]==0x80 && report[1]==0xf9 &&
        memcmp(report+2,"8BDI",4)==0) {
        memset(reply,0,64); reply[0]=0x81; reply[1]=0xf9; memcpy(reply+2,"8BDI",4);
        reply[6]=protocol.vibration_enabled; reply[7]=protocol.imu_enabled;
        reply[8]=protocol.streaming; reply[9]=bridge.settings.active_slot;
        memcpy(reply+16,&protocol.rumble_sequence,4); memcpy(reply+20,protocol.rumble,8);
        memcpy(reply+32,report+10,4);
        memcpy(reply+36,&bt_host_status.output_reports,4);
        memcpy(reply+40,&bt_host_status.rumble_reports,4);
        memcpy(reply+44,&usb_state.control_reports,4); memcpy(reply+48,&usb_state.interrupt_reports,4);
        memcpy(reply+52,bt_host_status.last_rumble,8); return 64;
    }
    if (length>=16 && length<=64 && report[0]==0x80 && report[1]==0xf7 &&
        memcmp(report+2,"8BDI",4)==0) {
        uint16_t cursor=(uint16_t)(report[14]|(uint16_t)report[15]<<8), end;
        uint8_t records[48]={0};
        uint8_t count=bt_host_trace_read(&cursor,&end,records);
        memset(reply,0,64); reply[0]=0x81; reply[1]=0xf7; memcpy(reply+2,"8BDI",4);
        reply[6]=count; reply[7]=128;
        memcpy(reply+8,records,24); memcpy(reply+36,records+24,24);
        memcpy(reply+32,report+10,4);
        memcpy(reply+60,&cursor,2); memcpy(reply+62,&end,2); return 64;
    }
    if (length>=15 && length<=64 && report[0]==0x80 && report[1]==0xf8 &&
        memcmp(report+2,"8BDISLOT",8)==0 && report[14]<BRIDGE_SLOT_COUNT) {
        bt_host_request_slot(report[14]);
        memset(reply,0,64); reply[0]=0x81; reply[1]=0xf8; memcpy(reply+2,"8BDI",4);
        reply[6]=report[14]; memcpy(reply+32,report+10,4); return 64;
    }
    if (length>=16 && length<=64 && report[0]==0x80 && report[1]==0xf6 &&
        memcmp(report+2,"8BDILEDS",8)==0 && report[14]<=7 && report[15]<=30) {
        led_test_channel=report[14]; led_test_until=now+(uint32_t)report[15]*1000;
        memset(reply,0,64); reply[0]=0x81; reply[1]=0xf6; memcpy(reply+2,"8BDI",4);
        memcpy(reply+32,report+10,4); return 64;
    }
    if (length >= 14 && length <= 64 && report[0] == 0x80 &&
        (report[1] == 0xf4 || report[1] == 0xf5) && memcmp(report+2,"8BDI",4)==0) {
        if (report[1]==0xf5) {
            if (memcmp(report+6,"PAIR",4)!=0) return 0;
            bt_host_request_pair();
        }
        memset(reply,0,64); reply[0]=0x81; reply[1]=report[1]; memcpy(reply+2,"8BDI",4);
        reply[6]=bt_host_status.state; reply[7]=bt_host_status.error;
        reply[8]=bridge.pairing; reply[9]=bt_host_status.connected;
        reply[10]=bt_host_status.setup_stage; reply[11]=bt_host_status.setup_error;
        reply[12]=bt_host_status.led; reply[13]=bridge.settings.active_slot;
        reply[14]=bridge.settings.slots[bridge.settings.active_slot].kind;
        reply[15]=board_button_pressed();
        uint16_t words[]={bt_host_status.inquiry_results,bt_host_status.opens,bt_host_status.closes,
                          bt_host_status.last_cid,bt_host_status.last_opcode};
        memcpy(reply+16,words,sizeof(words)); memcpy(reply+26,bt_host_status.peer_address,6);
        memcpy(reply+32,report+10,4);
        memcpy(reply+36,&bt_host_status.reports,4); memcpy(reply+40,&bt_host_status.published,4);
        memcpy(reply+44,bt_host_status.last_report,16);
        reply[60]=profile_leds[bridge.settings.active_slot]; reply[61]=bt_host_status.storage_error;
        return 64;
    }
    if (length >= 14 && length <= 64 && report[0] == 0x80 &&
        (report[1] == 0xf2 || report[1] == 0xf3) && memcmp(report + 2, "8BDI", 4) == 0) {
        if (report[1] == 0xf3) {
            if (length < 15 || memcmp(report + 6, "RDA!", 4) != 0 || report[14] > 1)
                return 0;
            radio_request_start(report[14]);
        }
        memset(reply, 0, 64);
        reply[0] = 0x81; reply[1] = report[1]; memcpy(reply + 2, "8BDI", 4);
        reply[6] = radio_status.phase; reply[7] = radio_status.error;
        reply[8] = radio_status.variant; reply[9] = radio_status.hci_version;
        uint16_t words[] = {radio_status.row, radio_status.i2c_writes, radio_status.hci_writes,
                            radio_status.events, radio_status.manufacturer, radio_status.lmp_subversion,
                            radio_status.acl_length, radio_status.acl_buffers};
        memcpy(reply + 10, words, sizeof(words));
        memcpy(reply + 26, radio_status.address, 6);
        memcpy(reply + 32, report + 10, 4);
        memcpy(reply + 36, radio_status.features, 8);
        uint32_t counts[] = {hci_uart_status.baud, hci_uart_status.rx_bytes,
                             hci_uart_status.tx_bytes, hci_uart_status.overruns};
        memcpy(reply + 44, counts, sizeof(counts));
        reply[60] = radio_status.last_event_length;
        reply[61] = radio_status.last_event[0]; reply[62] = radio_status.last_event[1];
        reply[63] = radio_status.last_event[2];
        return 64;
    }
    if (length >= 2 && report[0] == 0x80 && report[1] >= 0xf0)
        return diagnostic_hid_output(argument, report, length, now, reply);
    return pro_usb_output(&protocol, &bridge, report, length, now, reply);
}

void app_main(void) {
    if (!diagnostic_clock_init())
        diagnostic_enter_bootloader();
    board_gpio_init();
    gpio_bit_set(GPIOB, GPIO_PIN_15);
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_15);
    diagnostic_usb_pullup(RESET);
    (void)board_led_pwm_init(48000000, 7);
    board_led_pwm_select(profile_leds[0],16);
    memcpy(diagnostic.uid, (const void *)0x1ffff7e8, sizeof(diagnostic.uid));
    radio_identity[0] = 2;
    for (unsigned i = 1; i < 6; i++)
        radio_identity[i] = diagnostic.uid[i] ^ diagnostic.uid[i + 6];
    static const uint8_t test_mac[] = {2, 0, 0, 0, 0, 1};
    settings_t settings; uint32_t sequence;
    bool stored=settings_load(&settings_flash_io,&settings,&sequence);
    bridge_init(&bridge,stored ? &settings : NULL);
    pro_usb_init(&protocol, test_mac);
    /* Distinguish this build from real controllers sharing 057e:2009. */
    sdk_usb_descriptors.strings[1] = (uint8_t *)manufacturer;
    sdk_usb_descriptors.strings[2] = (uint8_t *)product;
    rcu_periph_clock_enable(RCU_USBD);
    rcu_periph_reset_enable(RCU_USBDRST);
    rcu_periph_reset_disable(RCU_USBDRST);
    usbd_drv_handler.dp_pullup = diagnostic_usb_pullup;
    sdk_usb_init(&device, &usb_state, &bridge, &protocol, USB_ID_IDLE);
    usb_state.output_handler = output;
    usb_state.output_argument = &diagnostic;
    /* No remote wake or deep sleep in this diagnostic. Avoid per-frame IRQs. */
    USBD_CTL &= ~(CTL_SOFIE | CTL_ESOFIE);
    NVIC_SetPriority(USBD_LP_CAN0_RX0_IRQn, 2);
    NVIC_SetPriority(USBD_HP_CAN0_TX_IRQn, 2);
    NVIC_EnableIRQ(USBD_LP_CAN0_RX0_IRQn);
    NVIC_EnableIRQ(USBD_HP_CAN0_TX_IRQn);
    /* 100 Hz diagnostic button/timeout service; PWM uses zero interrupts. */
    (void)SysTick_Config(480000);
    __enable_irq();
    while (milliseconds < 250)
        __WFI();
    usbd_connect(&device);
    diagnostic_status = 0x48494431;
    bool configured_once = false, radio_started = false, host_started = false;
    for (;;) {
        uint32_t now = diagnostic_millis();
        if (device.cur_status == USBD_CONFIGURED)
            configured_once = true;
        if (configured_once && !radio_started) {
            radio_request_start(0);
            radio_started = true;
        }
        radio_poll(now, radio_identity);
        if (radio_status.phase==RADIO_READY && !host_started) {
            bt_host_init(&bridge,&protocol,now);
            radio_set_packet_handler(bt_host_receive);
            host_started=true;
        }
        /* Consume newly arrived radio input before preparing the next USB IN. */
        sdk_usb_poll(&device, now);
        bridge_button(&bridge,board_button_pressed(),now);
        bridge_tick(&bridge,now);
        if (host_started) {
            if (diagnostic.boot_requested) bt_host_shutdown();
            bt_host_poll(now);
        }
        if (!diagnostic.boot_requested) {
            sdk_usb_identity_poll(&device,usb_identity_for_input(&bridge,bt_host_ready_mask()),now);
            /* Official core initialization re-enables these; keep normal
             * operation free of per-frame LED/service interrupts. */
            USBD_CTL &= ~(CTL_SOFIE | CTL_ESOFIE);
        }
        bt_host_status.led=bridge_led(&bridge,now);
        if ((int32_t)(now-led_test_until)<0)
            board_led_pwm_select(led_test_channel,64);
        else board_led_pwm_select(profile_leds[bridge.settings.active_slot],bt_host_status.led);
        if (!configured_once && now >= 15000)
            diagnostic_enter_bootloader();
        if (diagnostic.boot_requested) {
            uint32_t elapsed = now - diagnostic.boot_requested_ms;
            if ((!usb_state.tx_busy && elapsed >= 100 &&
                 (!host_started || bt_host_status.state==0)) || elapsed >= 500)
                diagnostic_enter_bootloader();
        }
        __WFI();
    }
}
