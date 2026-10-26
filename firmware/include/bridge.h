#ifndef REDEV_BRIDGE_H
#define REDEV_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_SLOT_COUNT 2u
#define BRIDGE_STALE_MS 100u
#define BRIDGE_PAIR_MS 60000u
#define BRIDGE_USB_VID 0x057eu
#define BRIDGE_USB_PID 0x2009u
#define BRIDGE_USB_REPORT_SIZE 64u

typedef enum { PEER_NONE, PEER_PRO, PEER_LEFT, PEER_RIGHT } peer_kind_t;
typedef enum { SLOT_EMPTY, SLOT_PRO, SLOT_JOYCONS } slot_kind_t;
typedef enum { GYRO_LEFT, GYRO_RIGHT } gyro_source_t;
typedef enum {
    BUTTON_NONE, BUTTON_SWITCH_SLOT, BUTTON_PAIR, BUTTON_CANCEL_PAIR, BUTTON_DELETE_PAIR
} button_action_t;

/* Canonical Bluetooth address order, not HCI wire order. No keys in logs. */
typedef struct {
    uint8_t address[6];
    uint8_t link_key[16];
    uint8_t key_type;
    uint8_t kind;
} bond_t;

typedef struct {
    uint8_t kind;
    uint8_t gyro_source;
    bond_t peers[2]; /* Pro: index 0. Joy-Cons: left 0, right 1. */
} pairing_slot_t;

typedef struct {
    uint8_t active_slot;
    pairing_slot_t slots[BRIDGE_SLOT_COUNT];
} settings_t;

/* Adapter boundary: calibrated to one virtual Pro Controller's stick/IMU
 * units and coordinate system BEFORE publishing. Never copy uncalibrated
 * Joy-Con IMU bytes across this boundary. Three chronological 5 ms samples. */
typedef struct {
    uint8_t buttons[3];
    uint16_t sticks[4]; /* LX, LY, RX, RY, 0..4095; neutral 2048. */
    int16_t imu[3][6];  /* ax, ay, az, gx, gy, gz */
    uint8_t battery;    /* 0, 2, 4, 6, 8 */
} controller_sample_t;

typedef struct {
    controller_sample_t sample;
    uint32_t last_ms;
    bool valid;
} live_peer_t;

typedef struct {
    bool raw, stable, held, first_click, wait_release;
    uint32_t edge_ms, press_ms, release_ms;
} button_state_t;

typedef struct {
    settings_t settings;
    live_peer_t live[2];
    pairing_slot_t pending;
    uint32_t epoch;
    uint32_t input_sequence;
    uint32_t pair_started_ms, chord_started_ms, slot_notice_ms;
    uint8_t notice_pulses;
    bool pairing, pending_present[2], settings_dirty;
    bool chord_down, chord_latched, slot_notice;
    button_state_t button;
} bridge_t;

void sample_neutral(controller_sample_t *sample);
bool settings_valid(const settings_t *settings);
void bridge_init(bridge_t *bridge, const settings_t *settings);
void bridge_switch_slot(bridge_t *bridge, uint32_t now);
void bridge_pair_begin(bridge_t *bridge, uint32_t now);
void bridge_pair_cancel(bridge_t *bridge);
void bridge_pair_delete(bridge_t *bridge, uint32_t now);
/* Called only after controller identity, link authentication, and HID setup
 * succeed. Replaces one matching side of a saved Joy-Con pair; an empty slot
 * commits only when BOTH halves are ready. Cancellation retains saved bonds. */
bool bridge_pair_offer(bridge_t *bridge, const bond_t *bond, uint32_t now);
bool bridge_admit(const bridge_t *bridge, const uint8_t address[6], unsigned *index);
bool bridge_publish(bridge_t *bridge, uint32_t epoch, unsigned peer_index,
                    const controller_sample_t *sample, uint32_t now);
void bridge_disconnect(bridge_t *bridge, uint32_t epoch, unsigned peer_index);
void bridge_snapshot(const bridge_t *bridge, uint32_t now, controller_sample_t *out);
void bridge_tick(bridge_t *bridge, uint32_t now);
button_action_t bridge_button(bridge_t *bridge, bool pressed, uint32_t now);
uint8_t bridge_led(const bridge_t *bridge, uint32_t now);
void bridge_report30(const bridge_t *bridge, bool imu_enabled, uint32_t now,
                     uint8_t out[BRIDGE_USB_REPORT_SIZE]);
#endif
