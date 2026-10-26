#ifndef REDEV_NINTENDO_H
#define REDEV_NINTENDO_H
#include "bridge.h"
typedef struct {
    uint8_t kind, stage, counter, retries, timer, expected, error;
    bool waiting, ready, timer_valid;
    uint32_t sent_ms, sequence;
    uint16_t center[4], positive[4], negative[4];
    int16_t origin[6], sensitivity[6];
    uint8_t address[6], rumble[8];
    uint8_t player_leds, sending_report;
    bool output_pending, rumble_pending, home_pending;
    uint8_t home_led[38];
    controller_sample_t sample;
} nintendo_t;
void nintendo_init(nintendo_t *state, const uint8_t address[6]);
/* Build one owned output report; call sent only after transport accepts it. */
size_t nintendo_command(nintendo_t *state, uint32_t now, uint8_t report[49]);
void nintendo_sent(nintendo_t *state, uint32_t now);
bool nintendo_input(nintendo_t *state, const uint8_t *report, size_t size);
void nintendo_queue_output(nintendo_t *state, const uint8_t rumble[8], uint8_t player_leds,
                           bool rumble_event);
void nintendo_queue_home(nintendo_t *state, const uint8_t pattern[38]);
#endif
