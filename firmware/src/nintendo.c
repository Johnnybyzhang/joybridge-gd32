#include "nintendo.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static int16_t i16(const uint8_t *p) { return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void unpack(const uint8_t *p, uint16_t *x, uint16_t *y) {
    *x = p[0] | ((uint16_t)(p[1] & 15) << 8);
    *y = (p[1] >> 4) | ((uint16_t)p[2] << 4);
}
static bool stick_cal(nintendo_t *s, unsigned hand, const uint8_t *p) {
    uint16_t values[6];
    for (unsigned i=0;i<3;i++) unpack(p+3*i, &values[2*i], &values[2*i+1]);
    unsigned center = hand ? 0 : 2, neg = hand ? 2 : 4, pos = hand ? 4 : 0;
    for (unsigned i=0;i<2;i++)
        if (values[center+i] < 512 || values[center+i] > 3583 ||
            values[neg+i] < 128 || values[pos+i] < 128 ||
            values[neg+i] > values[center+i] || values[pos+i] > 4095-values[center+i]) return false;
    for (unsigned i=0;i<2;i++) {
        s->center[2*hand+i]=values[center+i]; s->negative[2*hand+i]=values[neg+i];
        s->positive[2*hand+i]=values[pos+i];
    }
    return true;
}
static bool imu_cal(nintendo_t *s, const uint8_t *p) {
    int16_t origin[6], sensitivity[6];
    for (unsigned i=0;i<6;i++) {
        unsigned offset = i<3 ? i*2 : 12+(i-3)*2;
        origin[i]=i16(p+offset); sensitivity[i]=i16(p+offset+6);
        int32_t divisor=(int32_t)sensitivity[i]-origin[i];
        if (divisor<4096 || divisor>30000) return false;
    }
    memcpy(s->origin,origin,sizeof(origin)); memcpy(s->sensitivity,sensitivity,sizeof(sensitivity));
    return true;
}
void nintendo_init(nintendo_t *s, const uint8_t address[6]) {
    memset(s,0,sizeof(*s)); memcpy(s->address,address,6);
    static const uint8_t neutral[] = {0,1,0x40,0x40,0,1,0x40,0x40};
    memcpy(s->rumble,neutral,8); s->player_leds=1; sample_neutral(&s->sample);
}
void nintendo_queue_output(nintendo_t *s,const uint8_t rumble[8],uint8_t player_leds,bool rumble_event) {
    if (rumble_event || memcmp(s->rumble,rumble,8)!=0) s->rumble_pending=true;
    if (s->player_leds!=player_leds) s->output_pending=true;
    memcpy(s->rumble,rumble,8); s->player_leds=player_leds;
}
static const uint32_t spi_addresses[] = {0x6020,0x603d,0x8010,0x8026};
void nintendo_queue_home(nintendo_t *s,const uint8_t pattern[38]) {
    memcpy(s->home_led,pattern,38); s->home_pending=true;
}
static const uint8_t spi_lengths[] = {24,18,22,26};
size_t nintendo_command(nintendo_t *s, uint32_t now, uint8_t out[49]) {
    if (s->error) return 0;
    if (s->waiting) {
        if ((uint32_t)(now-s->sent_ms)>=500) {
            if (++s->retries>3) { s->error=1; return 0; }
            s->waiting=false;
            if (s->ready) {
                if (s->expected==0x38) s->home_pending=true;
                else s->output_pending=true;
            }
        }
    }
    bool rumble_only=s->ready && s->rumble_pending && (s->waiting || (!s->output_pending && !s->home_pending));
    if (!rumble_only && (s->waiting || (s->ready && !s->output_pending && !s->home_pending))) return 0;
    memset(out,0,49); out[0]=1; out[1]=s->counter&15; memcpy(out+2,s->rumble,8);
    if (rumble_only) {
        out[0]=0x10; s->sending_report=0x10; return 10;
    }
    if (s->ready && s->home_pending) { out[10]=0x38; memcpy(out+11,s->home_led,38); }
    else if (s->ready) { out[10]=0x30; out[11]=s->player_leds; }
    else if (s->stage==0) out[10]=2;
    else if (s->stage<=4) {
        unsigned i=s->stage-1; out[10]=0x10;
        uint32_t address=spi_addresses[i];
        for (unsigned j=0;j<4;j++) out[11+j]=(uint8_t)(address>>(8*j));
        out[15]=spi_lengths[i];
    } else if (s->stage==5) { out[10]=0x41; out[11]=3; out[12]=0; out[13]=1; out[14]=1; }
    else if (s->stage==6) { out[10]=0x40; out[11]=1; }
    else if (s->stage==7) { out[10]=0x48; out[11]=1; }
    else if (s->stage==8) { out[10]=0x30; out[11]=s->player_leds; }
    else { out[10]=3; out[11]=0x30; }
    s->expected=out[10];
    s->sending_report=1;
    return 49;
}
void nintendo_sent(nintendo_t *s,uint32_t now) {
    s->counter++;
    if (s->sending_report==0x10) { s->rumble_pending=false; return; }
    s->waiting=true; s->sent_ms=now;
    s->rumble_pending=false;
    if (s->ready) {
        if (s->expected==0x38) s->home_pending=false;
        else s->output_pending=false;
    }
}
static int16_t clamp16(int32_t value) { return value>32767 ? 32767 : value< -32768 ? -32768 : (int16_t)value; }
bool nintendo_input(nintendo_t *s,const uint8_t *p,size_t n) {
    if (n<1) return false;
    if (p[0]==0x21 && n>=15 && s->waiting && p[14]==s->expected) {
        if (!(p[13]&0x80)) { s->error=2; return false; }
        if (s->ready) { s->waiting=false; s->retries=0; return false; }
        if (s->stage==0) {
            if (n<25 || p[17]<1 || p[17]>3 || memcmp(p+19,s->address,6)!=0) { s->error=3; return false; }
            s->kind=p[17]==3 ? PEER_PRO : p[17]==1 ? PEER_LEFT : PEER_RIGHT;
        } else if (s->stage<=4) {
            unsigned i=s->stage-1;
            if (n<20u+spi_lengths[i] || u32(p+15)!=spi_addresses[i] || p[19]!=spi_lengths[i]) return false;
            const uint8_t *cal=p+20;
            bool ok=true;
            if (s->stage==1) ok=imu_cal(s,cal);
            else if (s->stage==2) {
                if (s->kind!=PEER_RIGHT) ok=stick_cal(s,0,cal);
                if (s->kind!=PEER_LEFT) ok=stick_cal(s,1,cal+9)&&ok;
            } else if (s->stage==3) {
                if (s->kind!=PEER_RIGHT && u16(cal)==0xa1b2) ok=stick_cal(s,0,cal+2);
                if (s->kind!=PEER_LEFT && u16(cal+11)==0xa1b2) ok=stick_cal(s,1,cal+13)&&ok;
            } else if (u16(cal)==0xa1b2) ok=imu_cal(s,cal+2);
            if (!ok) { s->error=4; return false; }
        }
        s->waiting=false; s->retries=0;
        if (++s->stage==10) s->ready=true;
    }
    if (p[0]!=0x30 || n<49 || !s->ready || (s->timer_valid && s->timer==p[1])) return false;
    s->timer=p[1]; s->timer_valid=true;
    sample_neutral(&s->sample); memcpy(s->sample.buttons,p+3,3);
    s->sample.battery=(p[2]>>4)&0x0e; if (s->sample.battery>8) s->sample.battery=8;
    uint16_t sticks[4]; unpack(p+6,&sticks[0],&sticks[1]); unpack(p+9,&sticks[2],&sticks[3]);
    for (unsigned i=0;i<4;i++) {
        if ((s->kind==PEER_LEFT && i>=2) || (s->kind==PEER_RIGHT && i<2)) continue;
        int32_t delta=(int32_t)sticks[i]-s->center[i];
        uint16_t range=delta>=0 ? s->positive[i] : s->negative[i];
        int32_t value=2048+delta*1536/range;
        s->sample.sticks[i]=(uint16_t)(value<0 ? 0 : value>4095 ? 4095 : value);
    }
    for (unsigned frame=0;frame<3;frame++) for (unsigned axis=0;axis<6;axis++) {
        int32_t raw=i16(p+13+frame*12+axis*2);
        if (axis>=3) raw-=s->origin[axis];
        int32_t calibrated=raw*(axis<3 ? 16384 : 13371)/
                                ((int32_t)s->sensitivity[axis]-s->origin[axis]);
        /* Right Joy-Con's Y/Z sensor axes oppose left/Pro in paired grip. */
        if (s->kind==PEER_RIGHT && axis%3!=0) calibrated=-calibrated;
        s->sample.imu[frame][axis]=clamp16(calibrated);
    }
    s->sequence++; return true;
}
