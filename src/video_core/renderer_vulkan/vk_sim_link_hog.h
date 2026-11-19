// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stop_token>
#include <thread>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;

/// FRITCHII_SIM_LINK_HOG=<MiB/s> (boot to exist, live rate; docs/fritchii/switches.md): real copies
/// on a transfer queue, 16 MiB at a time, paced to that rate, so the game shares the PCIe link with
/// them. FRITCHII_SIM_LINK_HOG_MODE (boot): 0 host to device memory, 1 device to device memory (the
/// control: same engine and video memory, nothing on the link), 2 device to host memory.
class SimLinkHog {
public:
    SimLinkHog(const Device& device, MemoryAllocator& allocator);
    ~SimLinkHog();

    SimLinkHog(const SimLinkHog&) = delete;
    SimLinkHog& operator=(const SimLinkHog&) = delete;

private:
    void Run(std::stop_token stop);

    const Device& device;
    int mode{};
    vk::Buffer source;
    vk::Buffer destination;
    vk::CommandPool pool;
    vk::CommandBuffers cmdbufs;
    vk::Fence fence;
    std::jthread thread;
};

} // namespace Vulkan
