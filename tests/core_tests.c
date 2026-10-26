#include "bridge.h"
#include "pro_usb.h"
#include "settings_store.h"
#include "h4.h"
#include "rda5876.h"
#include "diagnostic_hid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bond_t bond(peer_kind_t kind, uint8_t id) {
    bond_t b;
    memset(&b, 0, sizeof(b));
    b.address[0] = 2;
    b.address[5] = id;
    b.kind = (uint8_t)kind;
    b.key_type = 4;
    for (unsigned i = 0; i < 16; ++i)
        b.link_key[i] = (uint8_t)(id + i);
    return b;
}
static settings_t config(void) {
    settings_t s;
    memset(&s, 0, sizeof(s));
    s.active_slot = 1;
    s.slots[1].kind = SLOT_JOYCONS;
    s.slots[1].gyro_source = GYRO_RIGHT;
    s.slots[1].peers[0] = bond(PEER_LEFT, 1);
    s.slots[1].peers[1] = bond(PEER_RIGHT, 2);
    s.slots[0].kind = SLOT_PRO;
    s.slots[0].peers[0] = bond(PEER_PRO, 3);
    return s;
}
static void test_merging(void) {
    settings_t s = config();
    bridge_t b;
    bridge_init(&b, &s);
    controller_sample_t l, r, out;
    sample_neutral(&l);
    sample_neutral(&r);
    l.buttons[0] = 0xff;
    l.buttons[1] = 0xff;
    l.buttons[2] = 0xff;
    l.battery = 8;
    r.buttons[0] = 0xff;
    r.buttons[1] = 0xff;
    r.buttons[2] = 0xff;
    r.battery = 4;
    l.sticks[0] = 100;
    l.sticks[1] = 200;
    r.sticks[2] = 300;
    r.sticks[3] = 400;
    l.imu[2][4] = 123;
    r.imu[2][4] = 456;
    assert(bridge_publish(&b, b.epoch, 0, &l, 10));
    assert(bridge_publish(&b, b.epoch, 1, &r, 20));
    bridge_snapshot(&b, 30, &out);
    assert(out.buttons[0] == 0xcf && out.buttons[1] == 0x3f && out.buttons[2] == 0xcf);
    assert(out.sticks[0] == 100 && out.sticks[1] == 200 && out.sticks[2] == 300 &&
           out.sticks[3] == 400);
    assert(out.battery == 4 && out.imu[2][4] == 456);
    b.settings.slots[1].gyro_source = GYRO_LEFT;
    bridge_snapshot(&b, 30, &out);
    assert(out.imu[2][4] == 123);
    bridge_snapshot(&b, 110, &out); /* stale left, fresh right */
    assert(out.buttons[2] == 0 && out.sticks[0] == 2048 && out.buttons[0] == 0xcf);
    assert(out.imu[2][4] == 0 && out.imu[0][2] == 4096); /* no automatic source switch */
    bridge_snapshot(&b,120,&out); /* both controls stale, battery still known */
    assert(!out.buttons[0] && !out.buttons[2] && out.battery==4);
    bridge_disconnect(&b, b.epoch, 1);
    bridge_snapshot(&b, 111, &out);
    assert(out.buttons[0] == 0 && out.battery==8);
    uint32_t epoch = b.epoch;
    bridge_switch_slot(&b, 112);
    assert(!bridge_publish(&b, epoch, 0, &l, 113));
    bridge_snapshot(&b, 113, &out);
    assert(out.buttons[1] == 0);
    unsigned index = 99;
    assert(!bridge_admit(&b, s.slots[1].peers[0].address, &index));
    assert(bridge_admit(&b, s.slots[0].peers[0].address, &index) && index == 0);
    assert(!bridge_publish(&b, b.epoch, 1, &r, 114));
    assert(bridge_publish(&b, b.epoch, 0, &r, UINT32_MAX - 10u));
    bridge_snapshot(&b, 10, &out);
    assert(out.buttons[0] == 0xff); /* wraparound */
    bridge_snapshot(&b, 100, &out);
    assert(out.buttons[0] == 0 && out.battery==4);
}
static void test_pairing(void) {
    settings_t s = config();
    s.slots[1].gyro_source = GYRO_LEFT;
    bridge_t b;
    bridge_init(&b, &s);
    bond_t l = bond(PEER_LEFT, 10), r = bond(PEER_RIGHT, 11), p = bond(PEER_PRO, 12);
    bridge_pair_begin(&b, 100);
    assert(memcmp(&b.settings, &s, sizeof(s)) == 0);
    bridge_tick(&b, 100 + BRIDGE_PAIR_MS);
    assert(!b.pairing);
    assert(memcmp(&b.settings, &s, sizeof(s)) == 0);
    bridge_pair_begin(&b, 70000);
    assert(!bridge_pair_offer(&b, &s.slots[0].peers[0], 70001)); /* wrong profile */
    assert(!bridge_pair_offer(&b, &l, 70000 + BRIDGE_PAIR_MS)); /* no late commit */
    assert(bridge_pair_offer(&b, &l, 70002));
    assert(!b.pairing && b.settings_dirty);
    assert(memcmp(&b.settings.slots[1].peers[0], &l, sizeof(l)) == 0);
    assert(memcmp(&b.settings.slots[1].peers[1], &s.slots[1].peers[1], sizeof(r)) == 0);
    assert(b.settings.slots[1].gyro_source == GYRO_LEFT);
    assert(memcmp(&b.settings.slots[0], &s.slots[0], sizeof(pairing_slot_t)) == 0);
    bridge_pair_begin(&b, 71000);
    assert(bridge_pair_offer(&b, &r, 71001));
    assert(memcmp(&b.settings.slots[1].peers[0], &l, sizeof(l)) == 0);
    assert(memcmp(&b.settings.slots[1].peers[1], &r, sizeof(r)) == 0);

    /* Explicit deletion clears BOTH halves; a fresh slot needs a complete pair. */
    bridge_pair_delete(&b, 72000);
    pairing_slot_t empty = {0};
    assert(memcmp(&b.settings.slots[1], &empty, sizeof(empty)) == 0);
    assert(settings_valid(&b.settings));
    bridge_pair_begin(&b, 73000);
    assert(bridge_pair_offer(&b, &r, 73001) && b.pairing);
    assert(!bridge_pair_offer(&b, &r, 73002));
    bond_t duplicate = l;
    memcpy(duplicate.address, r.address, 6);
    assert(!bridge_pair_offer(&b, &duplicate, 73003));
    uint32_t pending_epoch = b.epoch;
    bridge_pair_cancel(&b);
    assert(!b.pairing && b.epoch != pending_epoch);
    assert(memcmp(&b.pending, &empty, sizeof(empty)) == 0); /* staged key discarded */
    assert(memcmp(&b.settings.slots[1], &empty, sizeof(empty)) == 0);
    bridge_pair_begin(&b, 74000);
    assert(bridge_pair_offer(&b, &r, 74001) && b.pairing);
    assert(bridge_pair_offer(&b, &l, 74002) && !b.pairing);
    assert(settings_valid(&b.settings));
    bridge_pair_begin(&b, 80000);
    assert(!bridge_pair_offer(&b, &p, 80001)); /* Slot 2 is always Joy-Cons. */
    bridge_switch_slot(&b, 80002);
    bridge_pair_begin(&b, 80003);
    assert(!bridge_pair_offer(&b, &l, 80004)); /* Slot 1 is always Pro. */
    bridge_pair_cancel(&b);
    assert(memcmp(&b.settings.slots[0], &s.slots[0], sizeof(pairing_slot_t)) == 0);
    bridge_pair_begin(&b, 80004);
    assert(bridge_pair_offer(&b, &p, 80005));
    assert(b.settings.slots[0].kind == SLOT_PRO && settings_valid(&b.settings));
    s.slots[1].peers[1].address[5] = 1;
    assert(!settings_valid(&s));
}
static void test_gyro_and_led(void) {
    settings_t s = config();
    bridge_t b;
    bridge_init(&b, &s);
    controller_sample_t l, r, out;
    sample_neutral(&l);
    sample_neutral(&r);
    l.buttons[1] = 0x20;
    l.buttons[2] = 0x40;
    r.buttons[0] = 0x40;
    for (uint32_t t = 0; t <= 2000; t += 10) {
        assert(bridge_publish(&b, b.epoch, 0, &l, t));
        assert(bridge_publish(&b, b.epoch, 1, &r, t));
        bridge_tick(&b, t);
        assert(b.settings.slots[1].gyro_source == (t < 1000 ? GYRO_RIGHT : GYRO_LEFT));
    }
    assert(bridge_led(&b, 2000) == 16);
    bridge_snapshot(&b, 2000, &out);
    assert(out.buttons[0] == 0 && out.buttons[1] == 0 && out.buttons[2] == 0);
    r.buttons[0] = 0;
    bridge_publish(&b, b.epoch, 1, &r, 2010);
    bridge_tick(&b, 2010);
    r.buttons[0] = 0x40;
    for (uint32_t t = 2020; t <= 3020; t += 10) {
        bridge_publish(&b, b.epoch, 0, &l, t);
        bridge_publish(&b, b.epoch, 1, &r, t);
        bridge_tick(&b, t);
    }
    assert(b.settings.slots[1].gyro_source == GYRO_RIGHT);
    uint8_t report[64];
    bridge_report30(&b, true, 3020, report);
    assert(report[0] == 0x30 && report[17] == 0 && report[18] == 0x10);
    bridge_report30(&b, false, 3020, report);
    for (unsigned i = 13; i < 64; ++i)
        assert(report[i] == 0);
}
static button_action_t click(bridge_t *b, uint32_t start) {
    assert(bridge_button(b, true, start) == BUTTON_NONE);
    assert(bridge_button(b, true, start + 20) == BUTTON_NONE);
    assert(bridge_button(b, false, start + 60) == BUTTON_NONE);
    return bridge_button(b, false, start + 80);
}
static void test_button(void) {
    bridge_t b;
    bridge_init(&b, NULL);
    bridge_button(&b, false, 0);
    assert(click(&b, 100) == BUTTON_NONE);
    assert(click(&b, 250) == BUTTON_SWITCH_SLOT);
    assert(b.settings.active_slot == 1);
    /* Third click cannot toggle again. */
    assert(click(&b, 400) == BUTTON_NONE);
    bridge_button(&b, false, 900);
    assert(click(&b, 1000) == BUTTON_NONE);
    bridge_button(&b, true, 1200);
    bridge_button(&b, true, 1220);
    assert(bridge_button(&b, true, 3220) == BUTTON_PAIR);
    assert(bridge_button(&b, true, 4000) == BUTTON_NONE);
    bridge_button(&b, false, 4100);
    assert(bridge_button(&b, false, 4120) == BUTTON_NONE);
    assert(b.pairing);
    assert(click(&b, 4200) == BUTTON_CANCEL_PAIR);
    assert(!b.pairing && b.settings.active_slot == 1);
    assert(click(&b, 4350) == BUTTON_NONE); /* cancellation is not first double tap */

    /* A saved pair survives the first hold; a second separate hold deletes both. */
    settings_t saved = config();
    bridge_init(&b, &saved);
    bridge_button(&b, false, 0);
    bridge_button(&b, true, 100);
    bridge_button(&b, true, 120);
    assert(bridge_button(&b, true, 2120) == BUTTON_PAIR);
    assert(memcmp(&b.settings, &saved, sizeof(saved)) == 0);
    assert(bridge_button(&b, true, 5000) == BUTTON_NONE); /* one hold cannot delete */
    bridge_button(&b, false, 5100);
    bridge_button(&b, false, 5120);
    bridge_button(&b, true, 5200);
    bridge_button(&b, true, 5220);
    assert(bridge_button(&b, true, 7220) == BUTTON_DELETE_PAIR);
    assert(!b.pairing && b.settings_dirty && b.settings.slots[1].kind == SLOT_EMPTY);
    assert(b.settings.slots[1].peers[0].kind == PEER_NONE &&
           b.settings.slots[1].peers[1].kind == PEER_NONE);
    assert(memcmp(&b.settings.slots[0], &saved.slots[0], sizeof(pairing_slot_t)) == 0);
    assert(bridge_led(&b, 7220) == 96 && bridge_led(&b, 7280) == 0 &&
           bridge_led(&b, 7620) == 96); /* three deletion pulses */
    bridge_init(&b, NULL); /* boot button held is ignored until release */
    assert(bridge_button(&b, true, 0) == BUTTON_NONE);
    assert(bridge_button(&b, true, 5000) == BUTTON_NONE);
    bridge_button(&b, false, 5100);
    assert(click(&b, 5200) == BUTTON_NONE);
}
typedef struct {
    uint8_t pages[2][1024];
    int remaining;
    bool read_fail;
} flash_t;
static bool flash_read(void *c, unsigned page, size_t off, uint8_t *out, size_t n) {
    flash_t *f = c;
    assert(page < 2 && off + n <= 1024);
    if (f->read_fail)
        return false;
    memcpy(out, f->pages[page] + off, n);
    return true;
}
static bool consume(flash_t *f) {
    if (f->remaining == 0)
        return false;
    if (f->remaining > 0)
        --f->remaining;
    return true;
}
static bool flash_erase(void *c, unsigned page) {
    flash_t *f = c;
    if (!consume(f))
        return false;
    memset(f->pages[page], 0xff, 1024);
    return true;
}
static bool flash_program(void *c, unsigned page, size_t off, const uint8_t *p, size_t n) {
    flash_t *f = c;
    assert(off % 2 == 0 && n % 2 == 0 && off + n <= 1024);
    for (size_t i = 0; i < n; i += 2) {
        if (!consume(f))
            return false;
        assert((f->pages[page][off + i] & p[i]) == p[i] &&
               (f->pages[page][off + i + 1] & p[i + 1]) == p[i + 1]);
        f->pages[page][off + i] &= p[i];
        f->pages[page][off + i + 1] &= p[i + 1];
    }
    return true;
}
static void test_storage(void) {
    flash_t f;
    memset(&f, 0xff, sizeof(f));
    f.remaining = -1;
    f.read_fail = false;
    settings_io_t io = {&f, flash_read, flash_erase, flash_program};
    settings_t s = config(), out;
    uint32_t seq;
    assert(!settings_load(&io, &out, &seq));
    assert(settings_save(&io, &s));
    assert(settings_load(&io, &out, &seq) && seq == 1 && memcmp(&s, &out, sizeof(s)) == 0);
    flash_t baseline = f;
    settings_t changed = s;
    changed.active_slot = 0;
    /* Cut power before/after every erase and halfword program, including commit. */
    for (int cut = 0; cut <= 65; ++cut) {
        f = baseline;
        f.remaining = cut;
        bool saved = settings_save(&io, &changed);
        assert(settings_load(&io, &out, &seq));
        assert(seq == 1 || seq == 2);
        assert(memcmp(&out, seq == 1 ? &s : &changed, sizeof(out)) == 0);
        if (saved)
            assert(seq == 2);
        assert(memcmp(f.pages[0], baseline.pages[0], 1024) == 0);
    }
    f = baseline;
    f.remaining = -1;
    assert(settings_save(&io, &changed));
    f.pages[1][20] ^= 1;
    assert(settings_load(&io, &out, &seq) && seq == 1); /* corrupt new CRC -> old */
    f = baseline;
    f.read_fail = true;
    assert(!settings_save(&io, &changed));
    f = baseline;
    f.remaining = -1;
    bridge_t bridge;
    bridge_init(&bridge, &s);
    bridge_pair_begin(&bridge, 0);
    bridge_pair_delete(&bridge, 2000);
    assert(settings_save(&io, &bridge.settings));
    assert(settings_load(&io, &out, &seq) && seq == 2);
    pairing_slot_t empty = {0};
    assert(memcmp(&out.slots[1], &empty, sizeof(empty)) == 0);
    assert(memcmp(&out.slots[0], &s.slots[0], sizeof(pairing_slot_t)) == 0);
    /* Appending never erases either page and survives every torn halfword. */
    for (int cut=0;cut<=64;cut++) {
        f=baseline; f.remaining=cut;
        settings_append_result_t result=settings_append(&io,&changed);
        assert(settings_load(&io,&out,&seq) && (seq==1 || seq==2));
        assert(memcmp(&out,seq==1 ? &s : &changed,sizeof(out))==0);
        assert(memcmp(f.pages[0],baseline.pages[0],1024)==0);
        if (result==SETTINGS_APPENDED) assert(seq==2);
    }
    f=baseline; f.remaining=20;
    assert(settings_append(&io,&changed)==SETTINGS_APPEND_ERROR);
    f.remaining=-1;
    assert(settings_append(&io,&changed)==SETTINGS_APPENDED); /* skip torn record */
    assert(settings_load(&io,&out,&seq) && seq==2);
    f=baseline; f.remaining=-1;
    for (unsigned i=0;i<15;i++) {
        changed.active_slot=(uint8_t)(i&1);
        assert(settings_append(&io,&changed)==SETTINGS_APPENDED);
        assert(settings_load(&io,&out,&seq) && seq==i+2);
    }
    flash_t full=f;
    assert(settings_append(&io,&s)==SETTINGS_APPEND_FULL);
    assert(memcmp(&full,&f,sizeof(f))==0); /* full journal is not erased live */
    for (int cut=0;cut<=65;cut++) {
        f=full; f.remaining=cut;
        bool saved_now=settings_save(&io,&s);
        assert(settings_load(&io,&out,&seq) && (seq==16 || seq==17));
        assert(memcmp(&out,seq==16 ? &changed : &s,sizeof(out))==0);
        if (saved_now) assert(seq==17);
    }
}
static void test_usb(void) {
    bridge_t b;
    bridge_init(&b, NULL);
    uint8_t mac[6] = {2, 1, 2, 3, 4, 5};
    pro_usb_t u;
    pro_usb_init(&u, mac);
    uint8_t out[64], r[64] = {0x80, 1};
    assert(pro_usb_device_descriptor[8] == 0x7e && pro_usb_device_descriptor[9] == 5);
    assert(pro_usb_device_descriptor[10] == 9 && pro_usb_device_descriptor[11] == 0x20);
    assert(pro_usb_output(&u, &b, r, 2, 0, out) == 64 && out[0] == 0x81 && out[3] == 3 &&
           out[4] == 5 && out[9] == 2);
    r[1] = 4;
    assert(pro_usb_output(&u, &b, r, 2, 0, out) == 0 && u.usb_only);
    memset(r, 0, 64);
    r[0] = 1;
    r[10] = 0x02;
    assert(pro_usb_output(&u, &b, r, 11, 0, out) == 64 && out[13] == 0x82 && out[17] == 3 &&
           memcmp(out + 19, mac, 6) == 0);
    r[10] = 0x40;
    assert(pro_usb_output(&u, &b, r, 11, 0, out) == 64 && out[13] == 0 && !u.imu_enabled);
    r[11] = 1;
    pro_usb_output(&u, &b, r, 12, 0, out);
    assert(u.imu_enabled && out[13] == 0x80);
    r[10]=0x48; r[11]=1;
    pro_usb_output(&u,&b,r,12,0,out);
    assert(u.vibration_enabled && out[13]==0x80);
    uint8_t rumble_report[10]={0x10,0,0x28,0x61,0x42,0x50,0x4c,0x41,0x40,0x48};
    uint32_t rumble_sequence=u.rumble_sequence;
    assert(!pro_usb_output(&u,&b,rumble_report,10,0,out));
    assert(u.rumble_sequence==rumble_sequence+1 && memcmp(u.rumble,rumble_report+2,8)==0);
    assert(!pro_usb_output(&u,&b,rumble_report,10,0,out));
    assert(u.rumble_sequence==rumble_sequence+2); /* preserve identical refreshes */
    r[10]=0x38;
    r[11]=1; r[12]=0x60; r[13]=0x60; r[14]=0;
    assert(pro_usb_output(&u,&b,r,14,0,out)==64 && !out[13] && !u.home_sequence);
    assert(pro_usb_output(&u,&b,r,15,0,out)==64 && out[13]==0x80 && u.home_sequence==1);
    assert(memcmp(u.home_led,r+11,4)==0);
    for (unsigned i=4;i<38;i++) assert(u.home_led[i]==0); /* short SDL pattern is padded */
    for (unsigned i=11;i<64;i++) r[i]=(uint8_t)i;
    assert(pro_usb_output(&u,&b,r,64,0,out)==64 && out[13]==0x80 && u.home_sequence==2);
    assert(memcmp(u.home_led,r+11,38)==0); /* full patterns stay bounded */
    r[10] = 3;
    r[11] = 0x30;
    pro_usb_output(&u, &b, r, 12, 0, out);
    assert(u.streaming && out[13] == 0x80);
    r[10] = 0x10;
    r[11] = 0x20;
    r[12] = 0x60;
    r[13] = r[14] = 0;
    r[15] = 24;
    pro_usb_output(&u, &b, r, 16, 0, out);
    assert(out[13] == 0x90 && out[15] == 0x20 && out[16] == 0x60 && out[19] == 24);
    assert(out[26] == 0 && out[27] == 0x40); /* virtual accel scale */
    r[15] = 255;
    pro_usb_output(&u, &b, r, 16, 0, out);
    assert(out[13] == 0);
    memset(r + 11, 0xff, 4);
    r[15] = 1;
    pro_usb_output(&u, &b, r, 16, 0, out);
    assert(out[13] == 0);
    r[10] = 0x11;
    pro_usb_output(&u, &b, r, 64, 0, out);
    assert(out[13] == 0); /* no SPI writes */
    for (size_t n = 0; n < 11; ++n) {
        memset(r, 0, sizeof(r));
        r[0] = 1;
        assert(pro_usb_output(&u, &b, r, n, 0, out) == 0);
    }
    assert(pro_usb_output(&u, &b, r, 65, 0, out) == 0);
    /* Malformed length sweep must never touch response canaries. */
    struct {
        uint8_t pre[8], reply[64], post[8];
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    r[10] = 0x10;
    for (unsigned size = 0; size < 256; ++size) {
        r[11] = 0x20;
        r[12] = 0x60;
        r[13] = r[14] = 0;
        r[15] = (uint8_t)size;
        pro_usb_output(&u, &b, r, 16, 0, guarded.reply);
        for (unsigned i = 0; i < 8; ++i)
            assert(guarded.pre[i] == 0xa5 && guarded.post[i] == 0xa5);
    }
}
static unsigned packets;
static void packet(void *c, const uint8_t *data, size_t n) {
    (void)c;
    assert(data[0] == 4 && n == 3);
    ++packets;
}
static void test_h4(void) {
    h4_parser_t p;
    h4_init(&p, packet, NULL);
    packets = 0;
    uint8_t events[] = {4, 0x0e, 0, 4, 0x0f, 0};
    for (unsigned i = 0; i < sizeof(events); ++i)
        h4_feed(&p, events + i, 1);
    assert(packets == 2);
    uint8_t large[1208] = {2, 0, 0, 0xb0, 4}; /* 1200-byte ACL payload */
    memset(large + 5, 4, 1200);
    large[1205] = 4;
    large[1206] = 0x0e;
    large[1207] = 0;
    h4_feed(&p, large, sizeof(large));
    assert(p.oversized == 1 && packets == 3 && p.used == 0);
}
static void test_rda_packets(void) {
    uint8_t packets_out[2][14];
    rda5876_baud_packets(packets_out);
    static const uint8_t expected[2][14] = {
        {1, 2, 0xfd, 10, 0x80, 1, 0x60, 0, 0, 0x80, 0x60, 0xe3, 0x16, 0},
        {1, 2, 0xfd, 10, 0x80, 1, 0x40, 0, 0, 0x80, 0, 0xb3, 2, 6}};
    assert(memcmp(packets_out, expected, sizeof(expected)) == 0);
    uint8_t rf[4];
    rda5876_rf_bytes(0x30, 0x0129, rf);
    assert(rf[0] == 0x2c && rf[1] == 0x30 && rf[2] == 1 && rf[3] == 0x29);
    assert(rda5876_write_word(0, 0, 0x81, packets_out[0]) == 0);
}
static void test_diagnostic_hid(void) {
    diagnostic_hid_t state = {0};
    uint8_t request[64] = {0x80, 0xf0}, reply[64];
    memcpy(request + 2, "8BDI", 4);
    memcpy(request + 10, "test", 4);
    assert(diagnostic_hid_output(&state, request, 64, 123, reply) == 64);
    assert(reply[0] == 0x81 && reply[1] == 0xf0 && reply[8] == 123);
    assert(memcmp(reply + 32, "test", 4) == 0 && !state.boot_requested);
    request[1] = 0xf1;
    assert(diagnostic_hid_output(&state, request, 64, 200, reply) == 0);
    assert(!state.boot_requested);
    memcpy(request + 6, "BOOT", 4);
    for (size_t n = 0; n < 14; n++)
        assert(diagnostic_hid_output(&state, request, n, 200, reply) == 0);
    assert(diagnostic_hid_output(&state, request, 65, 200, reply) == 0);
    assert(diagnostic_hid_output(&state, request, 64, 200, reply) == 64);
    assert(state.boot_requested && state.boot_requested_ms == 200 && reply[7] == 1);
    assert(diagnostic_hid_output(&state, request, 64, 400, reply) == 64);
    assert(state.boot_requested_ms == 200);
}
int main(void) {
    test_merging();
    test_pairing();
    test_gyro_and_led();
    test_button();
    test_storage();
    test_usb();
    test_h4();
    test_rda_packets();
    test_diagnostic_hid();
    puts("PASS: merge/staleness/epochs, transactional pairing, gyro/LED, button, journal power-cut "
         "checks, USB bounds, H4 framing");
    return 0;
}
