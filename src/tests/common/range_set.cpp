// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.inc"

TEST_CASE("RangeSet Containing returns the unclipped interval", "[common]") {
    Common::RangeSet<u64> ranges;
    CHECK_FALSE(ranges.Containing(10).has_value());

    ranges.Add(100, 50);
    ranges.Add(200, 20);
    const auto inside = ranges.Containing(120);
    REQUIRE(inside.has_value());
    CHECK(inside->first == 100);
    CHECK(inside->second == 150);

    CHECK_FALSE(ranges.Containing(150).has_value());
    CHECK_FALSE(ranges.Containing(160).has_value());

    const auto second = ranges.Containing(200);
    REQUIRE(second.has_value());
    CHECK(second->first == 200);
    CHECK(second->second == 220);
}

TEST_CASE("RangeSet Containing follows a merged run", "[common]") {
    Common::RangeSet<u64> ranges;
    ranges.Add(0, 20);
    ranges.Add(20, 20);
    ranges.Add(40, 20);
    const auto run = ranges.Containing(25);
    REQUIRE(run.has_value());
    CHECK(run->first == 0);
    CHECK(run->second == 60);
}
