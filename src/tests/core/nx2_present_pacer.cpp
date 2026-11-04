// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_pacer.h"

namespace {
using namespace std::chrono_literals;
using Pacer = Service::Nx2PresentPacer;
constexpr auto Start = Pacer::Clock::time_point{1s};
constexpr auto One = 16'666'666ns;
constexpr auto Two = 33'333'333ns;
constexpr auto Three = 50'000'000ns;
} // namespace

TEST_CASE("A shorter present interval applies to this present", "[nx2]") {
    Pacer pacer;
    REQUIRE_FALSE(pacer.Advance(Start, Three).sleep);
    const auto step = pacer.Advance(Start + 2'679us, Two);
    REQUIRE(step.sleep);
    REQUIRE(step.deadline == Start + Two);
    REQUIRE(step.deadline - (Start + 2'679us) == 30'654'333ns);
}

TEST_CASE("A longer present interval applies to this present", "[nx2]") {
    Pacer pacer;
    pacer.Advance(Start, One);
    const auto step = pacer.Advance(Start + 1ms, Three);
    REQUIRE(step.sleep);
    REQUIRE(step.deadline == Start + Three);
}

TEST_CASE("A missed slot does not release the next present as well", "[nx2]") {
    Pacer pacer;
    pacer.Advance(Start, 10ms);
    const auto late = pacer.Advance(Start + 20ms, 10ms);
    REQUIRE_FALSE(late.sleep);
    REQUIRE(late.deadline == Start + 20ms);
    const auto next = pacer.Advance(Start + 20ms, 10ms);
    REQUIRE(next.sleep);
    REQUIRE(next.deadline == Start + 30ms);
}

TEST_CASE("An on-time present keeps one interval between releases", "[nx2]") {
    Pacer pacer;
    auto when = Start;
    for (int i = 0; i < 5; ++i) {
        const auto step = pacer.Advance(when, One);
        REQUIRE_FALSE(step.sleep);
        REQUIRE(step.deadline == when);
        when += One;
    }
}
