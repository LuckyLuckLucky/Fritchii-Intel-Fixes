// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include <algorithm>
#include <ranges>
#include <fmt/format.h>
#include "common/lowend_sim.h"
#include "common/nx2_hitch.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/link_stats.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr u64 FENCE_RESERVE_SIZE = 8;

MasterSemaphore::MasterSemaphore(const Device& device_) : device(device_) {
    // Once every member is set, whichever way the constructor ends.
    SCOPE_EXIT {
        Common::Nx2Hitch::SetStallTickReporter(&MasterSemaphore::StallTicks, this);
    };
    if (!device.HasTimelineSemaphore()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        free_queue.resize(FENCE_RESERVE_SIZE);
        std::ranges::generate(free_queue,
                              [&] { return device.GetLogical().CreateFence(fence_ci); });
        wait_thread = std::jthread([this](std::stop_token token) { WaitThread(token); });
        return;
    }

    static constexpr VkSemaphoreTypeCreateInfo semaphore_type_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    static constexpr VkSemaphoreCreateInfo semaphore_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type_ci,
        .flags = 0,
    };
    semaphore = device.GetLogical().CreateSemaphore(semaphore_ci);

    if (!Settings::values.renderer_debug) {
        return;
    }
    // Validation layers have a bug where they fail to track resource usage when using timeline
    // semaphores and synchronizing with GetSemaphoreCounterValue. To workaround this issue, have
    // a separate thread waiting for each timeline semaphore value.
    debug_thread = std::jthread([this](std::stop_token stop_token) {
        u64 counter = 0;
        while (!stop_token.stop_requested()) {
            if (semaphore.Wait(counter, 10'000'000)) {
                ++counter;
            }
        }
    });
}

MasterSemaphore::~MasterSemaphore() {
    Common::Nx2Hitch::ClearStallTickReporter(this);
}

std::string MasterSemaphore::StallTicks(const void* self_) {
    const auto* const self = static_cast<const MasterSemaphore*>(self_);
    // What the card has reported, read now (the known tick only moves when someone refreshes).
    const u64 known = self->gpu_tick.load(std::memory_order_acquire);
    u64 read = 0;
    const u64 reported = self->TryReadGpuTick(read) ? (std::max)(read, known) : known;
    const u64 submitted = self->stall_submitted_tick.load(std::memory_order_acquire);
    const u64 submitted_ns = self->stall_submitted_ns.load(std::memory_order_relaxed);
    const u64 now = Common::Nx2Hitch::NowNs();
    return fmt::format("newest handed out {}, newest submitted {} ({:.1f} ms ago), newest "
                       "reported {} (known {}), {} submitted and not reported",
                       self->CurrentTick() - 1, submitted,
                       submitted_ns != 0 && now > submitted_ns
                           ? static_cast<double>(now - submitted_ns) / 1e6
                           : 0.0,
                       reported, known, submitted > reported ? submitted - reported : 0);
}

bool MasterSemaphore::TryReadGpuTick(u64& tick) const noexcept {
    if (!semaphore) {
        return false;
    }
    try {
        const u64 counter = semaphore.GetCounter();
        if (counter >= CurrentTick()) {
            return false; // no submission signals this: a lost device's value
        }
        tick = counter;
        return true;
    } catch (...) {
        return false;
    }
}

void MasterSemaphore::Refresh() {
    if (!semaphore) {
        // If we don't support timeline semaphores, there's nothing to refresh
        return;
    }

    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        try {
            counter = semaphore.GetCounter();
        } catch (const vk::Exception& exception) {
            if (exception.GetResult() == VK_ERROR_DEVICE_LOST) {
                device.ReportLoss();
            }
            throw;
        }
        // Unchanged: no store, which would take the cache line from the threads reading it.
        if (counter <= this_tick) {
            return;
        }
        // A lost device can report UINT64_MAX with VK_SUCCESS. Past every tick handed out it is
        // no progress: kept out, the known tick stays the last real one, nothing looks finished,
        // and the wait that follows gets the loss.
        if (counter >= CurrentTick()) [[unlikely]] {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void MasterSemaphore::Wait(u64 tick) {
    const Common::LowEndSim::CpuWaitScope sim_wait; // waiting for the GPU is not CPU work
    if (!semaphore) {
        // Fast check: already reached the requested tick?
        if (gpu_tick.load(std::memory_order_acquire) >= tick) {
            return;
        }

        u64 last_tick = gpu_tick.load(std::memory_order_relaxed);
        while (gpu_tick.load(std::memory_order_acquire) < tick) {
            gpu_tick.wait(last_tick, std::memory_order_acquire);
            last_tick = gpu_tick.load(std::memory_order_relaxed);
        }
        return;
    }

    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }

    // Update the GPU tick and try again
    Refresh();

    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const bool link_stats = VideoCommon::LinkStats::On();
    const auto wait_start =
        link_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try {
        while (!semaphore.Wait(tick)) {
        }
    } catch (const vk::Exception& exception) {
        if (exception.GetResult() == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        // The throw ends the process; say why first.
        LOG_CRITICAL(Render_Vulkan, "Waiting for GPU tick {} failed: {}", tick, exception.what());
        throw;
    }
    if (link_stats) [[unlikely]] {
        VideoCommon::LinkStats::NoteWait(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_start)
                .count()));
    }

    Refresh();
}

void MasterSemaphore::RequireSide(u64 value, u64 after_tick, u64 submitted) {
    std::scoped_lock lock{side_mutex};
    side_submitted = (std::max)(side_submitted, submitted);
    side_waits.push_back(SideWait{value, after_tick});
    side_pending.store(side_waits.size(), std::memory_order_release);
}

u64 MasterSemaphore::SideWaitValue(u64 host_tick, VkSemaphore& side_handle) {
    if (side_pending.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    const vk::Semaphore* const side = side_semaphore.load(std::memory_order_acquire);
    if (side == nullptr) {
        return 0;
    }
    side_reported = (std::max)(side_reported, side->GetCounter());
    std::scoped_lock lock{side_mutex};
    std::erase_if(side_waits, [this](const SideWait& wait) { return wait.value <= side_reported; });
    side_pending.store(side_waits.size(), std::memory_order_release);
    u64 value = 0;
    for (const SideWait& wait : side_waits) {
        if (wait.after_tick < host_tick) {
            value = (std::max)(value, wait.value);
        }
    }
    if (value > side_submitted) [[unlikely]] {
        // Must be 0: nothing would ever signal it.
        side_unsubmitted.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR(Render_Vulkan,
                  "FRITCHII_READ_SIDE_COPY: main tick {} would wait for side value {}, newest "
                  "submitted {}: wait dropped (must be 0)",
                  host_tick, value, side_submitted);
        value = side_submitted > side_reported ? side_submitted : 0;
    }
    if (value != 0) {
        side_handle = **side;
        side_main_waits.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

VkResult MasterSemaphore::SubmitQueue(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                                      VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                      u64 host_tick) {
    const VkResult result =
        semaphore ? SubmitQueueTimeline(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore,
                                        host_tick)
                  : SubmitQueueFence(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore,
                                     host_tick);
    if (result == VK_SUCCESS) {
        queue_submitted_tick.store(host_tick, std::memory_order_release);
    }
    if (result == VK_SUCCESS && Common::Nx2Hitch::StallOn()) [[unlikely]] {
        stall_submitted_ns.store(Common::Nx2Hitch::NowNs(), std::memory_order_relaxed);
        stall_submitted_tick.store(host_tick, std::memory_order_release);
    }
    return result;
}

static constexpr VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkResult MasterSemaphore::SubmitQueueTimeline(vk::CommandBuffer& cmdbuf,
                                              vk::CommandBuffer& upload_cmdbuf,
                                              VkSemaphore signal_semaphore,
                                              VkSemaphore wait_semaphore, u64 host_tick) {
    const VkSemaphore timeline_semaphore = *semaphore;

    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        std::array<VkSemaphoreSubmitInfo, 2> signal_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = timeline_semaphore,
                .value = host_tick,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            },
            {},
        }};
        u32 num_signal_semaphores = 1;
        if (signal_semaphore) {
            signal_infos[1] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = signal_semaphore,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            };
            num_signal_semaphores = 2;
        }

        // FRITCHII_READ_SIDE_COPY: a side copy not reported done yet (RequireSide).
        VkSemaphore side_handle = VK_NULL_HANDLE;
        const u64 side_value = SideWaitValue(host_tick, side_handle);
        std::array<VkSemaphoreSubmitInfo, 2> wait_infos{};
        u32 num_wait_semaphores = 0;
        if (wait_semaphore) {
            wait_infos[num_wait_semaphores++] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = wait_semaphore,
                .value = 0,
                .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
                .deviceIndex = 0,
            };
        }
        if (side_value != 0) {
            wait_infos[num_wait_semaphores++] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = side_handle,
                .value = side_value,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            };
        }

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? wait_infos.data() : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = signal_infos.data(),
        };
        return device.GetGraphicsQueue().Submit2(submit_info2);
    }

    const u32 num_signal_semaphores = signal_semaphore ? 2 : 1;
    const std::array signal_values{host_tick, u64(0)};
    const std::array signal_semaphores{timeline_semaphore, signal_semaphore};

    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    // FRITCHII_READ_SIDE_COPY: a side copy not reported done yet (RequireSide).
    VkSemaphore side_handle = VK_NULL_HANDLE;
    const u64 side_value = SideWaitValue(host_tick, side_handle);
    std::array<VkSemaphore, 2> wait_sems{};
    std::array<VkPipelineStageFlags, 2> wait_masks{};
    std::array<u64, 2> wait_values{}; // 0 for the binary one
    u32 num_wait_semaphores = 0;
    if (wait_semaphore) {
        wait_sems[num_wait_semaphores] = wait_semaphore;
        wait_masks[num_wait_semaphores] = wait_stage_mask;
        ++num_wait_semaphores;
    }
    if (side_value != 0) {
        wait_sems[num_wait_semaphores] = side_handle;
        wait_masks[num_wait_semaphores] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        wait_values[num_wait_semaphores] = side_value;
        ++num_wait_semaphores;
    }
    // Pointers must be null when the count is zero (best-practices)
    const VkSemaphore* p_wait_sems =
        (num_wait_semaphores > 0) ? wait_sems.data() : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? wait_masks.data() : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? signal_semaphores.data() : nullptr;
    const VkTimelineSemaphoreSubmitInfo timeline_si{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = num_wait_semaphores,
        .pWaitSemaphoreValues    = num_wait_semaphores ? wait_values.data() : nullptr,
        .signalSemaphoreValueCount = num_signal_semaphores,
        .pSignalSemaphoreValues = signal_values.data(),
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    return device.GetGraphicsQueue().Submit(submit_info);
}

VkResult MasterSemaphore::SubmitQueueFence(vk::CommandBuffer& cmdbuf,
                                           vk::CommandBuffer& upload_cmdbuf,
                                           VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                           u64 host_tick) {
    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo signal_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = signal_semaphore,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = num_signal_semaphores ? &signal_info : nullptr,
        };

        auto fence = GetFreeFence();
        auto result = device.GetGraphicsQueue().Submit2(submit_info2, *fence);

        if (result == VK_SUCCESS) {
            std::scoped_lock lock{wait_mutex};
            wait_queue.emplace(host_tick, std::move(fence));
            wait_cv.notify_one();
        }

        return result;
    }

    const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;

    const VkSemaphore* p_wait_sems =
            (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? &signal_semaphore : nullptr;
    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    auto fence = GetFreeFence();
    auto result = device.GetGraphicsQueue().Submit(submit_info, *fence);

    if (result == VK_SUCCESS) {
        std::scoped_lock lock{wait_mutex};
        wait_queue.emplace(host_tick, std::move(fence));
        wait_cv.notify_one();
    }

    return result;
}

void MasterSemaphore::WaitThread(std::stop_token token) {
    while (!token.stop_requested()) {
        u64 host_tick;
        vk::Fence fence;
        {
            std::unique_lock lock{wait_mutex};
            wait_cv.wait(lock, token, [this] { return !wait_queue.empty(); });
            if (token.stop_requested()) {
                return;
            }
            std::tie(host_tick, fence) = std::move(wait_queue.front());
            wait_queue.pop();
        }

        fence.Wait();
        fence.Reset();

        {
            std::scoped_lock lock{free_mutex};
            free_queue.push_front(std::move(fence));
            gpu_tick.store(host_tick, std::memory_order_release);
        }
        gpu_tick.notify_one();
    }
}

vk::Fence MasterSemaphore::GetFreeFence() {
    std::scoped_lock lock{free_mutex};
    if (free_queue.empty()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        return device.GetLogical().CreateFence(fence_ci);
    }

    auto fence = std::move(free_queue.back());
    free_queue.pop_back();
    return fence;
}

} // namespace Vulkan
