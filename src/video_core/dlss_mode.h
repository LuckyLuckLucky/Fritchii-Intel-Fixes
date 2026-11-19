// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include "common/settings_enums.h"

namespace VideoCommon::DlssMode {

using Quality = Settings::NvidiaDlssQuality;

struct Extent {
    u32 width;
    u32 height;
};

struct Range {
    Extent min;
    Extent max;
};

/// What the Auto preset draws with: K, or the lighter CNN model E where the switch allows it and
/// the card has under 7 GiB of video memory (0: unknown, K). Any other choice stays as it is.
constexpr Settings::DlssPreset ResolvePreset(Settings::DlssPreset chosen, bool low_vram_cnn,
                                             u64 video_memory) {
    if (chosen != Settings::DlssPreset::Auto) {
        return chosen;
    }
    const bool small_card = video_memory != 0 && video_memory < (u64{7} << 30);
    return low_vram_cnn && small_card ? Settings::DlssPreset::E : Settings::DlssPreset::K;
}

constexpr bool Fits(Extent render, Extent output, Quality mode, Range range) {
    return render.width != 0 && render.height != 0 && output.width != 0 && output.height != 0 &&
           render.width >= range.min.width && render.width <= range.max.width &&
           render.height >= range.min.height && render.height <= range.max.height &&
           (mode != Quality::Native ||
            (render.width == output.width && render.height == output.height));
}

constexpr Quality Preferred(Extent render, Extent output) {
    const float ratio =
        output.width ? static_cast<float>(render.width) / static_cast<float>(output.width) : 0.0f;
    if (ratio >= 0.99f) {
        return Quality::Native;
    }
    if (ratio >= 0.65f) {
        return Quality::Quality;
    }
    if (ratio >= 0.57f) {
        return Quality::Balanced;
    }
    if (ratio >= 0.49f) {
        return Quality::Performance;
    }
    return Quality::UltraPerformance;
}

struct Selection {
    std::optional<Quality> mode;
    std::optional<Range> requested_range;
    bool fallback{};
};

template <typename Query>
Selection Select(Quality requested, Extent render, Extent output, Query&& query) {
    std::optional<Range> requested_range;
    if (requested != Quality::Automatic) {
        requested_range = query(requested);
        if (requested_range && Fits(render, output, requested, *requested_range)) {
            return {requested, requested_range, false};
        }
    }
    const auto preferred = Preferred(render, output);
    for (const auto mode : {preferred, Quality::Quality, Quality::Balanced, Quality::Performance,
                            Quality::UltraPerformance, Quality::Native}) {
        if (const auto range = query(mode); range && Fits(render, output, mode, *range)) {
            return {mode, requested_range, requested != Quality::Automatic};
        }
    }
    return {std::nullopt, requested_range, requested != Quality::Automatic};
}

} // namespace VideoCommon::DlssMode
