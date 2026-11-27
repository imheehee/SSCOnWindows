// SPDX-License-Identifier: MIT
#include "a760_firmware.h"

#include <algorithm>
#include <array>

namespace {
uint16_t le16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t le32(const uint8_t *bytes) {
    return le16(bytes) | (static_cast<uint32_t>(le16(bytes + 2)) << 16);
}
} // namespace

std::vector<uint8_t> a760::build_payload(const std::vector<uint8_t> &fw, const std::vector<uint8_t> &cfg) {
    constexpr std::array<uint8_t, 8> magic{'R', 'e', 'a', 'l', 't', 'e', 'c', 'h'};
    constexpr std::array<uint8_t, 4> extension{0x51, 0x04, 0xfd, 0x77};
    // The pinned config has an empty record section. Reject any register overrides.
    if (cfg.size() != 6 || le32(cfg.data()) != 0x8723ab55 || le16(cfg.data() + 4) != 0)
        return {};
    if (fw.size() < 26 || !std::equal(magic.begin(), magic.end(), fw.begin()) ||
        !std::equal(extension.begin(), extension.end(), fw.end() - 4) ||
        le32(fw.data() + 8) != patch_version)
        return {};

    const size_t count = le16(fw.data() + 12);
    const size_t table_end = 14 + 8 * count;
    size_t trailer = fw.size() - 4;
    if (!count || table_end + 3 > trailer) return {};
    size_t payload_end = table_end;
    size_t chosen_offset = 0;
    size_t chosen_length = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto chip = le16(fw.data() + 14 + 2 * i);
        const size_t length = le16(fw.data() + 14 + 2 * count + 2 * i);
        const size_t offset = le32(fw.data() + 14 + 4 * count + 4 * i);
        if (length < 4 || offset < table_end || offset > trailer || length > trailer - offset)
            return {};
        payload_end = std::max(payload_end, offset + length);
        // The separately verified ROM version is 1. btrtl selects ROM+1.
        if (chip == 2) {
            if (chosen_length) return {};
            chosen_offset = offset;
            chosen_length = length;
        }
    }
    if (!chosen_length) return {};
    while (trailer - payload_end >= 3) {
        const auto opcode = fw[trailer - 1];
        const size_t length = fw[trailer - 2];
        if (opcode == 0xff || !length || length > trailer - payload_end - 2) return {};
        trailer -= length + 2;
        if (opcode != 0 || length != 1) continue;
        if (fw[trailer] != 14) return {};

        std::vector<uint8_t> payload(fw.begin() + chosen_offset, fw.begin() + chosen_offset + chosen_length);
        std::copy_n(fw.begin() + 8, 4, payload.end() - 4);
        payload.insert(payload.end(), cfg.begin(), cfg.end());
        return payload;
    }
    return {};
}
