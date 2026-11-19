// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#include <thread>
#include <queue>
#include <string>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

class MasterSemaphore {
    using Waitable = std::pair<u64, vk::Fence>;

public:
    explicit MasterSemaphore(const Device& device);
    ~MasterSemaphore();

    /// Returns the current logical tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// Returns the last known GPU tick.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    /// Returns true when a tick has been hit by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    /// Advance to the logical tick and return the old one
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// The card's own count read now, for reports. Never throws and never reports a device loss
    /// (the loss report itself calls it). False when it can't be read or isn't a real tick.
    [[nodiscard]] bool TryReadGpuTick(u64& tick) const noexcept;

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

    /// Submits the device graphics queue, updating the tick as necessary
    VkResult SubmitQueue(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                         VkSemaphore signal_semaphore, VkSemaphore wait_semaphore, u64 host_tick);

    /// The newest tick vkQueueSubmit took (not only handed to the worker: IsSubmitted's sense).
    [[nodiscard]] u64 QueueSubmittedTick() const noexcept {
        return queue_submitted_tick.load(std::memory_order_acquire);
    }

    /// The timeline semaphore (null without timeline semaphores).
    [[nodiscard]] VkSemaphore TimelineHandle() const noexcept {
        return semaphore ? *semaphore : VK_NULL_HANDLE;
    }

    /// FRITCHII_READ_SIDE_COPY >= 2 (vk_side_queue.h): the side queue's timeline semaphore
    /// (null: none), and a side value that every main submission with a tick after
    /// `after_tick` (the main tick the side copy itself waits for) waits for, stage
    /// ALL_COMMANDS, until the side semaphore reports it. Submissions up to `after_tick` never
    /// wait for it: the side copy waits for them (a tick handed to the worker but not given to
    /// the queue yet would deadlock otherwise). Any thread.
    void SetSideSemaphore(const vk::Semaphore* side) noexcept {
        side_semaphore.store(side, std::memory_order_release);
    }
    /// `submitted`: the newest side value submitted (Submit calls this after its vkQueueSubmit).
    void RequireSide(u64 value, u64 after_tick, u64 submitted);

    /// FRITCHII_READ_SIDE_COPY: main submissions that waited for a side value (stage
    /// ALL_COMMANDS), and the must-be-0 one: a wait asked for a side value never submitted, which
    /// would block the main queue forever (dropped, logged). Taken (reset) by the log line.
    struct SideStats {
        u64 waits;
        u64 unsubmitted;
    };
    [[nodiscard]] SideStats TakeSideStats() noexcept {
        return SideStats{
            .waits = side_main_waits.exchange(0, std::memory_order_relaxed),
            .unsubmitted = side_unsubmitted.exchange(0, std::memory_order_relaxed),
        };
    }

private:
    /// EDEN_NX2_STALL_SNAPSHOT: the ticks for the "Stall snapshot:" line (common/nx2_hitch.h).
    static std::string StallTicks(const void* self);

    VkResult SubmitQueueTimeline(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                                 VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                 u64 host_tick);
    VkResult SubmitQueueFence(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                              VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                              u64 host_tick);

    void WaitThread(std::stop_token token);

    vk::Fence GetFreeFence();

private:
    const Device& device;             ///< Device.
    vk::Semaphore semaphore;          ///< Timeline semaphore.
    /// FRITCHII_READ_SIDE_COPY: the side value the next submissions wait for, and the newest
    /// value the side semaphore reported (read on the worker at each submit).
    std::atomic<const vk::Semaphore*> side_semaphore{nullptr};
    struct SideWait {
        u64 value;
        u64 after_tick;
    };
    std::mutex side_mutex;
    std::vector<SideWait> side_waits; ///< not reported done yet, oldest first
    std::atomic<u64> side_pending{0}; ///< side_waits.size(), read without the lock
    u64 side_submitted{0};            ///< the newest side value submitted (`side_mutex`)
    std::atomic<u64> side_main_waits{0};
    std::atomic<u64> queue_submitted_tick{0};
    std::atomic<u64> side_unsubmitted{0};
    u64 side_reported{0};
    /// The side value submission `host_tick` waits for (0: none); the worker's, per submit.
    [[nodiscard]] u64 SideWaitValue(u64 host_tick, VkSemaphore& side_handle);
    /// On their own cache lines: the GPU thread reads current_tick several times a draw, while
    /// threads refreshing gpu_tick would otherwise make each of those reads miss.
    alignas(64) std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    alignas(64) std::atomic<u64> current_tick{1}; ///< Current logical tick.
    alignas(64) std::mutex wait_mutex;
    std::mutex free_mutex;
    std::condition_variable_any wait_cv;
    std::queue<Waitable> wait_queue;  ///< Queue for the fences to be waited on by the wait thread.
    std::deque<vk::Fence> free_queue; ///< Holds available fences for submission.
    std::jthread debug_thread;        ///< Debug thread to workaround validation layer bugs.
    std::jthread wait_thread;         ///< Helper thread that waits for submitted fences.
    /// EDEN_NX2_STALL_SNAPSHOT only: the newest tick vkQueueSubmit took, and when (steady ns).
    std::atomic<u64> stall_submitted_tick{0};
    std::atomic<u64> stall_submitted_ns{0};
};

} // namespace Vulkan
