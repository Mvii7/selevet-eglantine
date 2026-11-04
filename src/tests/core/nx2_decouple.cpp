// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_decouple.h"
#include "core/hle/service/nx2_frame_step.h"

namespace {

void Put(std::vector<u8>& image, u64 offset, std::initializer_list<u32> words) {
    for (const u32 word : words) {
        std::memcpy(image.data() + offset, &word, sizeof(word));
        offset += sizeof(word);
    }
}

u32 Word(const std::vector<u8>& image, u64 offset) {
    u32 word;
    std::memcpy(&word, image.data() + offset, sizeof(word));
    return word;
}

// clang -O2 -target aarch64 for `rate = 60 / interval; time = 1 / (float)rate`.
constexpr u32 kVfr[] = {0x52800788, 0x1e2e1001, 0x1ac00908, 0x1e230100,
                        0xb9000028, 0x1e201820, 0xbd000040, 0xd65f03c0};
// `time = (float)interval * (1/60)`.
constexpr u32 kScale[] = {0x52911128, 0x1e230000, 0x72a79108, 0x1e270101,
                          0x1e210800, 0xbd000020, 0xd65f03c0};
// `time = (float)interval / 60`.
constexpr u32 kDivide[] = {0x1e230000, 0x52a84e08, 0x1e270101, 0x1e211800, 0xbd000020, 0xd65f03c0};

std::vector<u8> Image() {
    return std::vector<u8>(0x3000, 0);
}

} // namespace

TEST_CASE("Decouple keeps a steady 60 Hz step", "[nx2]") {
    Service::Nx2DecoupleState state;
    const double frame = 1.0 / 60.0;
    const double dt = Service::Nx2DecoupleAdvance(state, frame, 1.0 / 20.0);
    CHECK(std::abs(dt - frame) < 1e-12);
}

TEST_CASE("Decouple takes a lasting slower frame after two presents", "[nx2]") {
    Service::Nx2DecoupleState state;
    const double cap = 1.0 / 20.0;
    const double first = Service::Nx2DecoupleAdvance(state, 0.025, cap);
    const double eased = (1.0 / 60.0) + (0.025 - 1.0 / 60.0) * 0.1;
    CHECK(std::abs(first - eased) < 1e-12);
    const double second = Service::Nx2DecoupleAdvance(state, 0.025, cap);
    CHECK(std::abs(second - 0.025) < 1e-12);
}

TEST_CASE("Decouple drops back after two faster frames", "[nx2]") {
    Service::Nx2DecoupleState state;
    const double cap = 1.0 / 20.0;
    const double frame = 1.0 / 60.0;
    Service::Nx2DecoupleAdvance(state, 0.040, cap);
    const double slow = Service::Nx2DecoupleAdvance(state, 0.040, cap);
    CHECK(std::abs(slow - 0.040) < 1e-12);
    const double easing = Service::Nx2DecoupleAdvance(state, frame, cap);
    CHECK(easing > frame);
    CHECK(easing < slow);
    const double dropped = Service::Nx2DecoupleAdvance(state, frame, cap);
    CHECK(std::abs(dropped - frame) < 1e-9);
}

TEST_CASE("Decouple does not repay a hitch on the next frame", "[nx2]") {
    Service::Nx2DecoupleState state;
    const double cap = 1.0 / 20.0;
    const double frame = 1.0 / 60.0;
    const double spiked = Service::Nx2DecoupleAdvance(state, 0.080, cap);
    CHECK(spiked < 0.02);
    Service::Nx2DecoupleAdvance(state, 0.080, cap);
    const double capped = state.dt;
    CHECK(std::abs(capped - cap) < 1e-12);
    // The 30 ms the cap dropped is not added to the next frame. One fast frame only eases down.
    const double next = Service::Nx2DecoupleAdvance(state, frame, cap);
    const double eased = cap + (frame - cap) * 0.1;
    CHECK(std::abs(next - eased) < 1e-12);
    const double dropped = Service::Nx2DecoupleAdvance(state, frame, cap);
    CHECK(std::abs(dropped - frame) < 1e-9);
}

TEST_CASE("Decouple sample is full speed below 60 without a 30 FPS cap", "[nx2]") {
    CHECK(std::abs(Service::Nx2DecoupleSample(1.0 / 40.0, 1.0) - 1.0 / 40.0) < 1e-12);
    CHECK(std::abs(Service::Nx2DecoupleSample(1.0 / 30.0, 1.0) - 1.0 / 30.0) < 1e-12);
    CHECK(std::abs(Service::Nx2DecoupleSample(1.0 / 300.0, 1.0) - 1.0 / 60.0) < 1e-12);
    CHECK(std::abs(Service::Nx2DecoupleSample(1.0 / 120.0, 2.0) - 1.0 / 60.0) < 1e-12);
    CHECK(std::abs(Service::Nx2DecoupleSample(1.0 / 30.0, 0.5) - 1.0 / 60.0) < 1e-12);

    Service::Nx2DecoupleState state;
    const double wanted = Service::Nx2DecoupleSample(0.040, 2.0);
    Service::Nx2DecoupleAdvance(state, wanted, 1.0 / 20.0);
    CHECK(std::abs(Service::Nx2DecoupleAdvance(state, wanted, 1.0 / 20.0) - 1.0 / 20.0) < 1e-12);
}

TEST_CASE("Decouple cap clamps one long step", "[nx2]") {
    Service::Nx2DecoupleState wide;
    Service::Nx2DecoupleAdvance(wide, 0.080, 1.0 / 20.0);
    CHECK(std::abs(Service::Nx2DecoupleAdvance(wide, 0.080, 1.0 / 20.0) - 0.05) < 1e-12);

    Service::Nx2DecoupleState tight;
    Service::Nx2DecoupleAdvance(tight, 0.040, 1.0 / 30.0);
    const double capped = Service::Nx2DecoupleAdvance(tight, 0.040, 1.0 / 30.0);
    CHECK(std::abs(capped - 1.0 / 30.0) < 1e-12);
}

TEST_CASE("Bananza frame step patch rewrites the one reciprocal", "[nx2]") {
    auto image = Image();
    Put(image, 0x40, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], kVfr[4], kVfr[5], kVfr[6], kVfr[7]});
    constexpr u64 kLoad = 0x80000000;
    const auto scan = Service::Nx2PatchBananzaFrameStep(image, 0, 0x80, 0x1000, 0x2FFC, kLoad);
    REQUIRE(scan.patch);
    CHECK(scan.vfr == 1);
    CHECK(scan.patch->site_offset == 0x54);
    CHECK(Word(image, 0x54) == 0x1400000B);
    CHECK(Word(image, 0x80) == 0xD0000010);
    CHECK(Word(image, 0x84) == 0xBD4FFE00);
    CHECK(Word(image, 0x88) == 0x17FFFFF4);
    float stored;
    std::memcpy(&stored, image.data() + 0x2FFC, sizeof(stored));
    CHECK(std::abs(stored - 1.0f / 60.0f) < 1e-7f);
}

TEST_CASE("Bananza frame step patch finds a scale or a divide when it is the only one", "[nx2]") {
    constexpr u64 kLoad = 0x80000000;
    // The scale multiplies at word 4. The divide divides at word 3.
    const std::pair<std::span<const u32>, u64> patterns[] = {
        {std::span<const u32>(kScale).first(5), 0x20 + 16},
        {std::span<const u32>(kDivide).first(4), 0x20 + 12},
    };
    for (const auto& [words, site] : patterns) {
        auto image = Image();
        std::vector<u32> copied(words.begin(), words.end());
        Put(image, 0x20, {copied[0], copied[1], copied[2], copied[3]});
        if (copied.size() > 4) {
            Put(image, 0x30, {copied[4]});
        }
        const auto scan = Service::Nx2PatchBananzaFrameStep(image, 0, 0x40, 0x1000, 0x2FFC, kLoad);
        REQUIRE(scan.patch);
        CHECK(scan.patch->site_offset == site);
        const u32 branch = Word(image, scan.patch->site_offset);
        CHECK((branch & 0xFC000000) == 0x14000000);
        const u32 adrp = Word(image, 0x40);
        const u32 ldr = Word(image, 0x44);
        s32 imm21 = static_cast<s32>((((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3));
        if (imm21 & (1 << 20)) {
            imm21 |= ~((1 << 21) - 1);
        }
        const s32 page = imm21 << 12;
        const u64 loaded = (static_cast<u64>(static_cast<s64>((kLoad + 0x40) & ~u64{0xFFF}) + page)) +
                           (((ldr >> 10) & 0xFFF) << 2);
        CHECK(loaded == kLoad + 0x2FFC);
        CHECK((ldr & 31) == 0);
    }
}

TEST_CASE("Bananza frame step patch leaves ambiguous or broken code alone", "[nx2]") {
    constexpr u64 kLoad = 0x80000000;
    auto doubled = Image();
    Put(doubled, 0x40, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], kVfr[4], kVfr[5], kVfr[6], kVfr[7]});
    Put(doubled, 0x100, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], kVfr[4], kVfr[5], kVfr[6], kVfr[7]});
    const auto ambiguous = Service::Nx2PatchBananzaFrameStep(doubled, 0, 0x180, 0x1000, 0x2FFC, kLoad);
    CHECK_FALSE(ambiguous.patch);
    CHECK(ambiguous.vfr == 2);
    CHECK(Word(doubled, 0x54) == kVfr[5]);

    auto broken = Image();
    Put(broken, 0x40, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], 0xd65f03c0, kVfr[5]});
    const auto missed = Service::Nx2PatchBananzaFrameStep(broken, 0, 0x80, 0x1000, 0x2FFC, kLoad);
    CHECK_FALSE(missed.patch);
    CHECK(Word(broken, 0x54) == kVfr[5]);

    auto tight = Image();
    Put(tight, 0x40, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], kVfr[4], kVfr[5], kVfr[6], kVfr[7]});
    const auto no_room = Service::Nx2PatchBananzaFrameStep(tight, 0, 0x60, 0x60, 0x2FFC, kLoad);
    CHECK_FALSE(no_room.patch);
    CHECK(no_room.vfr == 1);
    CHECK(Word(tight, 0x54) == kVfr[5]);
}

TEST_CASE("Bananza frame step patch follows a pooled 1/60 multiply", "[nx2]") {
    constexpr u64 kLoad = 0x80000000;
    auto image = Image();
    const u32 recip = 0x3c888889;
    std::memcpy(image.data() + 0x2000, &recip, sizeof(recip));
    // scvtf s0, w0; adrp x8, #0x1000; ldr s1, [x8]; fmul s0, s0, s1
    Put(image, 0x1000, {0x1E230000, 0xB0000008, 0xBD400101, 0x1E210800});
    const auto scan = Service::Nx2PatchBananzaFrameStep(image, 0x1000, 0x10, 0x1100, 0x2FFC, kLoad);
    REQUIRE(scan.patch);
    CHECK(scan.scale == 1);
    CHECK(scan.pooled == 1);
    CHECK(scan.patch->site_offset == 0x100C);
    const u32 adrp = Word(image, 0x1010);
    const u32 ldr = Word(image, 0x1014);
    s32 imm21 = static_cast<s32>((((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3));
    if (imm21 & (1 << 20)) {
        imm21 |= ~((1 << 21) - 1);
    }
    const u64 loaded = static_cast<u64>(static_cast<s64>(((kLoad + 0x1010) & ~u64{0xFFF}) +
                                                         (static_cast<s64>(imm21) << 12))) +
                       (((ldr >> 10) & 0xFFF) << 2);
    CHECK(loaded == kLoad + 0x2FFC);
    CHECK((ldr & 31) == 0);
}

TEST_CASE("A reciprocal step is preferred over another scale in the same image", "[nx2]") {
    auto image = Image();
    Put(image, 0x40, {kVfr[0], kVfr[1], kVfr[2], kVfr[3], kVfr[4], kVfr[5], kVfr[6], kVfr[7]});
    Put(image, 0x100, {kScale[0], kScale[1], kScale[2], kScale[3], kScale[4], kScale[5], kScale[6]});
    const auto scan = Service::Nx2PatchBananzaFrameStep(image, 0, 0x180, 0x1000, 0x2FFC, 0x80000000);
    REQUIRE(scan.patch);
    CHECK(scan.vfr == 1);
    CHECK(scan.scale == 1);
    CHECK(scan.patch->site_offset == 0x54);
    CHECK(Word(image, 0x110) == kScale[4]);
}
