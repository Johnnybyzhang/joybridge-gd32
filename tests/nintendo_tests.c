#include "nintendo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put16(uint8_t *p,int value) { p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void pack(uint8_t *p,unsigned x,unsigned y) { p[0]=(uint8_t)x; p[1]=(uint8_t)((x>>8)|(y<<4)); p[2]=(uint8_t)(y>>4); }
int main(void) {
    static const uint8_t address[]={0x50,0x23,0x6d,0x95,0x7c,0x7a};
    nintendo_t s; nintendo_init(&s,address);
    uint8_t command[49],reply[50];
    for (unsigned stage=0;stage<10;stage++) {
        assert(nintendo_command(&s,stage*100,command)==49);
        nintendo_sent(&s,stage*100);
        memset(reply,0,sizeof(reply)); reply[0]=0x21; reply[13]=0x80; reply[14]=command[10];
        if (!stage) { reply[17]=3; memcpy(reply+19,address,6); }
        else if (stage<=4) {
            memcpy(reply+15,command+11,5); uint8_t *cal=reply+20;
            if (stage==1) {
                for (unsigned i=0;i<3;i++) { put16(cal+6+2*i,16384); put16(cal+18+2*i,13371); }
            } else if (stage==2) {
                pack(cal,1536,1536); pack(cal+3,2048,2048); pack(cal+6,1536,1536);
                pack(cal+9,2048,2048); pack(cal+12,1536,1536); pack(cal+15,1536,1536);
            } else memset(cal,0xff,command[15]);
            assert(!nintendo_input(&s,reply,19)); assert(s.stage==stage);
        }
        assert(!nintendo_input(&s,reply,sizeof(reply))); assert(s.stage==stage+1); assert(!s.error);
    }
    assert(s.ready && s.kind==PEER_PRO);
    memset(reply,0,sizeof(reply)); reply[0]=0x30; reply[1]=17; reply[2]=0x90; reply[3]=8;
    pack(reply+6,2048,2048); pack(reply+9,3584,512);
    for (unsigned i=0;i<3;i++) { put16(reply+17+12*i,4096); put16(reply+19+12*i,-100); }
    assert(!nintendo_input(&s,reply,48));
    assert(nintendo_input(&s,reply,49));
    assert(s.sample.battery==8 && s.sample.buttons[0]==8 && s.sample.sticks[0]==2048);
    assert(s.sample.sticks[2]==3584 && s.sample.sticks[3]==512);
    assert(s.sample.imu[2][2]==4096 && s.sample.imu[2][3]==-100);
    assert(!nintendo_input(&s,reply,49) && s.sequence==1);
    uint8_t rumble[8]={0,1,0x40,0x40,0,1,0x40,0x40};
    nintendo_queue_output(&s,rumble,2,false);
    assert(nintendo_command(&s,1100,command)==49 && command[10]==0x30 && command[11]==2);
    nintendo_sent(&s,1100);
    nintendo_queue_output(&s,rumble,4,false); /* Coalesce while an earlier ACK is pending. */
    assert(!nintendo_command(&s,1110,command));
    memset(reply,0,sizeof(reply)); reply[0]=0x21; reply[13]=0x80; reply[14]=0x30;
    nintendo_input(&s,reply,15);
    assert(s.stage==10 && s.ready && !s.waiting);
    assert(nintendo_command(&s,1120,command)==49 && command[11]==4);
    nintendo_sent(&s,1120); nintendo_input(&s,reply,15);
    assert(!nintendo_command(&s,1130,command));
    /* Raw HD data, including identical refreshes, uses unacknowledged 0x10. */
    uint8_t hd[8]={0x28,0x61,0x42,0x50,0x4c,0x41,0x40,0x48};
    nintendo_queue_output(&s,hd,4,true);
    assert(nintendo_command(&s,1140,command)==10 && command[0]==0x10);
    assert(memcmp(command+2,hd,8)==0);
    nintendo_sent(&s,1140);
    assert(!s.waiting && !nintendo_command(&s,1141,command));
    nintendo_queue_output(&s,hd,4,true);
    assert(nintendo_command(&s,1142,command)==10); /* same bytes still refresh */
    nintendo_sent(&s,1142);
    nintendo_queue_output(&s,hd,2,false);
    assert(nintendo_command(&s,1150,command)==49 && command[10]==0x30);
    nintendo_sent(&s,1150);
    hd[1]^=2;
    nintendo_queue_output(&s,hd,2,true);
    assert(nintendo_command(&s,1151,command)==10 && memcmp(command+2,hd,8)==0);
    nintendo_sent(&s,1151);
    assert(s.waiting && s.expected==0x30 && s.sent_ms==1150); /* rumble never blocks on LED ACK */
    nintendo_input(&s,reply,15);
    assert(!s.waiting && !nintendo_command(&s,1152,command));
    nintendo_queue_output(&s,rumble,2,true);
    assert(nintendo_command(&s,1153,command)==10 && memcmp(command+2,rumble,8)==0);
    nintendo_sent(&s,1153); /* explicit stop */
    uint8_t home[38]={1,0x60,0x60,0};
    nintendo_queue_home(&s,home);
    nintendo_queue_output(&s,rumble,8,false);
    assert(nintendo_command(&s,1160,command)==49 && command[10]==0x38);
    assert(memcmp(command+11,home,38)==0);
    nintendo_sent(&s,1160);
    assert(s.waiting && !s.home_pending && s.output_pending); /* HOME never loses player LEDs */
    nintendo_queue_output(&s,hd,8,true);
    assert(nintendo_command(&s,1161,command)==10 && command[0]==0x10);
    nintendo_sent(&s,1161);
    assert(s.waiting && s.expected==0x38 && s.sent_ms==1160);
    home[1]=0x30; nintendo_queue_home(&s,home); /* newer pattern survives the old ACK */
    reply[14]=0x38; nintendo_input(&s,reply,15);
    assert(nintendo_command(&s,1162,command)==49 && command[10]==0x38 && command[12]==0x30);
    nintendo_sent(&s,1162);
    assert(!nintendo_command(&s,1661,command));
    assert(nintendo_command(&s,1662,command)==49 && command[10]==0x38 && command[12]==0x30);
    nintendo_sent(&s,1662); nintendo_input(&s,reply,15);
    assert(nintendo_command(&s,1663,command)==49 && command[10]==0x30 && command[11]==8);
    nintendo_sent(&s,1663); reply[14]=0x30; nintendo_input(&s,reply,15);
    assert(!nintendo_command(&s,1664,command));
    nintendo_init(&s,address); assert(nintendo_command(&s,0,command)); nintendo_sent(&s,0);
    assert(!nintendo_command(&s,499,command)); assert(nintendo_command(&s,500,command));
    nintendo_sent(&s,500); memset(reply,0,sizeof(reply)); reply[0]=0x21; reply[13]=0x80; reply[14]=2; reply[17]=3;
    assert(!nintendo_input(&s,reply,50) && s.error==3); /* Mismatched device identity. */
    puts("PASS: Nintendo setup/calibration, rumble refresh, HOME/player LED ordering, bounded replies, signed IMU, identity guard");
    return 0;
}
