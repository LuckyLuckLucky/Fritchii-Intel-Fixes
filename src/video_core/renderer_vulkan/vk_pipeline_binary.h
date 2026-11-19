// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// Fritchii's own store of compiled pipelines (VK_KHR_pipeline_binary), so a pipeline built once
// (by Prepare Shaders or in play) is loaded from our file next time instead of compiled again,
// whatever the driver's own cache kept. AMD's Windows driver keeps only part of the ~120k
// prepared pipelines, and compiles the rest again at first use (0.2-1.1 s each); NVIDIA on Linux
// prunes its cache when other programs fill theirs.
//
// FRITCHII_PIPELINE_BINARY (boot, nx2_switches.txt):
//   0 off: the extension isn't enabled, nothing changes (the default, except on AMD's Windows
//     driver where 1 is);
//   1 load and save: pipelines are built from stored binaries, new ones are saved;
//   2 load only: stored binaries are used, nothing new is saved;
//   3 verify: as 1, and every 16th pipeline built from binaries is also compiled the normal way
//     to compare the driver's binary keys (a compile cost, for checking a driver only).
// FRITCHII_PIPELINE_BINARY_MAX_MB (boot): the file stops growing past this many MiB (6144).
// FRITCHII_PIPELINE_BINARY_HARVEST=0 (boot): a pipeline the driver's own cache still holds is
// compiled with capture like any other, instead of being taken from that cache.
// FRITCHII_PIPELINE_BINARY_SHARED_MB (boot, 64): a binary several pipelines name (the same driver
// key; NVIDIA gives every pipeline one 2 MiB binary) is made once and kept for the session, up to
// this many MiB; 0 makes each pipeline's binaries for it alone.
//
// One file per game: <shader dir>/<title id>/pipeline_binaries.fritchii.bin, named by the device
// and driver (vendor, device, driver id, driver version, pipeline cache UUID). A file of another
// device or driver is started over. Each entry is found by the driver's own key of the pipeline's
// create info (vkGetPipelineKeyKHR), so a pipeline whose shaders or state changed simply misses.
// Only an index stays in memory; each entry is read and checked (hash) when its pipeline is built.
namespace Vulkan {

class Device;

namespace PipelineBinary {

/// FRITCHII_PIPELINE_BINARY: 0..3 when set (read once), else the default ChooseDefault picked.
[[nodiscard]] int SwitchValue();

/// The default when FRITCHII_PIPELINE_BINARY isn't set: 1 on AMD's Windows driver, else 0.
/// Called by the device before it decides to enable the extension.
void ChooseDefault(u32 vendor_id);

/// Opens this game's store (LoadDiskResources). Nothing happens unless the device enabled
/// VK_KHR_pipeline_binary (FRITCHII_PIPELINE_BINARY and the driver's support).
void Open(const Device& device, const std::filesystem::path& title_dir);

/// Logs the session's counts, writes what is queued and the index, and closes the store.
void Close(std::string_view when);

/// Added to the preparation stamp: " pipebin-<file id>" while a store that saves is open, so
/// Prepare Shaders runs once more to fill a new (or started over) store. Empty otherwise.
[[nodiscard]] std::string StampSuffix();

/// As vk::Device::CreateGraphicsPipeline / CreateComputePipeline, through the store when open.
[[nodiscard]] vk::Pipeline CreateGraphicsPipeline(const Device& device,
                                                  const VkGraphicsPipelineCreateInfo& ci,
                                                  VkPipelineCache cache);
[[nodiscard]] vk::Pipeline CreateComputePipeline(const Device& device,
                                                 const VkComputePipelineCreateInfo& ci,
                                                 VkPipelineCache cache);

/// Told after each CreateComputePipeline: the stage's module, how long the create took (ms) and
/// whether it made a pipeline. Set once (the pipeline cache's run-time compute counter); kept here
/// so the files the preparation stamp hashes stay as they are.
using ComputeBuildObserver = void (*)(VkShaderModule module, double ms, bool made);
void SetComputeBuildObserver(ComputeBuildObserver observer);

/// One line every 10 s while the counts change (called from the pipeline cache's report).
void ReportPeriodic();

/// One line with the counts so far; `when` names the point of the session.
void LogCounts(std::string_view when);

/// A device loss in a session that built pipelines from the store: the next boot starts it over.
void NoteDeviceLoss();

} // namespace PipelineBinary
} // namespace Vulkan
