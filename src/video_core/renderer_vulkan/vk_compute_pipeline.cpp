// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_optimization.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_frame_dump.h"
#include "video_core/renderer_vulkan/vk_pipeline_binary.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/early_compute.h"
#include "video_core/memstage_stats.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_profile.h"
#include "video_core/shader_handle_program.h"
#include "video_core/shader_notify.h"
#include "video_core/texture_handle_values.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"

namespace Vulkan {

using Shader::ImageBufferDescriptor;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_WORDS_OFFSET;
using Tegra::Texture::TexturePair;

namespace {
std::vector<ByteAddressedRange> ByteAddressedMappedRanges(Tegra::MemoryManager& memory,
                                                        GPUVAddr address, u64 size) {
    if (size == 0 || address > (std::numeric_limits<u64>::max)() - size ||
        !memory.IsWithinGPUAddressRange(address + size - 1)) {
        throw Shader::RuntimeError("Unknown packed surface access range {:#x}+{:#x}", address, size);
    }
    std::vector<ByteAddressedRange> ranges;
    while (size != 0) {
        const u64 count{(std::min)(size, 0x1000 - (address & 0xfff))};
        const auto mapped{memory.GpuToCpuAddress(address)};
        if (!mapped || *mapped > (std::numeric_limits<u64>::max)() - count) {
            throw Shader::RuntimeError("Unmapped packed surface access range at {:#x}", address);
        }
        if (!ranges.empty() && ranges.back().address + ranges.back().size == *mapped) {
            ranges.back().size += count;
        } else {
            ranges.push_back({*mapped, count});
        }
        address += count;
        size -= count;
    }
    return ranges;
}

// EDEN_NX2_UPSCALER_TRACE=1 waits for the first native dispatch using subgroup shuffles (software
// HMMA) at or after frame EDEN_NX2_UPSCALER_TRACE_FRAME (default 600), then logs the resources of
// every native compute dispatch in the next EDEN_NX2_UPSCALER_TRACE_FRAMES frames (default 3), at
// most 4000 lines. It prints guest addresses and hashes of constant data, never the data.
struct UpscalerTrace {
    bool enabled{};
    u64 first_frame{};
    u64 frames{};
    u64 armed_frame{};
    bool armed{};
    std::atomic<u64> frame{};
    u32 lines{};
};

UpscalerTrace& GetUpscalerTrace() {
    static UpscalerTrace trace{[] {
        const auto number{[](const char* name, u64 fallback) {
            const char* const value{std::getenv(name)};
            return value ? u64{std::strtoull(value, nullptr, 10)} : fallback;
        }};
        const char* const enabled{std::getenv("EDEN_NX2_UPSCALER_TRACE")};
        return UpscalerTrace{
            .enabled = enabled != nullptr && *enabled != '\0' && *enabled != '0',
            .first_frame = number("EDEN_NX2_UPSCALER_TRACE_FRAME", 600),
            .frames = number("EDEN_NX2_UPSCALER_TRACE_FRAMES", 3),
        };
    }()};
    return trace;
}

u64 HashGuestMemory(Tegra::MemoryManager& memory, GPUVAddr address, u32 size) {
    std::vector<u8> bytes(size);
    memory.ReadBlockUnsafe(address, bytes.data(), bytes.size());
    u64 hash{0xcbf29ce484222325ULL};
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    return hash;
}

void TraceUpscalerDispatch(UpscalerTrace& trace, const Shader::Info& info, u64 shader_hash,
                           Tegra::Engines::KeplerCompute& kepler_compute,
                           Tegra::MemoryManager& gpu_memory, BufferCache& buffer_cache,
                           TextureCache& texture_cache,
                           std::span<const VideoCommon::ImageViewInOut> views) {
    static constexpr u32 LINE_LIMIT = 4000;
    const auto log{[&trace](std::string_view line) {
        if (trace.lines++ < LINE_LIMIT) {
            LOG_INFO(Render_Vulkan, "Upscaler trace {}", line);
        }
    }};
    const auto& qmd{kepler_compute.launch_description};
    const auto& cbuf0{qmd.const_buffer_config[0]};
    const bool has_cbuf0{(qmd.const_buffer_enable_mask & 1) != 0 && cbuf0.size != 0 &&
                         gpu_memory.IsFullyMappedRange(cbuf0.Address(), cbuf0.size)};
    log(fmt::format("f={} prog={:#x} hash={:016x} grid={}x{}x{} block={}x{}x{} cb0={:#x}+{:#x} "
                    "cb0_fnv={:016x}",
                    trace.frame.load(std::memory_order_relaxed),
                    kepler_compute.regs.code_loc.Address(), shader_hash, qmd.grid_dim_x.Value(),
                    qmd.grid_dim_y.Value(), qmd.grid_dim_z.Value(), qmd.block_dim_x.Value(),
                    qmd.block_dim_y.Value(), qmd.block_dim_z.Value(), cbuf0.Address(),
                    cbuf0.size.Value(),
                    has_cbuf0 ? HashGuestMemory(gpu_memory, cbuf0.Address(), cbuf0.size) : 0));

    struct Range {
        GPUVAddr begin;
        GPUVAddr end;
    };
    std::vector<Range> ssbos;
    for (size_t index = 0; index < info.storage_buffers_descriptors.size(); ++index) {
        const auto& desc{info.storage_buffers_descriptors[index]};
        GPUVAddr slot{qmd.const_buffer_config[desc.cbuf_index].Address() + desc.cbuf_offset};
        if (desc.indirect_offset != ~0U) {
            slot = gpu_memory.Read<u64>(slot) + desc.indirect_offset;
        }
        const GPUVAddr address{gpu_memory.Read<u64>(slot)};
        const auto device_address{address ? gpu_memory.GpuToCpuAddress(address) : std::nullopt};
        u32 size{};
        if (device_address) {
            const u32 layout_size{static_cast<u32>(gpu_memory.GetMemoryLayoutSize(address))};
            const u64 next{gpu_memory.Read<u64>(slot + 8)};
            const bool next_is_size{(next >> 32) == 0 && static_cast<u32>(next) != 0 &&
                                    static_cast<u32>(next) <= layout_size};
            size = next_is_size ? static_cast<u32>(next) : (std::min)(layout_size, 8U << 20);
        }
        ssbos.push_back({address, address + size});
        log(fmt::format("  ssbo{} cb{}+{:#x} {} va={:#x} size={:#x}{} image_gpu_dirty={} "
                        "buffer_gpu_dirty={}",
                        index, desc.cbuf_index, desc.cbuf_offset, desc.is_written ? "w" : "r",
                        address, size, device_address ? "" : " null",
                        device_address && texture_cache.IsRegionGpuModified(*device_address, size),
                        device_address && buffer_cache.IsRegionGpuModified(*device_address, size)));
    }

    size_t view_index{};
    const auto log_views{[&](const auto& descriptors, std::string_view kind) {
        for (const auto& desc : descriptors) {
            for (u32 element = 0; element < desc.count; ++element, ++view_index) {
                const auto& view{texture_cache.GetImageView(views[view_index].id)};
                const auto* const image{view.BackingImage()};
                std::string aliases;
                if (image) {
                    const GPUVAddr end{image->gpu_addr + image->guest_size_bytes};
                    for (size_t ssbo = 0; ssbo < ssbos.size(); ++ssbo) {
                        if (ssbos[ssbo].begin < end && image->gpu_addr < ssbos[ssbo].end) {
                            aliases += fmt::format(" alias=ssbo{}", ssbo);
                        }
                    }
                }
                log(fmt::format("  {}{} cb{}+{:#x} tic={} {} va={:#x} size={:#x} {}x{} fmt={}{}",
                                kind, view_index, desc.cbuf_index, desc.cbuf_offset,
                                views[view_index].index, image ? "" : "null",
                                image ? image->gpu_addr : 0, image ? image->guest_size_bytes : 0,
                                view.size.width, view.size.height,
                                static_cast<u32>(view.format), aliases));
            }
        }
    }};
    log_views(info.texture_buffer_descriptors, "texbuf");
    log_views(info.image_buffer_descriptors, "imgbuf");
    log_views(info.texture_descriptors, "tex");
    log_views(info.image_descriptors, "img");
}

// FRITCHII_COMPUTE_FAST_FIRST's counts, logged every 10 s (ComputePipeline::ReportFastFirst).
struct FastFirstWindow {
    u64 served{};          ///< unoptimized builds that became the pipeline (dispatches used them)
    double served_ms{};
    u64 beaten{};          ///< unoptimized builds the optimized one beat (never bound, freed)
    u64 failed{};          ///< unoptimized builds that made nothing (the optimized one served)
    u64 optimized{};       ///< optimized builds of programs an unoptimized one served
    u64 optimized_failed{};
    double paired_fast_ms{};
    double paired_optimized_ms{};
    u64 swaps{};

    [[nodiscard]] bool Empty() const noexcept {
        return served == 0 && beaten == 0 && failed == 0 && optimized == 0 &&
               optimized_failed == 0 && swaps == 0;
    }
};

/// A program an unoptimized build serves, until its optimized one is swapped in.
struct FastFirstOpen {
    const void* pipeline;
    u64 hash;
    std::chrono::steady_clock::time_point at;
    bool optimized_done;
    bool warned;
};

struct FastFirstState {
    std::mutex mutex;
    FastFirstWindow window;
    std::vector<FastFirstOpen> open;
    u64 stuck_total{};
    u32 lines{};
};

/// Never destroyed: pipelines may be freed after static destruction began.
FastFirstState& GetFastFirst() {
    static FastFirstState* const state = new FastFirstState;
    return *state;
}

/// Anything to report (ReportFastFirst returns at once while false).
std::atomic_bool g_fast_first_seen{false};
constexpr u32 FastFirstLineLimit = 64;

void EraseOpenLocked(FastFirstState& state, const void* pipeline) {
    std::erase_if(state.open,
                  [pipeline](const FastFirstOpen& open) { return open.pipeline == pipeline; });
}

void ForgetFastFirst(const void* pipeline) {
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    EraseOpenLocked(state, pipeline);
}

void NoteFastBuild(const void* pipeline, u64 hash, bool served, double compile_ms,
                   bool& fast_open) {
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    // Under the lock: ReportFastFirst clears it under the lock when nothing is left.
    g_fast_first_seen.store(true, std::memory_order_relaxed);
    if (!served) {
        ++state.window.beaten;
        return;
    }
    ++state.window.served;
    state.window.served_ms += compile_ms;
    state.open.push_back({pipeline, hash, std::chrono::steady_clock::now(), false, false});
    fast_open = true;
    if (state.lines < FastFirstLineLimit) {
        ++state.lines;
        LOG_INFO(Render_Vulkan,
                 "Compute fast-first: pipeline {:016x} built unoptimized in {:.0f} ms "
                 "(FRITCHII_COMPUTE_FAST_FIRST={}); its optimized build follows on its worker",
                 hash, compile_ms, ComputePipeline::FastFirstMode());
    }
}

void NoteFastFailed() {
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    g_fast_first_seen.store(true, std::memory_order_relaxed);
    ++state.window.failed;
}

void NoteOptimizedAfterFast(const void* pipeline, u64 hash, bool ok, double fast_ms,
                            double optimized_ms, bool& fast_open) {
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    if (!ok) {
        ++state.window.optimized_failed;
        EraseOpenLocked(state, pipeline);
        fast_open = false;
        LOG_WARNING(Render_Vulkan,
                    "Compute fast-first: the optimized build of {:016x} failed, it keeps its "
                    "unoptimized pipeline (same results, more GPU time)",
                    hash);
        return;
    }
    ++state.window.optimized;
    state.window.paired_fast_ms += fast_ms;
    state.window.paired_optimized_ms += optimized_ms;
    for (FastFirstOpen& open : state.open) {
        if (open.pipeline == pipeline) {
            open.optimized_done = true;
        }
    }
    if (state.lines < FastFirstLineLimit) {
        ++state.lines;
        LOG_INFO(Render_Vulkan,
                 "Compute fast-first: pipeline {:016x} optimized in {:.0f} ms (unoptimized: {:.0f} "
                 "ms), swapped in at its next dispatch",
                 hash, optimized_ms, fast_ms);
    }
}

void NoteFastSwap(const void* pipeline, bool& fast_open) {
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    ++state.window.swaps;
    EraseOpenLocked(state, pipeline);
    fast_open = false;
}
} // namespace

void ComputePipeline::ReportFastFirst() {
    if (!g_fast_first_seen.load(std::memory_order_relaxed)) {
        return;
    }
    // GPU thread only.
    static std::chrono::steady_clock::time_point next_report{};
    const auto now = std::chrono::steady_clock::now();
    if (next_report == std::chrono::steady_clock::time_point{}) {
        next_report = now + std::chrono::seconds{10};
        return;
    }
    if (now < next_report) {
        return;
    }
    next_report = now + std::chrono::seconds{10};
    auto& state = GetFastFirst();
    std::scoped_lock lock{state.mutex};
    u64 waiting = 0;
    u64 unswapped = 0;
    u64 stuck = 0;
    for (FastFirstOpen& open : state.open) {
        if (open.optimized_done) {
            ++unswapped; // built, not dispatched since
            continue;
        }
        ++waiting;
        if (now - open.at < std::chrono::seconds{30}) {
            continue;
        }
        ++stuck;
        if (!open.warned) {
            open.warned = true;
            ++state.stuck_total;
            LOG_WARNING(Render_Vulkan,
                        "Compute fast-first: pipeline {:016x} still runs unoptimized {:.0f} s "
                        "after its unoptimized build, its optimized build has not ended (must "
                        "be 0)",
                        open.hash, std::chrono::duration<double>(now - open.at).count());
        }
    }
    const FastFirstWindow window = std::exchange(state.window, FastFirstWindow{});
    if (window.Empty() && stuck == 0) {
        if (state.open.empty()) {
            g_fast_first_seen.store(false, std::memory_order_relaxed);
            next_report = {};
        }
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Compute fast-first (FRITCHII_COMPUTE_FAST_FIRST={}) in the last 10 s: {} unoptimized "
             "builds served dispatches ({:.0f} ms, {:.0f} ms each), {} beaten by the optimized "
             "build, {} failed; {} optimized builds followed ({} failed), those programs {:.0f} ms "
             "unoptimized vs {:.0f} ms optimized; {} swapped in; now {} waiting for their "
             "optimized build, {} built and not dispatched since; must be 0: {} programs "
             "unoptimized over 30 s ({} since boot)",
             FastFirstMode(), window.served, window.served_ms,
             window.served != 0 ? window.served_ms / static_cast<double>(window.served) : 0.0,
             window.beaten, window.failed, window.optimized, window.optimized_failed,
             window.paired_fast_ms, window.paired_optimized_ms, window.swaps, waiting, unswapped,
             stuck, state.stuck_total);
}

void ComputePipeline::TickUpscalerTrace() {
    auto& trace{GetUpscalerTrace()};
    if (trace.enabled) {
        if (trace.frame.fetch_add(1, std::memory_order_relaxed) == 0) {
            LOG_INFO(Render_Vulkan, "Upscaler trace enabled, arming from frame {}",
                     trace.first_frame);
        }
    }
}

ComputePipeline::ComputePipeline(const Device& device_, Scheduler& scheduler, vk::PipelineCache& pipeline_cache_,
                                 DescriptorPool& descriptor_pool,
                                 GuestDescriptorQueue& guest_descriptor_queue_,
                                 DescriptorBufferRing& descriptor_buffer_ring_,
                                 Common::ThreadWorker* thread_worker,
                                 PipelineStatistics* pipeline_statistics,
                                 VideoCore::ShaderNotify* shader_notify, const Shader::Info& info_,
                                 vk::ShaderModule spv_module_, u64 shader_hash_,
                                 std::function<vk::ShaderModule()> make_module)
    : device{device_}, owner_scheduler{scheduler}, build_notify{shader_notify},
      pipeline_cache(pipeline_cache_), guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_}, info{info_},
      shader_hash{shader_hash_}, spv_module(std::move(spv_module_)),
      make_module_fn(std::move(make_module)) {
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    std::copy_n(info.constant_buffer_used_sizes.begin(), uniform_buffer_sizes.size(),
                uniform_buffer_sizes.begin());
    num_descriptor_entries = NumDescriptorEntries(info);

    DescriptorLayoutBuilder builder{device};
    builder.Add(info, VK_SHADER_STAGE_COMPUTE_BIT);

    uses_push_descriptor = builder.CanUsePushDescriptor();
    uses_descriptor_buffer = builder.CanUseDescriptorBuffer() && descriptor_buffer_ring.IsValid();
    descriptor_set_layout =
        builder.CreateDescriptorSetLayout(uses_push_descriptor, uses_descriptor_buffer);
    if (uses_descriptor_buffer) {
        descriptor_buffer_layout = builder.MakeDescriptorBufferLayout(*descriptor_set_layout);
        if (!descriptor_buffer_ring.CanAllocate(descriptor_buffer_layout.size)) {
            LOG_DEBUG(Render_Vulkan,
                        "Compute shader {:016X} needs {} descriptor bytes per dispatch, falling "
                        "back to sets",
                        shader_hash, descriptor_buffer_layout.size);
            uses_descriptor_buffer = false;
            descriptor_buffer_layout = {};
            descriptor_set_layout = builder.CreateDescriptorSetLayout(false);
        }
    }
    pipeline_layout = builder.CreatePipelineLayout(*descriptor_set_layout);
    if (!uses_descriptor_buffer) {
        descriptor_update_template =
            builder.CreateTemplate(*descriptor_set_layout, *pipeline_layout, uses_push_descriptor);
        if (!uses_push_descriptor) {
            descriptor_allocator =
                descriptor_pool.Allocator(device, scheduler, *descriptor_set_layout, info);
        }
    }

    // FRITCHII_COMPUTE_FAST_FIRST: only builds queued at run time. The boot, warm-up and
    // preparation builds run here on their own threads, and no dispatch waits for them. The
    // programs always built unoptimized (BuildsUnoptimized) gain nothing from it.
    fast_first_allowed = thread_worker != nullptr &&
                         !BuildsUnoptimized(device.GetDriverID(), shader_hash);
    auto func{[this, pipeline_statistics] {
        if (!AcquireModule()) {
            // No module (a precompiled one that could not be made): the build ends with no
            // pipeline (BuildFailed), and the pipeline cache replaces it by a translated one at
            // the next dispatch.
            Publish(vk::Pipeline{}, true, 0.0);
            return;
        }
        const auto begin = std::chrono::steady_clock::now();
        vk::Pipeline made;
        try {
            // Through Fritchii's pipeline binary store when it is on (FRITCHII_PIPELINE_BINARY).
            made = CreateHandle(false);
        } catch (const vk::Exception& exception) {
            LOG_CRITICAL(Render_Vulkan, "Adreno rejected compute shader {:016X}: {}", shader_hash,
                         exception.what());
        }
        const double compile_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        if (made) {
            // Log compute pipeline creation
            if (GPU::Logging::IsActive()) {
                GPU::Logging::GPULogger::GetInstance().LogPipelineStateChange(
                    "ComputePipeline created"
                );
            }
            if (pipeline_statistics) {
                pipeline_statistics->Collect(device, *made);
            }
        }
        // Nothing reads the module once the pipeline exists; the driver keeps its SPIR-V while it
        // lives. An unoptimized build still compiling from it lets go of it last.
        ReleaseModule(true);
        Publish(std::move(made), true, compile_ms);
    }};
    build_task = std::move(func);
    if (thread_worker) {
        build_task_done = false;
        thread_worker->QueueWork([this] {
            RunBuild();
            // The last access to this pipeline from the worker.
            build_task_done.store(true, std::memory_order::release);
        });
    } else {
        RunBuild();
    }
}

ComputePipeline::~ComputePipeline() {
    if (fast_open) {
        ForgetFastFirst(this);
    }
}

vk::Pipeline ComputePipeline::CreateHandle(bool unoptimized) {
    const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .requiredSubgroupSize = GuestWarpSize,
    };
    // Only warp-level ops observe the subgroup size. Forcing 32 on other shaders makes Intel
    // compile SIMD32, which can spill a register-heavy kernel for minutes (Mario Kart World).
    // A shader whose divergent barrier lost its execution barrier (ConditionalBarrierPass) relies
    // on a whole warp running in step: at a smaller subgroup size it reads stale shared memory,
    // which shows as banding over the image in Mario Kart World on Intel's Windows driver.
    const bool uses_warp_ops = info.demoted_conditional_barrier ||
                               info.uses_subgroup_vote || info.uses_subgroup_invocation_id ||
                               info.uses_subgroup_shuffles || info.uses_subgroup_mask ||
                               info.uses_quad_shuffles || info.uses_fswzadd ||
                               info.uses_cooperative_matrix;
    // Shaders without them may be given a narrower size where the driver's choice spills
    // (Intel, Device::NarrowComputeSubgroupSize).
    const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT narrow_subgroup_size_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .requiredSubgroupSize = uses_warp_ops
                                    ? 0
                                    : device.NarrowComputeSubgroupSize(info.uses_global_memory),
    };
    const void* const stage_next =
        device.IsExtSubgroupSizeControlSupported() && uses_warp_ops &&
                device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT)
            ? static_cast<const void*>(&subgroup_size_ci)
        : narrow_subgroup_size_ci.requiredSubgroupSize != 0
            ? static_cast<const void*>(&narrow_subgroup_size_ci)
            : nullptr;
    VkPipelineCreateFlags flags{};
    if (!unoptimized && device.IsKhrPipelineExecutablePropertiesEnabled() &&
        Settings::values.renderer_debug.GetValue()) {
        flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    }
    if (uses_descriptor_buffer) {
        flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    if (unoptimized || BuildsUnoptimized(device.GetDriverID(), shader_hash)) {
        flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    }
    const VkComputePipelineCreateInfo compute_ci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = flags,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = stage_next,
            .flags = info.uses_cooperative_matrix
                         ? VkPipelineShaderStageCreateFlags{
                               VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT}
                         : VkPipelineShaderStageCreateFlags{},
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *spv_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *pipeline_layout,
        .basePipelineHandle = 0,
        .basePipelineIndex = 0,
    };
    if (unoptimized) {
        // FRITCHII_COMPUTE_FAST_FIRST: never stored. Not through the binary store or a pipeline
        // cache, which keep the optimized build of this program for the next session.
        return device.GetLogical().CreateComputePipeline(compute_ci);
    }
    return PipelineBinary::CreateComputePipeline(device, compute_ci, *pipeline_cache);
}

bool ComputePipeline::AcquireModule() {
    std::scoped_lock lock{module_mutex};
    if (!spv_module && make_module_fn && !module_release_due) {
        // A precompiled pipeline's module, from the SPIR-V its preparation kept: made by the
        // first build that needs it (its worker, or a dispatch's unoptimized build).
        auto make = std::move(make_module_fn);
        make_module_fn = nullptr;
        try {
            spv_module = make();
        } catch (const std::exception& exception) {
            LOG_ERROR(Render_Vulkan, "Precompiled compute shader {:016X}: {}", shader_hash,
                      exception.what());
        }
    }
    if (!spv_module) {
        return false;
    }
    ++module_users;
    return true;
}

void ComputePipeline::ReleaseModule(bool optimized_done) {
    std::scoped_lock lock{module_mutex};
    --module_users;
    module_release_due = module_release_due || optimized_done;
    if (module_release_due && module_users == 0) {
        spv_module.reset();
    }
}

bool ComputePipeline::Publish(vk::Pipeline made, bool optimized, double compile_ms) {
    const bool made_ok = static_cast<bool>(made);
    bool first = false;
    bool after_fast = false;
    double served_fast_ms = 0.0;
    {
        std::scoped_lock lock{build_mutex};
        if (!is_built.load(std::memory_order::relaxed)) {
            // Nothing binds `pipeline` before is_built: Configure's build wait precedes the bind.
            pipeline = std::move(made);
            has_pipeline.store(made_ok, std::memory_order::release);
            running_optimized = optimized;
            if (!optimized) {
                fast_ms = compile_ms;
            }
            built_at.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                           std::memory_order::release);
            is_built.store(true, std::memory_order::release);
            // All waiters: the scheduler's wait in Configure and FRITCHII_ASYNC_COMPUTE_SKIP's
            // bounded wait on the GPU thread can wait at once.
            build_condvar.notify_all();
            first = true;
        } else if (optimized && !running_optimized) {
            // An unoptimized build serves: this one is swapped in by the scheduler worker
            // before its next bind (SwapInOptimized).
            after_fast = true;
            served_fast_ms = fast_ms;
            if (made_ok) {
                optimized_pipeline = std::move(made);
                optimized_ready.store(true, std::memory_order::release);
            }
        }
        // Otherwise `made` lost the race (an unoptimized build after the optimized one): it was
        // never bound, and is destroyed when this returns.
    }
    if (first && build_notify) {
        build_notify->MarkShaderComplete();
    }
    if (!optimized) {
        NoteFastBuild(this, shader_hash, first, compile_ms, fast_open);
    } else if (after_fast) {
        NoteOptimizedAfterFast(this, shader_hash, made_ok, served_fast_ms, compile_ms, fast_open);
    }
    return first;
}

void ComputePipeline::SwapInOptimized() {
    if (retired_fast && owner_scheduler.IsFree(retired_tick)) {
        retired_fast.reset();
    }
    if (!optimized_ready.load(std::memory_order::acquire)) {
        return;
    }
    {
        std::scoped_lock lock{build_mutex};
        // The unoptimized pipeline may be in command buffers the GPU has not finished: kept
        // until it passed the recording thread's tick now, which is at or after the tick of the
        // command buffer this worker records (one swap per pipeline).
        retired_fast = std::move(pipeline);
        retired_tick = owner_scheduler.CurrentTick();
        pipeline = std::move(optimized_pipeline);
        running_optimized = true;
        optimized_ready.store(false, std::memory_order::release);
    }
    NoteFastSwap(this, fast_open);
}

int ComputePipeline::FastFirstMode() {
    const int value = VideoCore::Nx2Debug::SwitchValue(
        VideoCore::Nx2Debug::g_compute_fast_first, "FRITCHII_COMPUTE_FAST_FIRST", 0);
    return value == 1 || value == 2 ? value : 0;
}

bool ComputePipeline::UsesFastFirst() const {
    return fast_first_allowed && FastFirstMode() != 0;
}

void ComputePipeline::RunBuild() {
    // A draw or dispatch that needs the pipeline builds it on the scheduler thread instead of
    // waiting for the worker queue, which can hold hundreds of builds, to reach it.
    if (build_claimed.exchange(true)) {
        return;
    }
    // FRITCHII_COMPUTE_FAST_FIRST=2 (test): the worker builds the unoptimized pipeline first, so
    // every program built at run time is served by it until the optimized one is swapped in.
    if (fast_first_allowed && FastFirstMode() == 2 &&
        !fast_claimed.exchange(true, std::memory_order::acq_rel)) {
        static_cast<void>(RunFastBuild());
    }
    build_task();
    build_task = nullptr;
}

void ComputePipeline::RunNeededBuild() {
    if (is_built.load(std::memory_order::acquire)) {
        return;
    }
    if (UsesFastFirst()) {
        if (fast_claimed.exchange(true, std::memory_order::acq_rel)) {
            // Another thread's unoptimized build: the caller waits for it (that thread falls
            // back to the optimized build itself when it makes nothing).
            return;
        }
        // The optimized build stays with its worker (queued, or compiling there now).
        if (RunFastBuild() || is_built.load(std::memory_order::acquire)) {
            return;
        }
        // It made nothing: the optimized build here, as without the switch.
    }
    RunBuild();
}

bool ComputePipeline::RunFastBuild() {
    if (is_built.load(std::memory_order::acquire) || !AcquireModule()) {
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    vk::Pipeline made;
    try {
        made = CreateHandle(true);
    } catch (const vk::Exception& exception) {
        LOG_WARNING(Render_Vulkan, "Compute fast-first: the unoptimized build of {:016x} failed: {}",
                    shader_hash, exception.what());
    }
    const double compile_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
            .count();
    ReleaseModule(false);
    if (!made) {
        NoteFastFailed();
        return false;
    }
    static_cast<void>(Publish(std::move(made), false, compile_ms));
    return true;
}

bool ComputePipeline::WaitBuiltFor(std::chrono::nanoseconds timeout) {
    std::unique_lock lock{build_mutex};
    return build_condvar.wait_for(lock, timeout,
                                  [this] { return is_built.load(std::memory_order::relaxed); });
}

void ComputePipeline::KickBuild(Common::ThreadWorker& worker) {
    // A claimed build runs already (a worker, the scheduler's wait, an earlier kick). The queued
    // pipeline worker's task finds the claim taken and returns at once when it gets there.
    // kick_pending keeps the pipeline alive (IsBuildTaskDone) until the kick task is done. With
    // FRITCHII_COMPUTE_FAST_FIRST the kick makes the unoptimized build (its claim is the one
    // that counts), so the skip ends after the short compile.
    const bool claimed = UsesFastFirst() ? fast_claimed.load(std::memory_order::acquire)
                                         : build_claimed.load(std::memory_order::acquire);
    if (is_built.load(std::memory_order::acquire) || claimed ||
        kick_pending.exchange(true, std::memory_order::acq_rel)) {
        return;
    }
    worker.QueueWork([this] {
        RunNeededBuild();
        kick_pending.store(false, std::memory_order::release);
    });
}

void ComputePipeline::BindStoragePointers(BufferCache& buffer_cache) {
    namespace Profile = VideoCommon::Nx2Profile;
    const Profile::Scope pointers_scope{Profile::Zone::ComputePointers};
    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    buffer_cache.SetComputeShaderHash(shader_hash);
    size_t ssbo_index{};
    for (const auto& desc : info.storage_buffers_descriptors) {
        ASSERT(desc.count == 1);
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written, desc.indirect_offset,
                                              desc.write_extent,
                                              Shader::StorageWriteExtraOf(info, ssbo_index),
                                              Shader::StorageReadExtentOf(info, ssbo_index));
        ++ssbo_index;
    }
}

bool ComputePipeline::CanReplaceOcarinaUpscale() const {
    // The scaled 1440p passes only while the DLSS render-scale patch shrank the scene
    // (FRITCHII_DLSS_RENDER_SCALE); without Max Resolution they follow the game's dynamic size.
    const bool scaled_shader = (shader_hash == Ocarina1440pScaledShader ||
                                shader_hash == Ocarina1440pScaledFieldShader) &&
                               Settings::AppliedOcarinaRenderScale().has_value();
    const bool known_shader =
        shader_hash == OcarinaThreeQuarterShader || shader_hash == 0xef542df87cacfe9eULL ||
        shader_hash == Ocarina1440pShader || scaled_shader ||
        shader_hash == OcarinaFullSizeShader;
    return known_shader && !info.stores_global_memory &&
           info.image_buffer_descriptors.empty() && info.image_descriptors.size() == 1 &&
           info.image_descriptors.front().count == 1 && info.image_descriptors.front().is_written &&
           !info.image_descriptors.front().is_read && !info.image_descriptors.front().is_byte_addressed &&
           std::ranges::none_of(info.storage_buffers_descriptors, &Shader::StorageBufferDescriptor::is_written);
}

// SEAM >= 3 (EXP123, vk_mem_stage.cpp): every raw handle this dispatch binds, in the order
// Configure's handle loop reads them, into the dispatch packet. Called by the early fill, or by
// Configure when no fill made them.
void ComputePipeline::FillHandleValues(MemStage& seam,
                                       const Tegra::Engines::KeplerCompute& kepler_compute,
                                       Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                                       bool in_configure) {
    seam.FillHandleValues(true, null_pointer_handles, in_configure, [&](std::vector<u32>& values) {
        const auto& qmd{kepler_compute.launch_description};
        VideoCommon::FillStageHandleValues<true>(
            info, gpu_memory,
            VideoCommon::ComputeHandleCbufs<std::remove_cvref_t<decltype(qmd)>>{qmd},
            device.GetStorageBufferAlignment(), null_pointer_handles,
            [&values](u32 raw) { values.push_back(raw); });
    });
}

void ComputePipeline::MakeHandleValues(const Tegra::Engines::KeplerCompute& kepler_compute,
                                       Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                                       std::vector<u32>& values) const {
    values.clear();
    const auto& qmd{kepler_compute.launch_description};
    VideoCommon::FillStageHandleValues<true>(
        info, gpu_memory, VideoCommon::ComputeHandleCbufs<std::remove_cvref_t<decltype(qmd)>>{qmd},
        device.GetStorageBufferAlignment(), null_pointer_handles,
        [&values](u32 raw) { values.push_back(raw); });
}

bool ComputePipeline::Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                Tegra::MemoryManager& gpu_memory, Scheduler& scheduler,
                                BufferCache& buffer_cache, TextureCache& texture_cache,
                                TextureCacheRuntime& texture_runtime,
                                std::optional<ByteAddressedShadow>& shadow,
                                BindCommand* deferred_bind) {
    shadow.reset();
    guest_descriptor_queue.Acquire(scheduler, num_descriptor_entries, uses_descriptor_buffer);

    // SEAM = 2 (EXP123): the memory stage filled the dispatch's buffer side before this call
    // (MemStage::FillDispatch); its calls below are skipped and its section played in place.
    MemStage& mem_stage = buffer_cache.runtime.Seam();
    const MemStage::DispatchPacket* const held = mem_stage.HeldDispatch();
    // One follower: a posted dispatch R plays from ring B (nothing below may touch the buffer
    // cache then: it is E's, E running ahead).
    const bool ring_held = held != nullptr && mem_stage.HeldDispatchFromRing();
    namespace Profile = VideoCommon::Nx2Profile;
    // FRITCHII_ASYNC_COMPUTE_SKIP: the decision on an unbuilt pipeline bound them already
    // (BindStoragePointers), with nothing in between; consumed here whatever path runs.
    const bool prebound = std::exchange(pointers_prebound, false);
    if (held == nullptr && !prebound) {
        BindStoragePointers(buffer_cache);
    }

    // Ends after FillImageViews (views_scope.reset() below).
    std::optional<Profile::Scope> views_scope;
    views_scope.emplace(Profile::Zone::ComputeViews);
    texture_cache.SynchronizeDescriptors(true);

    boost::container::small_vector<VideoCommon::ImageViewInOut, 64> views;
    boost::container::small_vector<VideoCommon::SamplerId, 64> samplers;

    const auto& qmd{kepler_compute.launch_description};
    const bool via_header_index{qmd.linked_tsc != 0};
    // The handle words and handle programs: one function for the back's own read and the memory
    // seam's fill (texture_handle_values.h). EDEN_NX2_HANDLE_ALIGN=0 reads a single handle
    // program's words at the unaligned address (A/B, HandleProgramAlignment).
    const VideoCommon::ComputeHandleCbufs<std::remove_cvref_t<decltype(qmd)>> cbuf_view{qmd};
    const u64 storage_alignment{device.GetStorageBufferAlignment()};
    // SEAM >= 3 (EXP123, vk_mem_stage.cpp): the raw handles come in the dispatch packet, with the
    // EXP21 value they were made with; made here when no early fill made them.
    const std::vector<u32>* handle_values{};
    bool packet_null_pointers{};
    // A ring-held dispatch (one follower) always carries E's values, whatever SEAM reads now.
    if (mem_stage.ValuesByValue() || ring_held) {
        handle_values = mem_stage.HandleValues(true, packet_null_pointers);
        if (handle_values == nullptr) {
            FillHandleValues(mem_stage, kepler_compute, gpu_memory,
                             VideoCore::Nx2Debug::Experiment(21) != 0, true);
            handle_values = mem_stage.HandleValues(true, packet_null_pointers);
        }
    }
    const bool verify_handles{handle_values != nullptr &&
                              (MemStage::VerifyBits() & MemStage::VerifyHandles) != 0};
    size_t handle_cursor{};
    const auto next_value{[&](u32& value) {
        if (handle_cursor < handle_values->size()) {
            value = (*handle_values)[handle_cursor++];
            return true;
        }
        ++handle_cursor;
        return false;
    }};
    const auto read_handle{[&](const auto& desc, u32 index) {
        if (handle_values == nullptr) {
            return TexturePair(VideoCommon::RawTextureHandle<true>(info, desc, index, gpu_memory,
                                                                   cbuf_view, storage_alignment),
                               via_header_index);
        }
        u32 value{};
        if (!next_value(value)) [[unlikely]] {
            value = VideoCommon::RawTextureHandle<true>(info, desc, index, gpu_memory, cbuf_view,
                                                        storage_alignment);
        } else if (verify_handles) [[unlikely]] {
            // FRITCHII_MEMSTAGE_VERIFY & 1 (5.4): the back's own read.
            const u32 own{VideoCommon::RawTextureHandle<true>(info, desc, index, gpu_memory,
                                                              cbuf_view, storage_alignment)};
            mem_stage.NoteValueCheck(MemStage::Value::Handle, own == value,
                                     VideoCommon::HandleWordAddress(desc, index, cbuf_view),
                                     shader_hash, value, own);
        }
        return TexturePair(value, via_header_index);
    }};
    const auto add_image{[&](const auto& desc, bool blacklist) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle{read_handle(desc, index)};
            views.push_back({
                .index = handle.first,
                .blacklist = blacklist,
                .id = {},
            });
        }
    }};
    for (const auto& desc : info.texture_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        add_image(desc, false);
    }
    // EDEN_NX2_EXP21 (default 1; 0 = off): no texture for a handle that is a buffer address.
    // SEAM >= 3: the value the packet's handles were made with.
    const bool null_pointer_handles{handle_values != nullptr
                                        ? packet_null_pointers
                                        : VideoCore::Nx2Debug::Experiment(21) != 0};
    u32 texture{};
    for (const auto& desc : info.texture_descriptors) {
        if (desc.count > 1 && desc.handle_program != ~0U) {
            // A table of handles the shader indexes, read in one go; elements repeating a
            // sampler share its lookup.
            const size_t first{views.size()};
            u32 last_sampler_index{~0U};
            VideoCommon::SamplerId sampler{};
            const auto table_element{[&](u32, u32 raw) {
                const auto handle{TexturePair(raw, via_header_index)};
                views.push_back({handle.first});
                if (handle.second != last_sampler_index) {
                    sampler = texture_cache.GetSamplerId(handle.second, true);
                    last_sampler_index = handle.second;
                }
                samplers.push_back(sampler);
            }};
            if (handle_values == nullptr) {
                VideoCommon::RawTextureHandleTable(info, desc, gpu_memory, cbuf_view,
                                                   storage_alignment, table_element);
            } else {
                // SEAM >= 3: the table's values from the dispatch packet.
                boost::container::small_vector<u32, 16> own;
                if (verify_handles || handle_cursor + desc.count > handle_values->size())
                    [[unlikely]] {
                    VideoCommon::RawTextureHandleTable(
                        info, desc, gpu_memory, cbuf_view, storage_alignment,
                        [&own](u32, u32 raw) { own.push_back(raw); });
                }
                for (u32 element = 0; element < desc.count; ++element) {
                    u32 value{};
                    if (!next_value(value)) [[unlikely]] {
                        value = own[element];
                    } else if (verify_handles) [[unlikely]] {
                        mem_stage.NoteValueCheck(
                            MemStage::Value::Handle, own[element] == value,
                            VideoCommon::HandleWordAddress(desc, element, cbuf_view), shader_hash,
                            value, own[element]);
                    }
                    table_element(element, value);
                }
            }
            if (VideoCommon::HandleTableNotes::Wanted()) {
                std::vector<u32> tics(desc.count);
                for (u32 index = 0; index < desc.count; ++index) {
                    tics[index] = views[first + index].index;
                }
                VideoCommon::HandleTableNotes::Note(shader_hash, texture, tics);
            }
            ++texture;
            continue;
        }
        for (u32 index = 0; index < desc.count; ++index) {
            const bool null_handle{null_pointer_handles &&
                                   VideoCommon::HandleIsStoragePointer(info, desc, index)};
            if (null_handle && handle_values != nullptr) {
                // SEAM >= 3: the packet keeps its place (SKIPPED_HANDLE_VALUE).
                ++handle_cursor;
            }
            const auto handle{null_handle ? VideoCommon::NULL_TEXTURE_HANDLE
                                          : read_handle(desc, index)};
            views.push_back({handle.first});

            VideoCommon::SamplerId sampler = texture_cache.GetSamplerId(handle.second, true);
            samplers.push_back(sampler);
        }
        ++texture;
    }
    for (const auto& desc : info.image_descriptors) {
        add_image(desc, desc.is_written);
    }
    if (handle_values != nullptr) {
        mem_stage.LookedAtHandleValues(true, handle_cursor);
    }
    texture_cache.FillImageViews(std::span(views.data(), views.size()), true);
    views_scope.reset();
    // FRITCHII_EARLY_COMPUTE (dry): a feeder's sampled and storage images, and whether one has a
    // GPU write the host GPU has not run (the buffers are judged where they are bound).
    if (VideoCommon::EarlyCompute::g_early &&
        VideoCommon::EarlyCompute::IsFeeder(shader_hash)) [[unlikely]] {
        const size_t first_image = Shader::NumDescriptors(info.texture_buffer_descriptors) +
                                   Shader::NumDescriptors(info.image_buffer_descriptors);
        bool pending = false;
        for (size_t i = first_image; i < views.size() && !pending; ++i) {
            pending = texture_cache.EarlyImageWritePending(views[i].id);
        }
        const bool writes_images = std::ranges::any_of(
            info.image_descriptors, [](const auto& desc) { return desc.is_written; });
        VideoCommon::EarlyCompute::NoteImages(shader_hash, views.size() > first_image, pending,
                                              writes_images);
    }
    // Not for a ring-held dispatch (one follower): the trace reads the buffer cache, E's.
    if (auto& trace{GetUpscalerTrace()}; trace.enabled && kepler_compute.IsNativeMode() && !ring_held) {
        const u64 frame{trace.frame.load(std::memory_order_relaxed)};
        if (!trace.armed && frame >= trace.first_frame && info.uses_subgroup_shuffles) {
            trace.armed = true;
            trace.armed_frame = frame;
            LOG_INFO(Render_Vulkan, "Upscaler trace armed at frame {} by {:016x}", frame,
                     shader_hash);
        }
        if (trace.armed && frame > trace.armed_frame && frame <= trace.armed_frame + trace.frames) {
            TraceUpscalerDispatch(trace, info, shader_hash, kepler_compute, gpu_memory,
                                  buffer_cache, texture_cache, views);
        }
    }

    static const bool record_sampled = [] {
        const char* const value = std::getenv("EDEN_NX2_FRAME_DUMP_SAMPLED");
        return value && *value != '\0' && *value != '0';
    }();
    if (record_sampled || deferred_bind) {
        sampled_images.clear();
        const size_t first = Shader::NumDescriptors(info.texture_buffer_descriptors) +
                             Shader::NumDescriptors(info.image_buffer_descriptors);
        for (size_t index = first; index < first + Shader::NumDescriptors(info.texture_descriptors);
             ++index) {
            sampled_images.push_back({views[index].index, views[index].id, samplers[index - first]});
        }
    }

    std::optional<size_t> shadow_index;
    size_t image_index{Shader::NumDescriptors(info.texture_buffer_descriptors) +
                       Shader::NumDescriptors(info.image_buffer_descriptors) +
                       Shader::NumDescriptors(info.texture_descriptors)};
    written_images.clear();
    for (const auto& desc : info.image_descriptors) {
        for (u32 i = 0; i < desc.count; ++i, ++image_index) {
            if (desc.is_written) {
                written_images.push_back(views[image_index].id);
            }
            const auto& view{texture_cache.GetImageView(views[image_index].id)};
            if (!desc.is_byte_addressed || view.format != PixelFormat::R16_FLOAT) {
                continue;
            }
            if (shadow_index || !kepler_compute.IsNativeMode() ||
                kepler_compute.GetIndirectComputeAddress() || desc.count != 1 ||
                desc.is_read || !desc.is_written || desc.type != Shader::TextureType::Color2D ||
                desc.format != Shader::ImageFormat::R32_UINT || info.uses_global_memory) {
                throw Shader::RuntimeError("Unsupported packed R16 surface dispatch or unbounded global access");
            }
            shadow_index = image_index;
        }
    }
    if (shadow_index) {
        const auto max_dim{device.GetMaxComputeWorkGroupCount()};
        if (qmd.grid_dim_x == 0 || qmd.grid_dim_y == 0 || qmd.grid_dim_z == 0 ||
            qmd.grid_dim_x > max_dim[0] || qmd.grid_dim_y > max_dim[1] ||
            qmd.grid_dim_z > max_dim[2]) {
            throw Shader::RuntimeError("Invalid packed R16 surface dispatch dimensions");
        }
        const auto& target_view{texture_cache.GetImageView(views[*shadow_index].id)};
        const auto* target{target_view.BackingImage()};
        if (!target) {
            throw Shader::RuntimeError("Unknown packed R16 surface backing");
        }
        const auto target_ranges{ByteAddressedMappedRanges(gpu_memory, target->gpu_addr,
                                                          target->guest_size_bytes)};
        for (size_t i = 0; i < views.size(); ++i) {
            if (i == *shadow_index) {
                continue;
            }
            const auto& view{texture_cache.GetImageView(views[i].id)};
            if (view.IsBuffer()) {
                continue;
            }
            const auto* image{view.BackingImage()};
            if (!image) {
                throw Shader::RuntimeError("Unknown packed R16 surface peer image at binding {}", i);
            }
            for (const auto& a : target_ranges) {
                for (const auto& b : ByteAddressedMappedRanges(gpu_memory, image->gpu_addr,
                                                               image->guest_size_bytes)) {
                    if (a.Overlaps(b)) {
                        throw Shader::RuntimeError("Aliased packed R16 surface peer image at binding {}", i);
                    }
                }
            }
        }
        image_index = Shader::NumDescriptors(info.texture_buffer_descriptors) +
                      Shader::NumDescriptors(info.image_buffer_descriptors) +
                      Shader::NumDescriptors(info.texture_descriptors);
        for (const auto& desc : info.image_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++image_index) {
                if (image_index != *shadow_index) {
                    static_cast<void>(texture_cache.GetImageView(views[image_index].id)
                        .StorageView(desc.type, desc.format, desc.is_byte_addressed));
                }
            }
        }
        shadow = texture_runtime.CreateByteAddressedShadow(target_view);
    }

    if (held == nullptr) {
        // A held dispatch has no texel buffers (else it fills here, LateFill).
        buffer_cache.UnbindComputeTextureBuffers();
    }
    size_t index{};
    const auto add_buffer{[&](const auto& desc) {
        constexpr bool is_image = std::is_same_v<decltype(desc), const ImageBufferDescriptor&>;
        for (u32 i = 0; i < desc.count; ++i) {
            bool is_written{false};
            if constexpr (is_image) {
                is_written = desc.is_written;
            }
            ImageView& image_view = texture_cache.GetImageView(views[index].id);
            PixelFormat format{image_view.format};
            if constexpr (is_image) {
                if (const auto explicit_format{PixelFormatFromImageFormat(desc.format)}) {
                    format = *explicit_format;
                }
            }
            buffer_cache.BindComputeTextureBuffer(index, image_view.GpuAddr(),
                                                  image_view.BufferSize(), format,
                                                  is_written, is_image);
            ++index;
        }
    }};
    std::ranges::for_each(info.texture_buffer_descriptors, add_buffer);
    std::ranges::for_each(info.image_buffer_descriptors, add_buffer);

    if (held != nullptr) {
        // SEAM = 2: the `before` section, filled before Configure, played here.
        const Profile::Scope sync_scope{Profile::Zone::ComputeSync};
        mem_stage.PlayHeld(held->before);
    } else {
        // The dispatch's `before` section: its uploads, binds and descriptors are played here,
        // before the image descriptors.
        MemStage::FillPlay seam{mem_stage, MemStage::Site::DispatchBefore};
        {
            const Profile::Scope resolve_scope{Profile::Zone::ComputeResolve};
            buffer_cache.UpdateComputeBuffers();
        }
        {
            const Profile::Scope sync_scope{Profile::Zone::ComputeSync};
            buffer_cache.BindHostComputeBuffers();
        }
    }
    if (ring_held) {
        // One follower (FRITCHII_ONEF_OP_DISPATCH): R plays a dispatch E filled. The buffer cache
        // is E's: its "uploaded" mark came in the packet (FillRingDispatch).
        if ((held->flags & MemStage::PostCopyBarrier) != 0) {
            mem_stage.PlayPostCopyBarrier();
        }
    } else if (buffer_cache.any_buffer_uploaded) {
        buffer_cache.runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }

    RescalingPushConstant rescaling;
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    if (!shadow) {
        PushImageDescriptors(texture_cache, guest_descriptor_queue, info, rescaling, samplers_it,
                             views_it);
    } else {
        auto sampled_info{info};
        sampled_info.image_descriptors.clear();
        PushImageDescriptors(texture_cache, guest_descriptor_queue, sampled_info, rescaling,
                             samplers_it, views_it);
        for (const auto& desc : info.image_descriptors) {
            bool is_rescaled{};
            for (u32 i = 0; i < desc.count; ++i) {
                const size_t view_index{static_cast<size_t>(views_it - views.data())};
                auto& view{texture_cache.GetImageView((views_it++)->id)};
                const VkImageView storage{view_index == *shadow_index ? shadow->view :
                    view.StorageView(desc.type, desc.format, desc.is_byte_addressed)};
                guest_descriptor_queue.AddImage(storage);
                is_rescaled |= texture_cache.IsRescaling(view);
            }
            rescaling.PushImage(is_rescaled);
        }
    }

    if (guest_descriptor_queue.UpdateCount() != num_descriptor_entries) {
        LOG_CRITICAL(Render_Vulkan, "Compute descriptor count mismatch: expected {}, got {}",
                     num_descriptor_entries, guest_descriptor_queue.UpdateCount());
        return false;
    }

    // FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS: a pipeline that counts as unbuilt by the test delay
    // waits here as a building one does, until the delay ends.
    const auto test_deadline = std::exchange(test_wait_until, {});
    if (!is_built.load(std::memory_order::relaxed) ||
        test_deadline != std::chrono::steady_clock::time_point{}) {
        // Wait for the pipeline to be built
        scheduler.Record([this, test_deadline](vk::CommandBuffer) {
            const auto begin = std::chrono::steady_clock::now();
            // FRITCHII_COMPUTE_FAST_FIRST: the unoptimized build here, the optimized one on its
            // worker; else the optimized build here unless a worker has it.
            RunNeededBuild();
            bool unoptimized = false;
            {
                std::unique_lock lock{build_mutex};
                build_condvar.wait(lock,
                                   [this] { return is_built.load(std::memory_order::relaxed); });
                unoptimized = !running_optimized;
            }
            const bool test = test_deadline != std::chrono::steady_clock::time_point{};
            if (test) {
                std::this_thread::sleep_until(test_deadline);
            }
            // Every GPU channel waits with the dispatch, so a long wait is a freeze (port of the
            // log line of zortos293/Lichii #995: first-time compute programs waited 0.6-1 s each).
            const auto waited = std::chrono::steady_clock::now() - begin;
            if (waited > std::chrono::milliseconds{50}) {
                LOG_INFO(Render_Vulkan, "Compute dispatch waited {:.1f} ms for pipeline {:016x}{}{}",
                         std::chrono::duration<double, std::milli>(waited).count(), shader_hash,
                         test ? " (FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS)" : "",
                         unoptimized ? " (an unoptimized build, FRITCHII_COMPUTE_FAST_FIRST)"
                                     : "");
            }
        });
    }

    // Log compute pipeline binding
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogPipelineBind(true, "compute pipeline");
    }

    const DescriptorUpdateEntry* const descriptor_data{guest_descriptor_queue.UpdateData()};
    VkDeviceSize descriptor_buffer_offset{};
    u32 descriptor_buffer_chunk{};
    if (uses_descriptor_buffer) {
        const DescriptorBufferRing::Allocation alloc{
            descriptor_buffer_ring.Allocate(scheduler, descriptor_buffer_layout.size)};
        if (!alloc.host) {
            LOG_DEBUG(Render_Vulkan, "Failed to reserve descriptor memory, skipping dispatch");
            return false;
        }
        WriteDescriptorBuffer(device, descriptor_buffer_layout, descriptor_data, alloc.host);
        descriptor_buffer_offset = alloc.offset;
        descriptor_buffer_chunk = alloc.chunk;
    }

    const bool bind_descriptor_buffer{
        uses_descriptor_buffer &&
        (deferred_bind || scheduler.UpdateDescriptorBufferChunk(descriptor_buffer_chunk))};

    if (shadow) {
        image_index = Shader::NumDescriptors(info.texture_buffer_descriptors) +
                      Shader::NumDescriptors(info.image_buffer_descriptors) +
                      Shader::NumDescriptors(info.texture_descriptors);
        for (const auto& desc : info.image_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++image_index) {
                if (desc.is_written) {
                    texture_cache.MarkModification(
                        texture_cache.GetImageView(views[image_index].id).image_id);
                }
            }
        }
    }

    const bool is_rescaling = !info.texture_descriptors.empty() || !info.image_descriptors.empty();
    auto bind = [this, descriptor_data, is_rescaling, descriptor_buffer_offset,
                      descriptor_buffer_chunk, bind_descriptor_buffer,
                      rescaling_data = rescaling.Data()](vk::CommandBuffer cmdbuf) {
        // FRITCHII_COMPUTE_FAST_FIRST: the optimized pipeline replaces the unoptimized one here,
        // on the scheduler worker (the only thread that binds it). Off: two loads.
        if (optimized_ready.load(std::memory_order::acquire) || retired_fast) [[unlikely]] {
            SwapInOptimized();
        }
        if (bind_descriptor_buffer) {
            const VkDescriptorBufferBindingInfoEXT binding_info{
                descriptor_buffer_ring.BindingInfo(descriptor_buffer_chunk)};
            cmdbuf.BindDescriptorBuffersEXT(binding_info);
        }
        if (!pipeline) {
            return;
        }
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        // FRITCHII_SLOW_GPU: the program a slow submission's dispatches ran (one TLS load off).
        vk::NoteBoundProgram(VK_PIPELINE_BIND_POINT_COMPUTE, ShaderHash(), 0);
        if (!descriptor_set_layout) {
            return;
        }
        if (is_rescaling) {
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                                 rescaling_data.data());
        }
        if (uses_descriptor_buffer) {
            const u32 buffer_index{};
            cmdbuf.SetDescriptorBufferOffsetsEXT(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout,
                                                 0, buffer_index, descriptor_buffer_offset);
        } else if (uses_push_descriptor) {
            cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                    0, descriptor_data);
        } else {
            const VkDescriptorSet descriptor_set{descriptor_allocator.Commit()};
            const vk::Device& dev{device.GetLogical()};
            dev.UpdateDescriptorSet(descriptor_set, *descriptor_update_template, descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0,
                                      descriptor_set, nullptr);
        }
    };
    if (deferred_bind) {
        *deferred_bind = std::move(bind);
    } else {
        scheduler.Record(std::move(bind));
    }
    return true;
}

} // namespace Vulkan
