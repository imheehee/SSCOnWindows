// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

inline constexpr uint16_t A760_VID = 0x0BDA;
inline constexpr uint16_t A760_PID = 0xA760;
inline constexpr uint16_t A760_REV = 0x0200;

// Sends only HCI Read Local Version Information to a single, exact WinUSB device.
std::string diagnose_a760(bool korean = false);
