// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

// One terrain piece's indirect-draw record. `gpu` and `cpu` are the addresses of that record.
struct IndirectResetPiece {
    u64 gpu;
    u64 cpu;
};

// Adjacent pieces, `count` records of `record_size` bytes starting at `gpu`/`cpu`.
struct IndirectResetSpan {
    u64 gpu;
    u64 cpu;
    u32 count;
};

// Sort by CPU address and merge pieces whose records sit next to each other in both address
// spaces. Donkey Kong Bananza's hilltop layer resets every rock and dirt piece; one upload per
// piece spends the frame locking the buffer cache.
inline void CoalesceIndirectResetPieces(std::span<IndirectResetPiece> pieces, u32 record_size,
                                        std::vector<IndirectResetSpan>& out) {
    out.clear();
    if (pieces.empty() || record_size == 0) {
        return;
    }
    std::sort(pieces.begin(), pieces.end(), [](const IndirectResetPiece& a, const IndirectResetPiece& b) {
        return a.cpu < b.cpu || (a.cpu == b.cpu && a.gpu < b.gpu);
    });
    out.reserve(pieces.size());
    for (const IndirectResetPiece& piece : pieces) {
        if (!out.empty()) {
            IndirectResetSpan& last = out.back();
            const u64 cpu_end = last.cpu + u64{last.count} * record_size;
            const u64 gpu_end = last.gpu + u64{last.count} * record_size;
            if (piece.cpu == cpu_end && piece.gpu == gpu_end) {
                ++last.count;
                continue;
            }
        }
        out.push_back(IndirectResetSpan{piece.gpu, piece.cpu, 1});
    }
}

} // namespace VideoCommon
