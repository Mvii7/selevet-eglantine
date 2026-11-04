// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace Service {

// One present per requested vblank interval. The interval is the one on this present, not the
// previous one. A present that misses its slot is shown now. The next slot is one interval after
// that, so a miss does not also release the following present.
class Nx2PresentPacer {
public:
    using Clock = std::chrono::steady_clock;

    struct Step {
        Clock::time_point deadline;
        bool sleep{};
    };

    Step Advance(Clock::time_point now, std::chrono::nanoseconds interval) {
        Clock::time_point deadline = have ? released + interval : now;
        if (!have || now > deadline) {
            deadline = now;
        }
        released = deadline;
        have = true;
        return {deadline, now < deadline};
    }

private:
    Clock::time_point released{};
    bool have{};
};

} // namespace Service
