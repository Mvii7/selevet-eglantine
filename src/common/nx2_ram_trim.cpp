// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_ram_trim.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace Common {

namespace {

constexpr size_t PageSize = 4096;
constexpr size_t ChunkPages = 256;
constexpr SIZE_T MinRegionSize = SIZE_T{64} << 20;

bool IsZero(const u64* page) {
    u64 bits = 0;
    for (size_t i = 0; i < PageSize / sizeof(u64); ++i) {
        bits |= page[i];
    }
    return bits == 0;
}

// Guest memory the game cleared or only read keeps pages of nothing but zeros in the working set
// (over 1 GB on title 1), which pushes the pages it does use out to disk on a PC short of RAM. A
// page zero on two passes in a row leaves the working set; a later access just faults it back.
// Reads go through ReadProcessMemory, which fails instead of faulting on memory being unmapped or
// protected, and nothing here writes guest memory.
void TrimPass(std::map<ULONG_PTR, std::vector<bool>>& zero_before, u64& trimmed_bytes) {
    const HANDLE process = GetCurrentProcess();
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> query(ChunkPages);
    std::vector<u64> buffer(ChunkPages * PageSize / sizeof(u64));
    std::map<ULONG_PTR, std::vector<bool>> zero_now;
    MEMORY_BASIC_INFORMATION mbi{};
    ULONG_PTR address = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto base = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        const SIZE_T size = mbi.RegionSize;
        address = base + size;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_MAPPED || size < MinRegionSize ||
            (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_READONLY)) {
            continue;
        }
        const size_t pages = size / PageSize;
        auto& now = zero_now[base];
        now.assign(pages, false);
        const auto before = zero_before.find(base);
        const bool has_before = before != zero_before.end() && before->second.size() == pages;
        for (size_t done = 0; done < pages; done += ChunkPages) {
            const size_t count = (std::min)(pages - done, ChunkPages);
            for (size_t i = 0; i < count; ++i) {
                query[i].VirtualAddress = reinterpret_cast<PVOID>(base + (done + i) * PageSize);
            }
            if (!QueryWorkingSetEx(process, query.data(),
                                   static_cast<DWORD>(count * sizeof(query[0])))) {
                break;
            }
            bool any_valid = false;
            for (size_t i = 0; i < count && !any_valid; ++i) {
                any_valid = query[i].VirtualAttributes.Valid != 0;
            }
            if (any_valid) {
                SIZE_T read = 0;
                const auto chunk = reinterpret_cast<LPCVOID>(base + done * PageSize);
                if (ReadProcessMemory(process, chunk, buffer.data(), count * PageSize, &read) &&
                    read == count * PageSize) {
                    for (size_t i = 0; i < count; ++i) {
                        if (query[i].VirtualAttributes.Valid == 0 ||
                            !IsZero(buffer.data() + i * (PageSize / sizeof(u64)))) {
                            continue;
                        }
                        now[done + i] = true;
                        if (has_before && before->second[done + i]) {
                            // On memory that isn't locked, this only removes it from the working set.
                            VirtualUnlock(query[i].VirtualAddress, PageSize);
                            trimmed_bytes += PageSize;
                        }
                    }
                }
                // Stay out of the game's way: at most about 4 GB of scanning a second.
                std::this_thread::sleep_for(std::chrono::microseconds{250});
            }
        }
    }
    zero_before = std::move(zero_now);
}

} // Anonymous namespace

void StartNx2RamTrim() {
    // EDEN_NX2_RAM_TRIM=<seconds between passes> turns this on. Off by default: on title 1 it freed
    // about 170 MB on average with no clear gain in hard faults or frame time.
    static const int seconds = [] {
        const char* value = std::getenv("EDEN_NX2_RAM_TRIM");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    if (seconds <= 0) {
        return;
    }
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
            SetThreadDescription(GetCurrentThread(), L"RamTrim");
            std::map<ULONG_PTR, std::vector<bool>> zero_before;
            u64 trimmed_bytes = 0;
            u32 passes = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds{seconds});
                const auto start = std::chrono::steady_clock::now();
                const u64 before = trimmed_bytes;
                TrimPass(zero_before, trimmed_bytes);
                if (++passes <= 4 || passes % 8 == 0) {
                    LOG_INFO(Common, "nx2:ramtrim pass {} took {} ms, {} MiB of zero pages released",
                             passes,
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count(),
                             (trimmed_bytes - before) >> 20);
                }
            }
        }).detach();
    });
}

void SetNx2HighPriority() {
    const char* value = std::getenv("EDEN_NX2_HIGH_PRIORITY");
    if (value == nullptr || value[0] != '0') {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
}

} // namespace Common

#else

namespace Common {

void StartNx2RamTrim() {}

void SetNx2HighPriority() {}

} // namespace Common

#endif
