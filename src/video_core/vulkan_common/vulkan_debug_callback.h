// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance);

/// FRITCHII_VA_REPORT (boot, default 0; a test tool for device losses with a fault address):
/// 1 = the driver reports every GPU virtual address range it binds or unbinds to a Vulkan object
/// (VK_EXT_device_address_binding_report, through a VK_EXT_debug_utils messenger of its own), kept
/// in a ring; on a device loss each fault address the driver names (VK_EXT_device_fault) is looked
/// up in it: which object type, size and handle held that page, whether it was still bound or
/// already unbound (a free the GPU still used), when, and on which thread.
namespace VaReport {
/// The switch, read once.
[[nodiscard]] bool Requested() noexcept;
/// CreateInstance: VK_EXT_debug_utils was enabled for the report (the device extension needs it).
void NoteInstanceReady() noexcept;
[[nodiscard]] bool InstanceReady() noexcept;
/// Device: the device extension and its feature are on.
void NoteDeviceReady() noexcept;
/// The messenger that fills the ring (an empty handle when the switch is off or the instance has
/// no VK_EXT_debug_utils).
[[nodiscard]] vk::DebugUtilsMessenger CreateMessenger(const vk::Instance& instance);
/// Device loss: logs what the ring knows about the page at `address`.
void LogOwners(u64 address, u64 precision) noexcept;
} // namespace VaReport

} // namespace Vulkan
