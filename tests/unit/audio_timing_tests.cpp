#include "doctest/doctest.h"
#include "audio_codec_config.h"
#include "audio_timing.h"

TEST_CASE("SSC deadline follows actual frame samples and sample rate") {
    audio_timing::EncodeWindow window;
    for (unsigned i = 1; i <= 100; ++i) window.add(i, 95);
    const auto stats = window.stats();
    CHECK(stats.mean == 50.5);
    CHECK(stats.max == 100.0);
    CHECK(stats.p95 == 95.0);
    CHECK(stats.p99 == 99.0);
    CHECK(stats.exceeded == 5);
    CHECK(audio_timing::frame_ms(864, 44100) > 18.69);
    CHECK(audio_timing::frame_ms(864, 48000) == 18.0);
}

TEST_CASE("RTP clock preserves 44.1 and 48 kHz over an hour, including wrap") {
    for (uint32_t rate : {44100u, 48000u}) {
        audio_timing::MediaClock clock;
        const uint32_t base = UINT32_MAX - 1000;
        for (uint64_t samples = 0; samples < uint64_t(rate) * 3600; samples += 864) {
            const auto elapsed = samples * 1000 / rate;
            REQUIRE(clock.due(base + static_cast<uint32_t>(samples), rate, elapsed, 40) == elapsed + 40);
        }
        CHECK(clock.stalls() == 0);
    }
}

TEST_CASE("Playout recovers from a stall without replaying the prior schedule") {
    audio_timing::MediaClock clock;
    CHECK(clock.due(0, 48000, 1000, 40) == 1040);
    CHECK(clock.due(864, 48000, 1200, 40) == 1200);
    CHECK(clock.due(1728, 48000, 1201, 40) == 1218);
    CHECK(clock.stalls() == 1);
}

TEST_CASE("Upsampling is invariant across capture callback boundaries") {
    const int16_t input[]{100, -100, 200, -200, 300, -300};
    int16_t whole[12]{}, split[12]{};
    std::vector<int16_t> previous;
    audio_timing::upsample_2x(input, 3, 2, previous, whole);
    previous.clear();
    audio_timing::upsample_2x(input, 1, 2, previous, split);
    audio_timing::upsample_2x(input + 2, 2, 2, previous, split + 4);
    CHECK(std::equal(whole, whole + 12, split));
    CHECK(split[10] == 300);
    CHECK(split[11] == -300);
}

TEST_CASE("AAC and SBC capabilities are matched on a single audio SEP") {
    using namespace audio_codec_config;
    const AacCaps aac48{2, 0x20, 0x04, 0x008, 160000};
    const AacCaps aac44{3, 0x40, 0x08, 0x010, 0};
    CHECK(aac_object(aac48, 48000, 2) == 2);
    CHECK(aac_object(aac48, 44100, 2) == 0);
    CHECK(aac_object(aac44, 44100, 2) == 0);
    CHECK(aac_object(aac44, 44100, 1) == 1);
    CHECK(aac_bitrate(aac48, 192000) == 160000);
    const SbcCaps sbc{4, 3, 1, 1, 1, 1, 10, 40};
    CHECK(sbc_mode(sbc, 48000, 2) == 4);
    CHECK(sbc_mode(sbc, 44100, 1) == 0);
    CHECK(sbc_bitpool(sbc, 53) == 40);
}

TEST_CASE("A large AAC access unit fits by fragmenting across the media MTU") {
    using audio_codec_config::aac_fragment_count;
    CHECK(aac_fragment_count(679, 679) == 1);
    CHECK(aac_fragment_count(680, 679) == 2);
    CHECK(aac_fragment_count(0, 679) == 0);
    CHECK(aac_fragment_count(680, 0) == 0);
}
