#include "bridge.h"
#include <string.h>

static uint32_t elapsed(uint32_t now, uint32_t then) {
    return now - then;
}
static bool address_valid(const uint8_t a[6]) {
    unsigned zero = 0, ones = 0;
    for (unsigned i = 0; i < 6; ++i) {
        zero |= a[i];
        ones |= (unsigned)(a[i] ^ 255u);
    }
    return zero != 0 && ones != 0;
}
static bool bond_valid(const bond_t *b, peer_kind_t kind) {
    return b->kind == kind && b->key_type <= 8u && address_valid(b->address);
}
bool settings_valid(const settings_t *s) {
    if (s->active_slot >= BRIDGE_SLOT_COUNT)
        return false;
    for (unsigned i = 0; i < BRIDGE_SLOT_COUNT; ++i) {
        const pairing_slot_t *slot = &s->slots[i];
        if (slot->gyro_source > GYRO_RIGHT)
            return false;
        if (slot->kind == SLOT_EMPTY)
            continue;
        if (slot->kind != (i == 0 ? SLOT_PRO : SLOT_JOYCONS))
            return false;
        if (slot->kind == SLOT_PRO) {
            if (!bond_valid(&slot->peers[0], PEER_PRO))
                return false;
        } else if (slot->kind == SLOT_JOYCONS) {
            if (!bond_valid(&slot->peers[0], PEER_LEFT) ||
                !bond_valid(&slot->peers[1], PEER_RIGHT) ||
                memcmp(slot->peers[0].address, slot->peers[1].address, 6) == 0)
                return false;
        } else
            return false;
    }
    /* One physical controller may not belong to two slots. */
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned b = 0; b < 2; ++b) {
            const pairing_slot_t *s0 = &s->slots[0], *s1 = &s->slots[1];
            unsigned n0 = s0->kind == SLOT_JOYCONS ? 2u : s0->kind == SLOT_PRO ? 1u : 0u;
            unsigned n1 = s1->kind == SLOT_JOYCONS ? 2u : s1->kind == SLOT_PRO ? 1u : 0u;
            if (a < n0 && b < n1 && memcmp(s0->peers[a].address, s1->peers[b].address, 6) == 0)
                return false;
        }
    return true;
}
void sample_neutral(controller_sample_t *s) {
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < 4; ++i)
        s->sticks[i] = 2048;
    for (unsigned i = 0; i < 3; ++i)
        s->imu[i][2] = 4096; /* stationary +1g */
}
void bridge_init(bridge_t *b, const settings_t *s) {
    memset(b, 0, sizeof(*b));
    if (s != NULL && settings_valid(s))
        b->settings = *s;
    b->epoch = 1;
    /* Avoid interpreting the second half of a boot-time press as a click. */
    b->button.wait_release = true;
}
static void clear_session(bridge_t *b) {
    memset(b->live, 0, sizeof(b->live));
    ++b->epoch;
    ++b->input_sequence;
    b->chord_down = b->chord_latched = false;
}
static void clear_pending(bridge_t *b) {
    b->pairing = false;
    memset(&b->pending, 0, sizeof(b->pending));
    memset(b->pending_present, 0, sizeof(b->pending_present));
}
void bridge_switch_slot(bridge_t *b, uint32_t now) {
    clear_pending(b);
    b->settings.active_slot ^= 1u;
    b->settings_dirty = true;
    clear_session(b);
    b->slot_notice = true;
    b->slot_notice_ms = now;
    b->notice_pulses = b->settings.active_slot + 1u;
}
void bridge_pair_begin(bridge_t *b, uint32_t now) {
    ++b->epoch; /* Transaction generation; existing saved input remains valid. */
    clear_pending(b);
    b->pending = b->settings.slots[b->settings.active_slot];
    if (b->pending.kind == SLOT_EMPTY)
        b->pending.gyro_source = GYRO_RIGHT;
    b->pair_started_ms = now;
    b->pairing = true;
    b->slot_notice = false;
    /* Seed the opposite Joy-Con from the saved pair, without changing storage. */
}
void bridge_pair_cancel(bridge_t *b) {
    if (!b->pairing) return;
    clear_pending(b);
    ++b->epoch;
}
void bridge_pair_delete(bridge_t *b, uint32_t now) {
    clear_pending(b);
    memset(&b->settings.slots[b->settings.active_slot], 0, sizeof(pairing_slot_t));
    b->settings_dirty = true;
    clear_session(b);
    b->slot_notice = true;
    b->slot_notice_ms = now;
    b->notice_pulses = 3;
}
bool bridge_pair_offer(bridge_t *b, const bond_t *bond, uint32_t now) {
    if (!b->pairing || elapsed(now, b->pair_started_ms) >= BRIDGE_PAIR_MS ||
        bond->kind < PEER_PRO || bond->kind > PEER_RIGHT ||
        !bond_valid(bond, (peer_kind_t)bond->kind))
        return false;
    unsigned other = b->settings.active_slot ^ 1u;
    const pairing_slot_t *os = &b->settings.slots[other];
    unsigned count = os->kind == SLOT_JOYCONS ? 2u : os->kind == SLOT_PRO ? 1u : 0u;
    for (unsigned i = 0; i < count; ++i)
        if (memcmp(os->peers[i].address, bond->address, 6) == 0)
            return false;
    slot_kind_t kind = bond->kind == PEER_PRO ? SLOT_PRO : SLOT_JOYCONS;
    if (kind != (b->settings.active_slot == 0 ? SLOT_PRO : SLOT_JOYCONS))
        return false;
    if (b->pending.kind != SLOT_EMPTY && b->pending.kind != kind)
        return false;
    unsigned index = bond->kind == PEER_RIGHT ? 1u : 0u;
    if (b->pending_present[index])
        return false;
    if (b->pending.peers[index ^ 1u].kind != PEER_NONE &&
        memcmp(b->pending.peers[index ^ 1u].address, bond->address, 6) == 0)
        return false;
    b->pending.kind = (uint8_t)kind;
    b->pending.peers[index] = *bond;
    b->pending_present[index] = true;
    if (kind == SLOT_PRO || (bond_valid(&b->pending.peers[0], PEER_LEFT) &&
                             bond_valid(&b->pending.peers[1], PEER_RIGHT))) {
        const pairing_slot_t *old=&b->settings.slots[b->settings.active_slot];
        for (unsigned i=0;i<2;i++)
            if (memcmp(&old->peers[i],&b->pending.peers[i],sizeof(bond_t))!=0)
                b->live[i].valid=false;
        b->settings.slots[b->settings.active_slot] = b->pending;
        b->settings_dirty = true;
        clear_pending(b);
        ++b->epoch;
        ++b->input_sequence; /* Retain the unchanged half through the commit. */
    }
    return true;
}
bool bridge_admit(const bridge_t *b, const uint8_t address[6], unsigned *index) {
    const pairing_slot_t *s = &b->settings.slots[b->settings.active_slot];
    unsigned count = s->kind == SLOT_JOYCONS ? 2u : s->kind == SLOT_PRO ? 1u : 0u;
    for (unsigned i = 0; i < count; ++i)
        if (memcmp(s->peers[i].address, address, 6) == 0) {
            if (index != NULL)
                *index = i;
            return true;
        }
    return false;
}
bool bridge_publish(bridge_t *b, uint32_t epoch, unsigned index, const controller_sample_t *s,
                    uint32_t now) {
    const pairing_slot_t *slot = &b->settings.slots[b->settings.active_slot];
    unsigned count = slot->kind == SLOT_JOYCONS ? 2u : slot->kind == SLOT_PRO ? 1u : 0u;
    if (epoch != b->epoch || index >= count || s->battery > 8u ||
        (s->battery & 1u) != 0)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (s->sticks[i] > 4095u)
            return false;
    b->live[index].sample = *s;
    b->live[index].last_ms = now;
    b->live[index].valid = true;
    if (slot->kind==SLOT_PRO || index==slot->gyro_source)
        ++b->input_sequence;
    return true;
}
void bridge_disconnect(bridge_t *b, uint32_t epoch, unsigned index) {
    if (epoch == b->epoch && index < 2) {
        b->live[index].valid = false;
        ++b->input_sequence;
    }
}
static bool fresh(const bridge_t *b, unsigned i, uint32_t now) {
    return b->live[i].valid && elapsed(now, b->live[i].last_ms) < BRIDGE_STALE_MS;
}
void bridge_snapshot(const bridge_t *b, uint32_t now, controller_sample_t *out) {
    sample_neutral(out);
    const pairing_slot_t *slot = &b->settings.slots[b->settings.active_slot];
    if (slot->kind == SLOT_PRO) {
        if (b->live[0].valid) out->battery=b->live[0].sample.battery;
        if (fresh(b, 0, now))
            *out = b->live[0].sample;
        return;
    }
    if (slot->kind != SLOT_JOYCONS)
        return;
    /* Battery is link metadata, not a held control: preserve the last known
     * level across input gaps while neutralizing stale buttons/sticks. */
    if (b->live[0].valid) out->battery=b->live[0].sample.battery;
    if (b->live[1].valid && (!b->live[0].valid || b->live[1].sample.battery<out->battery))
        out->battery=b->live[1].sample.battery;
    bool left = fresh(b, 0, now), right = fresh(b, 1, now);
    if (left) {
        const controller_sample_t *s = &b->live[0].sample;
        out->buttons[1] |= s->buttons[1] & 0x29u; /* -, L-stick, capture */
        out->buttons[2] = s->buttons[2] & 0xcfu;  /* no rail SL/SR */
        out->sticks[0] = s->sticks[0];
        out->sticks[1] = s->sticks[1];
    }
    if (right) {
        const controller_sample_t *s = &b->live[1].sample;
        out->buttons[0] = s->buttons[0] & 0xcfu;
        out->buttons[1] |= s->buttons[1] & 0x16u; /* +, R-stick, home */
        out->sticks[2] = s->sticks[2];
        out->sticks[3] = s->sticks[3];
    }
    if (fresh(b, slot->gyro_source == GYRO_LEFT ? 0u : 1u, now))
        memcpy(out->imu, b->live[slot->gyro_source].sample.imu, sizeof(out->imu));
    if (b->chord_down) { /* Capture+L+R is receiver-local while held. */
        out->buttons[0] &= (uint8_t)~0x40u;
        out->buttons[1] &= (uint8_t)~0x20u;
        out->buttons[2] &= (uint8_t)~0x40u;
    }
}
void bridge_tick(bridge_t *b, uint32_t now) {
    if (b->pairing && elapsed(now, b->pair_started_ms) >= BRIDGE_PAIR_MS) {
        bridge_pair_cancel(b);
    }
    if (b->slot_notice && elapsed(now, b->slot_notice_ms) >= 1000u)
        b->slot_notice = false;
    if (b->settings.slots[b->settings.active_slot].kind != SLOT_JOYCONS || b->pairing)
        return;
    /* Read source state directly; the public snapshot suppresses this chord. */
    bool down =
        fresh(b, 0, now) && fresh(b, 1, now) && (b->live[0].sample.buttons[1] & 0x20u) != 0 &&
        (b->live[0].sample.buttons[2] & 0x40u) != 0 && (b->live[1].sample.buttons[0] & 0x40u) != 0;
    if (down && !b->chord_down)
        b->chord_started_ms = now;
    if (down && !b->chord_latched && elapsed(now, b->chord_started_ms) >= 1000u) {
        b->settings.slots[b->settings.active_slot].gyro_source ^= 1u;
        ++b->input_sequence;
        b->settings_dirty = true;
        b->chord_latched = true;
        b->slot_notice = true;
        b->slot_notice_ms = now;
        b->notice_pulses = b->settings.slots[b->settings.active_slot].gyro_source + 1u;
    }
    if (!down)
        b->chord_latched = false;
    b->chord_down = down;
}
button_action_t bridge_button(bridge_t *b, bool pressed, uint32_t now) {
    button_state_t *s = &b->button;
    if (s->wait_release) {
        if (!pressed) {
            memset(s, 0, sizeof(*s));
            s->edge_ms = now;
        }
        return BUTTON_NONE;
    }
    if (pressed != s->raw) {
        s->raw = pressed;
        s->edge_ms = now;
    }
    if (s->first_click && elapsed(now, s->release_ms) > 350u && !s->stable)
        s->first_click = false;
    if (s->stable != s->raw && elapsed(now, s->edge_ms) >= 20u) {
        s->stable = s->raw;
        if (s->stable) {
            s->press_ms = now;
            s->held = false;
        } else if (!s->held) {
            if (b->pairing) {
                s->first_click = false;
                bridge_pair_cancel(b);
                return BUTTON_CANCEL_PAIR;
            }
            if (s->first_click && elapsed(now, s->release_ms) <= 350u) {
                s->first_click = false;
                bridge_switch_slot(b, now);
                return BUTTON_SWITCH_SLOT;
            }
            s->first_click = true;
            s->release_ms = now;
        }
    }
    if (s->stable && !s->held && elapsed(now, s->press_ms) >= 2000u) {
        s->held = true;
        s->first_click = false;
        if (b->pairing) {
            bridge_pair_delete(b, now);
            return BUTTON_DELETE_PAIR;
        }
        bridge_pair_begin(b, now);
        return BUTTON_PAIR;
    }
    return BUTTON_NONE;
}
uint8_t bridge_led(const bridge_t *b, uint32_t now) {
    if (b->pairing)
        return elapsed(now, b->pair_started_ms) % 400u < 80u ? 96u : 0u;
    if (b->slot_notice) {
        uint32_t t = elapsed(now, b->slot_notice_ms);
        return t < (uint32_t)b->notice_pulses * 200u && t % 200u < 60u ? 96u : 0u;
    }
    const pairing_slot_t *s = &b->settings.slots[b->settings.active_slot];
    if (s->kind == SLOT_EMPTY)
        return now % 3000u < 30u ? 32u : 0u;
    bool left = fresh(b, 0, now), right = fresh(b, 1, now);
    if (left && (s->kind == SLOT_PRO || right))
        return 16u; /* steady dim, hardware PWM */
    if (s->kind == SLOT_JOYCONS && (left || right))
        return now % 2000u < 40u || (now % 2000u >= 160u && now % 2000u < 200u) ? 64u : 0u;
    return now % 2000u < 40u ? 48u : 0u;
}
static void pack_stick(uint8_t *out, uint16_t x, uint16_t y) {
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)((x >> 8) | ((y & 15u) << 4));
    out[2] = (uint8_t)(y >> 4);
}
void bridge_report30(const bridge_t *b, bool imu, uint32_t now, uint8_t out[64]) {
    controller_sample_t s;
    bridge_snapshot(b, now, &s);
    memset(out, 0, 64);
    out[0] = 0x30;
    out[1] = (uint8_t)(now / 5u);
    out[2] = (uint8_t)((s.battery << 4) | 1u);
    memcpy(out + 3, s.buttons, 3);
    pack_stick(out + 6, s.sticks[0], s.sticks[1]);
    pack_stick(out + 9, s.sticks[2], s.sticks[3]);
    if (imu)
        for (unsigned frame = 0; frame < 3; ++frame)
            for (unsigned axis = 0; axis < 6; ++axis) {
                unsigned offset = 13u + frame * 12u + axis * 2u;
                uint16_t value = (uint16_t)s.imu[frame][axis];
                out[offset] = (uint8_t)value;
                out[offset + 1] = (uint8_t)(value >> 8);
            }
}
