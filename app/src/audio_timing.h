// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace audio_timing {
inline double frame_ms(uint32_t samples, uint32_t rate) {
    return rate ? 1000.0 * samples / rate : 0.0;
}

// Clock from RTP sample counts, retaining fractional milliseconds over time.
class MediaClock {
public:
    uint64_t due(uint32_t timestamp, uint32_t rate, uint64_t now, uint32_t buffer_ms) {
        if (!started_) {
            started_ = true;
            base_ms_ = now + buffer_ms;
            last_timestamp_ = timestamp;
        }
        samples_ += static_cast<uint32_t>(timestamp - last_timestamp_);
        last_timestamp_ = timestamp;
        auto target = base_ms_ + samples_ * 1000 / rate;
        // After an actual stall, restart the schedule without a catch-up burst.
        if (now > target + buffer_ms) {
            base_ms_ += now - target;
            target = now;
            ++stalls_;
        }
        return target;
    }

    uint64_t stalls() const {
        return stalls_;
    }

private:
    uint64_t base_ms_ = 0;
    uint64_t samples_ = 0;
    uint64_t stalls_ = 0;
    uint32_t last_timestamp_ = 0;
    bool started_ = false;
};

// Each input frame produces exactly two output frames across callback boundaries.
template <class Sample>
void upsample_2x(const Sample *input, uint32_t frames, uint32_t channels,
                 std::vector<Sample> &previous, Sample *output) {
    if (!frames) return;
    if (previous.size() != channels) previous.assign(input, input + channels);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const auto sample = *input++;
            output[channel] = static_cast<Sample>((previous[channel] + sample) * 0.5);
            output[channels + channel] = sample;
            previous[channel] = sample;
        }
        output += 2 * channels;
    }
}

struct EncodeStats {
    double mean = 0;
    double max = 0;
    double p95 = 0;
    double p99 = 0;
    uint64_t exceeded = 0;
};

class EncodeWindow {
public:
    void add(double ms, double budget) {
        values_[position_++ % values_.size()] = ms;
        if (ms > budget) ++exceeded_;
    }

    EncodeStats stats() const {
        auto sorted = values_;
        const auto count = std::min(position_, static_cast<uint64_t>(sorted.size()));
        if (!count) return {};
        std::sort(sorted.begin(), sorted.begin() + count);
        double total = 0;
        for (size_t i = 0; i < count; ++i) total += sorted[i];
        return {total / count, sorted[count - 1], sorted[(count * 95 + 99) / 100 - 1],
                sorted[(count * 99 + 99) / 100 - 1], exceeded_};
    }

private:
    std::array<double, 256> values_{};
    uint64_t position_ = 0;
    uint64_t exceeded_ = 0;
};
} // namespace audio_timing
