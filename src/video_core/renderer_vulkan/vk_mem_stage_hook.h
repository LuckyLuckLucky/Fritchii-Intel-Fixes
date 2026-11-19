// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <source_location>

#include "common/common_types.h"

// The memory stage's watch on the scheduler and the descriptor queues: vk_mem_stage.cpp.
namespace Vulkan::SeamHook {

enum class Use : u8 {
    Record,
    RecordUpload,
    OutsideRenderPass,
    RenderPass,
    Flush,
    Finish,
    DispatchWork,
    InvalidateState,
    DescriptorAdd,
    DescriptorAcquire,
};

inline std::atomic<u32> g_watch{0};

void OnUse(Use use, const std::source_location& location);

/// This thread holds a filled draw or dispatch packet (SEAM = 2/3) whose plays have not run yet:
/// a submit now lands between the fill and its PlayRest (Memory seam 5.5).
[[nodiscard]] bool Holding() noexcept;

inline void Watch(Use use,
                  const std::source_location& location = std::source_location::current()) {
    if (g_watch.load(std::memory_order_relaxed) != 0) [[unlikely]] {
        OnUse(use, location);
    }
}

inline std::atomic<bool> g_tick_watch{false};

void OnTickEnd(u64 tick);

/// The recording thread, right before the scheduler's current tick ends (packet ticks).
inline void TickEnd(u64 tick) {
    if (g_tick_watch.load(std::memory_order_relaxed)) [[unlikely]] {
        OnTickEnd(tick);
    }
}

} // namespace Vulkan::SeamHook
