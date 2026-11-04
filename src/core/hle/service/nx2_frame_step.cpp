// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/hle/service/nx2_decouple.h"
#include "core/hle/service/nx2_frame_step.h"
#include "core/memory.h"

namespace Service {
namespace {

constexpr int kWindow = 16;
constexpr u32 kNone = 32;
constexpr u32 kRecipBits = 0x3c888889; // 1/60
constexpr u32 kSixtyBits = 0x42700000; // 60.0f

struct Tag {
    u32 reg = kNone;
    int at = -1000;

    bool live(int now) const {
        return reg < kNone && now - at <= kWindow && now >= at;
    }
};

u32 ReadU32(std::span<const u8> image, u64 offset) {
    u32 value;
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

void WriteU32(std::span<u8> image, u64 offset, u32 value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

bool IsMovz(u32 word, u32& hw, u32& imm, u32& rd) {
    if ((word & 0xFF800000) != 0x52800000) {
        return false;
    }
    hw = (word >> 21) & 3;
    imm = (word >> 5) & 0xFFFF;
    rd = word & 31;
    return true;
}

bool IsMovk(u32 word, u32& hw, u32& imm, u32& rd) {
    if ((word & 0xFF800000) != 0x72800000) {
        return false;
    }
    hw = (word >> 21) & 3;
    imm = (word >> 5) & 0xFFFF;
    rd = word & 31;
    return true;
}

bool IsUdiv(u32 word, u32& rd, u32& rn, u32& rm) {
    // udiv wd, wn, wm
    if ((word & 0xFFE0FC00) != 0x1AC00800) {
        return false;
    }
    rm = (word >> 16) & 31;
    rn = (word >> 5) & 31;
    rd = word & 31;
    return true;
}

bool IsCvtFromW(u32 word, u32& sd, u32& rn) {
    const u32 op = word & 0xFFFFFC00;
    // scvtf/ucvtf of a 32-bit or 64-bit integer into a single.
    if (op != 0x1E220000 && op != 0x1E230000 && op != 0x1E620000 && op != 0x1E630000) {
        return false;
    }
    rn = (word >> 5) & 31;
    sd = word & 31;
    return true;
}

bool IsAdrp(u32 word, u32& rd, s64& byte_delta) {
    if ((word & 0x9F000000) != 0x90000000) {
        return false;
    }
    rd = word & 31;
    const u32 immlo = (word >> 29) & 3;
    const u32 immhi = (word >> 5) & 0x7FFFF;
    s32 imm21 = static_cast<s32>((immhi << 2) | immlo);
    if ((imm21 & (1 << 20)) != 0) {
        imm21 |= ~((1 << 21) - 1);
    }
    byte_delta = static_cast<s64>(imm21) << 12;
    return true;
}

bool IsLdrSUnsigned(u32 word, u32& rt, u32& rn, u32& byte_off) {
    if ((word & 0xFFC00000) != 0xBD400000) {
        return false;
    }
    byte_off = ((word >> 10) & 0xFFF) << 2;
    rn = (word >> 5) & 31;
    rt = word & 31;
    return true;
}

bool IsLdrSLiteral(u32 word, u32& rt, s64& byte_delta) {
    // ldr st, label. The 32-bit literal form is the only encoding under this mask.
    if ((word & 0xFF000000) != 0x1C000000) {
        return false;
    }
    rt = word & 31;
    s32 imm19 = static_cast<s32>((word >> 5) & 0x7FFFF);
    if ((imm19 & (1 << 18)) != 0) {
        imm19 |= ~((1 << 19) - 1);
    }
    byte_delta = static_cast<s64>(imm19) << 2;
    return true;
}

bool IsFmovOne(u32 word, u32& sd) {
    if ((word & 0xFFFFFFE0) != 0x1E2E1000) {
        return false;
    }
    sd = word & 31;
    return true;
}

bool IsFmovFromW(u32 word, u32& sd, u32& rn) {
    if ((word & 0xFFFFFC00) != 0x1E270000) {
        return false;
    }
    rn = (word >> 5) & 31;
    sd = word & 31;
    return true;
}

bool IsFdiv(u32 word, u32& sd, u32& sn, u32& sm) {
    if ((word & 0xFFE0FC00) != 0x1E201800) {
        return false;
    }
    sm = (word >> 16) & 31;
    sn = (word >> 5) & 31;
    sd = word & 31;
    return true;
}

bool IsFmul(u32 word, u32& sd, u32& sn, u32& sm) {
    if ((word & 0xFFE0FC00) != 0x1E200800) {
        return false;
    }
    sm = (word >> 16) & 31;
    sn = (word >> 5) & 31;
    sd = word & 31;
    return true;
}

bool IsTransparent(u32 word) {
    // str wt/st, [xn, #imm] without writeback, and nop. Anything else ends the chain: a frame
    // step's arithmetic is a straight line, and a branch would make the registers guessed.
    return word == 0xD503201F || (word & 0xFFC00000) == 0xB9000000 || (word & 0xFFC00000) == 0xBD000000;
}

void KillW(Tag& tag, u32 rd) {
    if (tag.reg == rd) {
        tag.reg = kNone;
    }
}

void KillS(Tag& tag, u32 sd) {
    if (tag.reg == sd) {
        tag.reg = kNone;
    }
}

u32 EncodeB(s64 byte_delta) {
    const s64 imm = byte_delta >> 2;
    return 0x14000000u | (static_cast<u32>(imm) & 0x03FFFFFFu);
}

u32 EncodeAdrp(u32 rd, s64 page_delta) {
    const s64 imm21 = page_delta >> 12;
    const u32 immlo = static_cast<u32>(imm21) & 3u;
    const u32 immhi = (static_cast<u32>(imm21) >> 2) & 0x7FFFFu;
    return 0x90000000u | (immlo << 29) | (immhi << 5) | rd;
}

u32 EncodeLdrS(u32 rt, u32 rn, u32 byte_off) {
    const u32 imm12 = byte_off >> 2;
    return 0xBD400000u | (imm12 << 10) | (rn << 5) | rt;
}

enum class Kind { Vfr, Scale, Divide, Plain };

struct Site {
    u64 offset{};
    u32 dest{};
    Kind kind{};
};

struct Chain {
    Tag sixty;
    Tag quot;
    Tag rate;
    Tag one;
    Tag conv;
    Tag recip;
    Tag sixty_f;
    Tag bits;
    u32 bits_value{};

    void Clear() {
        *this = {};
    }

    void KillWritesW(u32 rd) {
        KillW(sixty, rd);
        KillW(quot, rd);
        KillW(bits, rd);
    }

    void KillWritesS(u32 sd) {
        KillS(rate, sd);
        KillS(one, sd);
        KillS(conv, sd);
        KillS(recip, sd);
        KillS(sixty_f, sd);
    }
};

struct Found {
    std::vector<Site> sites;
    int pooled{};
};

bool PoolBits(std::span<const u8> image, u64 load_base, u64 guest, u32& bits) {
    if (guest < load_base) {
        return false;
    }
    const u64 offset = guest - load_base;
    if ((offset & 3) != 0 || offset + sizeof(u32) > image.size()) {
        return false;
    }
    bits = ReadU32(image, offset);
    return bits == kRecipBits || bits == kSixtyBits;
}

Found Scan(std::span<const u8> image, u64 text_off, u64 text_size, u64 load_base) {
    Found found;
    if (text_size < 4 || text_off > image.size() || text_size > image.size() - text_off) {
        return found;
    }
    Chain chain;
    // Page address last left in each X register by adrp. Cleared with the arithmetic chain.
    u64 page[32]{};
    const u64 end = text_off + (text_size & ~u64{3});
    int index = 0;
    const auto take_pool = [&](u32 rt, u32 bits) {
        chain.KillWritesS(rt);
        if (bits == kRecipBits) {
            chain.recip = {static_cast<u32>(rt), index};
        } else {
            chain.sixty_f = {static_cast<u32>(rt), index};
        }
        ++found.pooled;
    };
    for (u64 pc = text_off; pc + 4 <= end; pc += 4, ++index) {
        const u32 word = ReadU32(image, pc);
        u32 rt = 0;
        u32 addr_reg = 0;
        u32 byte_off = 0;
        s64 delta = 0;
        if (IsAdrp(word, addr_reg, delta)) {
            const s64 target = static_cast<s64>((load_base + pc) & ~u64{0xFFF}) + delta;
            page[addr_reg] = target > 0 ? static_cast<u64>(target) : 0;
            continue;
        }
        if (IsLdrSUnsigned(word, rt, addr_reg, byte_off)) {
            u32 bits = 0;
            if (page[addr_reg] != 0 && PoolBits(image, load_base, page[addr_reg] + byte_off, bits)) {
                take_pool(rt, bits);
            } else {
                chain.KillWritesS(rt);
            }
            continue;
        }
        if (IsLdrSLiteral(word, rt, delta)) {
            u32 bits = 0;
            const s64 guest = static_cast<s64>(load_base + pc) + delta;
            if (guest > 0 && PoolBits(image, load_base, static_cast<u64>(guest), bits)) {
                take_pool(rt, bits);
            } else {
                chain.KillWritesS(rt);
            }
            continue;
        }
        u32 hw = 0, imm = 0, rd = 0, rn = 0, rm = 0, sd = 0, sn = 0, sm = 0;
        if (IsMovz(word, hw, imm, rd)) {
            const bool sixty = hw == 0 && imm == 60;
            const bool recip_lo = hw == 0 && imm == 0x8889;
            const bool sixty_f = hw == 1 && imm == 0x4270;
            chain.KillWritesW(rd);
            if (sixty) {
                chain.sixty = {rd, index};
            } else if (recip_lo) {
                chain.bits = {rd, index};
                chain.bits_value = 0x8889;
            } else if (sixty_f) {
                chain.bits = {rd, index};
                chain.bits_value = kSixtyBits;
            }
            continue;
        }
        if (IsMovk(word, hw, imm, rd)) {
            if (hw == 1 && imm == 0x3c88 && chain.bits.live(index) && chain.bits.reg == rd &&
                chain.bits_value == 0x8889) {
                chain.bits.at = index;
                chain.bits_value = kRecipBits;
            } else {
                chain.KillWritesW(rd);
            }
            continue;
        }
        if (IsUdiv(word, rd, rn, rm)) {
            const bool from_sixty = chain.sixty.live(index) && rn == chain.sixty.reg;
            chain.KillWritesW(rd);
            if (from_sixty) {
                chain.quot = {rd, index};
            }
            continue;
        }
        if (IsCvtFromW(word, sd, rn)) {
            const bool from_quot = chain.quot.live(index) && rn == chain.quot.reg;
            chain.KillWritesS(sd);
            chain.conv = {sd, index};
            if (from_quot) {
                chain.rate = {sd, index};
            }
            continue;
        }
        if (IsFmovOne(word, sd)) {
            chain.KillWritesS(sd);
            chain.one = {sd, index};
            continue;
        }
        if (IsFmovFromW(word, sd, rn)) {
            const bool from_bits = chain.bits.live(index) && chain.bits.reg == rn;
            const u32 value = chain.bits_value;
            chain.KillWritesS(sd);
            if (from_bits && value == kRecipBits) {
                chain.recip = {sd, index};
            } else if (from_bits && value == kSixtyBits) {
                chain.sixty_f = {sd, index};
            }
            continue;
        }
        if (IsFdiv(word, sd, sn, sm)) {
            if (chain.one.live(index) && chain.rate.live(index) && sn == chain.one.reg &&
                sm == chain.rate.reg) {
                found.sites.push_back({pc, sd, Kind::Vfr});
            } else if (chain.conv.live(index) && chain.sixty_f.live(index) && sn == chain.conv.reg &&
                       sm == chain.sixty_f.reg) {
                found.sites.push_back({pc, sd, Kind::Divide});
            } else if (chain.one.live(index) && chain.conv.live(index) && sn == chain.one.reg &&
                       sm == chain.conv.reg) {
                // 1 / (float)n, with n not produced by dividing by 60. Bananza's refresh path.
                found.sites.push_back({pc, sd, Kind::Plain});
            }
            chain.KillWritesS(sd);
            continue;
        }
        if (IsFmul(word, sd, sn, sm)) {
            const bool scale = chain.conv.live(index) && chain.recip.live(index) &&
                               ((sn == chain.conv.reg && sm == chain.recip.reg) ||
                                (sn == chain.recip.reg && sm == chain.conv.reg));
            if (scale) {
                found.sites.push_back({pc, sd, Kind::Scale});
            }
            chain.KillWritesS(sd);
            continue;
        }
        if (IsTransparent(word)) {
            continue;
        }
        chain.Clear();
        std::memset(page, 0, sizeof(page));
    }
    return found;
}

bool FitsBranch(s64 delta) {
    return (delta & 3) == 0 && delta >= -128 * 1024 * 1024 && delta < 128 * 1024 * 1024;
}

bool WriteCave(std::span<u8> image, u64 site, u32 dest, u64 cave, u64 float_off, u64 load_base) {
    const s64 branch = static_cast<s64>(cave) - static_cast<s64>(site);
    const s64 back = static_cast<s64>(site + 4) - static_cast<s64>(cave + 8);
    if (!FitsBranch(branch) || !FitsBranch(back)) {
        return false;
    }
    const u64 pc = load_base + cave;
    const u64 target = load_base + float_off;
    const s64 page_delta = static_cast<s64>(target & ~u64{0xFFF}) - static_cast<s64>(pc & ~u64{0xFFF});
    const s64 imm21 = page_delta >> 12;
    if (imm21 < -(1 << 20) || imm21 >= (1 << 20)) {
        return false;
    }
    const u32 page_off = static_cast<u32>(target & 0xFFF);
    if ((page_off & 3) != 0 || page_off > 0x3FFC) {
        return false;
    }
    WriteU32(image, cave, EncodeAdrp(16, page_delta));
    WriteU32(image, cave + 4, EncodeLdrS(dest, 16, page_off));
    WriteU32(image, cave + 8, EncodeB(back));
    WriteU32(image, site, EncodeB(branch));
    const float initial = 1.0f / 60.0f;
    std::memcpy(image.data() + float_off, &initial, sizeof(initial));
    return true;
}

std::atomic<u64> g_guest_step{};
Nx2DecoupleState g_step{};
std::chrono::steady_clock::time_point g_last_present{};

} // namespace

Nx2FrameStepScan Nx2PatchBananzaFrameStep(std::span<u8> image, u64 text_off, u64 text_size,
                                          u64 text_end, u64 float_off, u64 load_base) {
    Nx2FrameStepScan result;
    const auto found = Scan(image, text_off, text_size, load_base);
    result.pooled = found.pooled;
    for (const auto& site : found.sites) {
        switch (site.kind) {
        case Kind::Vfr: ++result.vfr; break;
        case Kind::Scale: ++result.scale; break;
        case Kind::Divide: ++result.divide; break;
        case Kind::Plain: ++result.plain; break;
        }
    }
    const Site* chosen = nullptr;
    const auto only = [&](Kind kind) -> const Site* {
        const Site* found_site = nullptr;
        for (const auto& site : found.sites) {
            if (site.kind == kind) {
                found_site = &site;
            }
        }
        return found_site;
    };
    if (result.vfr == 1) {
        chosen = only(Kind::Vfr);
    } else if (result.vfr == 0 && result.scale == 1) {
        chosen = only(Kind::Scale);
    } else if (result.vfr == 0 && result.scale == 0 && result.divide == 1) {
        chosen = only(Kind::Divide);
    } else if (result.vfr == 0 && result.scale == 0 && result.divide == 0 && result.plain == 1) {
        chosen = only(Kind::Plain);
    }
    if (chosen == nullptr || float_off + sizeof(float) > image.size() || (text_size & 3) != 0) {
        return result;
    }
    const u64 code_end = text_off + text_size;
    const u64 cave = code_end;
    const bool float_hits_cave = float_off < cave + 12 && float_off + sizeof(float) > cave;
    if (chosen->offset < text_off || chosen->offset + 4 > code_end || cave + 12 > text_end ||
        cave + 12 > image.size() || float_hits_cave) {
        return result;
    }
    if (!WriteCave(image, chosen->offset, chosen->dest, cave, float_off, load_base)) {
        return result;
    }
    result.patch = Nx2FrameStepPatch{float_off, chosen->offset};
    return result;
}

bool Nx2DecoupleEnabled() {
    const char* value = std::getenv("EDEN_NX2_DECOUPLE");
    return value == nullptr || value[0] != '0';
}

int Nx2DecoupleCapDenominator() {
    const char* value = std::getenv("EDEN_NX2_DECOUPLE_DT_CAP");
    if (value == nullptr || value[0] == '\0') {
        return 20;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return 20;
    }
    if (parsed == 0) {
        return 10;
    }
    if (parsed >= 10 && parsed <= 60) {
        return static_cast<int>(parsed);
    }
    return 20;
}

void Nx2SetFrameStepAddress(u64 guest) {
    g_step = {};
    g_last_present = {};
    g_guest_step.store(guest, std::memory_order_release);
}

void Nx2NotePresentedFrame(Core::Memory::Memory& memory, std::chrono::steady_clock::time_point now,
                           double speed_scale) {
    const u64 guest = g_guest_step.load(std::memory_order_acquire);
    if (guest == 0) {
        return;
    }
    if (g_last_present != std::chrono::steady_clock::time_point{}) {
        const double raw = std::chrono::duration<double>(now - g_last_present).count();
        const double cap = 1.0 / Nx2DecoupleCapDenominator();
        const double dt = Nx2DecoupleAdvance(g_step, Nx2DecoupleSample(raw, speed_scale), cap);
        const float step = static_cast<float>(dt);
        u32 bits;
        std::memcpy(&bits, &step, sizeof(bits));
        memory.Write32(guest, bits);
    }
    g_last_present = now;
}

} // namespace Service
