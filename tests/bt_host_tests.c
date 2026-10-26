/* Real pinned BTstack HCI + L2CAP, with an emulated controller on the H4
 * boundary. Only the UART, radio boot, flash and reset hardware are replaced.
 * No mock HID callbacks can conceal allocation or channel teardown bugs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define REDEV_HARDWARE_H
_Noreturn void diagnostic_enter_bootloader(void);
#include "../firmware/platform/gd32f103/bt_host.c"

volatile hci_uart_status_t hci_uart_status;
const settings_io_t settings_flash_io={0};
bool settings_save(const settings_io_t *io,const settings_t *s) { (void)io; (void)s; return true; }
settings_append_result_t settings_append(const settings_io_t *io,const settings_t *s) {
    (void)io; (void)s; return SETTINGS_APPENDED;
}
_Noreturn void diagnostic_enter_bootloader(void) { abort(); }
static struct { uint8_t bytes[1030]; size_t length; } queue[128];
static unsigned queue_in,queue_out,accepts,disconnects,packets[2],page_requests,scan_writes;
static uint16_t next_page_handle=0x44;
static unsigned qos_requests;
static uint8_t actual_scan_enable;
static uint8_t qos_result;
static bool hold_qos;
static bool hold_disconnect, hold_page, successful_page, hold_name;
static const char *name_reply="Joy-Con (R)";
static bridge_t test_core;
static pro_usb_t test_usb;
static const uint8_t left_addr[]={0x74,0x84,0x69,0xc9,0x78,0x8e};
static const uint8_t right_addr[]={0x74,0x84,0x69,0xc9,0x48,0xd9};
bool hci_uart_tx_idle(void) { return true; }
bool hci_uart_send(const uint8_t *data,size_t length) {
    assert(queue_in-queue_out<128 && length<=1030);
    memcpy(queue[queue_in%128].bytes,data,length); queue[queue_in%128].length=length; queue_in++;
    return true;
}
static uint16_t word(const uint8_t *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static void put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void event(const uint8_t *bytes,size_t n) {
    uint8_t copy[1100]; copy[0]=4; memcpy(copy+1,bytes,n);
    bt_host_receive(copy,n+1);
}
static void complete(uint16_t opcode,const uint8_t *args,size_t n) {
    uint8_t e[100]={0x0e,(uint8_t)(3+n),1,0,0};
    put16(e+3,opcode); memcpy(e+5,args,n); event(e,n+5);
}
static void status(uint16_t opcode) {
    uint8_t e[]={0x0f,4,0,1,0,0}; put16(e+4,opcode); event(e,sizeof(e));
}
static void acl(uint16_t handle,uint16_t cid,const uint8_t *data,size_t n) {
    uint8_t copy[1100]={2};
    put16(copy+1,handle|0x2000); put16(copy+3,(uint16_t)(n+4));
    put16(copy+5,(uint16_t)n); put16(copy+7,cid);
    memcpy(copy+9,data,n); bt_host_receive(copy,n+9);
}
static void signal(uint16_t handle,uint8_t code,uint8_t id,const uint8_t *data,size_t n) {
    uint8_t b[100]={code,id,0,0}; put16(b+2,(uint16_t)n); memcpy(b+4,data,n); acl(handle,1,b,n+4);
}
static void disconnected(uint16_t handle) {
    uint8_t e[]={5,4,0,0,0,0x13}; put16(e+3,handle); event(e,sizeof(e));
}
static uint16_t local_for_remote(uint16_t handle,uint16_t remote) {
    peer_t *p=by_handle(handle); assert(p);
    return remote==0x60 ? p->control_cid : p->interrupt_cid;
}
static void send_configuration(uint16_t handle,uint16_t local) {
    uint8_t c[]={0,0,0,0,1,2,64,0}; put16(c,local); signal(handle,4,0x70,c,sizeof(c));
}
static void pump(void) {
    unsigned iterations=0;
    for (;;) {
        assert(++iterations<3000);
        bt_host_poll(++now_ms);
        if (queue_out==queue_in) {
            if (!tx_pending) break;
            continue;
        }
        uint8_t b[1030]; size_t n=queue[queue_out%128].length;
        memcpy(b,queue[queue_out%128].bytes,n); queue_out++;
        if (b[0]==1) {
            uint16_t opcode=word(b+1),handle=n>=6 ? word(b+4) : 0;
            switch (opcode) {
            case 0x0807: {
                qos_requests++;
                assert(n==24 && b[6]==0 && b[7]==HCI_SERVICE_TYPE_GUARANTEED);
                assert(little_endian_read_32(b,8)==8000 && little_endian_read_32(b,16)==8000);
                assert(little_endian_read_32(b,12)==UINT32_MAX && little_endian_read_32(b,20)==UINT32_MAX);
                assert(by_handle(handle)->controller.kind==PEER_PRO);
                if (qos_result) {
                    uint8_t e[]={0x0f,4,qos_result,1,7,8}; event(e,sizeof(e));
                } else {
                    status(opcode);
                    if (!hold_qos) {
                        uint8_t e[23]={0x0d,21,0}; put16(e+3,handle);
                        e[6]=HCI_SERVICE_TYPE_GUARANTEED;
                        put16(e+15,7500); event(e,sizeof(e));
                    }
                }
                break;
            }
            case 0x0c47: { assert(b[4]==1); scan_writes++; const uint8_t ok=0; complete(opcode,&ok,1); break; }
            case 0x0c1a: { actual_scan_enable=b[4]; const uint8_t ok=0; complete(opcode,&ok,1); break; }
            case 0x0c19: { const uint8_t r[]={0,actual_scan_enable}; complete(opcode,r,sizeof(r)); break; }
            case 0x0c1b: { const uint8_t r[]={0,0,8,0x12,0}; complete(opcode,r,sizeof(r)); break; }
            case 0x0c46: { const uint8_t r[]={0,1}; complete(opcode,r,sizeof(r)); break; }
            case 0x0401: status(opcode); break;
            case 0x0419: {
                status(opcode);
                if (!hold_name) {
                    uint8_t response[257]={7,255,0}; memcpy(response+3,b+4,6);
                    memcpy(response+9,name_reply,strlen(name_reply)); event(response,sizeof(response));
                }
                break;
            }
            case 0x0409: /* accept connection, then test supplies completion */
                assert(b[10]==HCI_ROLE_MASTER); accepts++; status(opcode); break;
            case 0x0406:
                disconnects++; status(opcode);
                if (!hold_disconnect) disconnected(handle);
                break;
            case 0x0405:
                page_requests++;
                status(opcode);
                if (!hold_page) {
                    uint8_t e[]={3,11,4,0,0,0,0,0,0,0,0,1,0};
                    memcpy(e+5,b+4,6);
                    if (successful_page) { e[2]=0; put16(e+3,next_page_handle++); }
                    event(e,sizeof(e));
                }
                break;
            case 0x041b: { /* remote features: legacy BR/EDR + encryption */
                status(opcode); uint8_t e[]={0x0b,11,0,0,0,4,0,0,0,0,0,0,0};
                put16(e+3,handle); event(e,sizeof(e)); break;
            }
            case 0x0411: { /* authentication uses the stored key through real HCI */
                status(opcode);
                hci_connection_t *c=hci_connection_for_handle(handle); assert(c);
                uint8_t request[8]={0x17,6}; reverse_bd_addr(c->address,request+2); event(request,sizeof(request));
                break;
            }
            case 0x040b: { /* link-key reply then authentication success */
                uint8_t response[7]={0}; memcpy(response+1,b+4,6); complete(opcode,response,sizeof(response));
                bd_addr_t address; reverse_bd_addr(b+4,address);
                hci_connection_t *c=hci_connection_for_bd_addr_and_type(address,BD_ADDR_TYPE_ACL); assert(c);
                uint8_t e[]={6,3,0,0,0}; put16(e+3,c->con_handle); event(e,sizeof(e)); break;
            }
            case 0x0413: {
                status(opcode); uint8_t e[]={8,4,0,0,0,1}; put16(e+3,handle); event(e,sizeof(e)); break;
            }
            case 0x1408: {
                uint8_t response[]={0,0,0,16}; put16(response+1,handle); complete(opcode,response,sizeof(response)); break;
            }
            case 0x040c: assert(!"unexpected negative link-key reply"); break;
            default: {
                uint8_t ok=0; complete(opcode,&ok,1); break;
            }
            }
        } else if (b[0]==2) {
            uint16_t handle=word(b+1)&0xfff, cid=word(b+7);
            if (cid==1) {
                uint8_t code=b[9],id=b[10]; const uint8_t *d=b+13;
                if (code==2) {
                    uint16_t remote=word(d)==PSM_HID_CONTROL ? 0x60 : 0x61;
                    uint8_t response[8]={0}; put16(response,remote); put16(response+2,word(d+2));
                    signal(handle,3,id,response,sizeof(response));
                    send_configuration(handle,word(d+2));
                } else if (code==3 && word(d+4)==0) send_configuration(handle,word(d));
                else if (code==4) {
                    uint16_t local=local_for_remote(handle,word(d));
                    uint8_t response[]={0,0,0,0,0,0}; put16(response,local);
                    signal(handle,5,id,response,sizeof(response));
                } else if (code==10) {
                    uint8_t response[]={0,0,1,0}; memcpy(response,d,2);
                    signal(handle,11,id,response,sizeof(response));
                } else if (code==6) signal(handle,7,id,d,4);
            } else {
                peer_t *peer=by_handle(handle); assert(peer);
                assert(b[9]==0xa2);
                packets[peer-peers]++;
            }
            uint8_t credits[]={0x13,5,1,0,0,1,0}; put16(credits+3,handle); event(credits,sizeof(credits));
        }
    }
}
static void incoming_acl(const uint8_t address[6],uint16_t handle) {
    uint8_t request[]={4,10,0,0,0,0,0,0,8,5,0,1};
    reverse_bd_addr(address,request+2); event(request,sizeof(request)); pump();
    assert(by_address(address)); /* allocated before any HID channel exists */
    uint8_t opened[]={3,11,0,0,0,0,0,0,0,0,0,1,0};
    put16(opened+3,handle); reverse_bd_addr(address,opened+5); event(opened,sizeof(opened)); pump();
}
static void incoming_channel(uint16_t handle,uint16_t psm,uint16_t remote) {
    uint8_t c[4]; put16(c,psm); put16(c+2,remote); signal(handle,2,(uint8_t)remote,c,sizeof(c)); pump();
}
static void setup_controller(peer_t *p,uint8_t kind) {
    assert(p->state==PEER_SETUP);
    for (unsigned stage=0;stage<10;stage++) {
        pump(); assert(p->controller.waiting && p->controller.stage==stage);
        uint8_t report[51]={0xa1,0x21}; uint8_t *r=report+1;
        r[13]=0x80; r[14]=p->controller.expected;
        if (!stage) { r[17]=kind==PEER_PRO ? 3 : kind==PEER_LEFT ? 1 : 2; memcpy(r+19,p->bond.address,6); }
        /* Invalid/absent calibration uses the Nintendo parser's factory
         * fallback only where allowed; supply valid symmetric values. */
        if (stage>=1 && stage<=4) {
            static const uint32_t addresses[]={0x6020,0x603d,0x8010,0x8026};
            static const uint8_t lengths[]={24,18,22,26};
            uint32_t a=addresses[stage-1];
            for (unsigned i=0;i<4;i++) r[15+i]=(uint8_t)(a>>(8*i));
            r[19]=lengths[stage-1];
            memset(r+20,0xff,lengths[stage-1]);
            if (stage==1) {
                memset(r+20,0,24);
                for (unsigned i=0;i<3;i++) { put16(r+26+i*2,16384); put16(r+38+i*2,13371); }
            }
            if (stage==2) {
                static const uint8_t cal[18]={0,6,0x60,0,8,0x80,0,6,0x60,0,8,0x80,0,6,0x60,0,6,0x60};
                memcpy(r+20,cal,18);
            }
        }
        acl(p->handle,p->interrupt_cid,report,sizeof(report)); pump();
        assert(!p->controller.error);
    }
    assert(p->state==PEER_READY);
}
static void report_input(peer_t *p,uint8_t timer,uint8_t button) {
    uint8_t r[50]={0xa1,0x30,timer,0x80,button};
    r[8]=r[11]=8; r[9]=r[12]=0x80;
    acl(p->handle,p->interrupt_cid,r,sizeof(r)); pump();
}
int main(void) {
    settings_t settings={0}; settings.active_slot=1; settings.slots[1].kind=SLOT_JOYCONS;
    settings.slots[1].gyro_source=GYRO_RIGHT;
    for (unsigned i=0;i<2;i++) {
        bond_t *b=&settings.slots[1].peers[i]; b->kind=i ? PEER_RIGHT : PEER_LEFT;
        memcpy(b->address,i ? right_addr : left_addr,6); memset(b->link_key,0x50+i,16); b->key_type=COMBINATION_KEY;
    }
    bridge_init(&test_core,&settings); uint8_t mac[6]={2,0,0,0,0,1}; pro_usb_init(&test_usb,mac);
    bt_host_init(&test_core,&test_usb,0);
    /* Radio initialization is covered by the ARM fixture. Start this HCI
     * emulator at working, preserving the application's policy and services. */
    queue_out=queue_in; tx_pending=false;
    extern hci_stack_t *test_hci_working(void);
    hci_stack_t *stack=test_hci_working(); pump();
    assert(scan_writes==1);
    assert(stack->default_link_policy_settings==(LM_LINK_POLICY_ENABLE_SNIFF_MODE|LM_LINK_POLICY_ENABLE_ROLE_SWITCH));
    incoming_acl(left_addr,0x40); incoming_acl(right_addr,0x41);
    assert(accepts==2 && by_address(left_addr)!=by_address(right_addr));
    /* Interleave both peers and open R's interrupt before its control: each
     * channel owns its own CID; neither peer depends on a global HID state. */
    incoming_channel(0x40,PSM_HID_CONTROL,0x60);
    incoming_channel(0x41,PSM_HID_INTERRUPT,0x61);
    incoming_channel(0x40,PSM_HID_INTERRUPT,0x61);
    incoming_channel(0x41,PSM_HID_CONTROL,0x60);
    peer_t *left=by_address(left_addr),*right=by_address(right_addr);
    assert(left && right && gap_security_level(0x40)>=LEVEL_2 && gap_security_level(0x41)>=LEVEL_2);
    setup_controller(left,PEER_LEFT); setup_controller(right,PEER_RIGHT);
    assert(bt_host_ready_mask()==3);
    assert(qos_requests==0); /* Pro experiment cannot affect paired Joy-Cons. */
    assert(scan_snapshot.phase==3 && scan_snapshot.links==2 && scan_snapshot.enable==0 &&
           scan_snapshot.type==1 && scan_snapshot.interval==0x800 && scan_snapshot.window==0x12);
    report_input(left,1,0); report_input(right,1,8);
    assert(test_core.live[0].valid && test_core.live[1].valid);
    assert(left->full_reports==1 && right->full_reports==1 && packets[0] && packets[1]);
    /* Pair/cancel must preserve both ACLs and inputs, including in Pro's path. */
    unsigned before=disconnects;
    bridge_pair_begin(&test_core,now_ms); pump();
    assert(bt_host_ready_mask()==3 && test_core.live[0].valid);
    assert(actual_scan_enable==2);
    bridge_pair_cancel(&test_core); pump(); assert(disconnects==before && bt_host_ready_mask()==3);
    assert(actual_scan_enable==0);
    /* Incoming keys are captured before any HID channels exist. */
    uint8_t key[16]; memset(key,0x77,sizeof(key)); key_put(left->bond.address,key,COMBINATION_KEY);
    assert(left->key_valid && !memcmp(left->bond.link_key,key,16));
    memcpy(left->bond.link_key,settings.slots[1].peers[0].link_key,16);
    /* A profile switch retains closing ownership until real disconnect events
     * free all four L2CAP channels and both HCI pool entries. */
    hold_disconnect=true; bridge_switch_slot(&test_core,now_ms); pump();
    assert(left->state==PEER_CLOSING && right->state==PEER_CLOSING && !bt_host_ready_mask());
    assert(actual_scan_enable==2);
    assert(!reserve_peer(left_addr,false,PEER_LEFT));
    hold_disconnect=false; disconnected(0x40); disconnected(0x41); pump();
    assert(!by_address(left_addr) && !by_address(right_addr));
    bridge_switch_slot(&test_core,now_ms); pump();
    incoming_acl(right_addr,0x42); incoming_channel(0x42,PSM_HID_CONTROL,0x60);
    /* Teardown during channel setup must release the pool, too. */
    bridge_switch_slot(&test_core,now_ms); pump();
    assert(!by_address(right_addr));
    bridge_switch_slot(&test_core,now_ms); pump();
    /* Outgoing pairing uses the fixed HID channels, including authentication,
     * with no generic HID or SDP allocation. Exercise its actual stack path. */
    successful_page=true;
    right=reserve_peer(right_addr,false,PEER_RIGHT); assert(right); right->outgoing=true;
    assert(!l2cap_create_channel(packet_handler,right->bond.address,PSM_HID_CONTROL,HID_MTU,&right->control_cid));
    pump(); assert(right->control_open && right->interrupt_open);
    setup_controller(right,PEER_RIGHT);
    assert(bt_host_ready_mask()==2);
    assert(actual_scan_enable==2); /* leave room for the missing Joy-Con */
    /* Ordinary reconnect remains available while the other half is active;
     * no outgoing pages may compete with the waking controller. */
    settings_t before_reconnect=test_core.settings;
    uint8_t reconnect_result[]={0x22,15,1,0,0,0,0,0,0,1,0,8,5,0,0,0,0xd0};
    const uint8_t stranger[6]={0x74,0x84,0x69,0,0,0x99};
    reverse_bd_addr(stranger,reconnect_result+3); event(reconnect_result,sizeof(reconnect_result));
    assert(!candidate.valid && !remote_name.pending);
    assert(!test_core.pairing);
    before=disconnects; unsigned pages_before=page_requests;
    now_ms+=60000; pump(); assert(page_requests==pages_before && !by_address(left_addr));
    incoming_acl(left_addr,0x46);
    incoming_channel(0x46,PSM_HID_CONTROL,0x60); incoming_channel(0x46,PSM_HID_INTERRUPT,0x61);
    left=by_address(left_addr);
    assert(left && left->key_valid && left->control_open && left->interrupt_open);
    assert(!memcmp(left->bond.link_key,settings.slots[1].peers[0].link_key,16));
    setup_controller(left,PEER_LEFT);
    assert(bt_host_ready_mask()==3 && disconnects==before);
    assert(actual_scan_enable==0);
    assert(!memcmp(&before_reconnect,&test_core.settings,sizeof(before_reconnect)));
    report_input(right,2,8); report_input(left,2,0);
    close_peer(left); pump(); assert(!by_address(left_addr) && right->state==PEER_READY);
    /* If incoming wins after discovery, do not create another control CID. */
    bridge_pair_begin(&test_core,now_ms); pump();
    reverse_bd_addr(left_addr,reconnect_result+3); event(reconnect_result,sizeof(reconnect_result)); assert(candidate.valid);
    incoming_acl(left_addr,0x47); incoming_channel(0x47,PSM_HID_CONTROL,0x60);
    incoming_channel(0x47,PSM_HID_INTERRUPT,0x61); left=by_address(left_addr);
    setup_controller(left,PEER_LEFT); now_ms+=200; pump();
    assert(!left->outgoing && !candidate.valid && bt_host_ready_mask()==3);
    bridge_pair_cancel(&test_core); pump();
    close_peer(left); pump(); assert(!by_address(left_addr));
    /* A page in flight is not an open ACL. A profile change must retain its
     * ownership until the controller reports page failure, then free both
     * application and BTstack allocations, without touching the other peer. */
    hold_page=true;
    left=reserve_peer(left_addr,false,PEER_LEFT); assert(left); left->outgoing=true;
    assert(!l2cap_create_channel(packet_handler,left->bond.address,PSM_HID_CONTROL,HID_MTU,&left->control_cid));
    pump(); bridge_switch_slot(&test_core,now_ms); pump();
    assert(left->state==PEER_CLOSING && left->control_cid);
    uint8_t failure[]={3,11,4,0,0,0,0,0,0,0,0,1,0};
    reverse_bd_addr(left_addr,failure+5); event(failure,sizeof(failure)); pump();
    assert(!by_address(left_addr) && !by_address(right_addr));
    hold_page=false;
    /* Saved Pro pairing/cancel has the same preservation rules as Joy-Cons. */
    pairing_slot_t *pro=&test_core.settings.slots[0];
    pro->kind=SLOT_PRO; pro->peers[0]=settings.slots[1].peers[0]; pro->peers[0].kind=PEER_PRO;
    pro->peers[0].address[0]=0x50;
    incoming_acl(pro->peers[0].address,0x45);
    incoming_channel(0x45,PSM_HID_CONTROL,0x60); incoming_channel(0x45,PSM_HID_INTERRUPT,0x61);
    peer_t *p=by_address(pro->peers[0].address); setup_controller(p,PEER_PRO);
    before=disconnects; bridge_pair_begin(&test_core,now_ms); pump();
    bridge_pair_cancel(&test_core); pump();
    assert(disconnects==before && bt_host_ready_mask()==1 && p->state==PEER_READY);
    bridge_switch_slot(&test_core,now_ms); pump();
    bridge_pair_begin(&test_core,now_ms); pump();
    /* Standard inquiry-with-RSSI has no EIR name. Saved Nintendo identities
     * must still become pairing candidates (0x10019 incorrectly dropped them). */
    uint8_t discovered[]={0x22,15,1,0,0,0,0,0,0,1,0,8,5,0,0,0,0xd0};
    reverse_bd_addr(left_addr,discovered+3); event(discovered,sizeof(discovered));
    assert(candidate.valid && candidate.kind==PEER_LEFT);
    bridge_pair_cancel(&test_core); pump();
    /* Unknown devices need a remote-name query, which cannot evict a peer
     * until the returned type is known and matches the selected profile. */
    uint8_t unknown[6]={0x74,0x84,0x69,0,0,9};
    bridge_pair_begin(&test_core,now_ms); pump();
    reverse_bd_addr(unknown,discovered+3); event(discovered,sizeof(discovered));
    assert(candidate.valid && !candidate.kind);
    hold_page=true; now_ms+=200; pump();
    peer_t *new_peer=by_address(unknown);
    assert(!remote_name.pending && new_peer && new_peer->expected_kind==PEER_RIGHT && !new_peer->key_valid);
    bridge_pair_cancel(&test_core); pump();
    reverse_bd_addr(unknown,failure+5); event(failure,sizeof(failure)); pump();
    assert(!by_address(unknown)); hold_page=false;
    bridge_pair_begin(&test_core,now_ms); pump(); name_reply="Pro Controller";
    event(discovered,sizeof(discovered)); now_ms+=200; pump();
    assert(!candidate.valid && !by_address(unknown));
    bridge_pair_cancel(&test_core); pump();
    bridge_pair_begin(&test_core,now_ms); pump(); hold_name=true;
    event(discovered,sizeof(discovered)); now_ms+=200; pump(); assert(remote_name.pending);
    bridge_pair_cancel(&test_core); pump();
    uint8_t late_name[257]={7,255,0}; reverse_bd_addr(unknown,late_name+3);
    memcpy(late_name+9,"Joy-Con (R)",11); event(late_name,sizeof(late_name)); pump();
    assert(!remote_name.pending && !candidate.valid && !by_address(unknown));
    /* Repeated profile cycles accept either arrival order, retain both keys,
     * and never start an outgoing connection while waiting for normal wake. */
    pages_before=page_requests;
    for (unsigned cycle=0;cycle<4;cycle++) {
        bridge_switch_slot(&test_core,now_ms); pump();
        bridge_switch_slot(&test_core,now_ms); pump();
        now_ms+=60000; pump(); assert(page_requests==pages_before);
        for (unsigned n=0;n<2;n++) {
            bool is_right=((cycle+n)&1)!=0;
            const uint8_t *address=is_right ? right_addr : left_addr;
            uint16_t handle=(uint16_t)(0x50+n);
            incoming_acl(address,handle);
            incoming_channel(handle,PSM_HID_CONTROL,0x60);
            incoming_channel(handle,PSM_HID_INTERRUPT,0x61);
            setup_controller(by_address(address),is_right ? PEER_RIGHT : PEER_LEFT);
            now_ms+=60000; pump(); assert(page_requests==pages_before);
        }
        assert(bt_host_ready_mask()==3);
    }
    /* Pro uses the same saved-key incoming path. */
    bridge_switch_slot(&test_core,now_ms); pump();
    incoming_acl(test_core.settings.slots[0].peers[0].address,0x55);
    incoming_channel(0x55,PSM_HID_CONTROL,0x60); incoming_channel(0x55,PSM_HID_INTERRUPT,0x61);
    p=by_address(test_core.settings.slots[0].peers[0].address);
    assert(p && !p->outgoing && p->key_valid); setup_controller(p,PEER_PRO);
    assert(bt_host_ready_mask()==1 && !test_core.pairing);
    assert(actual_scan_enable==0);
    assert(p->qos_state==5 && p->qos_latency_us==7500 && !p->qos_status);
    uint8_t diagnostic_reply[64]={0}; bt_host_rate_read(3,diagnostic_reply);
    assert(diagnostic_reply[6]==5 && little_endian_read_32(diagnostic_reply,28)==7500);
    unsigned qos_before=qos_requests; now_ms+=10000; pump(); assert(qos_requests==qos_before);
    close_peer(p); pump(); qos_result=0x0c;
    assert(actual_scan_enable==2);
    incoming_acl(test_core.settings.slots[0].peers[0].address,0x56);
    incoming_channel(0x56,PSM_HID_CONTROL,0x60); incoming_channel(0x56,PSM_HID_INTERRUPT,0x61);
    p=by_handle(0x56); setup_controller(p,PEER_PRO);
    assert(p->qos_state==6 && p->qos_status==0x0c && bt_host_ready_mask()==1);
    qos_before=qos_requests; now_ms+=10000; pump(); assert(qos_requests==qos_before);
    close_peer(p); pump(); qos_result=0; hold_qos=true;
    incoming_acl(test_core.settings.slots[0].peers[0].address,0x57);
    incoming_channel(0x57,PSM_HID_CONTROL,0x60); incoming_channel(0x57,PSM_HID_INTERRUPT,0x61);
    p=by_handle(0x57); setup_controller(p,PEER_PRO); assert(p->qos_state==4);
    now_ms+=5000; pump(); assert(p->qos_state==6 && p->qos_status==0xfe && bt_host_ready_mask()==1);
    puts("PASS: real BTstack dual ACL/authentication/interleaved HID, repeated normal-wake profile cycles in both orders, incoming/outgoing race, input isolation, outgoing pairing, pending-page teardown, pool reuse, pairing preservation, remote-name cancellation");
    return 0;
}


