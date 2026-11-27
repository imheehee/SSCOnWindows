/*
 * Guided WinUSB / Windows Bluetooth driver setup.
 * SPDX-License-Identifier: MIT
 */

#ifndef DRIVER_SWITCH_H
#define DRIVER_SWITCH_H

#include <cstdint>
#include <string>

enum class DongleSwitchResult {
    NotPresent,
    NoChange,
    ManualActionRequired,
};

struct DongleSwitchReport {
    DongleSwitchResult result;
    std::string message;
};

DongleSwitchReport get_dongle_driver_instructions(uint16_t vid, uint16_t pid, bool enable_winusb);

#endif
