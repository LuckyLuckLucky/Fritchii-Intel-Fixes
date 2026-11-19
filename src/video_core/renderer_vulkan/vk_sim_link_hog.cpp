// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "common/lowend_sim.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_sim_link_hog.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace {

constexpr VkDeviceSize ChunkBytes = VkDeviceSize{16} << 20;
constexpr double ChunkMiB = 16.0;
constexpr u64 FenceWaitNs = 100'000'000;

[[nodiscard]] const char* ModeName(int mode) {
    switch (mode) {
    case 1:
        return "device to device memory (control: nothing crosses the link)";
    case 2:
        return "device to host memory";
    default:
        return "host to device memory";
    }
}

} // Anonymous namespace

SimLinkHog::SimLinkHog(const Device& device_, MemoryAllocator& allocator) : device{device_} {
    if (const char* const value = std::getenv("FRITCHII_SIM_LINK_HOG_MODE")) {
        mode = std::clamp(std::atoi(value), 0, 2);
    }
    VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = ChunkBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    source = allocator.CreateBuffer(buffer_ci,
                                    mode == 0 ? MemoryUsage::Upload : MemoryUsage::DeviceLocal);
    if (mode == 0) {
        // Every page written once, so the copies read real memory.
        std::memset(source.Mapped().data(), 0x5a, source.Mapped().size());
    }
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    destination = allocator.CreateBuffer(
        buffer_ci, mode == 2 ? MemoryUsage::Download : MemoryUsage::DeviceLocal);

    const auto& logical = device.GetLogical();
    pool = logical.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = device.GetLinkHogFamily(),
    });
    cmdbufs = pool.Allocate(1);
    const vk::CommandBuffer cmdbuf{cmdbufs[0], device.GetDispatchLoader()};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pInheritanceInfo = nullptr,
    });
    cmdbuf.CopyBuffer(*source, *destination,
                      VkBufferCopy{.srcOffset = 0, .dstOffset = 0, .size = ChunkBytes});
    cmdbuf.End();
    fence = logical.CreateFence({
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    });
    LOG_INFO(Render_Vulkan, "FRITCHII_SIM_LINK_HOG: {} MiB/s, {}, queue family {}",
             Common::LowEndSim::LinkHogMibs(), ModeName(mode), device.GetLinkHogFamily());
    thread = std::jthread([this](std::stop_token stop) { Run(stop); });
}

SimLinkHog::~SimLinkHog() {
    thread.request_stop();
    if (thread.joinable()) {
        thread.join();
    }
}

void SimLinkHog::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("LinkHog");
    using Clock = std::chrono::steady_clock;
    const VkCommandBuffer cmdbuf = cmdbufs[0];
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    const vk::Queue queue = device.GetLinkHogQueue();
    auto next = Clock::now();
    auto window_start = next;
    u64 chunks = 0;
    double busy_seconds = 0.0;
    while (!stop.stop_requested()) {
        const int rate = Common::LowEndSim::LinkHogMibs();
        if (rate <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            next = Clock::now();
            window_start = next;
            chunks = 0;
            busy_seconds = 0.0;
            continue;
        }
        const auto start = Clock::now();
        if (const VkResult result = queue.Submit(submit, *fence); result != VK_SUCCESS) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SIM_LINK_HOG off: submit failed ({})",
                        static_cast<int>(result));
            return;
        }
        // In short waits, so a device lost elsewhere or the exit never leaves it stuck for long.
        VkResult waited = VK_TIMEOUT;
        while (waited == VK_TIMEOUT) {
            waited = fence.Wait(FenceWaitNs);
        }
        if (waited != VK_SUCCESS) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SIM_LINK_HOG off: fence wait failed ({})",
                        static_cast<int>(waited));
            return;
        }
        fence.Reset();
        const auto end = Clock::now();
        ++chunks;
        busy_seconds += std::chrono::duration<double>(end - start).count();

        const double window = std::chrono::duration<double>(end - window_start).count();
        if (window >= 10.0) {
            const double mib = static_cast<double>(chunks) * ChunkMiB;
            LOG_INFO(Render_Vulkan,
                     "Link load (FRITCHII_SIM_LINK_HOG={}, {}): {:.0f} MiB/s reached, {:.0f} MiB/s "
                     "while copying, busy {:.0f}% of the time",
                     rate, ModeName(mode), mib / window,
                     busy_seconds > 0.0 ? mib / busy_seconds : 0.0, 100.0 * busy_seconds / window);
            window_start = end;
            chunks = 0;
            busy_seconds = 0.0;
        }
        next += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(ChunkMiB / rate));
        // Far behind (the link can't do the rate): no burst to catch up.
        if (next + std::chrono::milliseconds{100} < end) {
            next = end;
        }
        if (next > end) {
            std::this_thread::sleep_until(next);
        }
    }
}

} // namespace Vulkan
