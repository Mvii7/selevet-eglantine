// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}

// The Switch 2 SDK's nn::vi::detail::DefaultDisplay::QueryVrrSupported learns VRR support from a
// display mode capability event Eden never signals, so it reports none. Title 4 then assumes a fixed
// 60 Hz display and its speed swings with the frame rate; told VRR works, it enables it and times
// its frames itself. Bananza still steps one, two or three vsyncs after that report, which is not
// 1.0x at a frame rate in between, so its step is replaced separately. Make the exported function
// store true and succeed (any SDK version: found by name). EDEN_NX2_REPORT_VRR=0 leaves it alone.
bool ReportVrrSupported(std::span<u8> image, std::string_view module_name) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REPORT_VRR");
        return value == nullptr || value[0] != '0';
    }();
    if (!enabled || image.size() < 8) {
        return false;
    }
    const auto read32 = [&](u64 offset) -> std::optional<u32> {
        if (offset + 4 > image.size()) return std::nullopt;
        u32 value;
        std::memcpy(&value, image.data() + offset, 4);
        return value;
    };
    const auto read64 = [&](u64 offset) -> std::optional<u64> {
        if (offset + 8 > image.size()) return std::nullopt;
        u64 value;
        std::memcpy(&value, image.data() + offset, 8);
        return value;
    };
    const auto mod0 = read32(4);
    if (!mod0 || read32(*mod0) != Common::MakeMagic('M', 'O', 'D', '0')) {
        return false;
    }
    const auto dynamic_rel = read32(*mod0 + 4);
    if (!dynamic_rel) {
        return false;
    }
    u64 symtab = 0, strtab = 0, strsz = 0, hash = 0;
    for (u64 entry = *mod0 + static_cast<s32>(*dynamic_rel);; entry += 16) {
        const auto tag = read64(entry), value = read64(entry + 8);
        if (!tag || !value || *tag == 0) break;
        switch (*tag) {
        case 4: hash = *value; break;    // DT_HASH
        case 5: strtab = *value; break;  // DT_STRTAB
        case 6: symtab = *value; break;  // DT_SYMTAB
        case 10: strsz = *value; break;  // DT_STRSZ
        default: break;
        }
    }
    if (symtab == 0 || strtab == 0) {
        return false;
    }
    // DT_HASH gives the symbol count; with only DT_GNU_HASH (the Switch 2 SDK) the table runs up
    // to the string table, which follows it.
    std::optional<u32> count = hash != 0 ? read32(hash + 4) : std::nullopt;
    if (!count && strtab > symtab) {
        count = static_cast<u32>((strtab - symtab) / 24);
    }
    if (!count) {
        return false;
    }
    constexpr std::string_view Target = "_ZNK2nn2vi6detail14DefaultDisplay17QueryVrrSupportedEPb";
    for (u32 index = 0; index < *count; ++index) {
        const u64 symbol = symtab + u64{index} * 24;
        const auto name_offset = read32(symbol);
        const auto value = read64(symbol + 8);
        if (!name_offset || !value || *value == 0 || *name_offset + Target.size() >= strsz ||
            strtab + *name_offset + Target.size() >= image.size()) {
            continue;
        }
        const auto* name = reinterpret_cast<const char*>(image.data() + strtab + *name_offset);
        if (std::string_view{name, Target.size()} != Target || name[Target.size()] != '\0') {
            continue;
        }
        // mov w8, #1; strb w8, [x1]; mov w0, #0; ret
        static constexpr std::array<u32, 4> Code{0x52800028, 0x39000028, 0x52800000, 0xd65f03c0};
        if (*value + sizeof(Code) > image.size()) {
            return false;
        }
        std::memcpy(image.data() + *value, Code.data(), sizeof(Code));
        LOG_INFO(Loader, "{}: QueryVrrSupported at {:#x} now reports VRR support", module_name, *value);
        return true;
    }
    return false;
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    const auto& last_segment = nso_header.segments.back();
    const u64 segment_end = u64{last_segment.location} + last_segment.size;
    if (segment_end + last_segment.bss_size + module_start >
        std::numeric_limits<u32>::max() - Core::Memory::YUZU_PAGEMASK) {
        LOG_ERROR(Loader, "NSO {} has an invalid image size", nso_file.GetName());
        return std::nullopt;
    }

    size_t compressed_buffer_size = 0;
    for (size_t i = 0; i < nso_header.segments.size(); ++i) {
        const auto& segment = nso_header.segments[i];
        const bool compressed = nso_header.IsSegmentCompressed(i);
        const u64 stored_size = compressed ? nso_header.segments_compressed_size[i] : segment.size;
        if (u64{segment.location} + segment.size > segment_end ||
            u64{segment.offset} + stored_size > nso_file.GetSize() ||
            (compressed && (stored_size > static_cast<u64>(std::numeric_limits<int>::max()) ||
                            segment.size > static_cast<u32>(std::numeric_limits<int>::max())))) {
            LOG_ERROR(Loader, "NSO {} segment {} has an invalid range", nso_file.GetName(), i);
            return std::nullopt;
        }
        if (compressed) {
            compressed_buffer_size = std::max(compressed_buffer_size, static_cast<size_t>(stored_size));
        }
    }

    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + segment_end);
    {
        std::vector<u8> compressed_data(compressed_buffer_size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            const auto& segment = nso_header.segments[i];
            auto destination = std::span{codeset.memory}.subspan(module_start + segment.location,
                                                              segment.size);
            if (nso_header.IsSegmentCompressed(i)) {
                const auto source = std::span{compressed_data}.first(nso_header.segments_compressed_size[i]);
                if (nso_file.Read(source.data(), source.size(), segment.offset) != source.size()) {
                    LOG_ERROR(Loader, "NSO {} segment {} could not be read", nso_file.GetName(), i);
                    return std::nullopt;
                }
                const int result = nso_header.IsZBICCompressed()
                    ? Common::Compression::DecompressDataZBIC(destination, source)
                    : Common::Compression::DecompressDataLZ4(destination.data(), destination.size(),
                                                             source.data(), source.size());
                if (result < 0 || static_cast<size_t>(result) != destination.size()) {
                    LOG_ERROR(Loader, "NSO {} segment {} decompression failed: expected {} bytes, got {}",
                              nso_file.GetName(), i, destination.size(), result);
                    return std::nullopt;
                }
            } else if (!destination.empty() &&
                       nso_file.Read(destination.data(), destination.size(), segment.offset) != destination.size()) {
                LOG_ERROR(Loader, "NSO {} segment {} could not be read", nso_file.GetName(), i);
                return std::nullopt;
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }
    if (name == "sdk") {
        if (!ReportVrrSupported(std::span{codeset.memory}.subspan(module_start), name)) {
            LOG_WARNING(Loader, "sdk: QueryVrrSupported was not patched. The game will assume a "
                                "fixed 60 Hz display, and its speed will follow the frame rate. "
                                "EDEN_NX2_REPORT_VRR=0 leaves this unpatched on purpose.");
        }
    }
#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
