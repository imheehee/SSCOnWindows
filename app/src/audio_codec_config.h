// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>

namespace audio_codec_config {
struct AacCaps {
    uint8_t seid, object, channels;
    uint16_t rates;
    uint32_t bitrate;
};

struct SbcCaps {
    uint8_t seid, rates, channels, blocks, subbands, allocation, minimum, maximum;
};

inline uint8_t aac_object(const AacCaps &caps, uint32_t rate, uint8_t channels) {
    const unsigned frequency = rate == 44100 ? 0x10 : rate == 48000 ? 0x08
                                                                    : 0;
    const unsigned channel = channels == 1 ? 8 : channels == 2 ? 4
                                                               : 0;
    if (!(caps.rates & frequency) || !(caps.channels & channel)) return 0;
    // The AVDTP typed settings use enum values 1=MPEG-2 LC, 2=MPEG-4 LC.
    return caps.object & 0x40 ? 2 : caps.object & 0x80 ? 1
                                                       : 0;
}

inline uint32_t aac_bitrate(const AacCaps &caps, uint32_t wanted) {
    return caps.bitrate ? std::min(wanted, caps.bitrate) : wanted;
}

inline uint8_t sbc_mode(const SbcCaps &caps, uint32_t rate, uint8_t channels) {
    const unsigned frequency = rate == 44100 ? 2 : rate == 48000 ? 1
                                                                 : 0;
    if (!(caps.rates & frequency) || !(caps.blocks & 1) || !(caps.subbands & 1) ||
        !(caps.allocation & 1) || caps.minimum < 2 || caps.maximum < caps.minimum ||
        caps.minimum > 53) return 0;
    if (channels == 1) return caps.channels & 8 ? 1 : 0;
    if (channels != 2) return 0;
    return caps.channels & 1 ? 4 : caps.channels & 2 ? 3
                               : caps.channels & 4   ? 2
                                                     : 0;
}

inline uint8_t sbc_bitpool(const SbcCaps &caps, uint8_t wanted) {
    return std::clamp(wanted, caps.minimum, std::min<uint8_t>(caps.maximum, 53));
}
} // namespace audio_codec_config
