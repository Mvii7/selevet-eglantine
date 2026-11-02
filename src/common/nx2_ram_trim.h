// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common {

/// Starts the background thread that gives RAM holding pages of zeros back to the system (once).
void StartNx2RamTrim();

/// Runs the process at high priority. EDEN_NX2_HIGH_PRIORITY=0 leaves it at normal.
void SetNx2HighPriority();

} // namespace Common
