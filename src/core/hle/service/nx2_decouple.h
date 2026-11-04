// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

namespace Service {

// One simulation step per present. A lasting change of frame length is taken after two frames;
// anything else eases a tenth of the way. A single hitch does not jump, and the time it did not
// step is not added to a later frame. `cap_seconds` stops one long frame from fast-forwarding.
struct Nx2DecoupleState {
    double dt = 1.0 / 60.0;
    double prev_raw = 0.0;
    int counter = 0;
};

// `speed_scale` is the emulation speed, 1 at 100%. The pacer already presents more or less often,
// so the step stays one 60 Hz tick when the present is that fast. A slower present, including one
// held at 30 Hz, is used at its own length: full speed there does not need a separate cap.
inline double Nx2DecoupleSample(double raw_seconds, double speed_scale) {
    const double scale = speed_scale > 0.0 ? speed_scale : 1.0;
    return (std::max)(raw_seconds * scale, 1.0 / 60.0);
}

inline double Nx2DecoupleAdvance(Nx2DecoupleState& state, double raw, double cap_seconds) {
    if (raw > 0.0) {
        const double prev = state.prev_raw;
        state.prev_raw = raw;
        bool jump = false;
        if (raw > state.dt * 1.25) {
            state.counter = state.counter > 0 ? state.counter + 1 : 1;
            if (state.counter >= 2) {
                state.dt = (std::min)(raw, prev);
                jump = true;
            }
        } else if (raw < state.dt * 0.8) {
            state.counter = state.counter < 0 ? state.counter - 1 : -1;
            if (state.counter <= -2) {
                state.dt = (std::max)(raw, prev);
                jump = true;
            }
        } else {
            state.counter = 0;
        }
        if (jump) {
            state.dt = (std::min)((std::max)(state.dt, 0.0001), 0.1);
            state.counter = 0;
        } else {
            const double step = (std::max)((std::min)(raw, state.dt * 1.5), 0.0001);
            state.dt += (step - state.dt) * 0.1;
        }
    } else {
        state.counter = 0;
    }
    state.dt = (std::min)(state.dt, cap_seconds);
    return state.dt;
}

} // namespace Service
