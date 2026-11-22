// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <stop_token>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/fritchii_occl_lean.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;
class TranslateQueue;
struct TranslationJob;
struct TranslationRecord;

namespace PrecompiledKept {
class Index;
struct Record;
} // namespace PrecompiledKept

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    std::string failure;
    /// Pointer slots left unbound at translation: null or unmapped plain slots and pointer-table
    /// entries (see UnboundStoragePointerSlots). One that later holds a mapped address makes the
    /// translation stale (CurrentComputePipeline rebuilds it).
    std::vector<Shader::StoragePointerSlot> unbound_pointer_slots;
    /// Constant buffer slots bound as storage buffer pointers (see MappedStoragePointerKeys)
    std::vector<u64> mapped_pointers;
    u32 rebuilds{};
    /// A rebuild still compiling: dispatches keep this entry's pipeline until it is ready.
    std::unique_ptr<ComputePipelineCacheEntry> pending;
    /// GPU thread: frame_count at the last dispatch through this entry (EXP51 eviction)
    u64 last_use_frame{};
    /// Whether a dispatch went through it this session (a boot build may never be dispatched)
    bool dispatched{};

    [[nodiscard]] ComputePipeline* Get(bool native, GPUVAddr address,
                                       const ComputePipelineCacheKey& key) const;
};

/// EDEN_NX2_ASYNC_DISCARD, read on first use: what a native draw does while its pipeline is
/// missing. 0 waits for every pipeline, 1 (the default) skips by the legacy heuristic alone, 2
/// skips only draws into render targets the frame clears and redraws (fable_answer_18 1b).
/// Non-native draws keep the legacy heuristic in every mode.
[[nodiscard]] int AsyncDiscardMode();

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    /// FRITCHII_OCCL_LEAN (helper CS, fritchii_occl_lean.h; E, right after CurrentGraphicsPipeline
    /// gave `full` for the draw of `regs`): the same pipeline without its pixel program when the
    /// draw writes no colour and that program can change nothing else, once made and built; else
    /// `full`. `mode` is the switch (1 to 3); `note` says whether the draw is timed and lean.
    [[nodiscard]] GraphicsPipeline* OcclLeanPipeline(GraphicsPipeline* full, const Maxwell& regs,
                                                     int mode, VideoCommon::OcclLean::DrawNote& note);

    /// FRITCHII_OCCL_FLAG (fritchii_occl_flag.h; the recording thread, right after the draw's
    /// pipeline is known, one follower off): for a counted draw that writes no colour and whose
    /// pixel program can change nothing (OCCL_LEAN's Lean class), `pipeline`'s flag variant once
    /// made and built; else `pipeline`. A flag variant passed in (a kept pipeline) is first
    /// turned back into its own pipeline, so a flag variant never draws another draw.
    [[nodiscard]] GraphicsPipeline* OcclFlagPipeline(GraphicsPipeline* pipeline,
                                                     const Maxwell& regs, bool allow_make);

    /// One follower (fable_b 18.4, E, right after a lookup): the key parts the lookup kept
    /// because their flag was clear, computed fresh from the registers and compared: bit 0 the
    /// programs (with `shaders_clear`), 1 the blend (`blending_clear`), 2 the vertex input
    /// (`vertex_input_clear`). Leaves the engine's flags as they were.
    [[nodiscard]] u32 OneFollowerLookupCheck(bool shaders_clear, bool blending_clear,
                                             bool vertex_input_clear);
    /// Whether the last CurrentGraphicsPipeline returned null because its draw was skipped for an
    /// unbuilt pipeline (asynchronous shaders), not because there is no pipeline to build.
    [[nodiscard]] bool LastLookupSkipped() const noexcept {
        return discard_decision.serial == lookup_serial && discard_decision.skip;
    }

    /// The device features FixedPipelineState::Refresh builds keys with (one follower's verify,
    /// vk_one_follower_draw.cpp, builds a key from each engine with them).
    /// graphics_cache's entries (one follower: a played draw's lookup that adds one missed).
    [[nodiscard]] size_t GraphicsEntries() const noexcept {
        return graphics_cache.size();
    }

    [[nodiscard]] const DynamicFeatures& Features() const noexcept {
        return dynamic_features;
    }

    /// EXP118 (the delta path, fritchii_delta_path.h): what a lookup now would give a draw that
    /// follows `pipeline`'s draw with only light state changed (the delta draw count's serial).
    enum class DeltaLookup : u8 {
        Keeps,       ///< The lookup would return `pipeline`
        NotCurrent,  ///< The cache's current pipeline is another one
        Inputs,      ///< The programs or viewport swizzles are marked: the lookup reads them again
        VertexInput, ///< Vertex input moved: static vertex input, or other keyed attribute types
    };
    /// Keeps when `pipeline` is the current pipeline, no flag that only the lookup clears is
    /// raised, and with Dirty::VertexInput raised the pipeline takes its vertex input dynamically
    /// and the attribute types its programs read are its key's. `apply` (the draw takes no
    /// lookup): leaves the types as that lookup would. The draw's vertex input update clears the
    /// flag, and RefreshGraphicsKey rewrites the unmasked types only while it is raised.
    [[nodiscard]] DeltaLookup DeltaKeeps(const GraphicsPipeline& pipeline, bool apply);

    void TickFrame();

    /// What the "Memory owners" log line counts here (GPU thread).
    struct MemoryOwnerCounts {
        size_t graphics_built{};       ///< Built graphics pipelines (each holds driver memory)
        size_t graphics_never_used{};  ///< Of them, boot builds no draw has used yet
        size_t graphics_entries{};     ///< graphics_cache entries, unbuilt ones included
        size_t compute{};              ///< Compute pipelines, pending rebuilds included
        size_t compute_never_dispatched{}; ///< Of them, boot builds no dispatch has used yet
        size_t awaiting_destruction{}; ///< Evicted or retired, destroyed once the GPU is done
        u64 cache_data_bytes{};        ///< vkGetPipelineCacheData's size, at most a minute old
    };
    [[nodiscard]] MemoryOwnerCounts MemoryOwners();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    /// FRITCHII_ASYNC_COMPUTE_SKIP: waits for `pipeline` at most what is left of this frame's
    /// FRITCHII_PIPELINE_WAIT_BUDGET (shared with the draws; 0 = no wait). True when built by
    /// then. GPU thread.
    [[nodiscard]] bool WaitComputeWithinBudget(ComputePipeline& pipeline);

    /// One follower (FRITCHII_ONEF_OP_DISPATCH, E with FRITCHII_ONEF_E_PIPELINE): the compute
    /// lookup when it is a plain hit: the shader known, its entry built before, no rebuild
    /// pending or due, nothing to create or retire. Then it does what CurrentComputePipeline
    /// does for a hit (ComputeShader's checks and hooks included, which may throw) and returns
    /// the pipeline. Anything else returns nullptr having changed nothing: the dispatch is lent
    /// and the lent call runs CurrentComputePipeline as before.
    [[nodiscard]] ComputePipeline* OneFollowerComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

    /// What a graphics warm-up entry came to: built, not buildable (a guess the translation
    /// rejects, which another warm-up would reject too), or refused by the driver.
    enum class WarmResult { Built, Unbuildable, DriverFailed };

    /// Graphics pipeline libraries (EDEN_NX2_GPL): builds the pre-rasterization and fragment
    /// shader libraries of the pipeline `key` would make, on the calling thread, with no draw and
    /// no pipeline kept (for a warm-up). `pools` must be the calling thread's own. Unbuildable
    /// when the device has no libraries or the pipeline takes the monolithic route, DriverFailed
    /// when a library build failed.
    /// The key may hold a single stage (no partner): a vertex-only key builds just that vertex
    /// program's pre-rasterization library (vertex SPIR-V does not depend on the fragment
    /// stage); a fragment-only key builds just the fragment library, emitted as if the partner
    /// writes every varying (the common variant). The key's state (attribute types, render
    /// target formats, depth and raster state) is what the libraries are keyed and built with.
    WarmResult WarmGraphicsLibraries(ShaderPools& pools, const GraphicsPipelineCacheKey& key,
                                     std::span<Shader::Environment* const> envs);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    /// current_pipeline->Next(graphics_key), with FRITCHII_PIPELINE_KEY_HASH (live, default 0):
    /// 1 compares the keys' fingerprints first (GraphicsPipeline::NextHashed); 2 runs both and
    /// counts the answers that differ (must be 0), keeping Next's. One counts line every 10 s.
    [[nodiscard]] GraphicsPipeline* NextGraphicsPipeline();
    struct KeyHashStats {
        u64 calls{};
        GraphicsPipeline::NextCounts counts{};
        u64 verified{};
        u64 differ{};
        u64 differ_logged{};
        std::chrono::steady_clock::time_point last_log{};
    };
    KeyHashStats key_hash_stats;

    /// FixedPipelineState::Refresh of graphics_key, keeping only the vertex attribute types the
    /// bound programs read (see VideoCommon::KeyedAttributeTypes), and no vertex input for a
    /// native task/mesh draw (ClearVertexInput, FRITCHII_MESH_KEY_VERTEX_INPUT).
    void RefreshGraphicsKey();
    /// RefreshGraphicsKey without the task/mesh part.
    void RefreshGraphicsKeyState();

    /// EDEN_NX2_PIPELINE_CLEAN / EDEN_NX2_EXP19: CurrentGraphicsPipeline without
    /// FixedPipelineState::Refresh nor the key comparison while nothing Refresh reads changed
    /// since the last lookup. `patch`: EXP19=2. With EXP50 the blend registers are compared by
    /// what they make (FixedPipelineState::BlendingMatches), not word for word. With EXP52 a
    /// change of programs alone keys only the attribute types again and looks the key up.
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineClean(bool patch);
    /// The registers Refresh reads with these features, as (first word, words) ranges.
    /// `blend_by_value` (EXP50): the blend registers go to their own ranges.
    /// `vi_static` (INTEL_FORK_VI_STATIC_PATCH, static vertex input only): the vertex input
    /// registers go to their own ranges (vi_ranges), patched instead of refreshed.
    void BuildCleanWatch(bool patch, bool blend_by_value, bool patch_vertex, bool vi_static);
    [[nodiscard]] bool CleanInputsUnchanged() const;
    void TakeCleanSnapshot();
    [[nodiscard]] bool CleanBlendUnchanged() const;
    void TakeCleanBlendSnapshot();
    [[nodiscard]] bool CleanViUnchanged() const;
    void TakeCleanViSnapshot();
    /// INTEL_FORK_VI_STATIC_PATCH: the vertex input part of FixedPipelineState::Refresh without
    /// dynamic vertex input (attributes, divisors, enabled_divisors under a raised
    /// Dirty::VertexInput, which it clears; strides without extended dynamic state; the static
    /// vertex repack), in graphics_key, nothing else.
    void PatchStaticVertexInput();

    struct CleanLookup {
        struct Range {
            u32 first;
            u32 count;
        };
        bool built{};
        bool patch{};                  ///< Built for CurrentGraphicsPipelineClean(true)
        bool blend_by_value{};         ///< Built for EXP50: blend registers in blend_ranges
        bool patch_vertex{};           ///< Built for EXP55: vertex attribute formats left out
        bool vi_static{};              ///< Built for INTEL_FORK_VI_STATIC_PATCH: vi_ranges
        std::vector<Range> ranges;     ///< Read by every Refresh
        /// With vi_static: the vertex input registers (formats, instancing, divisors, strides).
        std::vector<Range> vi_ranges;
        std::vector<u32> vi_snapshot;  ///< vi_ranges' words the key's vertex input was made from
        u64 vi_patched{};              ///< INTEL_FORK_VI_STATIC_PATCH: lookups patched
        u64 vi_full{};                 ///< ... and full Refreshes, while the switch is on
        u64 vi_mismatches{};           ///< =2: patched keys that differ from Refresh's (must be 0)
        u32 vi_mismatches_logged{};
        std::chrono::steady_clock::time_point vi_last_log{};
        std::vector<Range> xfb_ranges; ///< Read while transform feedback is enabled
        /// With blend_by_value: the registers the attachments and the dual-source bit come from.
        std::vector<Range> blend_ranges;
        std::vector<u32> snapshot;     ///< ranges' words at the last Refresh
        std::vector<u32> xfb_snapshot;
        /// blend_ranges' words the key's attachments and dual-source bit were last made from (or
        /// proved equal to, by value).
        std::vector<u32> blend_snapshot;
        std::array<u64, 6> hashes{};   ///< graphics_key.unique_hashes at the last Refresh
        const Tegra::Engines::Maxwell3D* maxwell3d{};
        Maxwell::PrimitiveTopology topology{};
        Tegra::Engines::Maxwell3D::EngineHint engine_state{};
        /// current_pipeline when its key was graphics_key after the last Refresh, else null.
        GraphicsPipeline* pipeline{};
        u64 hits{};
        u64 patches{};
        u64 misses{};
        /// Hits the old check would have refreshed: blend registers or Dirty::Blending moved
        /// while the state they make stayed the same (EXP50).
        u64 blend_saved{};
        /// Hits whose programs changed while every other Refresh input stood: only the attribute
        /// types were keyed again, then Next looked the new key up (EXP52).
        u64 programs_only{};
        /// EXP52=2: programs-only keys checked against a full Refresh, and those that differed.
        u64 programs_verified{};
        u64 programs_mismatches{};
        u32 programs_mismatches_logged{}; ///< For the whole session: the first 16 are logged
        /// Why lookups refreshed: pipeline changed or unknown, programs, Dirty::Blending,
        /// other flags / topology / engine hint / dual-source bit, watched registers. With EXP52
        /// on, programs count only when nothing else is in the way, which never refreshes.
        std::array<u64, 5> miss_reasons{};
        /// miss_reasons[3] ("other state") by its first differing part: vertex input, viewport
        /// swizzles, topology, engine state, dual-source blend.
        std::array<u64, 5> other_parts{};
        /// FRITCHII_ONEF_KEY_KEEP (fable_b 23.1 cut 2): OneFollower::g_key_writes when the
        /// snapshots last equalled the registers (~0: unknown); lookups that found it unmoved,
        /// those that kept the compare out (=2), and those whose compare then failed (=1, must
        /// be 0).
        u64 key_seen{~u64{0}};
        u64 key_untouched{};
        u64 key_skipped{};
        u64 key_failed{};
        u64 key_failed_session{};
        std::chrono::steady_clock::time_point last_log{};
    } clean;

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    /// INTEL_FORK_ASYNC_MODE (Intel's Windows driver only, Off elsewhere; set by the
    /// constructor): dry, lichii or lichii-bounded hand every unbuilt pipeline to
    /// LichiiBuiltPipeline instead of DrawMayBeSkipped / WaitBudgetSkips.
    IntelForkAsyncMode async_mode{IntelForkAsyncMode::Off};
    /// Port of Lichii build-386 PipelineCache::BuiltPipeline (L:vk_pipeline_cache.cpp:2720-2799)
    /// for an unbuilt `pipeline`: the pipeline (the draw waits) or nullptr (the draw is skipped).
    /// Dry always returns the pipeline. GPU thread (the thread that looks pipelines up).
    [[nodiscard]] GraphicsPipeline* LichiiBuiltPipeline(GraphicsPipeline* pipeline) const noexcept;
    /// Lichii's verdict on the current draw (Skip, or why it waits).
    [[nodiscard]] LichiiDrawClass ClassifyLichiiDraw() const;
    /// lichii-bounded: the wait budget left in frame `frame` (Lichii's native_wait_budget_us,
    /// reset each frame).
    struct LichiiBudget {
        u64 frame{~u64{0}};
        s64 left_us{};
    };
    mutable LichiiBudget lichii_budget;

    /// Whether the current draw may be skipped while its pipeline is not ready (native draws
    /// unless EDEN_NX2_ASYNC_DISCARD=0, and the legacy heuristic; full-screen passes never; with
    /// EDEN_NX2_ASYNC_DISCARD=2 only into recurring render targets). The one test BuiltPipeline
    /// and the translate queue both ask, answered once per lookup.
    [[nodiscard]] bool DrawMayBeSkipped() const;
    /// FRITCHII_ASYNC_REDRAWN when unset: 1 on AMD's and Intel's Windows drivers, else 0.
    [[nodiscard]] int AsyncRedrawnDefault() const;
    /// FRITCHII_ASYNC_REDRAWN_DEPTH (boot): depth pre-pass draws into redrawn targets count as
    /// recurring too. 1 by default on Intel's Windows driver.
    [[nodiscard]] int AsyncRedrawnDepthMode() const;
    /// FRITCHII_PIPELINE_WAIT_BUDGET (boot): ms a frame that depth-tested draws may wait for
    /// unbuilt pipelines together before they are skipped. 50 by default on Intel's Windows
    /// driver, 0 (off) elsewhere.
    [[nodiscard]] std::chrono::milliseconds PipelineWaitBudget() const;
    /// FRITCHII_PIPELINE_WAIT_BUDGET: whether a draw DrawMayBeSkipped keeps waiting for `pipeline`
    /// is skipped instead (after waiting what is left of the frame's budget). GPU thread.
    [[nodiscard]] bool WaitBudgetSkips(const GraphicsPipeline& pipeline) const;
    struct WaitBudgetState {
        u64 frame{~u64{0}};
        std::chrono::nanoseconds used{};
        /// Pipelines whose budget wait ran out: skipped at once until built (cleared every 10 s;
        /// a stale address costs at most a skip of an unbuilt pipeline, never a wrong draw).
        std::unordered_set<const GraphicsPipeline*> timed_out;
        /// 10 s counts: built within the wait, skipped by class of target (index:
        /// DrawTargetClass), of them at once (already timed out or budget spent)
        u32 waited_built{};
        std::array<u32, 3> skipped{};
        u32 skipped_at_once{};
        std::chrono::nanoseconds wait_time{};
        std::chrono::nanoseconds longest_frame{};
        u32 listed{};
    };
    mutable WaitBudgetState wait_budget;
    /// FRITCHII_ASYNC_REDRAWN's value: set live or in the environment, else AsyncRedrawnDefault
    /// or async_redrawn_auto.
    [[nodiscard]] bool AsyncRedrawnOn() const;
    /// Set once this session's prepared pipelines show the driver compiling them again
    /// (ReportKeptDriverCompiles): FRITCHII_ASYNC_REDRAWN on while it is not set.
    std::atomic<bool> async_redrawn_auto{};
    /// Counts CurrentGraphicsPipeline calls, so DrawMayBeSkipped classifies the targets once per
    /// lookup however often a lookup asks.
    u64 lookup_serial{};
    struct DiscardDecision {
        u64 serial{~u64{0}};
        bool skip{};
        /// The draw's targets as DrawMayBeSkipped classified them (FRITCHII_PIPELINE_WAIT_BUDGET)
        VideoCommon::DrawTargets targets{};
    };
    mutable DiscardDecision discard_decision;
    /// Counts one decision for the 10 s line, and lists a persistent target the first time a
    /// draw into it misses its pipeline. GPU thread.
    void NoteDiscardDecision(bool skip, const VideoCommon::DrawTargets& targets) const;
    /// TickFrame: the EDEN_NX2_ASYNC_DISCARD counts of the last 10 s.
    void ReportAsyncDiscard();
    /// EDEN_NX2_ASYNC_DISCARD counts (fable_answer_18 1c), GPU thread only. Lookups by class of
    /// their targets (index: VideoCommon::DrawTargetClass), a lookup that repeats the one before
    /// (same pipeline key and target, same frame: the rasterizer's second lookups) counted once.
    struct DiscardStats {
        std::array<u32, 3> skipped{};
        std::array<u32, 3> waited{};
        /// Skipped lookups FRITCHII_ASYNC_REDRAWN let through (counted in skipped's persistent
        /// column too; every lookup, repeats included)
        u32 skipped_redrawn{};
        /// Of them, depth pre-pass draws (FRITCHII_ASYNC_REDRAWN_DEPTH)
        u32 skipped_redrawn_depth{};
        /// GPU thread time in the translate queue's capped Wait for a draw that waits
        std::chrono::steady_clock::duration queue_wait{};
        std::chrono::nanoseconds held_at_window{}; ///< GraphicsPipeline::HoldTimeTotal then
        std::chrono::steady_clock::time_point window{};
        // The last lookup counted.
        GraphicsPipelineCacheKey last_key{};
        u64 last_frame{~u64{0}};
        GPUVAddr last_target{};
        VideoCommon::DrawTargetClass last_kind{};
        bool last_skip{};
    };
    mutable DiscardStats discard_stats;
    /// Persistent targets listed so far (32 at most per session).
    mutable std::unordered_set<GPUVAddr> listed_persistent_targets;

    /// With `keep_pointers`, the slots an evicted pipeline of this key bound stay storage buffer
    /// pointers whatever they hold now (ported).
    /// `store`: false keeps it out of the disk cache (FRITCHII_OCCL_LEAN's siblings).
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        const VideoCommon::EvictedPointerSlots::Slots* keep_pointers = nullptr,
        bool store = true);

    /// FRITCHII_OCCL_LEAN: `full`'s key without the pixel program, found in graphics_cache or
    /// translated now (one a frame) from the draw's environments; null when not available yet.
    /// Sets full.occl_class to Failed when it cannot be made.
    GraphicsPipeline* FindOrMakeOcclSibling(GraphicsPipeline& full);
    /// FRITCHII_OCCL_FLAG: full's flag variant, translated now (one a frame, shared with the
    /// lean siblings) from the draw's environments; null when not available yet. Sets
    /// full.occl_flag_state to 2 when it cannot be made.
    /// `kind`: 1 host flag program, 2 colour, 3 pre-pass (GraphicsPipeline::occl_flag_kind).
    GraphicsPipeline* FindOrMakeOcclFlag(GraphicsPipeline& full, u8 kind = 1);
    /// FRITCHII_OCCL_LEAN: frame_count of the last sibling translation (one a frame).
    u64 occl_made_frame{~u64{0}};

    /// The pointer slots kept for the current key (evicted_pointer_slots), null when none
    [[nodiscard]] const VideoCommon::EvictedPointerSlots::Slots* KeptPointers() const;
    /// Answers the kept slots of each program as storage buffer pointers (KeepStoragePointers)
    static void KeepPointers(std::span<VideoCommon::GraphicsEnvironment> envs,
                             const GraphicsPipelineCacheKey& key,
                             const VideoCommon::EvictedPointerSlots::Slots* keep_pointers);
    /// Logs a kept pointer slot (an index of keep_pointers) that this translation would otherwise
    /// have classified as no pointer: null, unmapped, or out of range right now.
    void WarnKeptPointer(size_t program, u64 slot) const;

    /// Stores a graphics pipeline built at run time in the disk cache (unless it is there and
    /// loads), with the environments it was translated from. GPU thread.
    void StoreRuntimeGraphicsPipeline(
        const GraphicsPipelineCacheKey& key,
        const std::array<VideoCommon::GraphicsEnvironment, Maxwell::MaxShaderProgram>& envs);

    /// EDEN_NX2_ASYNC_TRANSLATE=1/2 (translate_queue set). Queues the current key's translation
    /// from snapshot environments when the draw may be skipped; false when it is to be built on
    /// the GPU thread instead. `keep_pointers` as CreateGraphicsPipeline's.
    bool EnqueueTranslation(const VideoCommon::EvictedPointerSlots::Slots* keep_pointers = nullptr);
    /// A later lookup of a key whose entry is empty: its translated pipeline when ready; `pending`
    /// when the draw is to be skipped meanwhile. Waits (capped) or builds here for a draw that
    /// must not be skipped.
    std::unique_ptr<GraphicsPipeline> AdoptTranslation(bool& pending);
    /// What a finished job gives: its pipeline once its texture reads check out on the GPU
    /// thread. `retry` when it was not usable (to be translated again, or built here).
    std::unique_ptr<GraphicsPipeline> AdoptJob(std::unique_ptr<TranslationJob> job, bool& retry);
    /// The translation itself, on a translation thread.
    void TranslateOnWorker(TranslationJob& job);
    /// Between frames: adopts finished jobs whose draws did not come back, sweeps wedged ones
    /// and logs the counts every 10 s.
    void TickTranslations();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        std::unique_ptr<EmissionBudget::Reservation> reservation = nullptr,
        bool libraries_only = false, TranslationRecord* record = nullptr);

    /// EDEN_NX2_PRECOMPILED_FAST: the current key's pipeline from the
    /// shader infos and SPIR-V the precompiled preparation kept, when the key is precompiled and
    /// the game's state answers the recorded environments' questions as they were answered
    /// (FileEnvironment::AnswersHoldFor): created on the GPU thread without translating, built on
    /// a worker from the kept SPIR-V, stored in the disk cache like a translated one. Null when
    /// the key is not precompiled or a check differs: the draw takes the normal path then.
    /// `keep_pointers` (an evicted key's) are answered as pointers before the check, as a
    /// translation would see them.
    std::unique_ptr<GraphicsPipeline> CreatePrecompiledGraphicsPipeline(
        const VideoCommon::EvictedPointerSlots::Slots* keep_pointers = nullptr);
    /// The compute counterpart: the first of the key's records (one per storage pointer state it
    /// was prepared in) whose answers hold for `env`. An entry without a pipeline otherwise.
    ComputePipelineCacheEntry CreatePrecompiledComputePipeline(const ComputePipelineCacheKey& key,
                                                               Shader::Environment& env);
    /// EDEN_NX2_PRECOMPILED_FAST=2: the stages translated from the game's state as the normal
    /// path would, on the GPU thread, compared with the kept SPIR-V and infos (true when they
    /// are the same). The programs are translated and emitted once, into audit_pools.
    bool AuditPrecompiled(const PrecompiledKept::Index& index, const PrecompiledKept::Record& record,
                          std::span<Shader::Environment* const> envs, bool compute,
                          const ComputePipelineCacheKey* compute_key);
    /// TickFrame: precompiled graphics pipelines whose build failed are dropped (their keys
    /// rejected), so their next draw translates them.
    void CheckPrecompiledBuilds();
    /// TickFrame: the fast path's counts of the last 10 s.
    void ReportPrecompiled();
    /// ReportPrecompiled: how many prepared pipelines the driver compiled again (cache misses).
    void ReportKeptDriverCompiles();
    /// ReportKeptDriverCompiles: async_redrawn_auto once the driver compiles prepared pipelines
    /// again (thresholds at the definition).
    void MaybeAutoAsyncRedrawn();
    struct {
        u64 builds{};
        u64 slow{};
        u64 long_builds{};
        std::chrono::nanoseconds longest{};
        bool warned{};
        bool auto_checked{};
    } kept_compiles_session;
    /// Publishes the kept file's index for the GPU thread (`kept`, or a finished `part` the
    /// preparation could not rename over it); false when there is none for `stamp`. With
    /// `check` (a file another build may have written) it is published only when
    /// CheckKeptSample finds it equal, and deleted otherwise.
    bool LoadKept(const std::filesystem::path& kept, const std::filesystem::path& part,
                  const std::filesystem::path& precompiled, const std::string& stamp, bool check);
    /// The boot check of a kept file: a sample of its pipelines (64 graphics, up to 16 compute,
    /// spread over the index) translated again from their recorded environments with this build
    /// and its settings, and compared with the kept SPIR-V and infos. True when all are equal.
    bool CheckKeptSample(const PrecompiledKept::Index& index, std::string_view file_name);
    /// FRITCHII_PREP_COMPUTE_CHECK: builds a few of the biggest kept compute pipelines the way a
    /// dispatch builds them (kept info and SPIR-V, same layout and flags) and times them. True
    /// when most took over the miss threshold: the driver's cache lost the prepared compute
    /// pipelines. `summary` gets the log text.
    bool PreparedComputeLost(const PrecompiledKept::Index& index, std::string& summary);
    [[nodiscard]] std::shared_ptr<const PrecompiledKept::Index> KeptIndex() const;
    /// What a kept file must have been written for, beyond the prepared stamp (which holds the
    /// release, driver and GPU): the cache version, the settings SPIR-V depends on, the library
    /// split, the OoT visual workaround and canonical keys. Not the build: CheckKeptSample.
    [[nodiscard]] std::string KeptStamp(const std::string& prepared_stamp) const;

    /// Set by LoadKept; read by the GPU thread when a new key is looked up.
    mutable std::mutex kept_mutex;
    std::shared_ptr<const PrecompiledKept::Index> kept_index;
    std::atomic<bool> kept_available{};
    /// GPU thread: keys whose precompiled build failed or whose audit differed.
    std::unordered_set<u64> precompiled_rejected;
    /// GPU thread: precompiled graphics pipelines not built yet (checked by TickFrame).
    std::vector<GraphicsPipelineCacheKey> precompiled_pending;
    /// EDEN_NX2_PRECOMPILED_FAST=2: the audit's own pools (nothing else holds IR in them).
    std::unique_ptr<ShaderPools> audit_pools;
    /// GPU thread counts for the 10 s line (the worker builds count in an atomic).
    struct PrecompiledStats {
        u32 graphics{};
        u32 compute{};
        u32 failed{};
        u32 not_precompiled{};
        u32 audited{};
        u32 audit_differ{};
        std::chrono::steady_clock::duration gpu_time{};
        std::chrono::steady_clock::duration gpu_worst{};
        std::vector<std::pair<const char*, u32>> misses; ///< By reason (string literals)
        u64 bytes_at_window{};
        std::chrono::steady_clock::time_point window{};
    } precompiled_stats;
    void NotePrecompiledMiss(const char* reason);

    /// Builds a disk-cached graphics pipeline from the first of its records (newest first) that
    /// loads. When none does, the GPU thread's build of it is stored again.
    std::unique_ptr<GraphicsPipeline> CreateCachedGraphicsPipeline(
        const GraphicsPipelineCacheKey& key,
        std::vector<std::vector<VideoCommon::FileEnvironment>>& records,
        PipelineStatistics* statistics);

    /// Whether a slot the entry's translation left unbound (unbound_pointer_slots) now holds a
    /// mapped address; a table entry is followed through its slot's buffer. The first such slot
    /// and its address are stored in `filled` and `address` when given.
    [[nodiscard]] bool StaleUnboundPointerSlot(const ComputePipelineCacheEntry& entry,
                                               Shader::StoragePointerSlot* filled = nullptr,
                                               GPUVAddr* address = nullptr) const;

    /// Logs once per translation a dispatched native compute program's slots left unbound, with
    /// its shader hash and where the translation came from (players' logs then name a culling
    /// program that misses a list slot).
    void NoteUnboundPointerSlots(const ComputePipelineCacheKey& key,
                                 const ComputePipelineCacheEntry& entry, bool is_new);

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers)
    /// `store` false: the program is in the disk cache already (one EXP51 evicted), so no record
    /// is appended for it.
    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                    const ShaderInfo* shader,
                                                    std::span<const u64> keep_pointers = {},
                                                    bool store = true);

    /// A compute translation's SPIR-V and serialized info (the precompiled preparation keeps
    /// them; its audit compares them). With translate_only no pipeline is made.
    struct ComputeKeep {
        std::vector<u32> spirv;
        std::vector<u8> info;
        bool translate_only{};
        /// The program's storage descriptors, and how many have a proven read extent
        u32 storage_descriptors{};
        u32 proven_read_extents{};
    };

    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                     const ComputePipelineCacheKey& key,
                                                     Shader::Environment& env,
                                                     PipelineStatistics* statistics,
                                                     bool build_in_parallel, bool warm = false,
                                                     ComputeKeep* keep = nullptr);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();
    /// Writes the driver pipeline cache on the serialization thread, unless a write is queued
    /// already (`again_if_busy`: then one more follows it, for a save that must hold the latest
    /// builds). Callable from any thread.
    void QueueVulkanPipelineCacheSave(bool again_if_busy);
    /// True (and `last` set to now) when a write of the driver cache is due since `last`.
    [[nodiscard]] bool VulkanPipelineCacheSaveDue(
        std::chrono::steady_clock::time_point& last) const;

    /// The title's warm-compute.bin (ShaderWarm::ComputeWarmPath, EDEN_NX2_COMPUTE_WARM=<file>
    /// overrides): compute programs prepared from the game files are converted (missing reads
    /// guessed) and built in the background, then thrown away. Nothing uses them; they only fill
    /// the driver's shader cache, so a dispatch that later converts to the same SPIR-V finds the
    /// driver build ready instead of waiting seconds for it.
    void StartComputeWarm(u64 title_id);
    std::vector<std::jthread> compute_warm_threads;
    std::atomic<bool> compute_warm_done{true};

    /// The title's warm-graphics.bin (ShaderWarm::GraphicsWarmPath, EDEN_NX2_GRAPHICS_WARM=<file>
    /// overrides; needs graphics pipeline libraries): vertex and fragment programs prepared from
    /// the game files are built as single-stage libraries in the background after the compute
    /// warm-up, then released; the driver's disk cache keeps them, so a later pipeline with those
    /// stages costs a link.
    void StartGraphicsWarm(u64 title_id);
    std::vector<std::jthread> graphics_warm_threads;

    /// Set as soon as emulation is asked to stop (the loading stop token): the warm-ups end
    /// after their current build instead of compiling on until the cache is destroyed, which
    /// kept shutdown waiting on driver compiles.
    std::atomic<bool> warm_cancel{false};
    std::unique_ptr<std::stop_callback<std::function<void()>>> warm_stop_callback;

    /// Waits until the warm-up may build (ShaderNotify::WarmMayBuild). False when it is to stop.
    bool WaitForWarmTurn(std::stop_token stop);

    /// EDEN_NX2_PRECOMPILE: finds or assembles the title's precompiled
    /// file (VideoCommon::Nx2Precompile) and prepares it on precompile_thread, at idle priority
    /// while the game runs. Prepare Shaders Now (ShaderNotify::RequestPrecompile) prepares it
    /// again. Started at the end of LoadDiskResources.
    void StartPrecompiled(u64 title_id, const std::filesystem::path& base_dir);
    std::jthread precompile_thread;

    /// Builds and drops every pipeline of the game's precompiled file (written offline by
    /// tools/nx2-precompile, or assembled from a recipe), so the driver's own cache holds them
    /// before they are first drawn. Draws never use these pipelines: they build their own from
    /// the guest's shaders and find the driver's compiled code. Runs once per precompiled file,
    /// driver and release (or again when `force`). Fritchii runs it on precompile_thread,
    /// so `callback` (the loading screen in the ported feature) is empty and progress goes to ShaderNotify.
    void PreparePrecompiled(const std::filesystem::path& base_dir, std::stop_token stop_loading,
                            const VideoCore::DiskResourceLoadCallback& callback,
                            const std::filesystem::path& file = {}, bool force = false);
    /// A graphics key read from the precompiled file, as draws key it on this device: its driver
    /// fields rewritten, canonical with EDEN_NX2_CANONICAL_KEYS=1.
    void RecordedKeyForThisDevice(GraphicsPipelineCacheKey& key) const noexcept;

    /// Native pipelines: the key without state that cannot change the pipeline (see
    /// FixedPipelineState::CanonicalizeNative), with the ported attribute mask from the header. Not
    /// called: EDEN_NX2_CANONICAL_KEYS=1 canonicalizes draws, the loader and the precompiled
    /// preparation with CanonicalizeKeyState (vk_pipeline_cache.cpp), which leaves the attribute
    /// types to VideoCommon::KeyedAttributeTypes, the masking our keys had before.
    void CanonicalizeNativeKey(GraphicsPipelineCacheKey& key,
                               std::span<const Shader::Environment* const> envs) const noexcept;

    /// Native vertex programs found to load attributes their header does not list: their keys
    /// keep every attribute type.
    bool IsUnmaskedVertexProgram(u64 hash) const {
        if (!any_unmasked_vertex_program.load(std::memory_order_acquire)) {
            return false;
        }
        std::scoped_lock lock{unmasked_mutex};
        return unmasked_vertex_programs.contains(hash);
    }
    mutable std::mutex unmasked_mutex;
    std::unordered_set<u64> unmasked_vertex_programs;
    std::atomic<bool> any_unmasked_vertex_program{};
    /// disk_pipelines as boot left it: the preparation skips those keys (read off the GPU
    /// thread, which keeps adding to disk_pipelines).
    std::shared_ptr<const std::unordered_set<u64>> precompile_disk_keys;
    /// used_pipelines as boot left it (key hash -> sessions since last used), for
    /// FRITCHII_PREP_ORDER: the preparation takes what recent sessions drew first.
    std::shared_ptr<const std::unordered_map<u64, u8>> precompile_used_keys;

    /// Intel fork (phase D2, INTEL_FORK_STATIC_PREP): vertex layouts that draws keyed without
    /// dynamic vertex input, per vertex program pair (unique_hashes[0..1]), harvested from the
    /// disk cache at boot. The preparation turns recipe records recorded with dynamic vertex input
    /// into the keys a draw gives on a device without it.
    struct IntelStaticLayout {
        std::array<u32, Maxwell::NumVertexAttributes> attributes{};
        std::array<u32, Maxwell::NumVertexArrays> binding_divisors{};
        u64 enabled_divisors{};
        bool operator==(const IntelStaticLayout&) const = default;
    };
    void AddIntelStaticLayout(const GraphicsPipelineCacheKey& key);
    /// Intel fork D3: imports intel_layouts*.bin beside the cache, writes intel_layouts.bin.
    void SyncIntelLayoutFiles(const std::filesystem::path& base_dir);
    std::string IntelForkPrepStamp() const;
    std::vector<GraphicsPipelineCacheKey> IntelStaticKeys(
        const GraphicsPipelineCacheKey& record) const;
    mutable std::mutex intel_layouts_mutex;
    std::unordered_map<u64, std::vector<IntelStaticLayout>> intel_static_layouts;

    /// The pipeline cache warm-up builds go through: none by default, so the tens of thousands
    /// of warm-up programs stay in the driver's disk cache and out of vulkan_pipelines.bin (which
    /// is loaded into memory at every boot). EDEN_NX2_WARM_APP_CACHE=1 uses ours.
    /// The cache a pipeline built in play goes to (EDEN_NX2_AMD_APP_CACHE rules).
    [[nodiscard]] vk::PipelineCache& PlayPipelineCache();
    [[nodiscard]] vk::PipelineCache& WarmPipelineCache();
    vk::PipelineCache no_pipeline_cache;

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    /// EXP87 draw templates of every graphics pipeline: 2,048 sets of 4 (allocated on first use).
    DrawTemplates draw_templates{2048};
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    bool oot_visual_workaround{};
    u64 visual_workaround_title{};
    const size_t num_pipeline_workers;
    EmissionBudget emission_budget;

    /// EDEN_NX2_CANONICAL_KEYS=1, read by LoadDiskResources (draws look up canonical keys).
    bool canonical_keys{};

    GraphicsPipelineCacheKey graphics_key{};
    u64 vertex_attribute_types{}; ///< graphics_key's attribute types before the programs' mask
    /// The vertex input part of a key (RefreshGraphicsKey).
    struct KeyVertexInput {
        u64 attribute_types;
        std::array<FixedPipelineState::VertexAttribute, Maxwell::NumVertexAttributes> attributes;
        std::array<u32, Maxwell::NumVertexArrays> binding_divisors;
        std::array<u16, Maxwell::NumVertexArrays> vertex_strides;
    };
    /// graphics_key is a native task/mesh key without its vertex input (the registers' is in
    /// mesh_vertex_input_saved, put back before the next Refresh).
    bool mesh_vertex_input_cleared{};
    KeyVertexInput mesh_vertex_input_saved{};
    GraphicsPipeline* current_pipeline{};

    /// EDEN_NX2_GPL: shader libraries shared by the graphics pipelines. Declared before the
    /// pipelines, so it outlives them.
    GraphicsPipelineLibraryCache graphics_libraries;

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    /// Pipelines the translate queue did not use (discarded, stale, abandoned, rejected by the
    /// verify mode), handed over on the GPU thread: destroyed by TickFrame once the GPU and
    /// their queued build are done with them, never on a translation thread (fable_answer_18
    /// 2b). Beside evicted_pipelines rather than in it: one whose build waits behind the boot
    /// prebuild would hold the evictions behind it there, as that deque is freed from the front.
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> retired_translations; ///< (tick, pipeline)
    void RetireTranslatedPipeline(std::unique_ptr<GraphicsPipeline> pipeline);
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since
    /// FRITCHII_RAM_PREBUILT_KEEP: key hashes of boot builds destroyed before any draw used them,
    /// and how many went that way / were asked for again (translated anew) this session.
    std::unordered_set<u64> evicted_unused_prebuilt;
    u64 prebuilt_evicted_total{};
    u64 prebuilt_evicted_needed{};
    /// The pointer slots of evicted graphics pipelines, for their next translation (ported).
    /// Forgotten once a pipeline of the key is made again.
    VideoCommon::EvictedPointerSlots evicted_pointer_slots;

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    /// Replaced compute pipelines, destroyed once the GPU and their queued build are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    /// EXP51: destroys compute pipelines unused for a while (through retired_compute_pipelines).
    void EvictUnusedComputePipelines(int mode);
    /// What an evicted compute entry learned, kept so its next dispatch translates it as it was
    /// (the pointers it bound, the rebuilds it used) without a new disk cache record.
    struct EvictedCompute {
        std::vector<u64> mapped_pointers;
        u32 rebuilds{};
    };
    ::Common::unordered_map<ComputePipelineCacheKey, EvictedCompute> evicted_compute;
    size_t compute_evicted_total{};       ///< EXP51 evictions this session
    size_t compute_rebuilt_after_evict{}; ///< Evicted compute programs dispatched again
    u64 compute_slot_rebuilds{};          ///< Rebuilds because an unbound slot filled (session)
    u32 compute_unbound_logged{};         ///< NoteUnboundPointerSlots lines this session
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;
    /// EDEN_NX2_ASYNC_TRANSLATE=2: the GPU thread's reference translation at enqueue uses these
    /// rather than releasing main_pools under whatever the GPU thread's own builds hold
    /// (fable_answer_18 2c, the fallback). Made on first use, only in that mode.
    std::unique_ptr<ShaderPools> verify_pools;
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    /// Key hashes of disk-cached graphics pipelines none of whose records loaded (one written
    /// before a translator change can ask about slots it never classified): stored again when the
    /// GPU thread builds them.
    std::unordered_set<u64> failed_disk_pipelines;
    std::mutex failed_disk_pipelines_mutex;
    /// Disk cache records the translation rejected since the last report: the first few are
    /// logged, the rest counted (ReportDiskRecordFailures, after the boot and background builds).
    std::atomic<size_t> disk_record_failures{};
    void NoteDiskRecordFailure(std::string_view kind, u64 hash, const char* reason);
    void ReportDiskRecordFailures();
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    /// Data size of our cache file as last read or written; ~0: none (FRITCHII_PCACHE_SAVE).
    std::atomic<size_t> saved_cache_size{~size_t{0}};
    /// When MemoryOwners last asked the driver for the cache data size.
    std::chrono::steady_clock::time_point last_cache_size_query{};
    std::atomic_bool flush_in_flight{};
    std::atomic_bool save_again{};

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;

    /// Cached graphics pipelines boot skipped (not in used_pipelines) are built on background
    /// threads once the game runs, so a draw that needs one finds it built instead of translating
    /// it on the GPU thread. EDEN_NX2_BACKGROUND_PIPELINES=0 turns it off.
    std::unique_ptr<GraphicsPipeline> TakeBackgroundPipeline(const GraphicsPipelineCacheKey& key);
    void StopBackgroundPipelines();
    std::mutex background_mutex;
    /// Built and not yet asked for by a draw.
    std::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> background_ready;
    /// Keys the GPU thread built itself meanwhile: their background build is skipped or dropped.
    std::unordered_set<GraphicsPipelineCacheKey> background_claimed;
    std::atomic_bool background_active{};
    std::atomic_bool background_stop{};
    std::atomic<size_t> background_left{};
    /// Numbers the background threads for the EDEN_NX2_EXP5 thread limit.
    std::atomic<size_t> background_slots{};
    size_t background_taken{}; ///< Under background_mutex
    std::chrono::steady_clock::time_point background_start{};
    std::chrono::steady_clock::time_point background_last_save{}; ///< Under background_mutex
    /// Last: destroyed (its threads joined) before the state above.
    std::unique_ptr<Common::ThreadWorker> background_workers;

    /// EDEN_NX2_ASYNC_TRANSLATE=1/2: graphics pipelines translated off the GPU thread. Null with
    /// the switch off. Reset first in the destructor (its threads build with the members above).
    std::unique_ptr<TranslateQueue> translate_queue;
    /// Keys whose translations kept failing their checks or were swept: built on the GPU thread.
    std::unordered_set<GraphicsPipelineCacheKey> translate_sync_only;
    /// Unusable translations per key (a stale texture descriptor, a guarded read that gave up).
    std::unordered_map<GraphicsPipelineCacheKey, u32> translate_failures;
    /// Counts for the 10 s line, GPU thread only.
    struct TranslateStats {
        u32 queued{};
        u32 full{};        ///< Not queued: too many in flight
        u32 adopted{};
        u32 adopted_between_frames{};
        u32 no_pipeline{};
        u32 stale{};       ///< A texture descriptor changed before adoption
        u32 gave_up{};     ///< A guarded read gave up
        u32 differs{};     ///< Verify mode: not what the GPU thread made
        u32 requeued{};
        u32 waits{};
        u32 fallbacks{};   ///< Built on the GPU thread after all
        u32 swept{};
        u64 skipped{};     ///< Lookups skipped while translating
        size_t snapshot_bytes{};
        std::chrono::steady_clock::duration enqueue_time{};
        std::chrono::steady_clock::duration wait_time{};
        std::chrono::steady_clock::duration translation_time{};
        std::chrono::steady_clock::duration ready_total{};
        std::chrono::steady_clock::duration ready_max{};
        std::chrono::steady_clock::time_point window{};
    } translate_stats;
};

} // namespace Vulkan
