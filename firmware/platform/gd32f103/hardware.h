#ifndef REDEV_HARDWARE_H
#define REDEV_HARDWARE_H
#include "gd32_sdk.h"
#include <stdbool.h>
bool diagnostic_clock_init(void);
void diagnostic_usb_pullup(FlagStatus connected);
_Noreturn void diagnostic_enter_bootloader(void);
#endif
