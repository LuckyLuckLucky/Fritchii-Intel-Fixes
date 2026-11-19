// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "video_core/query_cache/query_cache_base.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
namespace VideoCore {
class RasterizerInterface;
}

namespace VideoCommon {
class StreamerInterface;
}

namespace Vulkan {
class Device;
class Scheduler;
class StagingBufferPool;

struct QueryCacheRuntimeImpl;

/// One follower (fable_b 16.2): a buffer E obtained at a posted fence's place for the query
/// values R syncs there (SyncValues copies into it, never entering the buffer cache).
struct OneFollowerSyncTarget {
    u64 begin;     ///< device address of the run's first byte (page aligned)
    u64 bytes;     ///< the run's bytes (whole pages)
    VkBuffer buffer;
    u64 offset;    ///< the run's offset in `buffer`
    bool payload;  ///< only payload releases' pages (a superset: not counted unused)
    bool used;
};
/// R, inside a posted fence: the op's targets (null elsewhere).
inline thread_local std::vector<OneFollowerSyncTarget>* t_onef_sync_targets = nullptr;

class QueryCacheRuntime {
public:
    explicit QueryCacheRuntime(VideoCore::RasterizerInterface* rasterizer,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Vulkan::BufferCache& buffer_cache_, const Device& device_,
                               const MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                               StagingBufferPool& staging_pool_,
                               ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                               DescriptorPool& descriptor_pool, TextureCache& texture_cache_);
    ~QueryCacheRuntime();

    template <typename SyncValuesType>
    void SyncValues(std::span<SyncValuesType> values, VkBuffer base_src_buffer = VkBuffer{});

    void Barriers(bool is_prebarrier);

    /// FRITCHII_OCCL_FLAG (fritchii_occl_flag.h): the open query slot's flag address for a draw
    /// drawn with a flag pipeline (0: no flag area), and a counted draw drawn without one.
    u64 OcclFlagAddress();
    void NoteOtherCountedDraw(bool blocks = true);
    /// FRITCHII_OCCL_FLAG pre-pass: a slot with a host query is open (the pre-pass must not be
    /// counted by it: pause before it, resume after it, a new slot of the same window).
    bool OcclQuerySlotOpen();
    void OcclPrepassPause();
    void OcclPrepassResume();
    bool OcclFlagReady();
    void SetNextFlagDraw(bool flag);

    void EndHostConditionalRendering();

    void PauseHostConditionalRendering();

    void ResumeHostConditionalRendering();

    bool HostConditionalRenderingCompareValue(VideoCommon::LookupData object_1, bool qc_dirty);

    bool HostConditionalRenderingCompareValues(VideoCommon::LookupData object_1,
                                               VideoCommon::LookupData object_2, bool qc_dirty,
                                               bool equal_check);

    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType query_type);

    void Bind3DEngine(Tegra::Engines::Maxwell3D* maxwell3d);

    template <typename Func>
    void View3DRegs(Func&& func);

private:
    void TraceConditionalRendering(const char* path, bool handled, DAddr address, DAddr address_2 = 0);
    void HostConditionalRenderingCompareValueImpl(VideoCommon::LookupData object, bool is_equal);
    void HostConditionalRenderingCompareBCImpl(DAddr address, bool is_equal,
                                               bool compare_to_zero = false);
    friend struct QueryCacheRuntimeImpl;
    std::unique_ptr<QueryCacheRuntimeImpl> impl;
};

struct QueryCacheParams {
    using RuntimeType = typename Vulkan::QueryCacheRuntime;
};

using QueryCache = VideoCommon::QueryCacheBase<QueryCacheParams>;

} // namespace Vulkan
