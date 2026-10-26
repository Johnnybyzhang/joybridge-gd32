#ifndef REDEV_GD32_SDK_H
#define REDEV_GD32_SDK_H
#include <stdbool.h>
#include <stdint.h>
/* The installed legacy DFP declares its own enum named bool. Keep the vendor
 * headers unmodified and isolate that name from the application's C99 bool. */
#pragma push_macro("bool")
#undef bool
#define bool gd32_sdk_bool
#include "gd32f10x.h"
#pragma pop_macro("bool")
#endif
