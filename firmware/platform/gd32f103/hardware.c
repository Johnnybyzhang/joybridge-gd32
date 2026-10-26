#include "hardware.h"
bool diagnostic_clock_init(void) {
    rcu_deinit();
    rcu_osci_on(RCU_HXTAL);
    if (rcu_osci_stab_wait(RCU_HXTAL) != SUCCESS)
        return false;
    /* Stock loader uses HXTAL PLL x6; the board's 8 MHz reference gives
     * a 48 MHz CPU and USB clock. Configure via official peripheral APIs. */
    fmc_wscnt_set(1);
    rcu_ahb_clock_config(RCU_AHB_CKSYS_DIV1);
    rcu_apb1_clock_config(RCU_APB1_CKAHB_DIV2);
    rcu_apb2_clock_config(RCU_APB2_CKAHB_DIV1);
    rcu_pll_config(RCU_PLLSRC_HXTAL, RCU_PLL_MUL6);
    rcu_osci_on(RCU_PLL_CK);
    if (rcu_osci_stab_wait(RCU_PLL_CK) != SUCCESS)
        return false;
    rcu_system_clock_source_config(RCU_CKSYSSRC_PLL);
    for (uint32_t retries = 0; retries < 100000; retries++) {
        if (rcu_system_clock_source_get() == RCU_SCSS_PLL) {
            rcu_usb_clock_config(RCU_CKUSB_CKPLL_DIV1);
            return true;
        }
    }
    return false;
}
void diagnostic_usb_pullup(FlagStatus connected) {
    if (connected == SET)
        gpio_bit_reset(GPIOB, GPIO_PIN_15);
    else
        gpio_bit_set(GPIOB, GPIO_PIN_15);
}
_Noreturn void diagnostic_enter_bootloader(void) {
    __disable_irq();
    /* Reserve this marker outside both our RAM sections and the loader's
     * cleared RAM. System reset starts the ORIGINAL loader with clean state. */
    *(volatile uint32_t *)0x20004ff0 = 0x5a5aa5a5;
    __DSB();
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_bit_set(GPIOB, GPIO_PIN_15);
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_15);
    for (volatile uint32_t delay = 0; delay < 200000; delay++)
        __NOP();
    NVIC_SystemReset();
}
