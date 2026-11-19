// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>

#include "common/common_types.h"
#include "common/lowend_sim.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;
class MemoryAllocator;

/// FRITCHII_SIM_GPU_SLOW=<factor> (live, default off; docs/fritchii/switches.md): makes this GPU
/// behave about `factor` times slower. Every main-queue submission gets GPU timestamps around its
/// work; for each millisecond of work the GPU did, (factor - 1) ms of a compute dispatch are added
/// at the end of later submissions, between barriers, so nothing else runs meanwhile. What was
/// added is measured too and the difference carried over, so the total follows the factor however
/// long the dispatch really took. FRITCHII_SIM_LINK's GPU time (bytes the GPU moves over the
/// simulated link) is added the same way. Worker thread only.
class SimGpuSlow {
public:
    SimGpuSlow();
    ~SimGpuSlow();

    /// Off, with nothing in flight: one relaxed load and one compare.
    [[nodiscard]] bool Wanted() const {
        return Common::LowEndSim::GpuSlowOn() || Common::LowEndSim::LinkOn() || read != next;
    }

    /// After the upload command buffer began: reads finished submissions, stamps this one's start.
    void Begin(const Device& device, vk::CommandBuffer upload_cmdbuf, MasterSemaphore& master);
    /// Before the submission's other end stamps, outside any render pass: the end of its work, the
    /// added load, the end of that.
    void End(vk::CommandBuffer cmdbuf, u64 tick);

private:
    static constexpr u32 Slots = 512;
    static constexpr u32 StampsPerSlot = 3;

    struct Entry {
        u64 tick{};
        s64 planned_ns{};
        u32 iterations{};
    };

    bool Create(const Device& device);
    void Poll(MasterSemaphore& master);
    void Report(std::chrono::steady_clock::time_point now, int percent);

    const Device* device{};
    std::unique_ptr<MemoryAllocator> allocator;
    vk::QueryPool pool;
    vk::Buffer sink;
    vk::DescriptorSetLayout set_layout;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSets descriptor_sets;
    vk::PipelineLayout layout;
    vk::ShaderModule shader;
    vk::Pipeline pipeline;
    bool host_reset{};
    bool failed{};
    bool recording{};

    double period_ns{};
    u64 mask{};
    std::array<Entry, Slots> ring{};
    u64 read{0}; ///< oldest submission not read back yet
    u64 next{0}; ///< the submission being stamped
    u64 last_end{};
    bool has_last_end{};

    s64 debt_ns{};        ///< GPU time still to add
    s64 in_flight_ns{};   ///< planned for submissions not read back yet
    double ns_per_iteration{2.0};
    u32 calibrations{};   ///< dispatches measured so far

    std::chrono::steady_clock::time_point window_start{};
    u64 window_work_ns{};
    u64 window_added_ns{};
    u64 window_dropped_ns{};
    u32 window_unstamped{};
    u32 window_submissions{};
    u32 window_dispatches{};
};

} // namespace Vulkan
