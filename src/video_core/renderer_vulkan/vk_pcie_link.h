// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

namespace Vulkan {

/// The narrowest PCI Express link between the GPU and the CPU that Windows reports.
struct PcieLink {
    bool known{};       ///< at least one link on the way had values
    u32 mb_per_s{};     ///< usable bandwidth of the narrowest link, one direction, before overhead
    u32 speed_code{};   ///< its speed (1 = 2.5 GT/s ... 4 = 16 GT/s, 5 = 32 GT/s)
    u32 lanes{};        ///< its width
    u32 own_mb_per_s{}; ///< the GPU's own link, which an enclosure or a riser may hide
    u32 links_read{};   ///< devices on the way that reported a link
    u32 devices{};      ///< devices with this vendor and device ID
    bool tunnelled{};   ///< a Thunderbolt or USB4 port is on the way (capped to about 22 Gb/s)
    std::string narrowest; ///< the device at the narrowest link
};

/// Reads the link properties of the GPU's PCI device and of each PCI device above it. It changes
/// nothing. Windows only; elsewhere `known` stays false.
[[nodiscard]] PcieLink ProbePcieLink(u32 vendor_id, u32 device_id);

/// FRITCHII_PCIE_LINK_LOG=1 or FRITCHII_LINK_STATS: one boot line with the result.
void LogPcieLink(u32 vendor_id, u32 device_id);

} // namespace Vulkan
