#include "board.h"
#include "gd32_sdk.h"

/* Board wiring comes from the supplied receiver evidence. Peripheral setup
 * uses the installed official GigaDevice DFP drivers and CMSIS definitions. */
static uint8_t pwm_channels;
static uint32_t pwm_period;

void board_gpio_init(void) {
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
    gpio_init(GPIOA, GPIO_MODE_IPU, GPIO_OSPEED_10MHZ, GPIO_PIN_1);
    gpio_bit_set(GPIOB, GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_9);
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_9);
    pwm_channels = 0;
}

bool board_button_pressed(void) {
    return gpio_input_bit_get(GPIOA, GPIO_PIN_1) == RESET;
}

void board_led_channels_set(uint8_t channels) {
    uint32_t low = 0;
    if (channels & LED_CHANNEL_PB9)
        low |= GPIO_PIN_9;
    if (channels & LED_CHANNEL_PB8)
        low |= GPIO_PIN_8;
    if (channels & LED_CHANNEL_PB5)
        low |= GPIO_PIN_5;
    /* Official SDK register macro supplies an atomic multi-pin update;
     * separate gpio_bit_set/reset calls would introduce an intermediate state. */
    GPIO_BOP(GPIOB) = ((GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_9) & ~low) | (low << 16);
}

static void pwm_timer_init(uint32_t timer, uint32_t period) {
    timer_parameter_struct parameters;
    timer_deinit(timer);
    timer_struct_para_init(&parameters);
    parameters.prescaler = 0;
    parameters.period = period - 1;
    timer_init(timer, &parameters);
    timer_interrupt_disable(timer, 0xffffu);
    timer_dma_disable(timer, 0xffffu);
    timer_auto_reload_shadow_enable(timer);
}

static void pwm_channel_init(uint32_t timer, uint16_t channel) {
    timer_oc_parameter_struct output;
    timer_channel_output_struct_para_init(&output);
    output.outputstate = TIMER_CCX_ENABLE;
    output.ocpolarity = TIMER_OC_POLARITY_LOW;
    timer_channel_output_config(timer, channel, &output);
    timer_channel_output_mode_config(timer, channel, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(timer, channel, TIMER_OC_SHADOW_ENABLE);
    timer_channel_output_pulse_value_config(timer, channel, 0);
}

bool board_led_pwm_init(uint32_t timer_clock_hz, uint8_t channels) {
    if (channels == 0 || channels > 7 || timer_clock_hz < 2000000u || timer_clock_hz > 120000000u ||
        timer_clock_hz % 2000u != 0)
        return false;
    pwm_period = timer_clock_hz / 2000u;
    if (pwm_period > 65535u)
        return false;
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_GPIOB);
    pwm_channels = channels;
    if (channels & LED_CHANNEL_PB5) {
        rcu_periph_clock_enable(RCU_TIMER2);
        gpio_pin_remap_config(GPIO_TIMER2_PARTIAL_REMAP, ENABLE);
        pwm_timer_init(TIMER2, pwm_period);
        pwm_channel_init(TIMER2, TIMER_CH_1);
        timer_event_software_generate(TIMER2, TIMER_EVENT_SRC_UPG);
        timer_enable(TIMER2);
        gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_5);
    }
    if (channels & (LED_CHANNEL_PB8 | LED_CHANNEL_PB9)) {
        rcu_periph_clock_enable(RCU_TIMER3);
        gpio_pin_remap_config(GPIO_TIMER3_REMAP, DISABLE);
        pwm_timer_init(TIMER3, pwm_period);
        if (channels & LED_CHANNEL_PB8)
            pwm_channel_init(TIMER3, TIMER_CH_2);
        if (channels & LED_CHANNEL_PB9)
            pwm_channel_init(TIMER3, TIMER_CH_3);
        timer_event_software_generate(TIMER3, TIMER_EVENT_SRC_UPG);
        timer_enable(TIMER3);
        if (channels & LED_CHANNEL_PB8)
            gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_8);
        if (channels & LED_CHANNEL_PB9)
            gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_9);
    }
    /* Explicit debug policy after SDK remap calls: leave SWD available.
     * Do not depend on readback of the SWJ field retaining its old value. */
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
    return true;
}

void board_led_pwm_set(uint8_t duty) {
    uint16_t compare = (uint16_t)((pwm_period * duty) / 255u);
    if (pwm_channels & LED_CHANNEL_PB5)
        timer_channel_output_pulse_value_config(TIMER2, TIMER_CH_1, compare);
    if (pwm_channels & LED_CHANNEL_PB8)
        timer_channel_output_pulse_value_config(TIMER3, TIMER_CH_2, compare);
    if (pwm_channels & LED_CHANNEL_PB9)
        timer_channel_output_pulse_value_config(TIMER3, TIMER_CH_3, compare);
}
void board_led_pwm_select(uint8_t channels, uint8_t duty) {
    uint16_t compare = (uint16_t)((pwm_period * duty) / 255u);
    if (pwm_channels & LED_CHANNEL_PB5)
        timer_channel_output_pulse_value_config(TIMER2, TIMER_CH_1, channels & LED_CHANNEL_PB5 ? compare : 0);
    if (pwm_channels & LED_CHANNEL_PB8)
        timer_channel_output_pulse_value_config(TIMER3, TIMER_CH_2, channels & LED_CHANNEL_PB8 ? compare : 0);
    if (pwm_channels & LED_CHANNEL_PB9)
        timer_channel_output_pulse_value_config(TIMER3, TIMER_CH_3, channels & LED_CHANNEL_PB9 ? compare : 0);
}
