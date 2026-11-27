// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline constexpr uint16_t A760_VID = 0x0BDA;
inline constexpr uint16_t A760_PID = 0xA760;
inline constexpr uint16_t A760_REV = 0x0200;

// Reads HCI and Realtek ROM versions from a single, exact WinUSB device.
std::string diagnose_a760(bool korean = false);

bool verify_a760_rom(const std::string &path, std::string &report);

// Runs before BTstack submits async reads on the same exclusive WinUSB handle.
bool initialize_a760_ram(void *device, void *usb, uint8_t event_pipe,
                         const std::vector<uint8_t> &payload, std::string &report);
