#include "bt_host.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"
#include "l2cap.h"
#include "classic/btstack_link_key_db.h"
#include "hci_uart.h"
#include "hardware.h"
#include "nintendo.h"
#include "settings_flash.h"
#include <string.h>

/* Nintendo uses the two fixed HID PSMs. BTstack owns HCI/security/L2CAP;
 * this adapter owns each address from ACL admission until ALL channels and
 * the ACL have gone. No SDP job, generic HID singleton, or scan-reset loop. */
enum { PEER_FREE, PEER_CONNECTING, PEER_CHANNELS, PEER_SETUP, PEER_READY, PEER_CLOSING };
#define PEER_COUNT 2u
#define HID_MTU 64u
#define PRO_QOS_LATENCY_US 8000u
typedef struct {
    uint8_t state, link_mode;
    uint16_t handle, control_cid, interrupt_cid, link_interval;
    bool control_open, interrupt_open, outgoing, key_valid, offered, disconnect_sent;
    bool send_requested;
    uint8_t slot, expected_kind;
    uint32_t epoch, since, full_reports, rumble_sequence, home_sequence;
    uint8_t qos_state, qos_status, qos_service;
    uint32_t qos_requested_ms, qos_latency_us;
    bond_t bond;
    nintendo_t controller;
} peer_t;
static peer_t peers[PEER_COUNT];
static bridge_t *core;
static pro_usb_t *usb;
static uint32_t now_ms, epoch, retry_at;
static bool initialized, shutting_down, pair_requested, inquiry, tx_pending;
static uint8_t slot_requested=0xff, output_turn;
static struct {
    bool valid, new_pair;
    uint8_t kind, page_scan_repetition;
    uint16_t clock_offset;
    bd_addr_t address;
} candidate;
/* Name requests are asynchronous HCI operations, not ACL peers. Retain the
 * operation across a cancelled pairing transaction and ignore its late reply. */
static struct { bool pending; uint32_t epoch; bd_addr_t address; } remote_name;
static uint8_t tx_frame[HCI_UART_TX_SIZE];
static void (*transport_handler)(uint8_t,uint8_t *,uint16_t);
static btstack_packet_callback_registration_t events;
bt_host_status_t bt_host_status;
static struct {
    uint32_t acl, reports, duplicate, short_report, rejected;
    uint32_t timer_ticks, timer_steps, timer_max, histogram[8];
} rate;
static uint32_t quiet_since, quiet_rx;
static bool quiet, append_full;
/* Read back the radio's page-scan configuration whenever the ACL count
 * changes. These standard read-only commands do not change link policy. */
static struct {
    uint8_t phase, links, enable, type, status;
    uint16_t interval, window;
    uint32_t sent_ms;
    bool waiting;
} scan_snapshot={.links=0xff};
static const hci_cmd_t read_scan_enable={0x0c19,""};
static bool connectable_requested=true;
static const hci_cmd_t read_page_scan_type={HCI_OPCODE_HCI_READ_PAGE_SCAN_TYPE,""};
static const hci_cmd_t *const scan_reads[]={
    &read_scan_enable, &hci_read_page_scan_activity, &read_page_scan_type
};

/* Historical record IDs stay stable. New records identify a peer by its
 * physical pool index or handle; no pairing keys are ever recorded. */
typedef struct { uint32_t ms; uint8_t event, status; uint16_t value; } trace_t;
static trace_t trace_records[128];
static uint16_t trace_next, trace_count;
enum { TRACE_COMMAND=1, TRACE_COMPLETE, TRACE_STATUS, TRACE_ACL_OPEN, TRACE_ACL_CLOSE,
       TRACE_AUTH, TRACE_KEY_GET, TRACE_KEY_PUT, TRACE_ADMIT, TRACE_HID_OPEN,
       TRACE_HID_CLOSE, TRACE_EPOCH, TRACE_CONNECT, TRACE_SAVE, TRACE_USB,
       TRACE_ROLE, TRACE_MODE, TRACE_ROLE_REQUEST, TRACE_TIMING_REQUEST, TRACE_QOS, TRACE_FLOW,
       TRACE_DISCOVER, TRACE_OWNER, TRACE_CHANNEL, TRACE_RELEASE, TRACE_WIRE_REQUEST };
static void trace(uint8_t event,uint8_t status,uint16_t value) {
    trace_records[trace_next%128]=(trace_t){now_ms,event,status,value};
    trace_next++;
    if (trace_count<128) trace_count++;
}
void bt_host_trace_usb(uint8_t report_id,uint8_t command,uint8_t length) {
    trace(TRACE_USB,report_id,(uint16_t)((uint16_t)command<<8|length));
}
uint8_t bt_host_trace_read(uint16_t *cursor,uint16_t *end,uint8_t out[48]) {
    uint16_t oldest=(uint16_t)(trace_next-trace_count);
    if (*cursor==0xffff || (uint16_t)(*cursor-oldest)>trace_count) *cursor=oldest;
    *end=trace_next;
    uint16_t count=(uint16_t)(trace_next-*cursor);
    if (count>6) count=6;
    for (unsigned i=0;i<count;i++)
        memcpy(out+i*8,&trace_records[(uint16_t)(*cursor+i)%128],8);
    return (uint8_t)count;
}

static uint32_t get_time(void) { return now_ms; }
static void timer_set(btstack_timer_source_t *timer,uint32_t delay) { timer->timeout=now_ms+delay; }
static void noop(void) {}
static const btstack_run_loop_t run_loop={
    .init=btstack_run_loop_base_init,
    .add_data_source=btstack_run_loop_base_add_data_source,
    .remove_data_source=btstack_run_loop_base_remove_data_source,
    .enable_data_source_callbacks=btstack_run_loop_base_enable_data_source_callbacks,
    .disable_data_source_callbacks=btstack_run_loop_base_disable_data_source_callbacks,
    .set_timer=timer_set, .add_timer=btstack_run_loop_base_add_timer,
    .remove_timer=btstack_run_loop_base_remove_timer, .execute=noop, .dump_timer=noop,
    .get_time_ms=get_time, .poll_data_sources_from_irq=noop,
    .execute_on_main_thread=btstack_run_loop_base_add_callback, .trigger_exit=noop
};
static void transport_init(const void *config) { (void)config; }
static int transport_open(void) { return 0; }
static int transport_close(void) { return 0; }
static void transport_register(void (*handler)(uint8_t,uint8_t *,uint16_t)) { transport_handler=handler; }
static int can_send(uint8_t type) { (void)type; return !tx_pending && hci_uart_tx_idle(); }
static int transport_send(uint8_t type,uint8_t *packet,int length) {
    if (length<0 || (size_t)length+1>sizeof(tx_frame) || !can_send(type)) return -1;
    tx_frame[0]=type; memcpy(tx_frame+1,packet,(size_t)length);
    if (!hci_uart_send(tx_frame,(size_t)length+1)) return -1;
    tx_pending=true;
    if (type==HCI_COMMAND_DATA_PACKET && length>=3) {
        bt_host_status.last_opcode=little_endian_read_16(packet,0);
        trace(TRACE_COMMAND,0,bt_host_status.last_opcode);
    }
    return 0;
}
static const hci_transport_t transport={
    .name="GD32 DMA H4", .init=transport_init, .open=transport_open,
    .close=transport_close, .register_packet_handler=transport_register,
    .can_send_packet_now=can_send, .send_packet=transport_send
};
void bt_host_receive(const uint8_t *packet,size_t length) {
    if (length>=5 && packet[0]==HCI_ACL_DATA_PACKET) rate.acl++;
    if (length==13 && packet[0]==HCI_EVENT_PACKET && packet[1]==HCI_EVENT_CONNECTION_REQUEST)
        trace(TRACE_WIRE_REQUEST,packet[12],little_endian_read_16(packet,3));
    if (transport_handler && length>1)
        transport_handler(packet[0],(uint8_t *)packet+1,(uint16_t)(length-1));
}
void btstack_assert_failed(const char *file,uint16_t line) {
    (void)file; (void)line;
    bt_host_status.assertions++;
    diagnostic_enter_bootloader();
}
static peer_t *by_address(const uint8_t address[6]) {
    for (unsigned i=0;i<PEER_COUNT;i++)
        if (peers[i].state && memcmp(peers[i].bond.address,address,6)==0) return &peers[i];
    return NULL;
}
static peer_t *by_cid(uint16_t cid) {
    if (!cid) return NULL;
    for (unsigned i=0;i<PEER_COUNT;i++)
        if (peers[i].state && (peers[i].control_cid==cid || peers[i].interrupt_cid==cid)) return &peers[i];
    return NULL;
}
static peer_t *by_handle(uint16_t handle) {
    if (handle==HCI_CON_HANDLE_INVALID) return NULL;
    for (unsigned i=0;i<PEER_COUNT;i++)
        if (peers[i].state && peers[i].handle==handle) return &peers[i];
    return NULL;
}
static const bond_t *saved_bond(const uint8_t address[6],unsigned *index) {
    const pairing_slot_t *slot=&core->settings.slots[core->settings.active_slot];
    unsigned count=slot->kind==SLOT_PRO ? 1u : slot->kind==SLOT_JOYCONS ? 2u : 0u;
    for (unsigned i=0;i<count;i++) if (memcmp(slot->peers[i].address,address,6)==0) {
        if (index) *index=i;
        return &slot->peers[i];
    }
    return NULL;
}
static peer_t *reserve_peer(const uint8_t address[6],bool new_pair,uint8_t kind) {
    peer_t *peer=by_address(address);
    if (peer) return peer->state!=PEER_CLOSING && peer->slot==core->settings.active_slot ? peer : NULL;
    for (unsigned i=0;i<PEER_COUNT;i++) if (peers[i].state==PEER_FREE) {
        peer=&peers[i]; memset(peer,0,sizeof(*peer));
        peer->state=PEER_CONNECTING; peer->handle=HCI_CON_HANDLE_INVALID;
        peer->epoch=core->epoch; peer->slot=core->settings.active_slot; peer->since=now_ms;
        peer->expected_kind=kind;
        peer->rumble_sequence=peer->home_sequence=UINT32_MAX;
        memcpy(peer->bond.address,address,6);
        const bond_t *bond=saved_bond(address,NULL);
        if (bond && !new_pair) {
            peer->bond=*bond; peer->key_valid=true; peer->offered=true;
            peer->expected_kind=bond->kind;
        }
        nintendo_init(&peer->controller,address);
        trace(TRACE_OWNER,(uint8_t)i,(uint16_t)((uint16_t)peer->slot<<8|peer->expected_kind));
        return peer;
    }
    return NULL;
}
static void close_peer(peer_t *peer) {
    if (peer->state==PEER_FREE || peer->state==PEER_CLOSING) return;
    unsigned index;
    if (saved_bond(peer->bond.address,&index) && peer->slot==core->settings.active_slot)
        bridge_disconnect(core,peer->epoch,index);
    peer->state=PEER_CLOSING;
    /* Foreground disconnects the ACL, including incomplete L2CAP setup.
     * Never forget a pending page or free a CID while BTstack still owns it. */
}
static bool retained_peer(peer_t *peer) {
    if (peer->slot!=core->settings.active_slot) return false;
    const bond_t *saved=saved_bond(peer->bond.address,NULL);
    if (saved && peer->key_valid && memcmp(saved,&peer->bond,sizeof(*saved))==0) return true;
    if (!core->pairing) return false;
    for (unsigned i=0;i<2;i++) if (core->pending_present[i] &&
        memcmp(&core->pending.peers[i],&peer->bond,sizeof(peer->bond))==0) return true;
    return false;
}
static void reconcile_peers(void) {
    for (unsigned i=0;i<PEER_COUNT;i++) {
        peer_t *peer=&peers[i];
        if (!peer->state || peer->state==PEER_CLOSING) continue;
        if (retained_peer(peer)) {
            peer->epoch=core->epoch;
            if (saved_bond(peer->bond.address,NULL)) peer->offered=true;
        } else close_peer(peer);
    }
}
static int accept_acl(bd_addr_t address,hci_link_type_t link_type) {
    peer_t *peer=by_address(address);
    bool allowed=link_type==HCI_LINK_TYPE_ACL && !shutting_down &&
        (saved_bond(address,NULL) || (core->pairing && peer && peer->slot==core->settings.active_slot));
    if (allowed) peer=reserve_peer(address,false,PEER_NONE);
    bool accepted=allowed && peer && peer->state!=PEER_CLOSING;
    trace(TRACE_ADMIT,accepted,accepted ? (uint16_t)(peer-peers) : 0xffff);
    return accepted;
}
static int key_get(bd_addr_t address,link_key_t key,link_key_type_t *type) {
    peer_t *peer=by_address(address);
    /* The ACL filter reserves incoming peers before authentication. New
     * pairing candidates intentionally do not reuse the old key. */
    if (!peer || peer->state==PEER_CLOSING || peer->slot!=core->settings.active_slot || !peer->key_valid) {
        trace(TRACE_KEY_GET,0,0); return 0;
    }
    memcpy(key,peer->bond.link_key,16); *type=(link_key_type_t)peer->bond.key_type;
    trace(TRACE_KEY_GET,1,(uint16_t)(peer-peers)); return 1;
}
static void key_put(bd_addr_t address,link_key_t key,link_key_type_t type) {
    peer_t *peer=by_address(address);
    if (!peer || peer->state==PEER_CLOSING) return;
    memcpy(peer->bond.link_key,key,16); peer->bond.key_type=(uint8_t)type; peer->key_valid=true;
    trace(TRACE_KEY_PUT,(uint8_t)type,(uint16_t)(peer-peers));
}
static void key_delete(bd_addr_t address) {
    peer_t *peer=by_address(address); if (peer) peer->key_valid=false;
}
static void key_local_address(bd_addr_t address) { (void)address; }
static const btstack_link_key_db_t key_db={.open=noop,.close=noop,.set_local_bd_addr=key_local_address,
    .get_link_key=key_get,.put_link_key=key_put,.delete_link_key=key_delete};

static void record_input_rate(peer_t *peer,const uint8_t *report,size_t length) {
    if (!length || report[0]!=0x30) return;
    rate.reports++;
    if (length<49) { rate.short_report++; return; }
    peer->full_reports++;
    if (!peer->controller.ready || !peer->controller.timer_valid) return;
    uint8_t delta=(uint8_t)(report[1]-peer->controller.timer);
    rate.histogram[delta<7 ? delta : 7]++;
    if (!delta) rate.duplicate++;
    else {
        rate.timer_ticks+=delta; rate.timer_steps++;
        if (delta>rate.timer_max) rate.timer_max=delta;
    }
}
static void packet_handler(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size);
static uint8_t nintendo_name_kind(const uint8_t *name,size_t length) {
    while (length && name[length-1]==0) length--;
    if (length==14 && !memcmp(name,"Pro Controller",14)) return PEER_PRO;
    if (length==11 && !memcmp(name,"Joy-Con (L)",11)) return PEER_LEFT;
    if (length==11 && !memcmp(name,"Joy-Con (R)",11)) return PEER_RIGHT;
    return PEER_NONE;
}
static bool kind_in_profile(uint8_t kind) {
    return kind && (core->settings.active_slot==0)==(kind==PEER_PRO);
}
static void send_output(peer_t *peer) {
    if (peer->state!=PEER_SETUP && peer->state!=PEER_READY) return;
    /* Basic L2CAP copies the bytes before returning; there is no borrowed
     * report pointer and one controller cannot overwrite another's output. */
    if (!l2cap_can_send_packet_now(peer->interrupt_cid)) return;
    uint8_t report[50]; report[0]=0xa2;
    size_t length=nintendo_command(&peer->controller,now_ms,report+1);
    if (!length) return;
    uint8_t result=l2cap_send(peer->interrupt_cid,report,(uint16_t)(length+1));
    if (!result) {
        nintendo_sent(&peer->controller,now_ms);
        bt_host_status.output_reports++;
        if (report[1]==0x10) bt_host_status.rumble_reports++;
        memcpy(bt_host_status.last_rumble,report+3,8);
    } else if (result!=BTSTACK_ACL_BUFFERS_FULL) {
        bt_host_status.error=result; close_peer(peer);
    }
}
static void packet_handler(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    if (type==L2CAP_DATA_PACKET) {
        peer_t *peer=by_cid(channel);
        if (!peer || peer->state==PEER_CLOSING || peer->epoch!=core->epoch || !size) return;
        if (channel==peer->control_cid) {
            /* HID virtual-cable unplug: tear down the link, retain offline bond. */
            if (packet[0]==0x15) close_peer(peer);
            return;
        }
        if (!peer->interrupt_open || packet[0]!=0xa1) return;
        packet++; size--; bt_host_status.reports++;
        record_input_rate(peer,packet,size);
        memset(bt_host_status.last_report,0,sizeof(bt_host_status.last_report));
        memcpy(bt_host_status.last_report,packet,size<16 ? size : 16);
        if (nintendo_input(&peer->controller,packet,size)) {
            unsigned index;
            if (bridge_admit(core,peer->bond.address,&index) &&
                bridge_publish(core,peer->epoch,index,&peer->controller.sample,now_ms)) bt_host_status.published++;
            else rate.rejected++;
        }
        return;
    }
    if (type!=HCI_EVENT_PACKET || size<2) return;
    bd_addr_t address;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        bt_host_status.state=btstack_event_state_get_state(packet);
        /* Power-on clears pending GAP tasks. Configure scan after that reset,
         * then the readback below observes the actual controller state. */
        if (bt_host_status.state==HCI_STATE_WORKING)
            gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
        break;
    case HCI_EVENT_COMMAND_COMPLETE:
        if (size>=6) trace(TRACE_COMPLETE,packet[5],little_endian_read_16(packet,3));
        if (size>=6 && little_endian_read_16(packet,3)==HCI_OPCODE_HCI_WRITE_SCAN_ENABLE) {
            /* Refresh after the write completes, not merely when queued. */
            scan_snapshot.waiting=false; scan_snapshot.phase=0;
        }
        if (size>=6 && scan_snapshot.waiting && scan_snapshot.phase<3 &&
            little_endian_read_16(packet,3)==scan_reads[scan_snapshot.phase]->opcode) {
            scan_snapshot.waiting=false; scan_snapshot.status=packet[5];
            unsigned minimum=scan_snapshot.phase==1 ? 10u : 7u;
            if (packet[5] || size<minimum) {
                if (!packet[5]) scan_snapshot.status=0xff;
                scan_snapshot.phase=0xff;
            } else {
                if (scan_snapshot.phase==0) scan_snapshot.enable=packet[6];
                else if (scan_snapshot.phase==1) {
                    scan_snapshot.interval=little_endian_read_16(packet,6);
                    scan_snapshot.window=little_endian_read_16(packet,8);
                } else scan_snapshot.type=packet[6];
                scan_snapshot.phase++;
            }
        }
        break;
    case HCI_EVENT_COMMAND_STATUS:
        if (size>=6) trace(TRACE_STATUS,packet[2],little_endian_read_16(packet,4));
        if (size>=6 && packet[2] && little_endian_read_16(packet,4)==HCI_OPCODE_HCI_QOS_SETUP) {
            for (unsigned i=0;i<PEER_COUNT;i++) if (peers[i].qos_state==4) {
                peers[i].qos_state=6; peers[i].qos_status=packet[2];
            }
        }
        break;
    case 0x0d: { /* Standard HCI QoS Setup Complete (21 parameter bytes). */
        if (size<23) break;
        peer_t *peer=by_handle(little_endian_read_16(packet,3));
        if (!peer || peer->state!=PEER_READY || peer->qos_state!=4) break;
        peer->qos_status=packet[2]; peer->qos_service=packet[6];
        peer->qos_latency_us=little_endian_read_32(packet,15);
        peer->qos_state=packet[2] ? 6 : 5;
        trace(TRACE_QOS,packet[2],peer->qos_latency_us<65536 ? (uint16_t)peer->qos_latency_us : 0xffff);
        break;
    }
    case HCI_EVENT_CONNECTION_COMPLETE: {
        if (size<13) break;
        hci_event_connection_complete_get_bd_addr(packet,address);
        peer_t *peer=by_address(address);
        uint16_t handle=little_endian_read_16(packet,3);
        trace(TRACE_ACL_OPEN,packet[2],handle);
        if (packet[2]) {
            if (peer) close_peer(peer);
            break;
        }
        if (!peer && saved_bond(address,NULL)) peer=reserve_peer(address,false,PEER_NONE);
        if (!peer) { gap_disconnect(handle); break; }
        peer->handle=handle;
        if (peer->state!=PEER_CLOSING) { peer->state=PEER_CHANNELS; peer->since=now_ms; }
        break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        if (size<6) break;
        uint16_t handle=little_endian_read_16(packet,3);
        trace(TRACE_ACL_CLOSE,packet[5],handle);
        peer_t *peer=by_handle(handle);
        if (peer) { close_peer(peer); peer->handle=HCI_CON_HANDLE_INVALID; }
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
        if (size>=5) trace(TRACE_AUTH,packet[2],little_endian_read_16(packet,3));
        break;
    case HCI_EVENT_ROLE_CHANGE:
        if (size>=10) trace(TRACE_ROLE,packet[2],packet[9]);
        break;
    case HCI_EVENT_MODE_CHANGE:
        if (size>=8) {
            peer_t *peer=by_handle(little_endian_read_16(packet,3));
            trace(TRACE_MODE,packet[5],little_endian_read_16(packet,6));
            if (peer && !packet[2]) {
                peer->link_mode=packet[5]; peer->link_interval=little_endian_read_16(packet,6);
            }
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet,address); gap_pin_code_negative(address); break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        hci_event_user_confirmation_request_get_bd_addr(packet,address);
        peer_t *peer=by_address(address);
        if (core->pairing && peer && peer->state!=PEER_CLOSING && !peer->offered)
            gap_ssp_confirmation_response(address);
        else gap_ssp_confirmation_negative(address);
        break;
    }
    case GAP_EVENT_INQUIRY_RESULT: {
        bt_host_status.inquiry_results++;
        if (candidate.valid) break;
        gap_event_inquiry_result_get_bd_addr(packet,address);
        if (by_address(address)) break;
        const bond_t *saved=saved_bond(address,NULL);
        if (!core->pairing) break;
        uint32_t cod=gap_event_inquiry_result_get_class_of_device(packet);
        if ((cod&0x1ffc)!=0x0508) break;
        uint8_t kind=PEER_NONE;
        if (gap_event_inquiry_result_get_name_available(packet)) {
            kind=nintendo_name_kind(gap_event_inquiry_result_get_name(packet),
                                    gap_event_inquiry_result_get_name_len(packet));
            if (!kind_in_profile(kind)) break;
        } else {
            if (saved) kind=saved->kind;
            /* A standard inquiry result carries no name. Resolve an unknown
             * peripheral after inquiry stops, before retiring any saved peer. */
        }
        memcpy(candidate.address,address,6); candidate.kind=kind; candidate.valid=true;
        candidate.new_pair=core->pairing;
        candidate.page_scan_repetition=gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
        candidate.clock_offset=gap_event_inquiry_result_get_clock_offset(packet)|0x8000;
        trace(TRACE_DISCOVER,kind,0); gap_inquiry_stop();
        break;
    }
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
        if (size<9 || !remote_name.pending) break;
        hci_event_remote_name_request_complete_get_bd_addr(packet,address);
        if (memcmp(address,remote_name.address,6)!=0) break;
        remote_name.pending=false;
        if (!core->pairing || remote_name.epoch!=core->epoch || !candidate.valid ||
            memcmp(address,candidate.address,6)!=0) break;
        uint8_t kind=packet[2] ? PEER_NONE : nintendo_name_kind(packet+9,size-9);
        trace(TRACE_DISCOVER,kind,packet[2]);
        if (kind_in_profile(kind)) candidate.kind=kind;
        else { candidate.valid=false; retry_at=now_ms+500; }
        break;
    }
    case GAP_EVENT_INQUIRY_COMPLETE:
        inquiry=false;
        retry_at=now_ms+((core->pairing || candidate.valid) ? 100u : 2000u);
        break;
    case L2CAP_EVENT_INCOMING_CONNECTION: {
        l2cap_event_incoming_connection_get_address(packet,address);
        uint16_t cid=l2cap_event_incoming_connection_get_local_cid(packet);
        uint16_t psm=l2cap_event_incoming_connection_get_psm(packet);
        peer_t *peer=by_address(address);
        if (!peer || peer->state==PEER_CLOSING || peer->epoch!=core->epoch) {
            l2cap_decline_connection(cid); break;
        }
        uint16_t *owned=psm==PSM_HID_CONTROL ? &peer->control_cid :
                        psm==PSM_HID_INTERRUPT ? &peer->interrupt_cid : NULL;
        if (!owned || *owned) { l2cap_decline_connection(cid); break; }
        *owned=cid; peer->handle=l2cap_event_incoming_connection_get_handle(packet);
        trace(TRACE_CHANNEL,(uint8_t)psm,cid);
        l2cap_accept_connection(cid);
        break;
    }
    case L2CAP_EVENT_CHANNEL_OPENED: {
        uint16_t cid=l2cap_event_channel_opened_get_local_cid(packet);
        peer_t *peer=by_cid(cid);
        uint8_t status=l2cap_event_channel_opened_get_status(packet);
        trace(TRACE_HID_OPEN,status,cid);
        if (!peer) { if (!status) l2cap_disconnect(cid); break; }
        if (status) {
            if (peer->control_cid==cid) peer->control_cid=0;
            if (peer->interrupt_cid==cid) peer->interrupt_cid=0;
            bt_host_status.error=status; close_peer(peer); break;
        }
        peer->handle=l2cap_event_channel_opened_get_handle(packet);
        if (peer->control_cid==cid) peer->control_open=true;
        else peer->interrupt_open=true;
        if (peer->state==PEER_CLOSING) break;
        if (l2cap_event_channel_opened_get_remote_mtu(packet)<HID_MTU) {
            bt_host_status.error=0xe1; close_peer(peer); break;
        }
        peer->since=now_ms;
        if (peer->control_open && peer->interrupt_open) {
            peer->state=PEER_SETUP; bt_host_status.opens++;
            bt_host_status.last_cid=peer->interrupt_cid;
            memcpy(bt_host_status.peer_address,peer->bond.address,6);
        }
        break;
    }
    case L2CAP_EVENT_CHANNEL_CLOSED: {
        uint16_t cid=l2cap_event_channel_closed_get_local_cid(packet);
        peer_t *peer=by_cid(cid);
        trace(TRACE_HID_CLOSE,0,cid);
        if (peer) {
            if (peer->control_cid==cid) { peer->control_cid=0; peer->control_open=false; }
            if (peer->interrupt_cid==cid) { peer->interrupt_cid=0; peer->interrupt_open=false; }
            close_peer(peer);
        }
        break;
    }
    case L2CAP_EVENT_CAN_SEND_NOW: {
        peer_t *peer=by_cid(l2cap_event_can_send_now_get_local_cid(packet));
        if (peer) { peer->send_requested=false; send_output(peer); }
        break;
    }
    default: break;
    }
}
static bool connections_busy(void) {
    for (unsigned i=0;i<PEER_COUNT;i++)
        if (peers[i].state && peers[i].state!=PEER_READY) return true;
    return false;
}
static void service_lifecycle(peer_t *peer) {
    if (!peer->state) return;
    hci_connection_t *connection=hci_connection_for_bd_addr_and_type(peer->bond.address,BD_ADDR_TYPE_ACL);
    if (peer->state!=PEER_CLOSING && peer->state!=PEER_READY && (uint32_t)(now_ms-peer->since)>=15000) {
        bt_host_status.error=0xe2; close_peer(peer);
    }
    if (peer->state==PEER_CLOSING) {
        if (connection && connection->state==OPEN && !peer->disconnect_sent) {
            peer->disconnect_sent=true; gap_disconnect(connection->con_handle);
        }
        if (!connection && !peer->control_cid && !peer->interrupt_cid) {
            trace(TRACE_RELEASE,(uint8_t)(peer-peers),peer->handle);
            memset(peer,0,sizeof(*peer)); bt_host_status.closes++;
        }
        return;
    }
    if (peer->outgoing && peer->control_open && !peer->interrupt_cid) {
        uint8_t result=l2cap_create_channel(packet_handler,peer->bond.address,PSM_HID_INTERRUPT,HID_MTU,&peer->interrupt_cid);
        if (result) { bt_host_status.error=result; close_peer(peer); }
    }
    if (peer->state!=PEER_SETUP && peer->state!=PEER_READY) return;
    nintendo_t *controller=&peer->controller;
    if (controller->error || (controller->kind &&
        ((peer->expected_kind && controller->kind!=peer->expected_kind) ||
         ((peer->slot==0)!=(controller->kind==PEER_PRO))))) {
        bt_host_status.error=controller->error ? 0x80|controller->error : 0xe3;
        close_peer(peer); return;
    }
    if (controller->ready) {
        peer->state=PEER_READY;
        if (core->pairing && !peer->offered && peer->key_valid &&
            gap_security_level(peer->handle)>=LEVEL_2) {
            peer->bond.kind=controller->kind;
            peer->offered=bridge_pair_offer(core,&peer->bond,now_ms);
            if (!peer->offered) close_peer(peer);
        }
    }
}
static void service_storage(void) {
    if (!core->settings_dirty || bt_host_status.storage_error || core->pairing || inquiry ||
        connections_busy() || !hci_uart_tx_idle() || tx_pending) return;
    btstack_linked_list_iterator_t iterator;
    hci_connections_get_iterator(&iterator);
    if (!btstack_linked_list_iterator_has_next(&iterator)) {
        if (!quiet || quiet_rx!=hci_uart_status.rx_bytes) {
            quiet=true; quiet_rx=hci_uart_status.rx_bytes; quiet_since=now_ms;
        } else if ((uint32_t)(now_ms-quiet_since)>=30) {
            bool saved=settings_save(&settings_flash_io,&core->settings);
            trace(TRACE_SAVE,saved ? 0 : 1,core->settings.active_slot);
            bt_host_status.storage_error=saved ? 0 : 1;
            if (saved) core->settings_dirty=false;
            append_full=false; quiet=false;
        }
    } else {
        quiet=false;
        if (append_full) return;
        settings_append_result_t saved=settings_append(&settings_flash_io,&core->settings);
        if (saved==SETTINGS_APPENDED) { core->settings_dirty=false; trace(TRACE_SAVE,0,core->settings.active_slot); }
        else if (saved==SETTINGS_APPEND_FULL) append_full=true;
        else { bt_host_status.storage_error=1; trace(TRACE_SAVE,1,core->settings.active_slot); }
    }
}
static void connect_peer(const uint8_t address[6],bool new_pair,uint8_t kind) {
    peer_t *peer=reserve_peer(address,new_pair,kind);
    if (!peer) return;
    peer->outgoing=true;
    uint8_t result=l2cap_create_channel(packet_handler,peer->bond.address,PSM_HID_CONTROL,HID_MTU,&peer->control_cid);
    trace(TRACE_CONNECT,result,peer->control_cid);
    if (result) { bt_host_status.error=result; close_peer(peer); }
    retry_at=now_ms+1000;
}
static void service_discovery(void) {
    /* Incoming reconnection can win the race after inquiry selected a
     * device. Its established channels must never be recreated. */
    if (candidate.valid && by_address(candidate.address)) candidate.valid=false;
    bool busy=connections_busy();
    if (inquiry && (busy || !core->pairing)) gap_inquiry_stop();
    if (inquiry || remote_name.pending ||
        (int32_t)(now_ms-retry_at)<0 || busy) return;
    /* On ordinary wake Nintendo HID devices page their saved host. Keep
     * page scan available instead of repeatedly paging those same devices.
     * Outgoing discovery/connection belongs only to explicit pairing. */
    if (!core->pairing) { candidate.valid=false; return; }
    if (candidate.valid) {
        if (!candidate.kind) {
            remote_name.pending=true; remote_name.epoch=core->epoch;
            memcpy(remote_name.address,candidate.address,6);
            int result=gap_remote_name_request(candidate.address,candidate.page_scan_repetition,candidate.clock_offset);
            if (result) {
                remote_name.pending=false; candidate.valid=false;
                bt_host_status.error=(uint8_t)result; retry_at=now_ms+500;
            }
            return;
        }
        /* Replacement may need one slot in the pool. Retire ONLY that side;
         * the opposite Joy-Con and its saved bond remain live. */
        for (unsigned i=0;i<PEER_COUNT;i++) if (candidate.new_pair && peers[i].state && peers[i].expected_kind==candidate.kind) {
            close_peer(&peers[i]); return;
        }
        /* A normal reconnect is authenticated with the saved key. Only an
         * explicit pairing transaction may create or replace a bond. */
        connect_peer(candidate.address,candidate.new_pair,candidate.kind);
        candidate.valid=false; return;
    }
    inquiry=gap_inquiry_start(4)==0;
    retry_at=now_ms+1000;
}
void bt_host_init(bridge_t *bridge,pro_usb_t *protocol,uint32_t now) {
    core=bridge; usb=protocol; now_ms=now; epoch=bridge->epoch; initialized=true;
    retry_at=now;
    btstack_memory_init(); btstack_run_loop_init(&run_loop);
    hci_init(&transport,NULL); hci_set_link_key_db(&key_db);
    /* BTstack's HID-host policy. Do not overwrite it per peer or after every
     * report. Incoming links request central role; role switching stays allowed. */
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    gap_set_security_level(LEVEL_2); gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT); gap_ssp_set_auto_accept(0);
    gap_register_classic_connection_filter(accept_acl);
    /* Nintendo selects its console protocol using the Classic host name.
     * Keep this exact name across radio bring-up and BTstack initialization.
     * USB product/manufacturer strings identify the receiver independently. */
    gap_set_local_name("Nintendo Switch"); gap_set_class_of_device(0x002508);
    l2cap_init();
    uint8_t status=l2cap_register_service(packet_handler,PSM_HID_CONTROL,HID_MTU,LEVEL_2);
    if (!status) status=l2cap_register_service(packet_handler,PSM_HID_INTERRUPT,HID_MTU,LEVEL_2);
    if (status) { bt_host_status.error=status; diagnostic_enter_bootloader(); }
    events.callback=packet_handler; hci_add_event_handler(&events);
    gap_discoverable_control(0); gap_connectable_control(1);
    hci_power_control(HCI_POWER_ON);
}
void bt_host_request_pair(void) { pair_requested=true; }
void bt_host_request_slot(uint8_t slot) { if (slot<BRIDGE_SLOT_COUNT) slot_requested=slot; }
void bt_host_shutdown(void) {
    if (!initialized || shutting_down) return;
    shutting_down=true; hci_power_control(HCI_POWER_OFF);
}
uint8_t bt_host_ready_mask(void) {
    uint8_t mask=0;
    if (!core) return 0;
    for (unsigned i=0;i<PEER_COUNT;i++) {
        const peer_t *peer=&peers[i]; unsigned index;
        if (peer->state==PEER_READY && peer->epoch==core->epoch &&
            bridge_admit(core,peer->bond.address,&index)) mask|=(uint8_t)(1u<<index);
    }
    return mask;
}
void bt_host_poll(uint32_t now) {
    if (!initialized) return;
    now_ms=now;
    if (tx_pending && hci_uart_tx_idle()) {
        tx_pending=false;
        uint8_t event[]={HCI_EVENT_TRANSPORT_PACKET_SENT,0};
        transport_handler(HCI_EVENT_PACKET,event,sizeof(event));
    }
    btstack_run_loop_base_execute_callbacks(); btstack_run_loop_base_process_timers(now);
    if (shutting_down) return;
    if (slot_requested<BRIDGE_SLOT_COUNT) {
        if (slot_requested!=core->settings.active_slot) bridge_switch_slot(core,now);
        slot_requested=0xff;
    }
    if (pair_requested) { pair_requested=false; bridge_pair_begin(core,now); }
    if (epoch!=core->epoch) {
        trace(TRACE_EPOCH,core->pairing,(uint16_t)core->epoch);
        epoch=core->epoch; reconcile_peers(); candidate.valid=false;
        if (inquiry) gap_inquiry_stop();
        retry_at=now+100u;
        quiet=false; bt_host_status.storage_error=0;
    }
    for (unsigned i=0;i<PEER_COUNT;i++) service_lifecycle(&peers[i]);
    /* A successful pair offer may advance the core epoch in the loop above.
     * Reconcile immediately, before sending or publishing in the new epoch. */
    if (epoch!=core->epoch) {
        epoch=core->epoch; reconcile_peers(); candidate.valid=false;
        if (inquiry) gap_inquiry_stop();
    }
    bt_host_status.pairing=core->pairing; bt_host_status.connected=0;
    for (unsigned n=0;n<PEER_COUNT;n++) {
        peer_t *peer=&peers[(n+output_turn)%PEER_COUNT];
        if (peer->state!=PEER_SETUP && peer->state!=PEER_READY) continue;
        bt_host_status.connected++;
        bt_host_status.setup_stage=peer->controller.stage; bt_host_status.setup_error=peer->controller.error;
        if (peer->state==PEER_READY) {
            /* Experimental Pro-only QoS request. This is a latency constraint,
             * not a promise of an 8 ms report period. Never retry or alter the
             * working Joy-Con reconnect policy on a rejected/rounded result. */
            if (peer->controller.kind==PEER_PRO && !peer->qos_state) {
                peer->qos_state=4; peer->qos_requested_ms=now;
                uint8_t status=gap_qos_set(peer->handle,HCI_SERVICE_TYPE_GUARANTEED,
                    8000u,UINT32_MAX,PRO_QOS_LATENCY_US,UINT32_MAX);
                trace(TRACE_TIMING_REQUEST,status,PRO_QOS_LATENCY_US);
                if (status) { peer->qos_state=6; peer->qos_status=status; }
            } else if (peer->qos_state==4 && (uint32_t)(now-peer->qos_requested_ms)>=5000u) {
                peer->qos_state=6; peer->qos_status=0xfe;
            }
            static const uint8_t neutral[]={0,1,0x40,0x40,0,1,0x40,0x40};
            uint8_t rumble[8]; memcpy(rumble,neutral,8);
            if (usb->vibration_enabled) {
                if (peer->controller.kind!=PEER_RIGHT) memcpy(rumble,usb->rumble,4);
                if (peer->controller.kind!=PEER_LEFT) memcpy(rumble+4,usb->rumble+4,4);
            }
            nintendo_queue_output(&peer->controller,rumble,usb->player_leds,peer->rumble_sequence!=usb->rumble_sequence);
            peer->rumble_sequence=usb->rumble_sequence;
            if (usb->home_sequence && peer->home_sequence!=usb->home_sequence && peer->controller.kind!=PEER_LEFT) {
                nintendo_queue_home(&peer->controller,usb->home_led); peer->home_sequence=usb->home_sequence;
            }
        }
        if (l2cap_can_send_packet_now(peer->interrupt_cid)) send_output(peer);
        /* CAN_SEND_NOW is required for progress if ACL credits are exhausted.
         * Request only for actual pending work, never spin on empty callbacks. */
        else if (!peer->send_requested && (!peer->controller.ready || peer->controller.rumble_pending ||
                 peer->controller.output_pending || peer->controller.home_pending)) {
            peer->send_requested=true;
            if (l2cap_request_can_send_now_event(peer->interrupt_cid)) peer->send_requested=false;
        }
    }
    output_turn^=1u;
    service_storage();
    if (bt_host_status.state==HCI_STATE_WORKING) service_discovery();
    if (bt_host_status.state==HCI_STATE_WORKING) {
        uint8_t required=core->settings.active_slot==0 ? 1u : 3u;
        bool listen=core->pairing || (bt_host_ready_mask() & required)!=required;
        if (listen!=connectable_requested) {
            connectable_requested=listen;
            gap_connectable_control(listen);
        }
        uint8_t links=0;
        for (unsigned i=0;i<PEER_COUNT;i++)
            if (peers[i].handle!=HCI_CON_HANDLE_INVALID && peers[i].state>=PEER_CHANNELS &&
                peers[i].state<PEER_CLOSING) links++;
        if (scan_snapshot.waiting && (uint32_t)(now-scan_snapshot.sent_ms)>=2000) {
            scan_snapshot.waiting=false; scan_snapshot.phase=0xff; scan_snapshot.status=0xfe;
        }
        if (!scan_snapshot.waiting && links!=scan_snapshot.links) {
            scan_snapshot.links=links; scan_snapshot.phase=0; scan_snapshot.status=0;
        }
        if (!scan_snapshot.waiting && scan_snapshot.phase<3 && hci_can_send_command_packet_now()) {
            scan_snapshot.waiting=true; scan_snapshot.sent_ms=now;
            hci_send_cmd(scan_reads[scan_snapshot.phase]);
        }
    }
    usb->events=0;
}
void bt_host_rate_read(uint8_t page,uint8_t reply[64]) {
    if (page==7) {
        reply[6]=scan_snapshot.phase; reply[7]=scan_snapshot.status;
        reply[8]=scan_snapshot.links; reply[9]=scan_snapshot.enable; reply[10]=scan_snapshot.type;
        reply[11]=0xa4; /* distinguish readback from retired <=0x10018 probes */
        memcpy(reply+12,&scan_snapshot.interval,2); memcpy(reply+14,&scan_snapshot.window,2);
        return;
    }
    if (page==6) {
        if (!core) { reply[6]=0xff; return; }
        const pairing_slot_t *slot=&core->settings.slots[core->settings.active_slot];
        reply[6]=core->settings.active_slot; reply[7]=slot->kind;
        for (unsigned i=0;i<2;i++) {
            uint8_t *out=reply+(i ? 36 : 8);
            const bond_t *bond=&slot->peers[i]; peer_t *peer=by_address(bond->address);
            out[0]=bond->kind; memcpy(out+16,bond->address,6); out[3]=0xff; out[11]=core->live[i].valid;
            if (!peer || peer->epoch!=core->epoch) continue;
            out[1]=peer->control_open && peer->interrupt_open; out[2]=peer->state==PEER_READY;
            out[3]=peer->controller.stage; out[4]=peer->controller.error; out[5]=peer->key_valid;
            memcpy(out+6,&peer->interrupt_cid,2); memcpy(out+8,&peer->handle,2);
            out[10]=peer->controller.kind; memcpy(out+12,&peer->full_reports,4);
            out[22]=peer->send_requested; out[23]=peer->state;
        }
        return;
    }
    if (page==3) {
        for (unsigned i=0;i<PEER_COUNT;i++) {
            const peer_t *peer=&peers[i]; uint8_t *out=reply+(i ? 36 : 8);
            bool open=peer->state>=PEER_CHANNELS && peer->state<PEER_CLOSING;
            out[0]=open; out[1]=peer->state==PEER_READY;
            out[2]=open ? (uint8_t)gap_get_role(peer->handle) : 0xff; out[3]=peer->link_mode;
            memcpy(out+4,&peer->handle,2); memcpy(out+6,&peer->link_interval,2);
            out[8]=peer->qos_state ? 5 : 0;
            out[9]=peer->qos_state ? peer->qos_state : 5; out[10]=peer->qos_status;
            out[11]=LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH;
            memcpy(out+12,&peer->full_reports,4);
            memcpy(out+16,&peer->qos_requested_ms,4); memcpy(out+20,&peer->qos_latency_us,4);
            reply[7]+=open;
        }
        reply[6]=core && core->settings.active_slot==0 ? 5 : 0;
        return;
    }
    if (page>3) { reply[6]=0xff; return; } /* retired invasive probes */
    reply[6]=reply[7]=0xff;
    for (unsigned i=0;i<PEER_COUNT;i++) if (peers[i].state==PEER_READY) {
        reply[6]=(uint8_t)gap_get_role(peers[i].handle); reply[7]=peers[i].link_mode; break;
    }
    if (!page) {
        uint32_t first[]={rate.acl,rate.reports,bt_host_status.published,rate.duplicate,rate.short_report,rate.rejected};
        uint32_t second[]={rate.timer_ticks,rate.timer_steps,rate.timer_max};
        memcpy(reply+8,first,sizeof(first)); memcpy(reply+36,second,sizeof(second));
    } else { memcpy(reply+8,rate.histogram,24); memcpy(reply+36,rate.histogram+6,8); }
}

