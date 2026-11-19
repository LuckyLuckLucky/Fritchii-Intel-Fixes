// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/settings.h"

/// The picture shown in the window is bigger than the window (a 1440p or 4K picture on a 1080p
/// screen, 8K on a 4K one): set by the display layer each frame, read by the Auto filter.
inline std::atomic<bool> g_present_picture_bigger{false};

/// Auto (Fritchii's default, 2026-11-18): Area when the picture is bigger than the window (an
/// exact average of every pixel a screen pixel covers: no shimmer), ScaleForce otherwise (an
/// upscaler: it reads a few pixels around one spot, which skips pixels when shrinking).
/// Intel fork (2026-11-19): the Area window filter shows a black picture on Intel's Windows
/// driver (Arc B580, 101.9175; sound plays): Fritchii's Auto default picks it whenever the picture
/// is bigger than the window, so a fresh install showed black until another filter was set.
/// Set by the blit screen for that driver (INTEL_FORK_AREA_FILTER=1 keeps Area); Area then reads
/// as Bilinear.
inline std::atomic<bool> g_present_area_broken{false};

static inline Settings::ScalingFilter ResolveScalingFilter(Settings::ScalingFilter filter) {
    if (filter == Settings::ScalingFilter::Auto) {
        filter = g_present_picture_bigger.load(std::memory_order_relaxed)
                     ? Settings::ScalingFilter::Area
                     : Settings::ScalingFilter::ScaleForce;
    }
    if (filter == Settings::ScalingFilter::Area &&
        g_present_area_broken.load(std::memory_order_relaxed)) {
        return Settings::ScalingFilter::Bilinear;
    }
    return filter;
}

static inline Settings::ScalingFilter GetScalingFilter() {
    return ResolveScalingFilter(Settings::values.scaling_filter.GetValue());
}

static inline Settings::AntiAliasing GetAntiAliasing() {
    return Settings::values.anti_aliasing.GetValue();
}

static inline Settings::ScalingFilter GetScalingFilterForAppletCapture() {
    return Settings::ScalingFilter::Bilinear;
}

static inline Settings::AntiAliasing GetAntiAliasingForAppletCapture() {
    return Settings::AntiAliasing::None;
}

struct PresentFilters {
    Settings::ScalingFilter (*get_scaling_filter)();
    Settings::AntiAliasing (*get_anti_aliasing)();
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
};
