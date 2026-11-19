// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <span>

#include "common/logging.h"
#include "video_core/host_shaders/sim_gpu_burn_comp_spv.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_sim_gpu_slow.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace {

/// Below this much owed time no dispatch is added. Each dispatch needs two full barriers, whose
/// flush slows the work after them, and that work is then measured and multiplied too: fewer,
/// longer dispatches keep that out of the numbers.
constexpr s64 MinAddNs = 1'000'000;
/// The longest single dispatch: far from the driver's hang timeout, and about one frame.
constexpr s64 MaxAddNs = 16'000'000;
/// Until the loop's speed on this card is measured, dispatches stay short.
constexpr s64 UncalibratedAddNs = 1'000'000;
constexpr u32 CalibrationDispatches = 8;
/// Owed time beyond this is dropped (and counted): the game can't be delayed by a backlog of
/// seconds after a load, which a slower card would never have built up.
constexpr s64 MaxDebtNs = 100'000'000;
constexpr s64 MinDebtNs = -50'000'000;
/// A reading over a second is a broken one, not GPU work.
constexpr double MaxReadingNs = 1e9;

struct PushConstants {
    u32 iterations;
    u32 seed;
};

} // Anonymous namespace

SimGpuSlow::SimGpuSlow() = default;
SimGpuSlow::~SimGpuSlow() = default;

bool SimGpuSlow::Create(const Device& device_) {
    device = &device_;
    const auto physical = device_.GetPhysical();
    const u32 bits =
        physical.GetQueueFamilyProperties()[device_.GetGraphicsFamily()].timestampValidBits;
    period_ns = physical.GetProperties().limits.timestampPeriod;
    if (bits == 0 || bits > 64 || period_ns <= 0) {
        LOG_WARNING(Render_Vulkan, "FRITCHII_SIM_GPU_SLOW: the graphics queue has no timestamps");
        return false;
    }
    mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const auto& logical = device_.GetLogical();
    try {
        pool = logical.CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = Slots * StampsPerSlot,
            .pipelineStatistics = 0,
        });
        allocator = std::make_unique<MemoryAllocator>(device_);
        sink = allocator->CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = 256,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::DeviceLocal);
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        };
        set_layout = logical.CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = 1,
            .pBindings = &binding,
        });
        const VkDescriptorPoolSize pool_size{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
        };
        descriptor_pool = logical.CreateDescriptorPool({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &pool_size,
        });
        descriptor_sets = descriptor_pool.Allocate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = *descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = set_layout.address(),
        });
        const VkDescriptorBufferInfo buffer_info{
            .buffer = *sink,
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_sets[0],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_info,
            .pTexelBufferView = nullptr,
        };
        logical.UpdateDescriptorSets(write, {});
        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
        };
        layout = logical.CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = set_layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_range,
        });
        shader = BuildShader(device_, std::span<const u32>(SIM_GPU_BURN_COMP_SPV));
        pipeline = logical.CreateComputePipeline({
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *shader,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        });
    } catch (const vk::Exception& error) {
        LOG_WARNING(Render_Vulkan, "FRITCHII_SIM_GPU_SLOW unavailable: {}", error.what());
        return false;
    }
    host_reset = device_.IsHostQueryResetSupported() &&
                 device_.GetDispatchLoader().vkResetQueryPool != nullptr;
    if (host_reset) {
        logical.ResetQueryPool(*pool, 0, Slots * StampsPerSlot);
    }
    LOG_INFO(Render_Vulkan,
             "FRITCHII_SIM_GPU_SLOW: GPU timestamps around every main-queue submission; added "
             "load in dispatches of {} to {} ms, as many a frame as it takes",
             MinAddNs / 1'000'000, MaxAddNs / 1'000'000);
    return true;
}

void SimGpuSlow::Poll(MasterSemaphore& master) {
    struct Result {
        u64 value;
        u64 available;
    };
    const int percent = Common::LowEndSim::GpuSlowPercent();
    while (read != next) {
        const u32 slot = static_cast<u32>(read % Slots);
        const Entry& entry = ring[slot];
        if (!master.IsFree(entry.tick)) {
            master.Refresh();
            if (!master.IsFree(entry.tick)) {
                return;
            }
        }
        std::array<Result, StampsPerSlot> stamps{};
        const VkResult result = device->GetLogical().GetQueryResults(
            *pool, slot * StampsPerSlot, StampsPerSlot, sizeof(stamps), stamps.data(),
            sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SIM_GPU_SLOW off: query result {}",
                        static_cast<int>(result));
            failed = true;
            read = next;
            return;
        }
        if (stamps[0].available == 0 || stamps[1].available == 0 || stamps[2].available == 0) {
            return; // done on the timeline but not visible yet: the next submission reads it
        }
        if (host_reset) {
            device->GetLogical().ResetQueryPool(*pool, slot * StampsPerSlot, StampsPerSlot);
        }
        // Submissions run in queue order but may overlap at their edges: each GPU tick counts
        // once.
        u64 start = stamps[0].value;
        if (has_last_end && ((start - last_end) & mask) > mask / 2) {
            start = last_end;
        }
        const u64 work_ticks = (stamps[1].value - start) & mask;
        const u64 added_ticks = (stamps[2].value - stamps[1].value) & mask;
        last_end = stamps[2].value;
        has_last_end = true;
        const double work_ns =
            work_ticks > mask / 2 ? 0.0 : static_cast<double>(work_ticks) * period_ns;
        const double added_ns = static_cast<double>(added_ticks) * period_ns;
        in_flight_ns -= entry.planned_ns;
        ++read;
        if (work_ns >= MaxReadingNs || added_ns >= MaxReadingNs) {
            continue;
        }
        window_work_ns += static_cast<u64>(work_ns);
        window_added_ns += static_cast<u64>(added_ns);
        ++window_submissions;
        window_dispatches += entry.iterations != 0 ? 1U : 0U;
        debt_ns += static_cast<s64>(work_ns * (percent - 100) / 100.0) - static_cast<s64>(added_ns);
        if (debt_ns > MaxDebtNs) {
            window_dropped_ns += static_cast<u64>(debt_ns - MaxDebtNs);
            debt_ns = MaxDebtNs;
        }
        debt_ns = (std::max)(debt_ns, MinDebtNs);
        if (entry.iterations != 0 && added_ns >= 20'000.0) {
            const double measured = added_ns / entry.iterations;
            ns_per_iteration =
                calibrations == 0 ? measured : ns_per_iteration * 0.8 + measured * 0.2;
            ++calibrations;
        }
    }
}

void SimGpuSlow::Report(std::chrono::steady_clock::time_point now, int percent) {
    if (window_start == std::chrono::steady_clock::time_point{}) {
        window_start = now;
        return;
    }
    const auto elapsed = now - window_start;
    if (elapsed < std::chrono::seconds{10}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double work_ms = static_cast<double>(window_work_ns) / 1e6;
    const double added_ms = static_cast<double>(window_added_ns) / 1e6;
    LOG_INFO(Render_Vulkan,
             "Low-end simulation, GPU x{:.2f}: work {:.0f} ms/s, added {:.0f} ms/s, x{:.2f} "
             "reached, GPU busy {:.0f}%; {:.0f} submissions/s, {:.0f} dispatches/s; {:.1f} ns an "
             "iteration, {:.0f} ms owed, {:.0f} ms dropped, {} submissions unmeasured",
             percent / 100.0, work_ms / seconds, added_ms / seconds,
             work_ms > 0.0 ? (work_ms + added_ms) / work_ms : 0.0,
             (work_ms + added_ms) / seconds / 10.0, window_submissions / seconds,
             window_dispatches / seconds, ns_per_iteration,
             static_cast<double>(debt_ns) / 1e6, static_cast<double>(window_dropped_ns) / 1e6,
             window_unstamped);
    window_start = now;
    window_work_ns = 0;
    window_added_ns = 0;
    window_dropped_ns = 0;
    window_unstamped = 0;
    window_submissions = 0;
    window_dispatches = 0;
}

void SimGpuSlow::Begin(const Device& device_, vk::CommandBuffer upload_cmdbuf,
                       MasterSemaphore& master) {
    recording = false;
    if (failed) {
        return;
    }
    if (read != next) {
        Poll(master);
    }
    const int percent = Common::LowEndSim::GpuSlowPercent();
    const bool link = Common::LowEndSim::LinkOn();
    if (percent <= 100 && !link) {
        // Turned off live: nothing is owed, what is in flight is still read back above.
        debt_ns = 0;
        return;
    }
    if (!pool && !Create(device_)) {
        failed = true;
        return;
    }
    if (link) {
        debt_ns += Common::LowEndSim::LinkTakeGpuNs();
        if (debt_ns > MaxDebtNs) {
            window_dropped_ns += static_cast<u64>(debt_ns - MaxDebtNs);
            debt_ns = MaxDebtNs;
        }
    }
    Report(std::chrono::steady_clock::now(), percent);
    if (next - read >= Slots) {
        ++window_unstamped; // the GPU is a whole ring behind: this one goes unmeasured
        return;
    }
    const u32 slot = static_cast<u32>(next % Slots);
    if (!host_reset) {
        upload_cmdbuf.ResetQueryPool(*pool, slot * StampsPerSlot, StampsPerSlot);
    }
    upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, slot * StampsPerSlot);
    recording = true;
}

void SimGpuSlow::End(vk::CommandBuffer cmdbuf, u64 tick) {
    if (!recording) {
        return;
    }
    recording = false;
    const u32 slot = static_cast<u32>(next % Slots);
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, slot * StampsPerSlot + 1);
    Entry entry{.tick = tick};
    const s64 owed = debt_ns - in_flight_ns;
    if (owed >= MinAddNs) {
        const s64 limit = calibrations < CalibrationDispatches ? UncalibratedAddNs : MaxAddNs;
        entry.planned_ns = (std::min)(owed, limit);
        const double iterations = static_cast<double>(entry.planned_ns) / ns_per_iteration;
        entry.iterations = static_cast<u32>(std::clamp(iterations, 1.0, 2'000'000'000.0));
        in_flight_ns += entry.planned_ns;
        // Nothing runs beside the added load: what came before ends first, what follows waits.
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        const VkDescriptorSet set = descriptor_sets[0];
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT,
                             PushConstants{entry.iterations, static_cast<u32>(tick)});
        cmdbuf.Dispatch(1, 1, 1);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, slot * StampsPerSlot + 2);
    ring[slot] = entry;
    ++next;
}

} // namespace Vulkan
