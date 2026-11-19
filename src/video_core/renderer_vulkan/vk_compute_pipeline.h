// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

class Device;
class PipelineStatistics;
class Scheduler;

class ComputePipeline {
public:
    explicit ComputePipeline(const Device& device, Scheduler& scheduler, vk::PipelineCache& pipeline_cache,
                             DescriptorPool& descriptor_pool,
                             GuestDescriptorQueue& guest_descriptor_queue,
                             DescriptorBufferRing& descriptor_buffer_ring,
                             Common::ThreadWorker* thread_worker,
                             PipelineStatistics* pipeline_statistics,
                             VideoCore::ShaderNotify* shader_notify, const Shader::Info& info,
                             vk::ShaderModule spv_module, u64 shader_hash,
                             std::function<vk::ShaderModule()> make_module = {});
    ~ComputePipeline();

    ComputePipeline& operator=(ComputePipeline&&) noexcept = delete;
    ComputePipeline(ComputePipeline&&) noexcept = delete;

    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(const ComputePipeline&) = delete;

    using BindCommand = std::function<void(vk::CommandBuffer)>;

    [[nodiscard]] bool Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                 Tegra::MemoryManager& gpu_memory, Scheduler& scheduler,
                                 BufferCache& buffer_cache, TextureCache& texture_cache,
                                 TextureCacheRuntime& texture_runtime,
                                 std::optional<ByteAddressedShadow>& shadow,
                                 BindCommand* deferred_bind = nullptr);

    void FillHandleValues(MemStage& seam, const Tegra::Engines::KeplerCompute& kepler_compute,
                          Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                          bool in_configure);

    /// One follower (FRITCHII_ONEF_OP_DISPATCH, E): the raw handles FillHandleValues makes, in
    /// the same order, into `values` (E's own vector; the seam's packet is not touched).
    void MakeHandleValues(const Tegra::Engines::KeplerCompute& kepler_compute,
                          Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                          std::vector<u32>& values) const;
    /// One follower (E): whether the program writes an image (a storage image or an image
    /// buffer), from its info (WrittenImages is the last Configure's, R's).
    [[nodiscard]] bool WritesImages() const noexcept {
        return std::ranges::any_of(info.image_descriptors, [](const auto& desc) {
                   return desc.is_written;
               }) ||
               std::ranges::any_of(info.image_buffer_descriptors,
                                   [](const auto& desc) { return desc.is_written; });
    }
    /// One follower (FRITCHII_ONEF_DISPATCH_RANGES): calls `visit(raw)` for every written image
    /// element, in Configure's order (WrittenImages' order), from the values MakeHandleValues
    /// made (one value per image element, the image descriptors' last). False when there are too
    /// few values for that.
    template <typename Visit>
    bool VisitWrittenImageHandles(std::span<const u32> values, Visit&& visit) const {
        const size_t count = Shader::NumDescriptors(info.image_descriptors);
        if (values.size() < count) {
            return false;
        }
        size_t at = values.size() - count;
        for (const auto& desc : info.image_descriptors) {
            for (u32 element = 0; element < desc.count; ++element, ++at) {
                if (desc.is_written) {
                    visit(values[at]);
                }
            }
        }
        return true;
    }
    /// One follower (E): whether a written image is byte-addressed (Configure's packed R16
    /// shadow may apply: it throws on unsupported shapes and makes its own resources).
    [[nodiscard]] bool WritesByteAddressedImages() const noexcept {
        return std::ranges::any_of(info.image_descriptors, [](const auto& desc) {
            return desc.is_written && desc.is_byte_addressed;
        });
    }

    static constexpr u64 Ocarina1440pShader = 0x6bd913e1f34e83ebULL;
    /// FRITCHII_DLSS_RENDER_SCALE: the same upscale to 2560x1440 from a scene the DLSS render
    /// scale shrank (Ultra Performance, 1280x720), in menus and in gameplay.
    static constexpr u64 Ocarina1440pScaledShader = 0x4c1ae2fe58f30a97ULL;
    static constexpr u64 Ocarina1440pScaledFieldShader = 0x9302bc0d6d1d9bc1ULL;
    /// The game's 3/4 upscale, which it runs to 2560x1440 at the Performance render scale.
    static constexpr u64 OcarinaThreeQuarterShader = 0xe40f5efb5480e692ULL;
    /// The same output pass at 1:1, run at 1920x1080 in handheld with the Max Resolution add-on.
    static constexpr u64 OcarinaFullSizeShader = 0xc033c3ddc9f0897bULL;
    [[nodiscard]] bool CanReplaceOcarinaUpscale() const;

    // FRITCHII_STAMP_SKIP_BEGIN: left out of the Preparing shaders stamp (CMakeModules/Nx2ShaderStamp.cmake).
    // Only what can't change a pipeline the driver compiles belongs here.
    /// Render heights below 720p: the game upscales those scenes with a pass that isn't one of the
    /// hashes above. A dispatch of this shape that writes the game's 2560x1440 target marks its
    /// pipeline (RasterizerVulkan::NoteLowSceneUpscale), and from the next dispatch on the
    /// pipeline counts as a scaled 1440p pass. TryOcarinaUpscale still checks every image.
    /// True the first time.
    bool LearnOcarinaUpscale() noexcept {
        return !learned_ocarina_upscale.exchange(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool LearnedOcarinaUpscale() const noexcept {
        return learned_ocarina_upscale.load(std::memory_order_relaxed);
    }
    /// One image, only written, and no other output: what an upscale pass looks like. Defined in
    /// vk_rasterizer.cpp: this class's .cpp is one of the files the stamp hashes.
    [[nodiscard]] bool HasOcarinaUpscaleShape() const;

    // FRITCHII_STAMP_SKIP_END
    bool IsBound() const noexcept {
        return static_cast<bool>(pipeline);
    }

    [[nodiscard]] u64 ShaderHash() const noexcept {
        return shader_hash;
    }

    /// What the memory stage's FillDispatch needs (SEAM = 2, vk_mem_stage.cpp).
    [[nodiscard]] MemStage::DispatchFacts MemFacts() const noexcept {
        return {
            .uniform_mask = info.constant_buffer_mask,
            .uniform_sizes = &uniform_buffer_sizes,
            .info = &info,
            .shader_hash = shader_hash,
            .has_texel_buffers =
                !info.texture_buffer_descriptors.empty() || !info.image_buffer_descriptors.empty(),
            .uses_descriptor_buffer = uses_descriptor_buffer,
        };
    }

    /// Storage images written by the last configured dispatch
    [[nodiscard]] std::span<const VideoCommon::ImageViewId> WrittenImages() const noexcept {
        return written_images;
    }

    struct SampledImage {
        u32 tic;
        VideoCommon::ImageViewId id;
        VideoCommon::SamplerId sampler;
    };

    /// The textures sampled by the last configured dispatch, kept while a sampled frame dump runs.
    [[nodiscard]] std::span<const SampledImage> SampledImages() const noexcept {
        return sampled_images;
    }

    /// Advances the frame window of EDEN_NX2_UPSCALER_TRACE
    static void TickUpscalerTrace();

    /// Made from a precompiled pipeline's kept info, its module from the kept SPIR-V
    /// (EDEN_NX2_PRECOMPILED_FAST): a failed build is replaced by a translated pipeline.
    void MarkPrecompiled() noexcept {
        precompiled = true;
    }
    [[nodiscard]] bool IsPrecompiled() const noexcept {
        return precompiled;
    }
    /// Whether the build ended without a pipeline (its module could not be made).
    [[nodiscard]] bool BuildFailed() const noexcept {
        // has_pipeline, not `pipeline`: the scheduler worker swaps that one
        // (FRITCHII_COMPUTE_FAST_FIRST).
        return is_built.load(std::memory_order::acquire) &&
               !has_pipeline.load(std::memory_order::acquire);
    }
    /// Whether the build ended, with a pipeline or not.
    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Whether no build of this pipeline is left queued on a worker thread (the pipeline workers
    /// or FRITCHII_ASYNC_COMPUTE_SKIP's kick thread), which holds it
    [[nodiscard]] bool IsBuildTaskDone() const noexcept {
        return build_task_done.load(std::memory_order::acquire) &&
               !kick_pending.load(std::memory_order::acquire);
    }

    // --- FRITCHII_ASYNC_COMPUTE_SKIP (vk_async_compute_skip.h) ---

    /// Waits at most `timeout` for the build, never building it on the calling thread. True
    /// once built.
    [[nodiscard]] bool WaitBuiltFor(std::chrono::nanoseconds timeout);
    /// Starts the build on `worker` now, unless a thread claimed it already: the pipeline
    /// workers are first in, first out and may hold hundreds of builds before this one.
    void KickBuild(Common::ThreadWorker& worker);
    /// When the build ended (steady clock); only once IsBuilt().
    [[nodiscard]] std::chrono::steady_clock::time_point BuiltAt() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{built_at.load(std::memory_order::acquire)}};
    }
    /// Configure's storage pointer binds (the uniform state, the storage buffer binds and the
    /// shader hash), done ahead so the dispatch's written ranges can be judged before anything
    /// is uploaded or marked. The buffer cache's lock held. Configure skips them once after.
    void BindStoragePointers(BufferCache& buffer_cache);
    /// The pointers BindStoragePointers bound are the ones Configure uses next (set by the
    /// caller after BindStoragePointers, for a dispatch that runs).
    void UsePreboundPointers() noexcept {
        pointers_prebound = true;
    }
    /// FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS (test): the next Configure records the build wait a
    /// pipeline still building records (on the scheduler worker, every channel behind it, the
    /// "Compute dispatch waited" line), held until `deadline`.
    void SetTestWaitUntil(std::chrono::steady_clock::time_point deadline) noexcept {
        test_wait_until = deadline;
    }

    // --- FRITCHII_COMPUTE_FAST_FIRST ---

    /// 0 off, 1 a dispatch that would wait for a first build gets an unoptimized build at once
    /// (the optimized one follows on its worker and is swapped in), 2 (test) every program built
    /// at run time takes that path, waited for or not. Live, any thread.
    [[nodiscard]] static int FastFirstMode();
    /// The 10 s line and the "unoptimized over 30 s" check. GPU thread, each frame.
    static void ReportFastFirst();

private:
    /// Builds the optimized pipeline once, on whichever thread gets here first.
    void RunBuild();
    /// A dispatch needs the pipeline now (Configure's wait, FRITCHII_ASYNC_COMPUTE_SKIP's kick):
    /// with FRITCHII_COMPUTE_FAST_FIRST the unoptimized build, else RunBuild.
    void RunNeededBuild();
    /// The unoptimized build (VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, same SPIR-V and
    /// layout), published when it ends before the optimized one. False when it made nothing.
    bool RunFastBuild();
    [[nodiscard]] bool UsesFastFirst() const;
    /// The pipeline from the module; the caller holds the module (AcquireModule).
    [[nodiscard]] vk::Pipeline CreateHandle(bool unoptimized);
    /// Makes the module when it is made late (precompiled), and holds it until ReleaseModule.
    /// False when there is none (it failed, or the optimized build released it).
    [[nodiscard]] bool AcquireModule();
    /// `optimized_done`: nothing builds from the module any more once its users let go.
    void ReleaseModule(bool optimized_done);
    /// The first build to end becomes the pipeline; a later optimized one waits for the swap.
    /// True when `made` became the pipeline.
    bool Publish(vk::Pipeline made, bool optimized, double compile_ms);
    /// Scheduler worker, before a bind: frees the swapped-out unoptimized pipeline once the GPU
    /// passed it, and swaps in a finished optimized one.
    void SwapInOptimized();

    const Device& device;
    Scheduler& owner_scheduler;
    VideoCore::ShaderNotify* const build_notify;
    vk::PipelineCache& pipeline_cache;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    Shader::Info info;
    u64 shader_hash{};
    // FRITCHII_STAMP_SKIP_BEGIN: NoteLowSceneUpscale's mark, not part of any pipeline.
    std::atomic<bool> learned_ocarina_upscale{};
    // FRITCHII_STAMP_SKIP_END
    u32 num_descriptor_entries{};
    boost::container::small_vector<VideoCommon::ImageViewId, 8> written_images;
    std::vector<SampledImage> sampled_images;

    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};

    vk::ShaderModule spv_module;
    vk::DescriptorSetLayout descriptor_set_layout;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
    DescriptorBufferLayout descriptor_buffer_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    std::function<void()> build_task;
    bool precompiled{};
    std::atomic_bool build_claimed{false};
    std::atomic_bool build_task_done{true};
    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    /// FRITCHII_ASYNC_COMPUTE_SKIP: the build's end (steady clock ticks), a kick queued, and
    /// Configure's pointer binds done ahead (BindStoragePointers).
    std::atomic<std::chrono::steady_clock::rep> built_at{};
    std::atomic_bool kick_pending{false};
    bool pointers_prebound{};
    std::chrono::steady_clock::time_point test_wait_until{};

    /// Whether `pipeline` is set (BuildFailed reads this, not the handle the worker swaps).
    std::atomic_bool has_pipeline{false};
    /// FRITCHII_COMPUTE_FAST_FIRST. The module, shared by both builds: made late for a
    /// precompiled pipeline, released when the optimized build and every fast one let go.
    std::function<vk::ShaderModule()> make_module_fn;
    std::mutex module_mutex;
    u32 module_users{};
    bool module_release_due{};
    /// Built at run time on a worker (not the boot, warm-up or preparation builds), and not one
    /// of the programs always built unoptimized.
    bool fast_first_allowed{};
    std::atomic_bool fast_claimed{false};
    /// Under build_mutex: `pipeline` is the optimized one (false while the fast one serves), and
    /// the fast build's compile time.
    bool running_optimized{true};
    double fast_ms{};
    /// The optimized pipeline waiting for SwapInOptimized (set under build_mutex).
    std::atomic_bool optimized_ready{false};
    vk::Pipeline optimized_pipeline;
    /// Scheduler worker only: the swapped-out unoptimized pipeline, freed once the GPU passed
    /// retired_tick.
    vk::Pipeline retired_fast;
    u64 retired_tick{};
    /// In the fast-first registry (its lock): no swap yet.
    bool fast_open{};
};

} // namespace Vulkan
