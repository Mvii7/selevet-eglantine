// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/indirect_draw_reset.h"

namespace {

using VideoCommon::CoalesceIndirectResetPieces;
using VideoCommon::IndirectResetPiece;
using VideoCommon::IndirectResetSpan;

constexpr u32 RecordSize = 20;

std::vector<IndirectResetSpan> Coalesce(std::vector<IndirectResetPiece> pieces) {
    std::vector<IndirectResetSpan> spans;
    CoalesceIndirectResetPieces(pieces, RecordSize, spans);
    return spans;
}

} // namespace

TEST_CASE("Indirect reset pieces merge when both addresses are contiguous", "[video_core]") {
    const auto spans = Coalesce({
        {1000, 500},
        {1020, 520},
        {1040, 540},
    });
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].gpu == 1000);
    CHECK(spans[0].cpu == 500);
    CHECK(spans[0].count == 3);
}

TEST_CASE("Indirect reset pieces stay split across a gap or an address mismatch", "[video_core]") {
    const auto gapped = Coalesce({
        {1000, 500},
        {1040, 540},
    });
    REQUIRE(gapped.size() == 2);
    CHECK(gapped[0].count == 1);
    CHECK(gapped[1].gpu == 1040);

    const auto cpu_only = Coalesce({
        {1000, 500},
        {2000, 520},
    });
    CHECK(cpu_only.size() == 2);

    const auto gpu_only = Coalesce({
        {1000, 500},
        {1020, 900},
    });
    CHECK(gpu_only.size() == 2);
}

TEST_CASE("Indirect reset pieces are sorted before they are merged", "[video_core]") {
    const auto spans = Coalesce({
        {1040, 540},
        {1000, 500},
        {1020, 520},
    });
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].count == 3);
    CHECK(spans[0].cpu == 500);
}

TEST_CASE("Indirect reset coalescing keeps an empty list empty", "[video_core]") {
    CHECK(Coalesce({}).empty());
}
