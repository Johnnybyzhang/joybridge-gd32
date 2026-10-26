#ifndef REDEV_BOARD_H
#define REDEV_BOARD_H
#include <stdbool.h>
#include <stdint.h>
enum { LED_CHANNEL_PB9 = 1, LED_CHANNEL_PB8 = 2, LED_CHANNEL_PB5 = 4 };
void board_gpio_init(void);
bool board_button_pressed(void);
void board_led_channels_set(uint8_t channels);
/* APB timer input clock must be established by the application's clock layer.
 * All selected LED channels use HARDWARE PWM, with zero timer interrupts. */
bool board_led_pwm_init(uint32_t timer_clock_hz, uint8_t channels);
void board_led_pwm_set(uint8_t duty);
void board_led_pwm_select(uint8_t channels, uint8_t duty);
#endif
