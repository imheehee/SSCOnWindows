/*
 * Driver state checks for guided USB Bluetooth driver changes.
 * SPDX-License-Identifier: MIT
 */

#include "driver_switch.h"

#include "driver_mode.h"

DongleSwitchReport get_dongle_driver_instructions(uint16_t vid, uint16_t pid, bool enable_winusb) {
    const auto status = detect_dongle_driver(vid, pid);
    if (status.mode == DongleDriverMode::NotPresent) {
        return {DongleSwitchResult::NotPresent, "The USB Bluetooth adapter was not detected."};
    }

    const auto requested = enable_winusb ? DongleDriverMode::WinUsbStream
                                         : DongleDriverMode::BthUsb;
    if (status.mode == requested) {
        return {DongleSwitchResult::NoChange,
                enable_winusb ? "WinUSB is active." : "BTHUSB is active."};
    }

    const std::string instructions = enable_winusb
                                         ? "In Device Manager, install a trusted signed WinUSB package for this device, then refresh."
                                         : "In Device Manager, roll back this device's driver or select the original signed Bluetooth driver, then refresh.";
    return {DongleSwitchResult::ManualActionRequired,
            instructions + " Device instance: " + status.instance_id};
}
