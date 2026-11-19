// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 6> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
    {Common::FS::EdenPath::KeysDir, "keys"},
    {Common::FS::EdenPath::ConfigDir, "config"},
}};

void WriteIps(const std::filesystem::path& path, u32 word) {
    std::vector<u8> ips{'P', 'A', 'T', 'C', 'H'};
    ips.push_back(static_cast<u8>(PatchOffset >> 16));
    ips.push_back(static_cast<u8>(PatchOffset >> 8));
    ips.push_back(static_cast<u8>(PatchOffset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

// An IPS (24-bit offsets) or IPS32 file with one record: `bytes` at `offset` of the NSO file.
std::vector<u8> MakeIps(bool ips32, u32 offset, std::span<const u8> bytes) {
    std::vector<u8> ips;
    if (ips32) {
        ips = {'I', 'P', 'S', '3', '2', static_cast<u8>(offset >> 24)};
    } else {
        ips = {'P', 'A', 'T', 'C', 'H'};
    }
    ips.insert(ips.end(), {static_cast<u8>(offset >> 16), static_cast<u8>(offset >> 8),
                           static_cast<u8>(offset), static_cast<u8>(bytes.size() >> 8),
                           static_cast<u8>(bytes.size())});
    ips.insert(ips.end(), bytes.begin(), bytes.end());
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    return ips;
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt) {
        if (const auto it = Settings::values.disabled_addons.find(OcarinaTitleId);
            it != Settings::values.disabled_addons.end()) {
            previous_disabled = it->second;
        }
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word);
        }
        system.emplace();
        system->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system->GetFileSystemController().CreateFactories(*system->GetFilesystem());
    }

    ~PatchManagerTestContext() {
        system.reset();
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        if (previous_disabled) {
            Settings::values.disabled_addons[OcarinaTitleId] = std::move(*previous_disabled);
        } else {
            Settings::values.disabled_addons.erase(OcarinaTitleId);
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system->GetFileSystemController(),
                                     system->GetContentProvider()};
    }

    void AddUserBytePatch(u32 offset, u8 value) {
        const auto exefs = root / "load" / fmt::format("{:016X}", OcarinaTitleId) / "UserMod" / "exefs";
        std::filesystem::create_directories(exefs);
        const std::array<u8, 16> ips{
            'I', 'P', 'S', '3', '2',
            static_cast<u8>(offset >> 24), static_cast<u8>(offset >> 16),
            static_cast<u8>(offset >> 8), static_cast<u8>(offset),
            0, 1, value, 'E', 'E', 'O', 'F',
        };
        std::ofstream file{exefs / fmt::format("{}.ips", OcarinaBuildIdHex), std::ios::binary};
        file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
        file.flush();
        REQUIRE(file.good());
    }

    // Writes `contents` as <load>/<Ocarina>/<mod>/exefs/<file_name>; returns its path.
    std::filesystem::path AddModFile(std::string_view mod, std::string_view file_name,
                                     std::span<const u8> contents) {
        const auto exefs =
            root / "load" / fmt::format("{:016X}", OcarinaTitleId) / std::string{mod} / "exefs";
        std::filesystem::create_directories(exefs);
        const auto path = exefs / std::string{file_name};
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
        file.flush();
        REQUIRE(file.good());
        return path;
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    std::optional<std::vector<std::string>> previous_disabled;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::optional<Core::System> system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso) {
    REQUIRE(nso.size() >= PatchOffset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + PatchOffset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

constexpr size_t OcarinaDark = 0x3C46200; // Dark's routine
constexpr size_t OcarinaCave = 0x3C46E00; // ours, at the free end of that page
constexpr size_t OcarinaWaterHook = 0x475F8;
constexpr size_t OcarinaWaterMul = 0x4760C;
constexpr size_t OcarinaRipples = 0x3C46F20; // the ripple routine, 148 bytes
constexpr u32 WaterFcvtzs = 0x1E380009; // fcvtzs w9, s0
constexpr u32 WaterMul = 0x1B097D08;    // mul w8, w8, w9
constexpr u32 WaterCall = 0x94EFFE4A;   // bl 0x3c46f20

// Ocarina's main as the high refresh rate tests need it: the water renderer's fcvtzs and mul (the
// ripple hook's and nop's originals), a BSS that holds the routine's state, and zero bytes from
// Dark's routine to the end of its page.
void SetTestEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::vector<u8> MakeOcarinaMain() {
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    auto nso = MakeMain(OcarinaBuildId, NopInstruction);
    nso.resize(Code + OcarinaCave + 0x200);
    std::memcpy(nso.data() + Code + OcarinaWaterHook, &WaterFcvtzs, sizeof(WaterFcvtzs));
    std::memcpy(nso.data() + Code + OcarinaWaterMul, &WaterMul, sizeof(WaterMul));
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[2].location = 0x4000000;
    header.segments[2].size = 0x100000;
    header.segments[2].bss_size = 0x1000000;
    std::memcpy(nso.data(), &header, sizeof(header));
    return nso;
}

// Writes `parts` into `code` where they belong (what the live switch writes to guest memory).
void WriteOotHighRefreshCode(std::span<u8> code, const FileSys::OotHighRefreshCode& parts) {
    using Parts = FileSys::OotHighRefreshCode;
    std::ranges::copy(parts.call, code.begin() + Parts::CallOffset);
    std::ranges::copy(parts.water, code.begin() + Parts::WaterOffset);
    std::ranges::copy(parts.water_mul, code.begin() + Parts::WaterMulOffset);
    std::ranges::copy(parts.dark, code.begin() + Parts::DarkOffset);
    std::ranges::copy(parts.cave, code.begin() + Parts::CaveOffset);
    std::ranges::copy(parts.ripples, code.begin() + Parts::RippleOffset);
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

TEST_CASE("Built-in patches preserve user overrides on already-patched input", "[patch_manager]") {
    PatchManagerTestContext context;
    constexpr size_t CallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
    constexpr u32 RoutineLastByte = sizeof(Loader::NSOHeader) + 0x3C46200 + 119;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};

    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(RoutineLastByte + 1);
    nso = context.Manager().PatchNSO(nso, "main");
    REQUIRE(nso.size() == RoutineLastByte + 1);
    REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), nso.begin() + CallOffset));
    REQUIRE(nso[RoutineLastByte] == 0x3F);
    REQUIRE(WordAt(nso) == BranchInstruction);

    // Over Max Resolution's branch (a mod writing into Dark's routine is his standalone Dynamic
    // FPS, which the built-in replaces: see below).
    constexpr u8 UserByte = 0x5A;
    context.AddUserBytePatch(PatchOffset, UserByte);
    auto expected = nso;
    expected[PatchOffset] = UserByte;
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(std::ranges::equal(patched, expected));
}

TEST_CASE("High refresh rate built-in calls its own routine only while its setting is on",
          "[patch_manager]") {
    constexpr std::string_view HighRefresh = "High refresh rate (built-in, experimental)";
    constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Dark = Code + OcarinaDark;
    constexpr size_t Routine = Code + OcarinaCave;
    constexpr size_t CallOffset = Code + 0x6E6CC4;
    constexpr u32 DynamicCall = 0x94D57D4F;     // bl 0x3c46200 (Dark's routine)
    constexpr u32 HighRefreshCall = 0x94D5804F; // bl 0x3c46e00 (ours)
    constexpr u32 DarkThreshold = 0x3F83D70A;   // Dark's routine +0x74: 1.03f
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    const auto listed = [](const FileSys::PatchManager& manager, std::string_view name) {
        const auto patches = manager.GetPatches();
        return std::ranges::find(patches, name, &FileSys::Patch::name) != patches.end();
    };
    // Dark's Dynamic FPS: his call and routine, not the high refresh mode (the water ripple
    // built-in may or may not be there next to it).
    const auto dark_only = [&](const std::vector<u8>& patched) {
        return word_at(patched, CallOffset) == DynamicCall &&
               word_at(patched, Dark + 0x74) == DarkThreshold &&
               !FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code));
    };
    // The ripple routine (main+0x3C46F20) behind its call and nop; nothing else of ours on the page
    // after the high refresh cave (the texture pool routine is not applied to this main).
    constexpr size_t Ripples = Code + OcarinaRipples;
    const auto ripples_on = [&](const std::vector<u8>& patched) {
        bool page_free = true;
        for (size_t offset = Routine + 0x8C; offset < Ripples; offset += 4) {
            page_free = page_free && word_at(patched, offset) == 0;
        }
        for (size_t offset = Ripples + 148; offset < Routine + 0x200; offset += 4) {
            page_free = page_free && word_at(patched, offset) == 0;
        }
        return word_at(patched, Code + OcarinaWaterHook) == WaterCall &&
               word_at(patched, Code + OcarinaWaterMul) == NopInstruction &&
               word_at(patched, Ripples) == 0x1E220101 &&        // scvtf s1, w8
               word_at(patched, Ripples + 0x10) == 0xB00092C9 && // adrp x9, 0x4e9f000
               word_at(patched, Ripples + 0x14) == 0x91388129 && // add x9, x9, #0xe20
               word_at(patched, Ripples + 0x18) == 0x9102012A && // add x10, x9, #0x80
               word_at(patched, Ripples + 0x88) == 0xD65F03C0 && // ret
               word_at(patched, Ripples + 0x8C) == 0x2A1F03E8 && // mov w8, wzr
               word_at(patched, Ripples + 0x90) == 0xD65F03C0 && // ret
               page_free;
    };
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();

    SECTION("Off: Dark's routine, with the water ripple built-in next to it, high refresh not listed") {
        Settings::values.oot_high_refresh.SetValue(false);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(dark_only(patched));
        for (size_t offset = Dark + 0x78; offset < Routine; offset += 4) {
            REQUIRE(word_at(patched, offset) == 0);
        }
        // v5.8: the ripple routine, its call and nop with Dark's mode too.
        REQUIRE(ripples_on(patched));
        REQUIRE_FALSE(listed(context.Manager(), HighRefresh));
        REQUIRE(listed(context.Manager(), DynamicFps));
        REQUIRE(listed(context.Manager(), "Water ripples at any frame rate (built-in)"));
    }

    SECTION("Off, with the water ripple built-in off in Add-Ons: Dark's routine alone, as before") {
        Settings::values.oot_high_refresh.SetValue(false);
        Settings::values.disabled_addons[OcarinaTitleId] = {
            std::string{"Water ripples at any frame rate (built-in)"}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        REQUIRE(dark_only(patched));
        REQUIRE(word_at(patched, Code + OcarinaWaterHook) == WaterFcvtzs);
        REQUIRE(word_at(patched, Code + OcarinaWaterMul) == WaterMul);
        for (size_t offset = Dark + 0x78; offset < Routine + 0x200; offset += 4) {
            REQUIRE(word_at(patched, offset) == 0);
        }
    }

    SECTION("On: the call to the high refresh routine, with its speed scale word") {
        Settings::values.oot_high_refresh.SetValue(true);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, CallOffset) == HighRefreshCall);
        // Dark's routine stays, nothing after it (another mod's spot).
        REQUIRE(word_at(patched, Dark + 0x74) == DarkThreshold);
        for (size_t offset = Dark + 0x78; offset < Routine; offset += 4) {
            REQUIRE(word_at(patched, offset) == 0);
        }
        REQUIRE(word_at(patched, Routine + 0x18) == 0xB40002CC); // cbz x12, +0x70 (Dark's)
        REQUIRE(word_at(patched, Routine + 0x6C) == 0x14000002); // b +0x74
        REQUIRE(word_at(patched, Routine + 0x70) == 0xD65F03C0); // ret
        REQUIRE(word_at(patched, Routine + 0x78) == 0x1E230848); // fmul s8, s2, s3
        REQUIRE(word_at(patched, Routine + 0x84) == 0x40400000); // 3.0f
        REQUIRE(word_at(patched, Routine + 0x88) == 0x3F800000); // 1.0f
        // The water ripple routine further on the page, its call and nop.
        REQUIRE(ripples_on(patched));
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});
        REQUIRE(listed(context.Manager(), HighRefresh));
    }

    SECTION("On, with another mod's code right after Dark's routine: still on") {
        // The Hyrule Cheat Menu writes main+0x3C46280..0x3C46398 (2026-11-10).
        Settings::values.oot_high_refresh.SetValue(true);
        nso[Dark + 0x80] = 1;
        nso[Dark + 0x190] = 1;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, CallOffset) == HighRefreshCall);
        REQUIRE(patched[Dark + 0x80] == 1);
        REQUIRE(patched[Dark + 0x190] == 1);
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});
    }

    SECTION("On, with a ripple routine word in use: Dark's routine") {
        Settings::values.oot_high_refresh.SetValue(true);
        nso[Ripples + 0x90] = 1;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, CallOffset) == DynamicCall);
        REQUIRE(word_at(patched, Code + OcarinaWaterHook) == WaterFcvtzs);
        REQUIRE(word_at(patched, Code + OcarinaWaterMul) == WaterMul);
        REQUIRE(patched[Ripples + 0x90] == 1);
        REQUIRE(word_at(patched, Ripples) == 0);
        REQUIRE(word_at(patched, Routine) == 0);
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)));
    }

    SECTION("On, with the old accumulator's place in use: still on (it is free now)") {
        Settings::values.oot_high_refresh.SetValue(true);
        nso[Routine + 0x10C] = 1;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, CallOffset) == HighRefreshCall);
        REQUIRE(patched[Routine + 0x10C] == 1);
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});
    }

    SECTION("On, with another word at the water hook: Dark's routine, no ripple built-in") {
        Settings::values.oot_high_refresh.SetValue(true);
        std::memcpy(nso.data() + Code + OcarinaWaterHook, &NopInstruction, sizeof(u32));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(dark_only(patched));
        REQUIRE(word_at(patched, Code + OcarinaWaterHook) == NopInstruction);
        REQUIRE(word_at(patched, Code + OcarinaWaterMul) == WaterMul);
        REQUIRE(word_at(patched, Ripples) == 0);
    }

    SECTION("On, with another word at the water mul: Dark's routine, no ripple built-in") {
        Settings::values.oot_high_refresh.SetValue(true);
        std::memcpy(nso.data() + Code + OcarinaWaterMul, &NopInstruction, sizeof(u32));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(dark_only(patched));
        REQUIRE(word_at(patched, Code + OcarinaWaterHook) == WaterFcvtzs);
        REQUIRE(word_at(patched, Ripples) == 0);
    }

    SECTION("On, with the state past the end of BSS: Dark's routine") {
        Settings::values.oot_high_refresh.SetValue(true);
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 0x4E9FE00 + 0xAF - 0x4100000; // one byte short
        std::memcpy(nso.data(), &header, sizeof(header));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(dark_only(patched));
        REQUIRE(word_at(patched, Code + OcarinaWaterHook) == WaterFcvtzs);
        REQUIRE(word_at(patched, Code + OcarinaWaterMul) == WaterMul);
    }

    SECTION("On, but off in Add-Ons: Dark's routine") {
        Settings::values.oot_high_refresh.SetValue(true);
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{HighRefresh}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(dark_only(patched));
    }
}

TEST_CASE("High refresh rate mode switches live between the two boot layouts",
          "[patch_manager]") {
    using FileSys::OotHighRefreshLayout;
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Scale = 0x88; // the speed scale word, in the cave
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    // What boots with the setting off and on.
    PatchManagerTestContext context;
    const auto nso = MakeOcarinaMain();
    Settings::values.oot_high_refresh.SetValue(false);
    const auto off = context.Manager().PatchNSO(nso, "main");
    Settings::values.oot_high_refresh.SetValue(true);
    const auto on = context.Manager().PatchNSO(nso, "main");
    REQUIRE(off != on);
    const auto off_parts = FileSys::ReadOotHighRefreshCode(std::span<const u8>{off}.subspan(Code));
    const auto on_parts = FileSys::ReadOotHighRefreshCode(std::span<const u8>{on}.subspan(Code));
    REQUIRE(off_parts);
    REQUIRE(on_parts);
    REQUIRE(FileSys::GetOotHighRefreshLayout(*off_parts) == OotHighRefreshLayout::Off);
    REQUIRE(FileSys::GetOotHighRefreshLayout(*on_parts) == OotHighRefreshLayout::On);

    SECTION("Switching writes exactly what boots in the other mode, both ways") {
        auto module = off;
        const auto switched_on = FileSys::SwitchOotHighRefreshCode(*off_parts, true);
        REQUIRE(switched_on);
        WriteOotHighRefreshCode(std::span{module}.subspan(Code), *switched_on);
        REQUIRE(module == on);
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span<const u8>{module}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});

        const auto switched_off = FileSys::SwitchOotHighRefreshCode(*switched_on, false);
        REQUIRE(switched_off);
        WriteOotHighRefreshCode(std::span{module}.subspan(Code), *switched_off);
        REQUIRE(module == off);
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span<const u8>{module}.subspan(Code)));
    }

    SECTION("Switching to the mode it is in changes nothing, the speed scale word kept") {
        auto turbo = *on_parts;
        constexpr std::array<u8, 4> TwoPointZero{0x00, 0x00, 0x00, 0x40}; // 2.0f
        std::ranges::copy(TwoPointZero, turbo.cave.begin() + Scale);
        REQUIRE(FileSys::GetOotHighRefreshLayout(turbo) == OotHighRefreshLayout::On);
        const auto still_on = FileSys::SwitchOotHighRefreshCode(turbo, true);
        REQUIRE(still_on);
        REQUIRE(still_on->cave == turbo.cave);
        REQUIRE(still_on->water == turbo.water);

        const auto still_off = FileSys::SwitchOotHighRefreshCode(*off_parts, false);
        REQUIRE(still_off);
        REQUIRE(still_off->cave == off_parts->cave);
        REQUIRE(still_off->water == off_parts->water);

        // Off from any speed scale: Dark's cave, the scale word zero again.
        const auto from_turbo = FileSys::SwitchOotHighRefreshCode(turbo, false);
        REQUIRE(from_turbo);
        REQUIRE(from_turbo->cave == off_parts->cave);
        REQUIRE(from_turbo->water == off_parts->water);
    }

    SECTION("Code in neither mode is never switched") {
        const auto refused = [](const FileSys::OotHighRefreshCode& parts) {
            return FileSys::GetOotHighRefreshLayout(parts) == OotHighRefreshLayout::Unknown &&
                   !FileSys::SwitchOotHighRefreshCode(parts, true) &&
                   !FileSys::SwitchOotHighRefreshCode(parts, false);
        };
        auto used_cave = *off_parts; // a word of our cave in use (another mod's code)
        used_cave.cave[0x80] = 1;
        REQUIRE(refused(used_cave));
        auto used_ripples = *off_parts; // a word of the ripple routine changed
        used_ripples.ripples[0x40] ^= 1;
        REQUIRE(refused(used_ripples));
        auto changed_dark = *on_parts; // Dark's routine changed: switching off would call it
        changed_dark.dark[0x40] ^= 1;
        REQUIRE(refused(changed_dark));
        // Dark's mode with the ripple built-in off: original words, both places free.
        auto off_bare = *off_parts;
        off_bare.water = {0x09, 0x00, 0x38, 0x1E};
        off_bare.water_mul = {0x08, 0x7D, 0x09, 0x1B};
        off_bare.cave.fill(0);
        off_bare.ripples.fill(0);
        REQUIRE(FileSys::GetOotHighRefreshLayout(off_bare) == OotHighRefreshLayout::Off);
        auto stray_scale = off_bare; // a free cave holds no speed scale word
        stray_scale.cave[Scale + 3] = 0x3F;
        REQUIRE(refused(stray_scale));
        auto no_call = *off_parts; // Dark's routine off in Add-Ons: the game's own fdiv
        no_call.call = {0x28, 0x18, 0x20, 0x1E};
        REQUIRE(refused(no_call));
        auto modded_water = *on_parts; // a mod's word at the water hook
        modded_water.water = {0x1F, 0x20, 0x03, 0xD5};
        REQUIRE(refused(modded_water));
        auto modded_mul = *on_parts; // a mod's word at the water mul
        modded_mul.water_mul = {0x08, 0x7D, 0x09, 0x1B};
        REQUIRE(refused(modded_mul));
        auto off_water_hooked = off_bare; // the hook without its nop or routine: half switched
        off_water_hooked.water = on_parts->water;
        REQUIRE(refused(off_water_hooked));
        auto off_mul_only = off_bare; // the nop without the hook: half switched
        off_mul_only.water_mul = on_parts->water_mul;
        REQUIRE(refused(off_mul_only));
        auto off_routine_only = off_bare; // the ripple routine without its call
        off_routine_only.ripples = on_parts->ripples;
        REQUIRE(refused(off_routine_only));
        auto half_written = off_bare; // the first half of the high refresh cave
        std::copy_n(on_parts->cave.begin(), 0x80, half_written.cave.begin());
        REQUIRE(refused(half_written));
        auto changed_routine = *on_parts;
        changed_routine.cave[0x5C] ^= 1;
        REQUIRE(refused(changed_routine));
        auto changed_ripples = *on_parts;
        changed_ripples.ripples[0x8C] ^= 1;
        REQUIRE(refused(changed_ripples));
    }

    SECTION("Switching off with the ripple built-in off restores the water words, frees both places") {
        const auto bare = FileSys::SwitchOotHighRefreshCode(*on_parts, false, false);
        REQUIRE(bare);
        REQUIRE(bare->water == std::array<u8, 4>{0x09, 0x00, 0x38, 0x1E});
        REQUIRE(bare->water_mul == std::array<u8, 4>{0x08, 0x7D, 0x09, 0x1B});
        REQUIRE(std::ranges::all_of(bare->cave, [](u8 byte) { return byte == 0; }));
        REQUIRE(std::ranges::all_of(bare->ripples, [](u8 byte) { return byte == 0; }));
        REQUIRE(FileSys::GetOotHighRefreshLayout(*bare) == OotHighRefreshLayout::Off);
        // And back on: everything as the high refresh mode boots.
        const auto again = FileSys::SwitchOotHighRefreshCode(*bare, true, false);
        REQUIRE(again);
        REQUIRE(again->water == on_parts->water);
        REQUIRE(again->water_mul == on_parts->water_mul);
        REQUIRE(again->cave == on_parts->cave);
        REQUIRE(again->ripples == on_parts->ripples);
    }

    SECTION("A module too small for the ripple routine holds neither") {
        const auto cut = std::span<const u8>{off}.subspan(Code, OcarinaRipples + 148 - 1);
        REQUIRE_FALSE(FileSys::ReadOotHighRefreshCode(cut));
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(cut));
    }

    SECTION("Its state (Dark's timer, the ripple slots, 0x4E9FE00..0x4E9FEB0) must lie in BSS") {
        REQUIRE(FileSys::OotHighRefreshStateInBss(0x4100000, 0x5100000));
        REQUIRE(FileSys::OotHighRefreshStateInBss(0x4E9FE00, 0x4E9FEB0));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0x4E9FE01, 0x5100000));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0x4100000, 0x4E9FEAF));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0, 0));
    }
}

TEST_CASE("Pickup counter built-in hooks the indicator's cut in both modes", "[patch_manager]") {
    constexpr std::string_view PickupCounter = "Pickup counter at any frame rate (built-in)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Hook = Code + 0xA68300;
    constexpr size_t Stub = Code + 0x3C46B00;
    constexpr u32 Fcvtzs = 0x1E38000C;  // fcvtzs w12, s0
    constexpr u32 HookWord = 0x14C77A00; // b 0x3c46b00
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();
    std::memcpy(nso.data() + Hook, &Fcvtzs, sizeof(Fcvtzs));
    const auto applied = [&](const std::vector<u8>& patched) {
        return word_at(patched, Hook) == HookWord &&
               word_at(patched, Stub + 0x10) == 0x90FE550D && // adrp x13, 0x6e6000
               word_at(patched, Stub + 0x14) == 0xB94CC5AD && // ldr w13, [x13, #0xcc4]
               word_at(patched, Stub + 0x38) == 0x913AC1AD && // add x13, x13, #0xeb0
               word_at(patched, Stub + 0xAC) == 0x173885D6 && // b 0xa68304
               word_at(patched, Stub + 0xB0) == 0;
    };

    SECTION("Dark's mode and the high refresh mode both get it") {
        Settings::values.oot_high_refresh.SetValue(false);
        REQUIRE(applied(context.Manager().PatchNSO(nso, "main")));
        Settings::values.oot_high_refresh.SetValue(true);
        REQUIRE(applied(context.Manager().PatchNSO(nso, "main")));
    }

    SECTION("Another word at the hook, a stub word in use, or off in Add-Ons: skipped whole") {
        SECTION("hook") {
            std::memcpy(nso.data() + Hook, &NopInstruction, sizeof(u32));
        }
        SECTION("stub") {
            nso[Stub + 0x50] = 1;
        }
        SECTION("Add-Ons") {
            Settings::values.disabled_addons[OcarinaTitleId] = {std::string{PickupCounter}};
        }
        const auto patched = context.Manager().PatchNSO(nso, "main");
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        REQUIRE(word_at(patched, Hook) == word_at(nso, Hook));
        REQUIRE(word_at(patched, Stub) == 0);
    }
}

TEST_CASE("High refresh fixes built-in hooks its sites in both modes", "[patch_manager]") {
    constexpr std::string_view Fixes = "High refresh fixes (built-in)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    struct Hook {
        size_t offset;
        u32 original;
        u32 branch;
    };
    // The game's word at each hook and the branch to its stub.
    constexpr std::array Hooks{
        Hook{0x415300, 0xD503237F, 0x14E0C428}, // sound job entry: pacibsp -> b 0x3c463a0
        Hook{0x415334, 0xBD401100, 0x14E0C43F}, // its step load -> b 0x3c46430
        Hook{0x3293410, 0x1E390008, 0x1426CC3C}, // ocarina playback tick -> b 0x3c46500
        Hook{0x2F53268, 0x5EA1B900, 0x1433CCBA}, // step rate countdown -> b 0x3c46550
        // the words that stub skips: checked, kept
        Hook{0x2F5326C, 0x1E380109, 0x1E380109},
        Hook{0x2F53270, 0x5E21D800, 0x5E21D800},
        Hook{0x2F53274, 0x1E203900, 0x1E203900},
        Hook{0x2F53278, 0x1E302000, 0x1E302000},
        Hook{0x2F5327C, 0x540017AA, 0x540017AA},
        Hook{0x2F53280, 0x1E312000, 0x1E312000},
        Hook{0x2F53284, 0x54001809, 0x54001809},
        Hook{0x2F53288, 0xB9409B0A, 0xB9409B0A},
        Hook{0x2F5328C, 0x7100053F, 0x7100053F},
        Hook{0x2F53290, 0x1A9FC529, 0x1A9FC529},
        Hook{0x53F914, 0x1100077A, 0x14DC1B27}, // ambient wave phase -> b 0x3c465b0
        Hook{0x13A06C, 0x1E212000, 0x1E212000}, // fcmp s0, s1 (kept)
        Hook{0x13A070, 0x5400196C, 0x14EC3168}, // ground velocity b.gt -> b 0x3c46610
        Hook{0x13D70C, 0x1E212000, 0x1E212000},
        Hook{0x13D710, 0x54001E2C, 0x14EC23D0}, // -> b 0x3c46650
        Hook{0x389AC28, 0x1E212000, 0x1E212000},
        Hook{0x389AC2C, 0x540017CC, 0x140EAE99}, // -> b 0x3c46690
        Hook{0x261EB18, 0x1E210801, 0x14589EEE}, // turn speed fmul -> b 0x3c466d0
        Hook{0x261EB1C, 0x1E280800, 0x1E280800}, // the second fmul (kept)
        Hook{0x3B5C7C8, 0x11000508, 0x1403A7D6}, // movie frame count -> b 0x3c46720
        Hook{0x3296B08, 0x1E390008, 0x1426BF22}, // Scarecrow's Song, held -> b 0x3c46790
        Hook{0x32926C8, 0x1E390008, 0x1426D046}, // Scarecrow's Song, last note -> b 0x3c467e0
        Hook{0x14632C, 0xBD466D00, 0x14EC0141}, // cloth time step loads -> b 0x3c46830
        Hook{0x2AB804, 0x1E270100, 0x14E66C23}, // -> b 0x3c46890
        Hook{0x2C207A4, 0xBD466D00, 0x14409853}, // -> b 0x3c468f0
        Hook{0x2C2208C, 0xBD466D02, 0x14409231}, // -> b 0x3c46950
        Hook{0x3CDFC0, 0x1E380008, 0x14E1E27C}, // time of day cut -> b 0x3c469b0
        Hook{0x632D00, 0x1E380016, 0x14D84F31}, // Deku Tree web cut -> b 0x3c469c4
        Hook{0x38AEBF4, 0x1E380001, 0x940E5FAE}, // solver steps -> bl 0x3c46aac
        Hook{0x38AFD20, 0x1E380001, 0x940E5B63}, // solver steps -> bl 0x3c46aac
        Hook{0x2C3AE20, 0x54000145, 0x14402F64}, // Epona's jump b.pl -> b 0x3c46bb0
        Hook{0x3292C88, 0x36000060, 0x1426CF56}, // song notes, playback -> b 0x3c469e0
        Hook{0x3292ED4, 0x36000B80, 0x1793B124}, // song notes, the other playback -> b 0x177f364
    };
    // Each stub's first and last word.
    struct Stub {
        size_t offset;
        size_t size;
        u32 first;
        u32 last;
    };
    constexpr std::array Stubs{
        Stub{0x3C463A0, 0x88, 0xA9BF47F0, 0xD65F03C0}, // sound gate: stp x16, x17 ... ret
        Stub{0x3C46430, 0x38, 0xBD401100, 0x176CE450}, // sound feed: ldr s0 ... b 0x177f5a4 (run count)
        Stub{0x3C46470, 0x88, 0xADBF07E0, 0xD65F03C0}, // step ticks: stp q0, q1 ... ret
        Stub{0x3C46500, 0x50, 0xA9BF47F0, 0x17D933B2}, // playback ... b 0x3293414
        Stub{0x3C46550, 0x60, 0xA9BF47F0, 0x17CC3330}, // countdown ... b 0x2f5326c
        Stub{0x3C465B0, 0x60, 0xA9BF47F0, 0x1723E4C3}, // ambient ... b 0x53f918
        Stub{0x3C46610, 0x38, 0x5400004C, 0x1713CF56}, // ground a: b.gt +8 ... b 0x13a39c
        Stub{0x3C46650, 0x38, 0x5400004C, 0x1713DD14}, // ground b ... b 0x13dad4
        Stub{0x3C46690, 0x38, 0x5400004C, 0x17F15218}, // ground c ... b 0x389af24
        Stub{0x3C466D0, 0x48, 0xA9BF47F0, 0x17A76103}, // turn speed ... b 0x261eb20
        Stub{0x3C46720, 0x64, 0xA9BF47F0, 0x17FC5813}, // movie ... b 0x3b5c7cc
        Stub{0x3C46790, 0x50, 0xA9BF47F0, 0x17D940CC}, // Scarecrow held ... b 0x3296b0c
        Stub{0x3C467E0, 0x50, 0xA9BF47F0, 0x17D92FA8}, // Scarecrow last ... b 0x32926cc
        Stub{0x3C46830, 0x5C, 0xA9BF47F0, 0x1713FEAA}, // cloth a ... b 0x146330
        Stub{0x3C46890, 0x5C, 0xA9BF47F0, 0x171993C8}, // cloth b ... b 0x2ab808
        Stub{0x3C468F0, 0x5C, 0xA9BF47F0, 0x17BF6798}, // cloth c ... b 0x2c207a8
        Stub{0x3C46950, 0x5C, 0xA9BF47F0, 0x17BF6DBA}, // cloth d ... b 0x2c22090
        Stub{0x3C469B0, 0x14, 0x9100F26D, 0x171E1D81}, // time of day: add x13 ... b 0x3cdfc4
        Stub{0x3C469C4, 0x14, 0x9123E26D, 0x1727B0CC}, // Deku Tree web: add x13 ... b 0x632d04
        Stub{0x3C469D8, 0x8, 0x3A83126F, 0x3DCCCCCD},  // the solver's constants
        Stub{0x3C469E0, 0x1C, 0xA9BF47F0, 0x17D930A7}, // song notes a ... b 0x3292c94
        Stub{0x3C46AAC, 0x50, 0x1E380001, 0xD65F03C0}, // solver: fcvtzs w1, s0 ... ret
        Stub{0x3C46BB0, 0x44, 0x54000205, 0x17BFD096}, // Epona: b.pl ... b 0x2c3ae48
        Stub{0x3C46E8C, 0x84, 0x1E38000C, 0xD65F03C0}, // whole units: fcvtzs w12, s0 ... ret
        Stub{0x3C46FB4, 0x48, 0xA9BF3FEE, 0xD65F03C0}, // song cue: stp x14, x15 ... ret
        Stub{0x177F364, 0x1C, 0xA9BF47F0, 0x146C4F32}, // song notes b ... b 0x3293044
        Stub{0x177F5A4, 0x1C, 0xA9BF47F0, 0x17B2575F}, // sound job runs ... b 0x415338
    };
    // The hooks a test switch can leave out, one at a time.
    struct Switched {
        const char* env;
        size_t offset;
        u32 original;
    };
    constexpr std::array SwitchedHooks{
        Switched{"FRITCHII_NO_HR_TIME", 0x3CDFC0, 0x1E380008},
        Switched{"FRITCHII_NO_HR_WEB", 0x632D00, 0x1E380016},
        Switched{"FRITCHII_NO_HR_HORSE", 0x2C3AE20, 0x54000145},
    };
    constexpr u32 GateCallWord = 0xB94CC610; // ldr w16, [x16, #0xcc4] (the frame step's call)
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();
    for (const Hook& hook : Hooks) {
        std::memcpy(nso.data() + Code + hook.offset, &hook.original, sizeof(u32));
    }
    const auto applied = [&](const std::vector<u8>& patched) {
        return std::ranges::all_of(Hooks,
                                   [&](const Hook& hook) {
                                       return word_at(patched, Code + hook.offset) == hook.branch;
                                   }) &&
               std::ranges::all_of(Stubs, [&](const Stub& stub) {
                   return word_at(patched, Code + stub.offset) == stub.first &&
                          word_at(patched, Code + stub.offset + stub.size - 4) == stub.last;
               });
    };

    SECTION("Dark's mode and the high refresh mode both get it, gated on the call word") {
        Settings::values.oot_high_refresh.SetValue(false);
        const auto dark = context.Manager().PatchNSO(nso, "main");
        REQUIRE(applied(dark));
        REQUIRE(word_at(dark, Code + Stubs[0].offset + 8) == GateCallWord);
        Settings::values.oot_high_refresh.SetValue(true);
        REQUIRE(applied(context.Manager().PatchNSO(nso, "main")));
    }

    SECTION("A site's test switch leaves only its hook out") {
        for (const Switched& site : SwitchedHooks) {
            SetTestEnv(site.env, "1");
            const auto patched = context.Manager().PatchNSO(nso, "main");
            SetTestEnv(site.env, "");
            REQUIRE(word_at(patched, Code + site.offset) == site.original);
            for (const Hook& hook : Hooks) {
                if (hook.offset != site.offset) {
                    REQUIRE(word_at(patched, Code + hook.offset) == hook.branch);
                }
            }
            for (const Stub& stub : Stubs) {
                REQUIRE(word_at(patched, Code + stub.offset) == stub.first);
            }
        }
    }

    SECTION("Cloth below 60 fps: on by default, its switch puts the two old words back") {
        constexpr std::array ClothStubs{0x3C46830, 0x3C46890, 0x3C468F0, 0x3C46950};
        const auto on = context.Manager().PatchNSO(nso, "main");
        SetTestEnv("FRITCHII_NO_CLOTH_SLOW", "1");
        const auto off = context.Manager().PatchNSO(nso, "main");
        SetTestEnv("FRITCHII_NO_CLOTH_SLOW", "");
        REQUIRE(applied(on));
        REQUIRE(applied(off));
        for (const size_t stub : ClothStubs) {
            REQUIRE(word_at(on, Code + stub + 0x1C) == NopInstruction);
            REQUIRE(word_at(on, Code + stub + 0x34) == 0x72A7B110);  // movk w16, #0x3d88, lsl #16
            REQUIRE(word_at(off, Code + stub + 0x1C) == 0x540001C1); // b.ne +0x38
            REQUIRE(word_at(off, Code + stub + 0x34) == 0x72A79110); // movk w16, #0x3c88, lsl #16
        }
    }

    SECTION("Another word at a hook, a stub word in use, or off in Add-Ons: skipped whole") {
        SECTION("first hook") {
            std::memcpy(nso.data() + Code + Hooks.front().offset, &NopInstruction, sizeof(u32));
        }
        SECTION("last hook") {
            std::memcpy(nso.data() + Code + Hooks.back().offset, &NopInstruction, sizeof(u32));
        }
        SECTION("a word the countdown stub skips") {
            std::memcpy(nso.data() + Code + 0x2F53290, &NopInstruction, sizeof(u32));
        }
        SECTION("stub") {
            nso[Code + Stubs.back().offset + Stubs.back().size - 4] = 1;
        }
        SECTION("Add-Ons") {
            Settings::values.disabled_addons[OcarinaTitleId] = {std::string{Fixes}};
        }
        const auto patched = context.Manager().PatchNSO(nso, "main");
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        for (const Hook& hook : Hooks) {
            REQUIRE(word_at(patched, Code + hook.offset) == word_at(nso, Code + hook.offset));
        }
        REQUIRE(word_at(patched, Code + Stubs.front().offset) == 0);
    }
}

TEST_CASE("Texture heap tidy-up built-in gaps the compaction asks", "[patch_manager]") {
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Hook = Code + 0x777278;
    constexpr size_t Stub = Code + 0x3C46DD0;
    constexpr u32 Tbz = 0x36080088;    // tbz w8, #1, 0x777288
    constexpr u32 Branch = 0x14D33ED6; // b 0x3c46dd0
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();
    std::memcpy(nso.data() + Hook, &Tbz, sizeof(Tbz));

    SECTION("Applied: the hook, the stub from its tbz to its way back") {
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, Hook) == Branch);
        REQUIRE(word_at(patched, Stub) == 0x36080168);        // tbz w8, #1, skip
        REQUIRE(word_at(patched, Stub + 0x2C) == 0x172CC123); // b 0x777288
    }
    SECTION("Its switch, or another word at the hook: the game's ask at every frame") {
        SECTION("switch") {
            SetTestEnv("FRITCHII_NO_TEX_COMPACT_GAP", "1");
        }
        SECTION("hook") {
            std::memcpy(nso.data() + Hook, &NopInstruction, sizeof(u32));
        }
        const auto patched = context.Manager().PatchNSO(nso, "main");
        SetTestEnv("FRITCHII_NO_TEX_COMPACT_GAP", "");
        REQUIRE(word_at(patched, Hook) == word_at(nso, Hook));
        REQUIRE(word_at(patched, Stub) == 0);
    }
}

TEST_CASE("Dark's standalone Dynamic FPS mod is skipped and hidden for the built-in",
          "[patch_manager]") {
    constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr u32 CallOffset = Code + 0x6E6CC4;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};
    constexpr std::array<u8, 4> Nop{0x1F, 0x20, 0x03, 0xD5};
    const auto ips_name = fmt::format("{}.ips", OcarinaBuildIdHex);
    const auto listed = [](const FileSys::PatchManager& manager, std::string_view name) {
        const auto patches = manager.GetPatches();
        return std::ranges::find(patches, name, &FileSys::Patch::name) != patches.end();
    };

    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();
    std::memcpy(nso.data() + PatchOffset, &TbzInstruction, sizeof(TbzInstruction));
    // What boots without mods: Dark's call and Max Resolution's branch.
    const auto builtin = context.Manager().PatchNSO(nso, "main");
    REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), builtin.begin() + CallOffset));
    REQUIRE(WordAt(builtin) == BranchInstruction);

    SECTION("An IPS writing Dark's call: skipped, not listed, the built-in applied") {
        const auto ips = MakeIps(false, CallOffset, Nop);
        const auto path = context.AddModFile("Dynamic FPS", ips_name, ips);
        REQUIRE(context.Manager().PatchNSO(nso, "main") == builtin);
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS"));
        REQUIRE(listed(context.Manager(), DynamicFps));
        // The player's files stay as they were.
        REQUIRE(std::filesystem::file_size(path) == ips.size());
    }

    SECTION("An IPSwitch patch writing into Dark's routine: the same") {
        constexpr std::string_view Pchtxt = "@nsobid-E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348\n"
                                            "@flag offset_shift 0x100\n"
                                            "@enabled\n"
                                            "03C46274 00000000\n"
                                            "@stop\n";
        context.AddModFile("Dynamic FPS 60", "dynamic_fps.pchtxt",
                           {reinterpret_cast<const u8*>(Pchtxt.data()), Pchtxt.size()});
        REQUIRE(context.Manager().PatchNSO(nso, "main") == builtin);
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS 60"));
        REQUIRE(listed(context.Manager(), DynamicFps));
    }

    SECTION("A mod patching an unrelated offset still applies and is listed") {
        constexpr u32 Unrelated = Code + 0x1000;
        context.AddModFile("Other mod", ips_name, MakeIps(true, Unrelated, Nop));
        auto expected = builtin;
        std::ranges::copy(Nop, expected.begin() + Unrelated);
        REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
        REQUIRE(listed(context.Manager(), "Other mod"));
    }

    SECTION("Next to an unrelated mod, only the Dynamic FPS mod is left out") {
        constexpr u32 Unrelated = Code + 0x1000;
        context.AddModFile("Dynamic FPS", ips_name, MakeIps(false, CallOffset, Nop));
        context.AddModFile("Other mod", ips_name, MakeIps(true, Unrelated, Nop));
        auto expected = builtin;
        std::ranges::copy(Nop, expected.begin() + Unrelated);
        REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
        REQUIRE(listed(context.Manager(), "Other mod"));
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS"));
    }

    SECTION("A mod patching Max Resolution's word still wins over that built-in") {
        context.AddModFile("Other mod", ips_name, MakeIps(true, PatchOffset, Nop));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(WordAt(patched) == NopInstruction);
        REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), patched.begin() + CallOffset));
        REQUIRE(listed(context.Manager(), "Other mod"));
    }
}

TEST_CASE("Built-in Texture Pool x2 doubles the allocator's initial pool size",
          "[patch_manager]") {
    constexpr std::string_view TexturePool = "Texture Pool x2 (built-in)";
    constexpr size_t CallOffset = sizeof(Loader::NSOHeader) + 0xC2A36C;
    constexpr size_t RoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46F10; // ours (the original: 0x3C462C0)
    constexpr u32 StpInstruction = 0xA90127E8;
    constexpr u32 CallInstruction = 0x94C072E9; // bl to our cave
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + CallOffset, &StpInstruction, sizeof(StpInstruction));

    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == CallInstruction);
    REQUIRE(Common::HexToString(std::span{patched}.subspan(RoutineOffset, 12)) ==
            "08F97FD3E82701A9C0035FD6");
    REQUIRE(WordAt(patched, RoutineOffset + 12) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{TexturePool}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == StpInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);

    Settings::values.disabled_addons.erase(OcarinaTitleId);
    std::memcpy(nso.data() + CallOffset, &NopInstruction, sizeof(NopInstruction));
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == NopInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);
}
