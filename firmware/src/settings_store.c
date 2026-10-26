#include "settings_store.h"
#include <string.h>
#define RECORD_MAGIC 0x31564452u
#define RECORD_COMMIT 0xa55au
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8u * i));
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}
static void encode(const settings_t *s, uint32_t seq, uint8_t r[128]) {
    memset(r, 0xff, 128);
    put32(r, RECORD_MAGIC);
    put16(r + 4, 1);
    put16(r + 6, 128);
    put32(r + 8, seq);
    r[12] = s->active_slot;
    for (unsigned slot = 0; slot < 2; ++slot) {
        size_t o = 16u + slot * 50u;
        r[o++] = s->slots[slot].kind;
        r[o++] = s->slots[slot].gyro_source;
        for (unsigned peer = 0; peer < 2; ++peer) {
            const bond_t *b = &s->slots[slot].peers[peer];
            memcpy(r + o, b->address, 6);
            memcpy(r + o + 6, b->link_key, 16);
            r[o + 22] = b->key_type;
            r[o + 23] = b->kind;
            o += 24;
        }
    }
    put32(r + 120, crc32(r, 120)); /* 124..125 remain erased; commit at 126 */
}
static bool decode(const uint8_t r[128], settings_t *s, uint32_t *seq) {
    if (get32(r) != RECORD_MAGIC || get16(r + 4) != 1 || get16(r + 6) != 128 ||
        get16(r + 126) != RECORD_COMMIT || get32(r + 120) != crc32(r, 120))
        return false;
    memset(s, 0, sizeof(*s));
    s->active_slot = r[12];
    *seq = get32(r + 8);
    for (unsigned slot = 0; slot < 2; ++slot) {
        size_t o = 16u + slot * 50u;
        s->slots[slot].kind = r[o++];
        s->slots[slot].gyro_source = r[o++];
        for (unsigned peer = 0; peer < 2; ++peer) {
            bond_t *b = &s->slots[slot].peers[peer];
            memcpy(b->address, r + o, 6);
            memcpy(b->link_key, r + o + 6, 16);
            b->key_type = r[o + 22];
            b->kind = r[o + 23];
            o += 24;
        }
    }
    return settings_valid(s);
}
static int latest(const settings_io_t *io, settings_t *s, uint32_t *seq) {
    settings_t candidate;
    uint32_t candidate_seq = 0;
    int page = -1;
    uint8_t r[128];
    for (unsigned i = 0; i < 2; ++i) {
      for (size_t offset=0;offset<SETTINGS_PAGE_SIZE;offset+=sizeof(r)) {
        if (!io->read(io->context, i, offset, r, sizeof(r)))
            return -2; /* I/O error is not erased flash */
        if (decode(r, &candidate, &candidate_seq) &&
            (page < 0 || (candidate_seq - *seq != 0u && candidate_seq - *seq < 0x80000000u))) {
            *s = candidate;
            *seq = candidate_seq;
            page = (int)i;
        }
      }
    }
    return page;
}
bool settings_load(const settings_io_t *io, settings_t *s, uint32_t *seq) {
    *seq = 0;
    memset(s, 0, sizeof(*s));
    return latest(io, s, seq) >= 0;
}
static bool write_record(const settings_io_t *io,const settings_t *s,unsigned page,size_t offset,uint32_t seq) {
    uint8_t record[128], verify[128];
    encode(s,seq,record);
    if (!io->program(io->context,page,offset,record,126) ||
        !io->read(io->context,page,offset,verify,128) || memcmp(record,verify,128)!=0) return false;
    uint8_t commit[2]; put16(commit,RECORD_COMMIT);
    if (!io->program(io->context,page,offset+126,commit,2) ||
        !io->read(io->context,page,offset,verify,128)) return false;
    settings_t decoded; uint32_t decoded_seq;
    return decode(verify,&decoded,&decoded_seq) && decoded_seq==seq;
}
settings_append_result_t settings_append(const settings_io_t *io,const settings_t *s) {
    if (!settings_valid(s)) return SETTINGS_APPEND_ERROR;
    uint32_t seq=0; settings_t old;
    int active=latest(io,&old,&seq);
    if (active==-2) return SETTINGS_APPEND_ERROR;
    uint8_t record[128];
    for (unsigned i=0;i<2;i++) {
        unsigned page=active<0 ? i : ((unsigned)active^1u^i);
        for (size_t offset=0;offset<SETTINGS_PAGE_SIZE;offset+=sizeof(record)) {
            if (!io->read(io->context,page,offset,record,sizeof(record))) return SETTINGS_APPEND_ERROR;
            bool erased=true;
            for (unsigned j=0;j<sizeof(record);j++) if (record[j]!=0xff) { erased=false; break; }
            if (erased) return write_record(io,s,page,offset,seq+1u) ? SETTINGS_APPENDED : SETTINGS_APPEND_ERROR;
        }
    }
    return SETTINGS_APPEND_FULL;
}
bool settings_save(const settings_io_t *io, const settings_t *s) {
    if (!settings_valid(s))
        return false;
    uint32_t seq = 0;
    settings_t old;
    int active = latest(io, &old, &seq);
    if (active == -2)
        return false;
    unsigned next = active < 0 ? 0u : (unsigned)active ^ 1u;
    return io->erase(io->context,next) && write_record(io,s,next,0,seq+1u);
}
