// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Service {

// Donkey Kong Bananza. Its present interval is a whole vsync count, so reporting VRR does not
// give it a 1.0x step at an in-between frame rate.
constexpr u64 Nx2BananzaProgramId = 0x040074A01BF12000ULL;

struct Nx2FrameStepPatch {
    u64 float_offset{};
    u64 site_offset{};
};

struct Nx2FrameStepScan {
    int vfr{};
    int scale{};
    int divide{};
    std::optional<Nx2FrameStepPatch> patch;
};

// Replace the one integer-to-seconds step in `image` with a load of the float at `float_off`.
// `text_end` is the first byte the cave must not touch (the code's page, or the next segment).
// Addresses are image offsets; `load_base` is only there so the page math matches the guest.
Nx2FrameStepScan Nx2PatchBananzaFrameStep(std::span<u8> image, u64 text_off, u64 text_size,
                                          u64 text_end, u64 float_off, u64 load_base);

bool Nx2DecoupleEnabled();
int Nx2DecoupleCapDenominator();

// Guest address of the step float, or 0 when this process has no patch.
void Nx2SetFrameStepAddress(u64 guest);
// `speed_scale` is Settings::SpeedLimit()/100 while the speed limit is on, and 1 when it is not.
void Nx2NotePresentedFrame(Core::Memory::Memory& memory, std::chrono::steady_clock::time_point now,
                           double speed_scale);

} // namespace Service
