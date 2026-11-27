// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*a760_winusb_prepare_fn)(void *context, void *device, void *usb, uint8_t event_pipe);
void ssc_winusb_select_a760(const char *path, a760_winusb_prepare_fn prepare, void *context);

#ifdef __cplusplus
}
#endif
