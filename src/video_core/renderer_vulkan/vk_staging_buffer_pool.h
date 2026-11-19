// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <climits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

#include "video_core/buffer_churn_stats.h"
#include "video_core/renderer_vulkan/vk_buffer_recycler.h"
#include "video_core/renderer_vulkan/vk_staging_ring.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct StagingBufferRef {
    VkBuffer buffer;
    VkDeviceAddress device_address;
    VkDeviceSize offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 log2_level;
    u64 index;
};

class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;

    /// What this pool's stream buffer carries, for its boot log line.
    enum class StreamRole {
        Everything, ///< one pool for everything (no split allocators)
        Transfers,  ///< the texture cache's and the back's uploads: only ever copied from
        Buffers,    ///< the buffer cache's: uniforms the shaders read straight from it, uploads
    };

    /// `stream_divisor`: the stream buffer is its usual size divided by this.
    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler, u32 stream_divisor = 1,
                               StreamRole stream_role = StreamRole::Everything);
    ~StagingBufferPool();

    /// `caller`: who asks, counted when a buffer has to be created (EDEN_NX2_BUFFER_CHURN_STATS).
    StagingBufferRef Request(
        size_t size, MemoryUsage usage, bool deferred = false,
        VideoCommon::BufferChurnStats::StagingCaller caller =
            VideoCommon::BufferChurnStats::StagingCaller::Other);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    void TickFrame();

    /// Out of memory: frees every staging buffer the GPU is done with and the recycler's pooled
    /// buffers. Returns how many went.
    size_t ReleaseIdleBuffers();

    /// Bytes of the cached staging buffers of a kind, idle or in use (EDEN_NX2_MEM_TRACE).
    [[nodiscard]] u64 CachedBytes(MemoryUsage usage);

    /// The stream buffer's size (the "Memory owners" line).
    [[nodiscard]] u64 StreamBufferBytes() const noexcept {
        return stream_buffer_size;
    }

    /// EXP104's pool, shared with the buffer cache that uses this staging pool.
    [[nodiscard]] BufferRecycler& Recycler() noexcept {
        return recycler;
    }

    /// The buffer cache's clock for this pool and its recycler, before any request: with
    /// FRITCHII_MEMSTAGE_TICKS their stamps are packet numbers.
    void UseClock(const SeamClock& clock_) {
        clock = clock_;
        recycler.UseClock(clock_);
    }

    /// The buffer cache's own pool (FRITCHII_MEMSTAGE_SPLIT_ALLOC): its requests are checked
    /// against the cache's user (memory stage spec 5.1).
    void MarkCacheSide() noexcept {
        cache_side = true;
    }

    /// FRITCHII_STAGING_RING (boot size, live use, default 0; 1 = 256 MiB, N = N MiB, 16-1024):
    /// one host-memory upload buffer, made and written once at boot, that uploads too big for the
    /// stream buffer take before a new staging buffer is made. Called once, for the texture
    /// cache's pool.
    void CreateUploadRing();

private:
    bool cache_side{};
    struct StreamBufferCommit {
        size_t upper_bound;
        u64 tick;
    };

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 tick = 0;

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        // Buffers handed out deferred, by index, until FreeDeferred gives them back: kept out of
        // entries so lookups and FreeDeferred don't scan them.
        std::unordered_map<u64, StagingBuffer> deferred;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    /// The two ends of the stream's busy check: true when the request must spill to a staging
    /// buffer. With the small BAR plan the GPU's tick is read again first.
    bool StreamRegionsBusy(size_t region_begin, size_t region_end, size_t size);
    /// FRITCHII_LINK_STATS: this pool's stream use, one line every 10 s.
    void LogStreamUse();

    /// Request without the link accounting.
    StagingBufferRef RequestBuffer(size_t size, MemoryUsage usage, bool deferred,
                                   VideoCommon::BufferChurnStats::StagingCaller caller);
    /// FRITCHII_LINK_STATS / FRITCHII_SIM_LINK: how this request's bytes cross the link.
    void NoteLink(const StagingBufferRef& ref, size_t size, MemoryUsage usage,
                  VideoCommon::BufferChurnStats::StagingCaller caller) const;

    StagingBufferRef GetStreamBuffer(size_t size,
                                     VideoCommon::BufferChurnStats::StagingCaller caller);

    struct UploadRing {
        vk::Buffer buffer;
        VkDeviceAddress address{};
        std::span<u8> mapped;
        StagingRingRegions regions;
        // The 10 s line.
        u64 uploads{};
        u64 bytes{};
        u64 refused{};
        u64 refused_behind_large{};
        u64 large{};
        u64 created_after_refusal{};
        s64 last_log_ns{};
    };

    /// A slice of the upload ring, when the regions it needs are idle.
    std::optional<StagingBufferRef> TryGetRingBuffer(size_t size);
    void LogUploadRing();

    bool AreRegionsActive(size_t region_begin, size_t region_end) const;

    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred,
                                      VideoCommon::BufferChurnStats::StagingCaller caller,
                                      bool stream_full = false);

    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);

    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred,
                                         VideoCommon::BufferChurnStats::StagingCaller caller,
                                         bool stream_full);

    StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(MemoryUsage usage);

    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);

    /// EXP104: offers an idle staging buffer being released to the pool (left empty if taken).
    void Retire(StagingBuffer& entry);
    size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    const Device& device;
    MemoryAllocator& memory_allocator;
    SeamClock clock;
    BufferRecycler recycler;

    vk::Buffer stream_buffer;
    bool stream_in_device_memory{}; ///< the CPU's writes into it cross the link
    bool crosses_link{};            ///< a discrete GPU: host and device memory are apart
    bool refresh_before_spill{};    ///< FRITCHII_SMALL_BAR_STREAMS
    StreamRole role{};

    /// FRITCHII_LINK_STATS. Requests may come from another thread than TickFrame's.
    struct StreamUse {
        std::atomic<u64> bytes{};
        std::atomic<u64> requests{};
        std::atomic<u64> frame_bytes{};
        std::atomic<u64> peak_frame_bytes{};
        std::atomic<u64> wraps{};
        std::atomic<u64> full{};
        std::atomic<u64> full_bytes{};
        std::atomic<u64> full_was_free{};
        std::atomic<u64> too_big{};
        std::atomic<u64> too_big_bytes{};
        size_t peak_busy_regions{};
        std::chrono::steady_clock::time_point window_start{};
    };
    mutable StreamUse stream_use;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;
    VkDeviceSize region_size;

    size_t iterator = 0;
    size_t used_iterator = 0;
    size_t free_iterator = 0;
    std::array<u64, NUM_SYNCS> sync_ticks{};

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 buffer_index = 0;
    u64 unique_ids{};

    std::optional<UploadRing> upload_ring;
};

} // namespace Vulkan
