// SPDX-License-Identifier: MIT
#include "a760_firmware.h"
#include "doctest/doctest.h"

TEST_CASE("A760 epatch selects chip 2, inserts version and appends empty config") {
    std::vector<uint8_t> image{
        'R', 'e', 'a', 'l', 't', 'e', 'c', 'h', 0x22, 0xd9, 0xc6, 0xdf, 2, 0,
        1, 0, 2, 0, 4, 0, 5, 0, 30, 0, 0, 0, 34, 0, 0, 0,
        1, 2, 3, 4, 0xaa, 0, 0, 0, 0, 0xff, 14, 1, 0, 0x51, 4, 0xfd, 0x77,
    };
    const std::vector<uint8_t> config{0x55, 0xab, 0x23, 0x87, 0, 0};
    const std::vector<uint8_t> expected{0xaa, 0x22, 0xd9, 0xc6, 0xdf, 0x55, 0xab, 0x23, 0x87, 0, 0};
    CHECK(a760::build_payload(image, config) == expected);

    auto nonempty_config = config;
    nonempty_config[4] = 3;
    nonempty_config.insert(nonempty_config.end(), {1, 0, 0});
    CHECK(a760::build_payload(image, nonempty_config).empty());
    image[40] = 3;
    CHECK(a760::build_payload(image, config).empty());
    image[40] = 14;
    image[26] = 0xff;
    CHECK(a760::build_payload(image, config).empty());
    image[26] = 34;
    image[16] = 1;
    CHECK(a760::build_payload(image, config).empty());
}

TEST_CASE("A760 fragment numbering follows btrtl and emits the terminal marker") {
    CHECK(a760::fragment(30210, 0).index == 0);
    CHECK(a760::fragment(30210, 0).length == 252);
    CHECK(a760::fragment(30210, 118).index == 118);
    const auto last = a760::fragment(30210, 119);
    CHECK(last.offset == 29988);
    CHECK(last.length == 222);
    CHECK(last.index == (119 | 0x80));
    CHECK(a760::fragment(252, 1).length == 0);
    CHECK(a760::fragment(252, 1).index == 0x81);
    CHECK(a760::fragment(50000, 127).index == 127);
    CHECK(a760::fragment(50000, 128).index == 1);
}
