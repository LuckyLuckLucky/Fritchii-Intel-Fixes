// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "common/settings_enums.h"
#include "vulkan/vulkan_core.h"

static const std::vector<VkPresentModeKHR> default_present_modes{VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                                 VK_PRESENT_MODE_FIFO_KHR};

// Converts a setting to a present mode (or vice versa)
static inline constexpr VkPresentModeKHR VSyncSettingToMode(Settings::VSyncMode mode) {
    switch (mode) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

static inline constexpr Settings::VSyncMode PresentModeToSetting(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return Settings::VSyncMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return Settings::VSyncMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR:
        return Settings::VSyncMode::Fifo;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return Settings::VSyncMode::FifoRelaxed;
    default:
        return Settings::VSyncMode::Fifo;
    }
}

class QWindow;

namespace Settings {
enum class VSyncMode : u32;
}

namespace VkDeviceInfo {
// Short class to record Vulkan driver information for configuration purposes
class Record {
public:
    explicit Record(std::string_view name, const std::vector<VkPresentModeKHR>& vsync_modes,
                    bool has_broken_compute, std::string_view driver_version = {},
                    bool fsr4_capable = false, bool nvidia = true,
                    std::string_view device_name = {},
                    VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER,
                    u64 local_memory = 0, bool amd_proprietary = false);
    ~Record();

    const std::string name;
    const std::vector<VkPresentModeKHR> vsync_support;
    const bool has_broken_compute;
    /// The driver's own version text (e.g. "581.57"), for support logs.
    const std::string driver_version;
    const bool fsr4_capable; ///< VideoCommon::IsFsr4CapableGpu
    const bool nvidia;       ///< an NVIDIA GPU (vendor 0x10DE): the only ones DLSS runs on
    /// The device's own name (no driver), as vulkan_device_name and the log's "Device:" keep it.
    const std::string device_name;
    const VkPhysicalDeviceType device_type;
    const u64 local_memory; ///< Largest device-local heap, in bytes
    const bool amd_proprietary; ///< AMD's own driver (VK_DRIVER_ID_AMD_PROPRIETARY)
};

/// Whether the card can run DLSS: NVIDIA, and not a GeForce or Quadro from before RTX (GTX, MX,
/// GT, Quadro P: Streamline refuses them). Names it doesn't know stay allowed.
[[nodiscard]] bool RunsDlss(const Record& record);

/// The card a game would run on for these saved settings (Vulkan::PickPhysicalDevice).
size_t PickedIndex(const std::vector<Record>& records, u32 saved_index,
                   std::string_view saved_name);

// TODO: Port some configure_graphics.cpp stuff
void PopulateRecords(std::vector<Record>& records, QWindow* window);
} // namespace VkDeviceInfo
