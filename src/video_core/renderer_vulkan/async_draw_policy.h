// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>

#include "common/common_types.h"

namespace Vulkan {

/// Whether a draw whose graphics pipeline is still building may be skipped (asynchronous
/// shaders) instead of waiting for the build.
/// @param native         The draw comes from a Switch 2 (native Turing macro) program.
/// @param depth_enabled  The depth test is on for the draw.
/// @param index_count    Index count of the draw.
/// @param vertex_count   Vertex count of the draw.
/// @param discard_native Let native draws follow the heuristic too (on unless EDEN_NX2_ASYNC_DISCARD=0).
/// @param recurring_target Every render target of the draw is cleared and redrawn every frame
///                         (EDEN_NX2_ASYNC_DISCARD=2 asks; the other modes pass true). Native
///                         draws into any other target wait.
/// @param keep_native_without_depth Native draws without depth always wait (Lichii #1296); false
///                         (INTEL_FORK_NO_FONT_ATLAS_KEEP=1) lets them follow the heuristic.
/// @return True when the draw may be skipped.
constexpr bool CanDiscardPendingDraw(bool native, bool depth_enabled, u32 index_count,
                                    u32 vertex_count, bool discard_native = false,
                                    bool recurring_target = true,
                                    bool keep_native_without_depth = true) {
    // Native draws can produce persistent resources, even with depth enabled. They are skipped
    // unless EDEN_NX2_ASYNC_DISCARD=0, trading a possible missing effect for no stall while a
    // new pipeline builds. With EDEN_NX2_ASYNC_DISCARD=2 only those into targets the frame
    // clears and redraws are: a skipped draw into a layer rendered once is missing for good
    // (title screen text, chat bubbles), one into a per-frame target for a frame.
    if (native && (!discard_native || !recurring_target)) {
        return false;
    }
    // Lichii #1296 (Eglantine #785): native draws without depth may build persistent textures
    // (font atlases, 30-672 vertices): skipping one loses its glyphs for the session, or the
    // text for a frame. They always wait.
    if (native && keep_native_without_depth) {
        return depth_enabled;
    }
    // Preserve the legacy async-shader heuristic.
    return depth_enabled || (index_count > 6 && vertex_count > 6);
}

constexpr bool DefersShaderEmission(bool enabled, bool native, size_t pending, size_t capacity) {
    return enabled && native && pending < capacity;
}

// Intel fork (2026-11-22, INTEL_FORK_ASYNC_MODE): Lichii build-386's helpers
// (L:renderer_vulkan/async_draw_policy.h:36-101, zortos293/Lichii tag build-386 = 00f580a94b, GPL),
// copied unchanged, and a classifier that names why Lichii waits for a draw.

/// Whether a draw writes color, and only into render targets first written in an earlier frame,
/// so skipping it leaves last frame's image rather than an unwritten one (Eglantine #874).
/// @param count       Color outputs of the draw (rt_control.count).
/// @param map         Render target index of a color output (rt_control.Map).
/// @param writes      Whether the draw writes color into a render target index.
/// @param has_history Whether a render target index was first written in an earlier frame.
template <typename Map, typename Writes, typename HasHistory>
constexpr bool ColorWritesHaveHistory(u32 count, Map&& map, Writes&& writes,
                                      HasHistory&& has_history) {
    bool writes_any = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 rt_index = map(i);
        if (!writes(rt_index)) {
            continue;
        }
        if (!has_history(rt_index)) {
            return false;
        }
        writes_any = true;
    }
    return writes_any;
}

/// How a native draw with an unbuilt pipeline is handled (the native_async_draws setting).
enum class NativePendingMode { Wait, Bounded, Skip };

/// What to do with a native draw whose pipeline is still building.
enum class NativeDrawAction {
    Wait,      ///< Use the pipeline; the draw waits for the build.
    WaitTimed, ///< Wait for the build up to the remaining budget, then skip if still unbuilt.
    Skip,      ///< Drop the draw.
};

/// @param mode           Wait, Bounded or Skip (EDEN_NX2_ASYNC_DISCARD=1 passes Skip).
/// @param can_discard    The legacy heuristic allows skipping this draw.
/// @param budget_left_us Microseconds left of this frame's wait budget.
constexpr NativeDrawAction DecideNativePending(NativePendingMode mode, bool can_discard,
                                               s64 budget_left_us) {
    // A draw the heuristic keeps must be waited for whatever the mode is.
    if (mode == NativePendingMode::Wait || !can_discard) {
        return NativeDrawAction::Wait;
    }
    if (mode == NativePendingMode::Skip || budget_left_us <= 0) {
        return NativeDrawAction::Skip;
    }
    return NativeDrawAction::WaitTimed;
}

/// Waits for a pending pipeline at most what is left of this frame's wait budget, and takes the
/// time spent from it.
/// @param budget_left_us Microseconds left of this frame's wait budget.
/// @param used_us        Adds the time spent, for the log.
/// @param wait_for       Waits up to the given time for the build; true once it is built.
/// @return True when the pipeline is built.
template <typename WaitFor>
bool WaitWithinBudget(s64& budget_left_us, s64& used_us, WaitFor&& wait_for) {
    const auto begin = std::chrono::steady_clock::now();
    const bool built = wait_for(std::chrono::microseconds{budget_left_us});
    const s64 spent = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - begin)
                          .count();
    budget_left_us -= spent;
    used_us += spent;
    return built;
}

/// INTEL_FORK_ASYNC_MODE: what Lichii build-386 (native_async_draws = Skip) does with a draw whose
/// pipeline is still building, and why it waits. The wait reasons follow the order of Lichii's
/// tests (L:vk_pipeline_cache.cpp:2744-2764).
enum class LichiiDrawClass : u32 {
    Skip,         ///< Lichii skips the draw.
    NoDepthTest,  ///< No depth buffer bound, or the depth test is off.
    Mesh,         ///< A task/mesh draw (Lichii's skip_unbuilt_mesh_draws is off by default).
    NoColour,     ///< Writes no colour (a depth-only pass): no history, so it waits.
    NewTarget,    ///< Writes a colour target first written in this frame, or never.
    Other,        ///< Any other draw Lichii waits for (a non-native draw the legacy rule keeps).
    Unclassified, ///< Statistics only: a held draw that no classified lookup named.
};
inline constexpr size_t LichiiDrawClassCount = 7;

/// Lichii's test for a native draw (L:vk_pipeline_cache.cpp:2744-2764): zeta_enable and
/// depth_test_enable, not a mesh draw, and ColorWritesHaveHistory; the first test that fails is
/// the reason.
template <typename Map, typename Writes, typename HasHistory>
constexpr LichiiDrawClass ClassifyLichiiNativeDraw(bool zeta_enabled, bool depth_test_enabled,
                                                   bool mesh, u32 count, Map&& map,
                                                   Writes&& writes, HasHistory&& has_history) {
    if (!zeta_enabled || !depth_test_enabled) {
        return LichiiDrawClass::NoDepthTest;
    }
    if (mesh) {
        return LichiiDrawClass::Mesh;
    }
    if (ColorWritesHaveHistory(count, map, writes, has_history)) {
        return LichiiDrawClass::Skip;
    }
    // False either because a written target has no history or because nothing is written.
    for (u32 i = 0; i < count; ++i) {
        if (writes(map(i))) {
            return LichiiDrawClass::NewTarget;
        }
    }
    return LichiiDrawClass::NoColour;
}

} // namespace Vulkan
