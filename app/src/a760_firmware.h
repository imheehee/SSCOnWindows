// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class A760Firmware {
public:
    static A760Firmware prepare(const std::filesystem::path &directory, std::string &report);
    const std::string &device_path() const {
        return device_path_;
    }
    const std::vector<uint8_t> &payload() const {
        return payload_;
    }

private:
    std::string device_path_;
    std::vector<uint8_t> payload_;
};

namespace a760 {
inline constexpr size_t fragment_size = 252;
inline constexpr uint32_t patch_version = 0xdfc6d922;

struct Fragment {
    size_t offset;
    size_t length;
    uint8_t index;
};

inline Fragment fragment(size_t payload_size, size_t number) {
    const size_t offset = number * fragment_size;
    const bool last = number == payload_size / fragment_size;
    const size_t index = number == 0 ? 0 : ((number - 1) % 127) + 1;
    return {offset, last ? payload_size % fragment_size : fragment_size,
            static_cast<uint8_t>(index | (last ? 0x80 : 0))};
}

std::vector<uint8_t> build_payload(const std::vector<uint8_t> &fw, const std::vector<uint8_t> &cfg);
} // namespace a760
