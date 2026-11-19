// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/nx2_hitch.h"
#include "common/nx2_timeline.h"
#include "video_core/buffer_churn_stats.h"
#include "video_core/churn_counters.h"
#include "video_core/gpu_split.h"
#include "video_core/link_stats.h"
#include "video_core/memory_stage_link.h"
#include "video_core/memstage_stats.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/small_card.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#else
// 128 MiB on Windows too: 256 MiB sat mostly unused in device memory on Switch 2 titles.
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif

// Unused space after the end of the stream buffer. Constant buffers are bound from it and the
// shaders declare each one at its full 64 KiB, so Metal checks 64 KiB from the binding offset
// against the end of the buffer ("argument c3[0] ... has space for 26624 bytes, but argument has a
// length(65536)"). The tail is never handed out, so every binding has the full 64 KiB behind it.
constexpr VkDeviceSize STREAM_BUFFER_TAIL = 64_KiB;

size_t GetStreamBufferSize(const Device& device) {
    // FRITCHII_SMALL_CARD (part Stream, boot, not in the default parts): 64 MiB, which sits in
    // device memory with resizable BAR. Saves 64 MiB; more waits on a wrap if a frame streams more.
    if (VideoCore::SmallCard::On(VideoCore::SmallCard::Stream) &&
        !device.HasDebuggingToolAttached()) {
        return MAX_STREAM_BUFFER_SIZE / 2;
    }
    if (!device.HasDebuggingToolAttached()) {
        if (device.IsIntegrated()) {
            // An integrated GPU's device-local host-visible heap can be no bigger than the stream
            // buffer (Radeon 780M: 256 MiB, 243 MiB budget), which then sat over that budget.
            // Leave a quarter of it free.
            VkDeviceSize heap_size = 0;
            ForEachDeviceLocalHostVisibleHeap(
                device, [&heap_size](size_t, VkMemoryHeap& heap) {
                    heap_size = (std::max)(heap_size, heap.size);
                });
            if (heap_size != 0 && heap_size <= MAX_STREAM_BUFFER_SIZE) {
                return Common::AlignUp(heap_size * 3 / 4, MAX_ALIGNMENT);
            }
        }
        return MAX_STREAM_BUFFER_SIZE;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](
                                                  size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = (std::max)(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= MAX_STREAM_BUFFER_SIZE) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return (std::min)(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}

/// FRITCHII_SIM_BAR_MIB (boot, unset = off, 0 = no room at all): the stream buffers get only this
/// much device memory the CPU can write; one that doesn't fit lands in host memory, as it does when
/// the real window is full. The pools ask one after the other while the rasterizer is made.
[[nodiscard]] bool SimBarFits(VkDeviceSize bytes) {
    static s64 left = [] {
        const char* const value = std::getenv("FRITCHII_SIM_BAR_MIB");
        return value != nullptr ? s64{std::atoi(value)} << 20 : s64{-1};
    }();
    if (left < 0) {
        return true;
    }
    if (static_cast<VkDeviceSize>(left) < bytes) {
        return false;
    }
    left -= static_cast<s64>(bytes);
    return true;
}

/// FRITCHII_SMALL_BAR_STREAMS (boot, default 0): on a discrete card whose CPU-visible device memory
/// is a small window, the stream buffers are sized from the room really free in it, the buffer
/// cache's first: the shaders read its uniforms straight from it, the other pool is only copied
/// from. FRITCHII_SMALL_BAR_STREAM_MIB=<transfers>,<buffers> sets the two sizes (implies it).
struct SmallBarPlan {
    bool active{};
    VkDeviceSize transfers{};
    VkDeviceSize buffers{};
};

[[nodiscard]] const SmallBarPlan& GetSmallBarPlan(const Device& device) {
    static const SmallBarPlan plan = [&device] {
        SmallBarPlan result;
        const char* const sizes = std::getenv("FRITCHII_SMALL_BAR_STREAM_MIB");
        const char* const mode = std::getenv("FRITCHII_SMALL_BAR_STREAMS");
        if (sizes == nullptr && (mode == nullptr || std::atoi(mode) == 0)) {
            return result;
        }
        if (device.IsIntegrated() || !device.HasSplitAllocators() ||
            device.HasDebuggingToolAttached()) {
            return result;
        }
        VkDeviceSize window = 0;
        u32 window_heap = 0;
        ForEachDeviceLocalHostVisibleHeap(device, [&](size_t index, VkMemoryHeap& heap) {
            if (heap.size > window) {
                window = heap.size;
                window_heap = static_cast<u32>(index);
            }
        });
        VkDeviceSize used = 0;
        const char* const sim_bar = std::getenv("FRITCHII_SIM_BAR_MIB");
        if (sim_bar != nullptr) {
            window = VkDeviceSize((std::max)(std::atoi(sim_bar), 0)) << 20;
        } else if (device.CanReportMemoryUsage()) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            (void)device.GetPhysical().GetMemoryProperties(&budget);
            used = budget.heapUsage[window_heap];
        }
        if (sim_bar == nullptr && (window == 0 || window > 1_GiB)) {
            LOG_INFO(Render_Vulkan,
                     "FRITCHII_SMALL_BAR_STREAMS: {} MiB of CPU-visible device memory, not a "
                     "small window: stream buffers as usual",
                     window >> 20);
            return result;
        }
        constexpr VkDeviceSize Reserve = 16_MiB; // the driver's own use of the window
        constexpr VkDeviceSize Smallest = 16_MiB;
        const VkDeviceSize free_bytes = window - (std::min)(used, window);
        const VkDeviceSize room = free_bytes > Reserve ? free_bytes - Reserve : 0;
        result.active = true;
        result.buffers = (std::min)(VkDeviceSize{64_MiB}, room);
        result.transfers = (std::min)(MAX_STREAM_BUFFER_SIZE, room - result.buffers);
        if (sizes != nullptr) {
            char* end = nullptr;
            const u64 transfers_mib = std::strtoull(sizes, &end, 10);
            const u64 buffers_mib = *end == ',' ? std::strtoull(end + 1, nullptr, 10) : 0;
            result.transfers = std::clamp<u64>(transfers_mib, 16, 256) << 20;
            result.buffers = std::clamp<u64>(buffers_mib, 16, 256) << 20;
        }
        result.transfers = Common::AlignDown((std::max)(result.transfers, Smallest), MAX_ALIGNMENT);
        result.buffers = Common::AlignDown((std::max)(result.buffers, Smallest), MAX_ALIGNMENT);
        LOG_INFO(Render_Vulkan,
                 "FRITCHII_SMALL_BAR_STREAMS: window {} MiB, {} MiB in use: {} MiB for uniforms "
                 "and buffer uploads, {} MiB for texture and query uploads",
                 window >> 20, used >> 20, result.buffers >> 20, result.transfers >> 20);
        return result;
    }();
    return plan;
}

[[nodiscard]] VkDeviceSize StreamBytes(const Device& device, u32 divisor,
                                       StagingBufferPool::StreamRole role) {
    if (const SmallBarPlan& plan = GetSmallBarPlan(device);
        plan.active && role != StagingBufferPool::StreamRole::Everything) {
        return role == StagingBufferPool::StreamRole::Buffers ? plan.buffers : plan.transfers;
    }
    return Common::AlignDown(GetStreamBufferSize(device) / (std::max)(divisor, 1U), MAX_ALIGNMENT);
}

[[nodiscard]] const char* StreamRoleName(StagingBufferPool::StreamRole role) {
    switch (role) {
    case StagingBufferPool::StreamRole::Transfers:
        return "texture and query uploads";
    case StagingBufferPool::StreamRole::Buffers:
        return "uniforms and buffer uploads";
    case StagingBufferPool::StreamRole::Everything:
        break;
    }
    return "all uploads";
}
} // Anonymous namespace

// stream_divisor: FRITCHII_MEMSTAGE_SPLIT_ALLOC makes two pools (the buffer cache's and the
// back's), which split one stream buffer's size between them instead of each taking all of it.
StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_, u32 stream_divisor,
                                     StreamRole stream_role)
    : device{device_}, memory_allocator{memory_allocator_}, clock{scheduler_},
      recycler{scheduler_},
      role{stream_role}, stream_buffer_size{StreamBytes(device, stream_divisor, stream_role)},
      region_size{stream_buffer_size / StagingBufferPool::NUM_SYNCS} {
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size + STREAM_BUFFER_TAIL,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const bool sim_bar_fits = SimBarFits(stream_ci.size);
    stream_buffer = memory_allocator.CreateBuffer(
        stream_ci, sim_bar_fits ? MemoryUsage::Stream : MemoryUsage::Upload);
    {
        // Where it landed decides who pays for its bytes on a slow link: in device memory the CPU
        // while writing, in host memory the GPU while reading. It lands in host memory without a
        // word when the small window is full, so the log says.
        const auto properties = device.GetPhysical().GetMemoryProperties().memoryProperties;
        const u32 type = stream_buffer.Location().memory_type;
        const u32 heap = properties.memoryTypes[type].heapIndex;
        stream_in_device_memory =
            (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        crosses_link = !device.IsIntegrated();
        const bool plan_active = GetSmallBarPlan(device).active;
        refresh_before_spill = plan_active;
        if (plan_active && !stream_in_device_memory && sim_bar_fits) {
            // The allocator's budget check sent it to host memory. The window's budget is only an
            // estimate (some drivers report little more than what is free): try past it.
            try {
                vk::Buffer retry =
                    memory_allocator.CreateBufferOverBudget(stream_ci, MemoryUsage::Stream, 1);
                const u32 retry_type = retry.Location().memory_type;
                if ((properties.memoryTypes[retry_type].propertyFlags &
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                    stream_buffer = std::move(retry);
                    stream_in_device_memory = true;
                }
            } catch (const vk::Exception&) {
                // It stays where it is; the warning below says so.
            }
        }
        const u32 final_type = stream_buffer.Location().memory_type;
        const u32 final_heap = properties.memoryTypes[final_type].heapIndex;
        bool has_window = false;
        ForEachDeviceLocalHostVisibleHeap(device,
                                          [&has_window](size_t, VkMemoryHeap&) { has_window = true; });
        if (crosses_link && has_window && !stream_in_device_memory) {
            LOG_WARNING(Render_Vulkan,
                        "Stream buffer for {}: {} MiB fell out of device memory into host memory "
                        "type {} (heap {}){}: the GPU reads it across the link",
                        StreamRoleName(stream_role), stream_ci.size >> 20, final_type, final_heap,
                        sim_bar_fits ? "" : " (FRITCHII_SIM_BAR_MIB: no room left)");
        } else {
            LOG_INFO(Render_Vulkan,
                     "Stream buffer for {}: {} MiB in memory type {} (heap {}, {} MiB), {}",
                     StreamRoleName(stream_role), stream_ci.size >> 20, final_type, final_heap,
                     properties.memoryHeaps[final_heap].size >> 20,
                     stream_in_device_memory ? "device memory" : "host memory");
        }
    }
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

void StagingBufferPool::CreateUploadRing() {
    // FRITCHII_STAGING_RING (port of DahBard/Blitchii's staging ring, r64): uploads over the
    // stream buffer's region (8 MiB on a discrete GPU) took a cached staging buffer or a new one,
    // often a driver allocation written for the first time (page faults); OoT's area loads made
    // hundreds. Host memory, like the staging buffers it stands in for.
    const int value = VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_staging_ring,
                                                       "FRITCHII_STAGING_RING");
    if (value <= 0 || upload_ring) {
        return;
    }
    const u64 mib = value == 1 ? 256 : std::clamp<u64>(static_cast<u64>(value), 16, 1024);
    const size_t size = static_cast<size_t>(mib) << 20;
    VkBufferCreateInfo ring_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size + STREAM_BUFFER_TAIL,
        // CreateStagingBuffer's usage.
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        ring_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        ring_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const auto start = std::chrono::steady_clock::now();
    vk::Buffer buffer;
    try {
        buffer = memory_allocator.CreateBuffer(ring_ci, MemoryUsage::Upload);
    } catch (const vk::Exception& e) {
        LOG_WARNING(Render_Vulkan, "FRITCHII_STAGING_RING={}: no memory for the ring ({}); off",
                    value, e.what());
        return;
    }
    if (buffer.Mapped().size() < size || !buffer.IsHostCoherent()) {
        LOG_WARNING(Render_Vulkan,
                    "FRITCHII_STAGING_RING={}: the ring's memory is not mapped host-coherent; off",
                    value);
        return;
    }
    // Every page written once now, so that no upload at a load takes the first-touch faults.
    std::memset(buffer.Mapped().data(), 0, buffer.Mapped().size());
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const u32 memory_type = buffer.Location().memory_type;
    const VkDeviceAddress address = device.IsBufferDeviceAddressSupported()
                                        ? device.GetLogical().GetBufferDeviceAddress(*buffer)
                                        : VkDeviceAddress{};
    const std::span<u8> mapped = buffer.Mapped();
    upload_ring.emplace(UploadRing{
        .buffer = std::move(buffer),
        .address = address,
        .mapped = mapped,
        .regions = StagingRingRegions{size},
    });
    LOG_INFO(Render_Vulkan,
             "Staging ring (FRITCHII_STAGING_RING={}, live: 0 stops its use): {} MiB of host memory "
             "in memory type {}, {} regions of {} MiB, made and written in {:.1f} ms",
             value, mib, memory_type, StagingRingRegions::NUM_REGIONS,
             (size / StagingRingRegions::NUM_REGIONS) >> 20, ms);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetRingBuffer(size_t size) {
    UploadRing& ring = *upload_ring;
    const auto is_free = [this](u64 stamp) { return clock.IsFree(stamp); };
    const u64 now = clock.Now();
    // The last upload's copy is recorded by now at the latest.
    ring.regions.Restamp(now);
    const std::optional<size_t> offset = ring.regions.Take(size, now, is_free);
    if (!offset) {
        ++ring.refused;
        if (size <= ring.regions.Size() && ring.regions.LargeUploadInFlight(is_free)) {
            ++ring.refused_behind_large;
        }
        return std::nullopt;
    }
    ++ring.uploads;
    ring.bytes += size;
    if (size > ring.regions.Size() / 2) {
        ++ring.large;
    }
    return StagingBufferRef{
        .buffer = *ring.buffer,
        .device_address = ring.address,
        .offset = static_cast<VkDeviceSize>(*offset),
        .mapped_span = ring.mapped.subspan(*offset, size),
        .usage = MemoryUsage::Upload,
        .log2_level{},
        .index{},
    };
}

void StagingBufferPool::LogUploadRing() {
    UploadRing& ring = *upload_ring;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (ring.last_log_ns == 0) {
        ring.last_log_ns = now;
        return;
    }
    if (now - ring.last_log_ns < 10'000'000'000) {
        return;
    }
    ring.last_log_ns = now;
    if (ring.uploads + ring.refused + ring.created_after_refusal != 0) {
        LOG_INFO(Render_Vulkan,
                 "Staging ring in 10 s (FRITCHII_STAGING_RING={}): {} uploads ({} MiB), {} over "
                 "half the ring; {} refused ({} while an upload over half the ring was in flight), "
                 "{} staging buffers made after a refusal",
                 VideoCore::Nx2Debug::g_staging_ring.load(std::memory_order_relaxed), ring.uploads,
                 ring.bytes >> 20, ring.large, ring.refused, ring.refused_behind_large,
                 ring.created_after_refusal);
    }
    ring.uploads = ring.bytes = ring.large = 0;
    ring.refused = ring.refused_behind_large = ring.created_after_refusal = 0;
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred,
                                            VideoCommon::BufferChurnStats::StagingCaller caller) {
    StagingBufferRef ref = RequestBuffer(size, usage, deferred, caller);
    if (VideoCommon::LinkStats::On()) [[unlikely]] {
        NoteLink(ref, size, usage, caller);
    }
    return ref;
}

void StagingBufferPool::NoteLink(const StagingBufferRef& ref, size_t size, MemoryUsage usage,
                                 VideoCommon::BufferChurnStats::StagingCaller caller) const {
    using Path = VideoCommon::LinkStats::Path;
    if (!crosses_link || usage == MemoryUsage::DeviceLocal) {
        return;
    }
    Path path = Path::GpuReadsHost;
    if (usage == MemoryUsage::Download) {
        path = Path::GpuWritesHost;
    } else if (ref.buffer == *stream_buffer && stream_in_device_memory) {
        path = Path::CpuWritesDevice;
    }
    VideoCommon::LinkStats::Note(path, caller, size);
    if (usage != MemoryUsage::Upload) {
        return;
    }
    // What the stream buffer's size decides: its traffic, and what is too big for a region.
    StreamUse& use = stream_use;
    if (ref.buffer == *stream_buffer) {
        use.bytes.fetch_add(size, std::memory_order_relaxed);
        use.requests.fetch_add(1, std::memory_order_relaxed);
        use.frame_bytes.fetch_add(size, std::memory_order_relaxed);
    } else if (size > region_size) {
        use.too_big.fetch_add(1, std::memory_order_relaxed);
        use.too_big_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

bool StagingBufferPool::StreamRegionsBusy(size_t region_begin, size_t region_end, size_t size) {
    if (!AreRegionsActive(region_begin, region_end)) {
        return false;
    }
    // The answer used the last GPU tick someone read, which can be old: with the small BAR plan
    // it is read again before a request leaves device memory. With the counter on it is read
    // again too, to count the spills that were not needed (they still spill then).
    const bool stats = VideoCommon::LinkStats::On();
    bool was_free = false;
    if (refresh_before_spill || stats) {
        clock.GetScheduler().GetMasterSemaphore().Refresh();
        was_free = !AreRegionsActive(region_begin, region_end);
    }
    const bool spill = !(refresh_before_spill && was_free);
    if (stats) [[unlikely]] {
        if (spill) {
            stream_use.full.fetch_add(1, std::memory_order_relaxed);
            stream_use.full_bytes.fetch_add(size, std::memory_order_relaxed);
        }
        if (was_free) {
            stream_use.full_was_free.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return spill;
}

void StagingBufferPool::LogStreamUse() {
    StreamUse& use = stream_use;
    const u64 frame = use.frame_bytes.exchange(0, std::memory_order_relaxed);
    if (frame > use.peak_frame_bytes.load(std::memory_order_relaxed)) {
        use.peak_frame_bytes.store(frame, std::memory_order_relaxed);
    }
    size_t busy = 0;
    const u64 gpu_tick = clock.KnownGpu();
    for (const u64 tick : sync_ticks) {
        busy += clock.IsKnownFree(tick, gpu_tick) ? 0 : 1;
    }
    use.peak_busy_regions = (std::max)(use.peak_busy_regions, busy);
    const auto now = std::chrono::steady_clock::now();
    if (use.window_start == std::chrono::steady_clock::time_point{}) {
        use.window_start = now;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - use.window_start).count();
    if (seconds < 10.0) {
        return;
    }
    const auto rate = [seconds](std::atomic<u64>& counter) {
        return static_cast<double>(counter.exchange(0, std::memory_order_relaxed)) / seconds;
    };
    const double requests = rate(use.requests);
    const double mb = rate(use.bytes) / 1e6;
    const double wraps = rate(use.wraps);
    const double full = rate(use.full);
    const double full_mb = rate(use.full_bytes) / 1e6;
    const double was_free = rate(use.full_was_free);
    const double too_big = rate(use.too_big);
    const double too_big_mb = rate(use.too_big_bytes) / 1e6;
    const u64 peak = use.peak_frame_bytes.exchange(0, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "Stream use for {} ({} MiB, {}, regions of {} MiB): {:.1f} MB/s in {:.0f} requests/s, "
             "worst frame {:.1f} MiB, {:.2f} wraps/s, at most {} of {} regions busy at a frame end "
             "({} MiB); full {:.1f}/s ({:.1f} MB/s went to host staging; {:.1f}/s were free at a "
             "fresh look at the GPU); {:.1f}/s over a region ({:.1f} MB/s to host staging)",
             StreamRoleName(role), stream_buffer_size >> 20,
             stream_in_device_memory ? "device memory" : "host memory", region_size >> 20, mb,
             requests, static_cast<double>(peak) / 1048576.0, wraps, use.peak_busy_regions,
             NUM_SYNCS, (use.peak_busy_regions * region_size) >> 20, full, full_mb, was_free,
             too_big, too_big_mb);
    use.peak_busy_regions = 0;
    use.window_start = now;
}

StagingBufferRef StagingBufferPool::RequestBuffer(
    size_t size, MemoryUsage usage, bool deferred,
    VideoCommon::BufferChurnStats::StagingCaller caller) {
    if (cache_side) {
        VideoCommon::MemoryStageLink::CheckCacheUser(VideoCommon::MemoryStageLink::Check::StagingUser);
    }
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size, caller);
    }
    return GetStagingBuffer(size, usage, deferred, caller);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    StagingBuffers& cache_level = GetCache(ref.usage)[ref.log2_level];
    auto node = cache_level.deferred.extract(ref.index);
    ASSERT(!node.empty());
    if (node.empty()) {
        return;
    }
    node.mapped().tick = clock.Now();
    cache_level.entries.push_back(std::move(node.mapped()));
}

void StagingBufferPool::TickFrame() {
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
    recycler.TickFrame();
    if (upload_ring) {
        LogUploadRing();
    }
    if (crosses_link && VideoCommon::LinkStats::On()) [[unlikely]] {
        LogStreamUse();
    }
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(
    size_t size, VideoCommon::BufferChurnStats::StagingCaller caller) {
    if (StreamRegionsBusy(Region(free_iterator) + 1,
                          (std::min)(Region(iterator + size) + 1, NUM_SYNCS), size)) {
        // EDEN_NX2_MEMSTAGE_STATS: the stream ring is busy (the memory stage's join 3).
        VideoCommon::MemStageStats::StreamFull();
        if (VideoCommon::LinkStats::On()) [[unlikely]] {
            VideoCommon::LinkStats::NoteStreamFull();
        }
        // Memory stage step 7: on GPUMem with the window open the busy regions may be stamped
        // with packets not played yet; they are never waited for, a new buffer is taken (counted).
        VideoCommon::MemoryStageLink::NoteStagingFallback();
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload, false, caller, true);
    }
    // Lazy: the regions used since the last request take this one's stamp. With packet ticks
    // that is a packet at or after theirs, so never free before their copies (vk_mem_stage.cpp).
    const u64 current_tick = clock.Now();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    // FRITCHII_STREAM_WRAP_GUARD (boot, default 1; port of zortos293/Lichii#1835 by
    // anthonyloukinas): a wrap whose first regions are still in use keeps the position. Moved to
    // the start before that check, the guard above would treat the first region as acquired and
    // hand it out again while a recorded command buffer still reads it (OoT: white or false-colour
    // 3D view). 0 = the old order.
    static const bool wrap_guard = [] {
        const char* value = std::getenv("FRITCHII_STREAM_WRAP_GUARD");
        return value == nullptr || std::atoi(value) != 0;
    }();
    if (!wrap_guard) {
        free_iterator = (std::max)(free_iterator, iterator + size);
    }

    if (iterator + size >= stream_buffer_size) {
        if (VideoCommon::LinkStats::On()) [[unlikely]] {
            stream_use.wraps.fetch_add(1, std::memory_order_relaxed);
        }
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        if (!wrap_guard) {
            used_iterator = 0;
            iterator = 0;
            free_iterator = size;
        }

        if (StreamRegionsBusy(0, Region(size) + 1, size)) {
            VideoCommon::MemStageStats::StreamFull();
            if (VideoCommon::LinkStats::On()) [[unlikely]] {
                VideoCommon::LinkStats::NoteStreamFull();
            }
            VideoCommon::MemoryStageLink::NoteStagingFallback();
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload, false, caller, true);
        }
        if (wrap_guard) {
            used_iterator = 0;
            iterator = 0;
            free_iterator = size;
        }
    } else if (wrap_guard) {
        free_iterator = (std::max)(free_iterator, iterator + size);
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = clock.KnownGpu();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [this, gpu_tick](u64 sync_tick) {
                           return !clock.IsKnownFree(sync_tick, gpu_tick);
                       });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(
    size_t size, MemoryUsage usage, bool deferred,
    VideoCommon::BufferChurnStats::StagingCaller caller, bool stream_full) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    // FRITCHII_STAGING_RING: a slice of the ring before a new buffer (live: 0 stops its use).
    if (upload_ring && usage == MemoryUsage::Upload && !deferred &&
        VideoCore::Nx2Debug::g_staging_ring.load(std::memory_order_relaxed) > 0) {
        if (const std::optional<StagingBufferRef> ref = TryGetRingBuffer(size)) {
            return *ref;
        }
        ++upload_ring->created_after_refusal;
    }
    return CreateStagingBuffer(size, usage, deferred, caller, stream_full);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil(size)];

    const auto is_free = [this](const StagingBuffer& entry) {
        return clock.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    const size_t position = static_cast<size_t>(std::distance(entries.begin(), it));
    const StagingBufferRef ref = it->Ref();
    if (!deferred) {
        cache_level.iterate_index = position + 1;
        it->tick = clock.Now();
        return ref;
    }
    // A deferred buffer leaves the entries until FreeDeferred; the last entry takes its place,
    // so the next lookup starts there.
    cache_level.iterate_index = position;
    cache_level.deferred.emplace(it->index, std::move(*it));
    if (it != entries.end() - 1) {
        *it = std::move(entries.back());
    }
    entries.pop_back();
    return ref;
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(
    size_t size, MemoryUsage usage, bool deferred,
    VideoCommon::BufferChurnStats::StagingCaller caller, bool stream_full) {
    auto const log2_size = Common::Log2Ceil<u32>(u32(size));
    // FRITCHII_HITCH_SPLIT: new staging buffers in the hitch line (their first writes, the page
    // faults of new memory, land in "texture uploads" for texture staging).
    const Common::Nx2Hitch::SplitScope hitch_timed{Common::Nx2Hitch::Cause::StagingCreate,
                                                   u64{1} << log2_size};
    // EDEN_NX2_MEMSTAGE_STATS: no free staging buffer of this size (any user).
    VideoCommon::MemStageStats::StagingCreated(u64{1} << log2_size);
    // EDEN_NX2_BUFFER_CHURN_STATS: the same, by caller.
    VideoCommon::BufferChurnStats::StagingCreated(caller, u64{1} << log2_size, stream_full);
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 1ULL << log2_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, // mesh emulation dispatch and draw arguments
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer buffer;
    // EDEN_NX2_EXP104: a pooled staging buffer of this memory usage and size the GPU finished
    // with (BufferRecycler), instead of a new one. Same create info, so the same buffer.
    if (BufferRecycler::Enabled()) {
        buffer = recycler.Take(BufferRecycler::StagingFamily(usage), log2_size);
    }
    if (!buffer) {
        try {
            buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
        } catch (const vk::Exception& e) {
            // Out of memory: free every idle staging buffer (and EXP104's pooled ones) and try
            // once more.
            const size_t freed = ReleaseIdleBuffers();
            LOG_WARNING(Render_Vulkan,
                        "Staging buffer of {} MiB could not be made ({}); freed {} idle "
                        "staging buffers and retrying",
                        buffer_ci.size >> 20, e.what(), freed);
            buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
        }
        VideoCommon::Churn::Add(VideoCommon::Churn::StagingBuffers);
        VideoCommon::Churn::Add(VideoCommon::Churn::StagingBytes, buffer_ci.size);
    }
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer entry{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2_size,
        .index = unique_ids++,
        .tick = clock.Now(),
    };
    const StagingBufferRef ref = entry.Ref();
    StagingBuffers& cache_level = GetCache(usage)[log2_size];
    if (deferred) {
        cache_level.deferred.emplace(entry.index, std::move(entry));
    } else {
        cache_level.entries.push_back(std::move(entry));
    }
    return ref;
}

size_t StagingBufferPool::ReleaseIdleBuffers() {
    size_t freed = recycler.DropAll();
    for (StagingBuffersCache* cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& level : *cache) {
            const size_t before = level.entries.size();
            std::erase_if(level.entries,
                          [this](const StagingBuffer& entry) { return clock.IsFree(entry.tick); });
            freed += before - level.entries.size();
            level.iterate_index = 0;
            level.delete_index = 0;
        }
    }
    return freed;
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

u64 StagingBufferPool::CachedBytes(MemoryUsage usage) {
    u64 bytes{};
    const StagingBuffersCache& cache = GetCache(usage);
    for (size_t level = 0; level < NUM_LEVELS; ++level) {
        const size_t count = cache[level].entries.size() + cache[level].deferred.size();
        if (count != 0) {
            bytes += u64{count} << level;
        }
    }
    return bytes;
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    ReleaseLevel(GetCache(usage), current_delete_level);
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t old_size = entries.size();

    // EDEN_NX2_EXP8 bit 1: keep two idle upload buffers per level up to 32 MiB (under 128 MiB in
    // all). Each level is swept every 64 frames, so after a pause each upload over 8 MiB (past
    // the stream buffer's regions) got a new dedicated allocation, written to for the first time
    // (16 MiB on an RTX 4080, idle system: 0.4 ms to create, 1.2 ms more for the first write).
    constexpr size_t KeepMaxLog2 = 25;
    constexpr size_t KeepPerLevel = 2;
    if (&cache == &upload_cache && log2 <= KeepMaxLog2 &&
        (VideoCore::Nx2Debug::Experiment(8) & 1) != 0) {
        size_t idle = 0;
        std::erase_if(entries, [this, &idle](StagingBuffer& entry) {
            if (!clock.IsFree(entry.tick) || ++idle <= KeepPerLevel) {
                return false;
            }
            Retire(entry);
            return true;
        });
        staging.delete_index = 0;
        if (staging.iterate_index > entries.size()) {
            staging.iterate_index = 0;
        }
        return;
    }

    const auto is_deletable = [this](StagingBuffer& entry) {
        if (!clock.IsFree(entry.tick)) {
            return false;
        }
        Retire(entry);
        return true;
    };
    // Deferred checkouts take entries out of the vector since the last sweep.
    const size_t begin_offset = (std::min)(staging.delete_index, old_size);
    const size_t end_offset = (std::min)(begin_offset + deletions_per_tick, old_size);
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end, is_deletable), end);

    const size_t new_size = entries.size();
    staging.delete_index += deletions_per_tick;
    if (staging.delete_index >= new_size) {
        staging.delete_index = 0;
    }
    if (staging.iterate_index > new_size) {
        staging.iterate_index = 0;
    }
}

void StagingBufferPool::Retire(StagingBuffer& entry) {
    // EDEN_NX2_EXP104: the pool takes it (leaving entry.buffer empty) or the erase destroys it
    // as before. The caller checked IsFree; the pool stamps the current tick anyway.
    recycler.Give(BufferRecycler::StagingFamily(entry.usage), entry.buffer,
                  u64{1} << entry.log2_level);
}

namespace {
using PoolGroup = VideoCommon::BufferChurnStats::PoolGroup;

VideoCommon::BufferChurnStats::PoolCounters& GroupCounters(BufferRecycler::Family family) {
    const PoolGroup group =
        family == BufferRecycler::Family::Cache ? PoolGroup::Cache : PoolGroup::Staging;
    return VideoCommon::BufferChurnStats::GetPool().group[static_cast<size_t>(group)];
}

// Every recycler's held and doomed bytes together. FRITCHII_MEMSTAGE_SPLIT_ALLOC gives each staging
// pool a recycler of its own (a pooled buffer must go back to the allocator that made it), and
// the caps (MaxHeldBytes, MaxDoomedBytes) stay one cap for all of them, as with a single pool:
// a recycler over the total evicts its own oldest entries. The gauges show the totals.
std::atomic<u64> g_total_held_bytes{};
std::atomic<u64> g_total_held_entries{};
std::atomic<u64> g_total_doomed_bytes{};

void AddTotals(s64 held_bytes, s64 held_entries, s64 doomed_bytes) noexcept {
    g_total_held_bytes.fetch_add(static_cast<u64>(held_bytes), std::memory_order_relaxed);
    g_total_held_entries.fetch_add(static_cast<u64>(held_entries), std::memory_order_relaxed);
    g_total_doomed_bytes.fetch_add(static_cast<u64>(doomed_bytes), std::memory_order_relaxed);
}
} // Anonymous namespace

BufferRecycler::BufferRecycler(Scheduler& scheduler_) : clock{scheduler_} {}

BufferRecycler::~BufferRecycler() {
    // Shutdown: the device is idle; the vk::Buffers destroy themselves.
    AddTotals(-static_cast<s64>(held_bytes), -static_cast<s64>(held_entries),
              -static_cast<s64>(doomed_bytes));
    held_bytes = held_entries = doomed_bytes = 0;
    PublishGauges();
}

bool BufferRecycler::Enabled() noexcept {
    return VideoCore::Nx2Debug::Experiment(104) > 0;
}

BufferRecycler::Family BufferRecycler::StagingFamily(MemoryUsage usage) noexcept {
    switch (usage) {
    case MemoryUsage::Upload:
        return Family::StagingUpload;
    case MemoryUsage::Download:
        return Family::StagingDownload;
    default:
        return Family::StagingDeviceLocal;
    }
}

vk::Buffer BufferRecycler::Take(Family family, u32 log2, u64* bytes) {
    VideoCommon::BufferChurnStats::PoolCounters& counters = GroupCounters(family);
    if (log2 >= NumClasses) {
        VideoCommon::BufferChurnStats::PoolAdd(counters.missed);
        return vk::Buffer{};
    }
    vk::Buffer buffer;
    {
        std::scoped_lock lock{mutex};
        std::deque<Entry>& list = lists[ListIndex(family, log2)];
        // Oldest first: entries are returned in tick order, so if the oldest is still in use
        // by the GPU, so are the others.
        if (!list.empty() && clock.IsFree(list.front().tick)) {
            Entry& entry = list.front();
            buffer = std::move(entry.buffer);
            if (bytes != nullptr) {
                *bytes = entry.bytes;
            }
            held_bytes -= entry.bytes;
            --held_entries;
            AddTotals(-static_cast<s64>(entry.bytes), -1, 0);
            list.pop_front();
            PublishGauges();
        }
    }
    VideoCommon::BufferChurnStats::PoolAdd(buffer ? counters.taken : counters.missed);
    return buffer;
}

bool BufferRecycler::Give(Family family, vk::Buffer& buffer, u64 bytes) {
    if (!buffer || bytes == 0 || !Enabled()) {
        return false;
    }
    VideoCommon::BufferChurnStats::PoolCounters& counters = GroupCounters(family);
    // Floor: the entry serves every create of its class (a buffer made while EXP104 was off
    // has its exact size).
    const u32 log2 = static_cast<u32>(Common::Log2Floor<u64>(bytes));
    if (bytes > (u64{1} << MaxLog2)) {
        VideoCommon::BufferChurnStats::PoolAdd(counters.refused);
        return false;
    }
    std::scoped_lock lock{mutex};
    lists[ListIndex(family, log2)].push_back(Entry{
        .buffer = std::move(buffer),
        .bytes = bytes,
        .tick = clock.Now(),
        .frame = frame,
        .family = family,
    });
    held_bytes += bytes;
    ++held_entries;
    AddTotals(static_cast<s64>(bytes), 1, 0);
    VideoCommon::BufferChurnStats::PoolAdd(counters.returned);
    // Over the cap (all recyclers): this one's entry returned longest ago goes, whatever its
    // class.
    while (g_total_held_bytes.load(std::memory_order_relaxed) > MaxHeldBytes) {
        std::deque<Entry>* oldest = nullptr;
        for (std::deque<Entry>& list : lists) {
            if (!list.empty() &&
                (oldest == nullptr || list.front().frame < oldest->front().frame)) {
                oldest = &list;
            }
        }
        if (oldest == nullptr) {
            break;
        }
        Doom(std::move(oldest->front()));
        oldest->pop_front();
    }
    PublishGauges();
    return true;
}

void BufferRecycler::Doom(Entry&& entry) {
    held_bytes -= entry.bytes;
    --held_entries;
    doomed_bytes += entry.bytes;
    AddTotals(-static_cast<s64>(entry.bytes), -1, static_cast<s64>(entry.bytes));
    VideoCommon::BufferChurnStats::PoolAdd(GroupCounters(entry.family).evicted);
    doomed.push_back(std::move(entry));
    doomed_count.store(doomed.size(), std::memory_order_relaxed);
}

void BufferRecycler::TickFrame() {
    {
        std::scoped_lock lock{mutex};
        ++frame;
        if (held_entries != 0) {
            // EXP104 off live: everything goes (still only once the GPU finished with it).
            const int exp104 = VideoCore::Nx2Debug::Experiment(104);
            const u64 age = exp104 > 1 ? static_cast<u64>(exp104) : DefaultAgeFrames;
            for (std::deque<Entry>& list : lists) {
                while (!list.empty() && (exp104 <= 0 || frame - list.front().frame > age)) {
                    Doom(std::move(list.front()));
                    list.pop_front();
                }
            }
            PublishGauges();
        }
    }
    if (HasDoomed()) {
        // The split's idle hook destroys them on the back when it runs out of work (EXP99's
        // way). Without the split nothing would: destroy here. A back that never idles still
        // keeps at most MaxDoomedBytes waiting.
        DestroyDoomed(VideoCommon::GpuSplit::OnBackThread() ? MaxDoomedBytes : 0);
    }
}

size_t BufferRecycler::DestroyDoomed(u64 keep_bytes) {
    std::deque<Entry> destroy;
    {
        std::scoped_lock lock{mutex};
        // keep_bytes counts every recycler's doomed bytes (one cap for all).
        while (!doomed.empty() &&
               g_total_doomed_bytes.load(std::memory_order_relaxed) > keep_bytes &&
               clock.IsFree(doomed.front().tick)) {
            doomed_bytes -= doomed.front().bytes;
            AddTotals(0, 0, -static_cast<s64>(doomed.front().bytes));
            destroy.push_back(std::move(doomed.front()));
            doomed.pop_front();
        }
        doomed_count.store(doomed.size(), std::memory_order_relaxed);
        PublishGauges();
    }
    const size_t count = destroy.size();
    if (count != 0) {
        Common::Nx2Timeline::Span span{"back: buffer pool destroy"};
        destroy.clear();
        VideoCommon::BufferChurnStats::PoolAdd(
            VideoCommon::BufferChurnStats::GetPool().destroyed_here, count);
    }
    return count;
}

void BufferRecycler::DestroyIdle(u32 max) {
    for (u32 i = 0; i < max; ++i) {
        vk::Buffer buffer;
        {
            std::scoped_lock lock{mutex};
            if (doomed.empty() || !clock.IsFree(doomed.front().tick)) {
                return;
            }
            doomed_bytes -= doomed.front().bytes;
            AddTotals(0, 0, -static_cast<s64>(doomed.front().bytes));
            buffer = std::move(doomed.front().buffer);
            doomed.pop_front();
            doomed_count.store(doomed.size(), std::memory_order_relaxed);
            PublishGauges();
        }
        Common::Nx2Timeline::Span span{"back: idle buffer pool destroy"};
        buffer.reset();
        VideoCommon::BufferChurnStats::PoolAdd(
            VideoCommon::BufferChurnStats::GetPool().destroyed_idle);
    }
}

size_t BufferRecycler::DropAll() {
    {
        std::scoped_lock lock{mutex};
        for (std::deque<Entry>& list : lists) {
            while (!list.empty()) {
                Doom(std::move(list.front()));
                list.pop_front();
            }
        }
    }
    return DestroyDoomed(0);
}

void BufferRecycler::PublishGauges() noexcept {
    VideoCommon::BufferChurnStats::Pool& pool = VideoCommon::BufferChurnStats::GetPool();
    pool.held_bytes.store(g_total_held_bytes.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    pool.held_entries.store(g_total_held_entries.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    pool.doomed_bytes.store(g_total_doomed_bytes.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
}

} // namespace Vulkan
