// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/ocarina_upscale.h"
#include "video_core/fsr.h"

TEST_CASE("OoT temporal backend selection is opt-in and vendor neutral", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    CHECK(SelectOcarinaUpscaler(false, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, "") == Native);
    CHECK(SelectOcarinaUpscaler(true, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, false, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, true, "native") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "dlss") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, true, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, false, false, "garbage") == Native);
}

TEST_CASE("OoT anti-aliasing choice picks the upscaler; DLSS keeps its setting",
          "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    using Settings::OotAntiAliasing;
    // Game and No sharpening: DLSS when its setting is on and it's available, as before.
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Game, false, true, "") == Native);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Game, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Game, true, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::NoSharpening, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::NoSharpening, false, false, "") == Native);
    // An FSR choice wins over the DLSS setting.
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Fsr3, true, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Fsr4, true, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Fsr3, false, false, "") == Fsr);
    // The environment override still decides.
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Fsr3, false, false, "native") == Native);
    CHECK(SelectOcarinaUpscaler(OotAntiAliasing::Fsr4, false, true, "dlss") == Dlss);
}

TEST_CASE("OoT FSR texture LOD bias follows the upscaler, the console mode and failure",
          "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::KeepOcarinaFsrLodBias;
    using VideoCommon::OcarinaFsrLodBias;
    CHECK(OcarinaFsrLodBias(Fsr) == -1.58f);
    CHECK(OcarinaFsrLodBias(Dlss) == 0.0f);
    CHECK(OcarinaFsrLodBias(Native) == 0.0f);
    CHECK(KeepOcarinaFsrLodBias(true, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(false, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, false, true));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, true));
}

TEST_CASE("OoT DLSS jitter: the override wins over the setting", "[video_core][ocarina]") {
    using VideoCommon::OcarinaDlssJitterOn;
    CHECK(OcarinaDlssJitterOn(-1, true));
    CHECK_FALSE(OcarinaDlssJitterOn(-1, false));
    CHECK_FALSE(OcarinaDlssJitterOn(0, true));
    CHECK(OcarinaDlssJitterOn(1, false));
    CHECK(OcarinaDlssJitterOn(2, false));
}

TEST_CASE("OoT texture sharpness: Auto from the sizes, Sharper, Custom", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::OcarinaTextureLodBias;
    using Settings::OotTextureSharpness;
    const auto about = [](float a, float b) { return std::abs(a - b) < 0.01f; };
    // 1440p -> 4K: NVIDIA's log2(render / output), AMD's one lower.
    CHECK(about(OcarinaTextureLodBias(Dlss, 1440, 2160, true, 1, OotTextureSharpness::Auto, 0.0f),
               -0.58f));
    CHECK(about(OcarinaTextureLodBias(Fsr, 1440, 2160, false, 1, OotTextureSharpness::Auto, 0.0f),
               -1.58f));
    // DLAA and 1080p -> 4K.
    CHECK(about(OcarinaTextureLodBias(Dlss, 1440, 1440, true, 1, OotTextureSharpness::Auto, 0.0f),
               0.0f));
    CHECK(about(OcarinaTextureLodBias(Dlss, 1080, 2160, true, 1, OotTextureSharpness::Auto, 0.0f),
               -1.0f));
    // FRITCHII_OOT_LOD_RATIO=2: FSR's formula for DLSS; 0: the fixed numbers.
    CHECK(about(OcarinaTextureLodBias(Dlss, 1440, 2160, true, 2, OotTextureSharpness::Auto, 0.0f),
               -1.58f));
    CHECK(about(OcarinaTextureLodBias(Dlss, 1080, 2160, true, 0, OotTextureSharpness::Auto, 0.0f),
               -0.58f));
    // DLSS without its jitter: no bias, Sharper neither; Custom is the player's value.
    CHECK(OcarinaTextureLodBias(Dlss, 1440, 2160, false, 1, OotTextureSharpness::Auto, 0.0f) ==
          0.0f);
    CHECK(OcarinaTextureLodBias(Dlss, 1440, 2160, false, 1, OotTextureSharpness::Sharper, 0.0f) ==
          0.0f);
    CHECK(OcarinaTextureLodBias(Dlss, 1440, 2160, false, 1, OotTextureSharpness::Custom, -1.75f) ==
          -1.75f);
    CHECK(about(
        OcarinaTextureLodBias(Dlss, 1440, 2160, true, 1, OotTextureSharpness::Sharper, 0.0f),
        -1.08f));
    CHECK(about(
        OcarinaTextureLodBias(Fsr, 1440, 2160, false, 1, OotTextureSharpness::Sharper, 0.0f),
        -2.08f));
    CHECK(OcarinaTextureLodBias(Fsr, 1440, 2160, false, 1, OotTextureSharpness::Custom, -1.52f) ==
          -1.52f);
    // The game's own upscale: nothing, whatever the choice.
    CHECK(OcarinaTextureLodBias(Native, 1440, 2160, true, 1, OotTextureSharpness::Custom, -1.75f) ==
          0.0f);
}

TEST_CASE("FSR temporal sharpness uses the existing stops slider", "[video_core][fsr][ocarina]") {
    CHECK(FSR::RcasSharpnessFromStops(0.0f) == 1.0f);
    CHECK(FSR::RcasSharpnessFromStops(0.81f) == 0.595f);
    CHECK(FSR::RcasSharpnessFromStops(1.0f) == 0.5f);
    CHECK(FSR::RcasSharpnessFromStops(2.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(3.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(-1.0f) == 1.0f);
}

TEST_CASE("OoT scaled upscale picks an output the upscaler can reach", "[video_core][ocarina]") {
    using VideoCommon::OcarinaExtent;
    using VideoCommon::SelectOcarinaScaledOutput;
    // 1x: as before (the target, the 4K output, the render size for native AA).
    auto choice = SelectOcarinaScaledOutput({1440, 810}, {1920, 1080}, false, false);
    CHECK(choice.output == OcarinaExtent{1920, 1080});
    CHECK_FALSE(choice.upscale_4k);
    choice = SelectOcarinaScaledOutput({2560, 1440}, {2560, 1440}, false, true);
    CHECK(choice.output == OcarinaExtent{3840, 2160});
    CHECK(choice.upscale_4k);
    CHECK(SelectOcarinaScaledOutput({1440, 810}, {1920, 1080}, true, false).output ==
          OcarinaExtent{1440, 810});
    // 0.5x: the target, a bigger step up.
    CHECK(SelectOcarinaScaledOutput({720, 405}, {1920, 1080}, false, false).output ==
          OcarinaExtent{1920, 1080});
    // 2x: the scene is larger than the target, so native AA at the scene's size.
    CHECK(SelectOcarinaScaledOutput({2880, 1620}, {1920, 1080}, false, false).output ==
          OcarinaExtent{2880, 1620});
    // 1440p pass: 1.5x still reaches 4K (native AA there), 2x does not.
    choice = SelectOcarinaScaledOutput({3840, 2160}, {2560, 1440}, false, true);
    CHECK(choice.output == OcarinaExtent{3840, 2160});
    CHECK(choice.upscale_4k);
    choice = SelectOcarinaScaledOutput({5120, 2880}, {2560, 1440}, false, true);
    CHECK(choice.output == OcarinaExtent{5120, 2880});
    CHECK_FALSE(choice.upscale_4k);
}

TEST_CASE("OoT FSR output size switch keeps 4K unless asked", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::OcarinaFsrOutputsGameSize;
    STATIC_REQUIRE_FALSE(OcarinaFsrOutputsGameSize(0, Fsr, 1440));
    STATIC_REQUIRE(OcarinaFsrOutputsGameSize(1, Fsr, 0));
    STATIC_REQUIRE(OcarinaFsrOutputsGameSize(1, Fsr, 2160));
    STATIC_REQUIRE(OcarinaFsrOutputsGameSize(2, Fsr, 1440));
    STATIC_REQUIRE(OcarinaFsrOutputsGameSize(2, Fsr, 1080));
    STATIC_REQUIRE_FALSE(OcarinaFsrOutputsGameSize(2, Fsr, 1600));
    STATIC_REQUIRE_FALSE(OcarinaFsrOutputsGameSize(2, Fsr, 0));
    STATIC_REQUIRE_FALSE(OcarinaFsrOutputsGameSize(1, Dlss, 1440));
    STATIC_REQUIRE_FALSE(OcarinaFsrOutputsGameSize(1, Native, 1440));
}

TEST_CASE("OoT upscaler output setting keeps Auto as before", "[video_core][ocarina]") {
    using VideoCommon::OcarinaPassOutput;
    using VideoCommon::PlanOcarinaPassOutput;
    // Auto: 4K from the 1440p pass, the game's 1440p with FRITCHII_FSR_OUTPUT, the target from
    // the 3/4 pass, the scene's size with DLSS Native.
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 1440, false, false) == OcarinaPassOutput{false, 2160});
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 1440, false, true) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, false, 1440, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 1440, true, false) == OcarinaPassOutput{true, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 720, false, false) == OcarinaPassOutput{false, 2160});
}

TEST_CASE("OoT upscaler output setting picks the new sizes", "[video_core][ocarina]") {
    using VideoCommon::OcarinaPassOutput;
    using VideoCommon::PlanOcarinaPassOutput;
    STATIC_REQUIRE(PlanOcarinaPassOutput(1800, true, 1440, false, false) == OcarinaPassOutput{false, 1800});
    STATIC_REQUIRE(PlanOcarinaPassOutput(2880, true, 1440, false, false) == OcarinaPassOutput{false, 2880});
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, true, 1440, false, false) == OcarinaPassOutput{false, 4320});
    // The picked size wins over FRITCHII_FSR_OUTPUT; DLSS Native over the picked size.
    STATIC_REQUIRE(PlanOcarinaPassOutput(2880, true, 1440, false, true) == OcarinaPassOutput{false, 2880});
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, true, 1440, true, false) == OcarinaPassOutput{true, 0});
    // At or below the scene: 1x at the target's size (1440p pass) or the scene's (3/4 pass).
    STATIC_REQUIRE(PlanOcarinaPassOutput(1440, true, 1440, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 1440, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, false, 1440, false, false) == OcarinaPassOutput{true, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1440, false, 1440, false, false) == OcarinaPassOutput{false, 0});
    // Bigger outputs need the 1440p pass; at most 3x the scene a side.
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, false, 1440, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, true, 1080, false, false) == OcarinaPassOutput{false, 2880});
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, true, 720, false, false) == OcarinaPassOutput{false, 2160});
    // A lowered scene (oot_render_height): 720p -> 1080p at its own size, 1080p -> 1080p native
    // AA, 1440p still the game's target; DLSS Native keeps the scene's size.
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 720, false, false) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 1080, false, false) == OcarinaPassOutput{true, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 810, false, true) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1440, true, 720, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 720, true, false) == OcarinaPassOutput{true, 0});
    // Below 720p: Auto is 1080p, and nothing goes over 3x (540p tops out at the 1440p target,
    // 360p at 1080p), with FRITCHII_FSR_OUTPUT too.
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 540, false, false) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(0, true, 360, false, true) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 540, false, false) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1440, true, 540, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(2160, true, 540, false, false) == OcarinaPassOutput{false, 0});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1440, true, 360, false, false) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(4320, true, 360, false, false) == OcarinaPassOutput{false, 0, 1080});
    STATIC_REQUIRE(PlanOcarinaPassOutput(1080, true, 360, true, false) == OcarinaPassOutput{true, 0});
}

TEST_CASE("OoT upscaler output setting checks the card", "[video_core][ocarina]") {
    using VideoCommon::ResolveOcarinaUpscaleOutput;
    constexpr unsigned long long GiB = 1ULL << 30;
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(0, 16 * GiB, false).height == 0);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(1234, 16 * GiB, false).height == 0);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(1800, 4 * GiB, true).height == 1800);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(2160, 4 * GiB, true).height == 2160);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(4320, 16 * GiB, false).height == 4320);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(4320, 12 * GiB, false).height == 4320);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(4320, 8 * GiB, false).height == 2160);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(2880, 8 * GiB, false).height == 2880);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(2880, 4 * GiB, false).height == 2160);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(2880, 16 * GiB, true).height == 2160);
    STATIC_REQUIRE(ResolveOcarinaUpscaleOutput(4320, 0, false).height == 4320);
}

TEST_CASE("OoT scaled upscale takes the picked big output", "[video_core][ocarina]") {
    using VideoCommon::OcarinaExtent;
    using VideoCommon::SelectOcarinaScaledOutput;
    auto choice = SelectOcarinaScaledOutput({2560, 1440}, {2560, 1440}, false, true, {5120, 2880});
    CHECK(choice.output == OcarinaExtent{5120, 2880});
    CHECK(choice.upscale_4k);
    choice = SelectOcarinaScaledOutput({3840, 2160}, {2560, 1440}, false, true, {3200, 1800});
    CHECK(choice.output == OcarinaExtent{3840, 2160});
    CHECK_FALSE(choice.upscale_4k);
}
