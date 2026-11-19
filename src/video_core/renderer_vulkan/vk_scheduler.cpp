// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <vector>
#include <string_view>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h> // _mm_pause (FRITCHII_WORKER_SPIN)
#endif

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "common/settings.h"
#include "common/dkb_cpu_trace.h"
#include "common/frame_budget.h"
#include "common/nx2_hitch.h"
#include "common/nx2_timeline.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_fastdraw_count.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/upload_batch.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
std::atomic<u64> g_profile_submits{};
std::atomic<u64> g_profile_renderpasses{};
std::atomic<u64> g_profile_finishes{};
std::atomic<u64> g_profile_finish_ns{};
std::atomic<u64> g_profile_window_start{};
std::mutex g_profile_end_mutex;
std::map<std::string, u64> g_profile_ends;
/// Submits (Flush and Finish) by call site: file name pointer and line, named at report time.
std::map<std::pair<const char*, u32>, u64> g_profile_submit_sites;

void CountSubmitSite(const std::source_location& location) {
    if (!VideoCommon::Nx2Profile::Enabled()) {
        return;
    }
    std::scoped_lock lock{g_profile_end_mutex};
    ++g_profile_submit_sites[{location.file_name(), location.line()}];
}

void CountRenderPassEnd(const char* file, u32 line) {
    if (!VideoCommon::Nx2Profile::Enabled()) {
        return;
    }
    std::string_view name{file};
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::scoped_lock lock{g_profile_end_mutex};
    ++g_profile_ends[fmt::format("{}:{}", name, line)];
}

void ReportSubmitCounts() {
    const u64 now = VideoCommon::Nx2Profile::Now();
    u64 start = g_profile_window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        g_profile_window_start.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 10'000'000'000ULL ||
        !g_profile_window_start.compare_exchange_strong(start, now)) {
        return;
    }
    const double seconds = static_cast<double>(now - start) / 1e9;
    LOG_INFO(Render_Vulkan,
             "Scheduler per second: {:.0f} submits, {:.0f} render passes, {:.0f} finishes "
             "waiting {:.1f} ms",
             static_cast<double>(g_profile_submits.exchange(0)) / seconds,
             static_cast<double>(g_profile_renderpasses.exchange(0)) / seconds,
             static_cast<double>(g_profile_finishes.exchange(0)) / seconds,
             static_cast<double>(g_profile_finish_ns.exchange(0)) / 1e6 / seconds);
    std::vector<std::pair<u64, std::string>> ends;
    std::vector<std::pair<u64, std::string>> submit_sites;
    {
        std::scoped_lock lock{g_profile_end_mutex};
        for (const auto& [site, count] : g_profile_ends) {
            ends.emplace_back(count, site);
        }
        g_profile_ends.clear();
        // One site can have several file name pointers (a header's inline callers).
        std::map<std::string, u64> merged;
        for (const auto& [site, count] : g_profile_submit_sites) {
            std::string_view name{site.first};
            if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
                name.remove_prefix(slash + 1);
            }
            merged[fmt::format("{}:{}", name, site.second)] += count;
        }
        g_profile_submit_sites.clear();
        for (const auto& [site, count] : merged) {
            submit_sites.emplace_back(count, site);
        }
    }
    std::sort(submit_sites.rbegin(), submit_sites.rend());
    std::string submits_text;
    for (size_t i = 0; i < submit_sites.size() && i < 6; ++i) {
        submits_text += fmt::format(" {} {:.0f},", submit_sites[i].second,
                                    static_cast<double>(submit_sites[i].first) / seconds);
    }
    if (!submits_text.empty()) {
        submits_text.pop_back();
        LOG_INFO(Render_Vulkan, "Submits per second by site ({} sites, top 6):{}",
                 submit_sites.size(), submits_text);
    }
    std::sort(ends.rbegin(), ends.rend());
    for (size_t i = 0; i < ends.size() && i < 16; ++i) {
        LOG_INFO(Render_Vulkan, "Render pass ends {:.0f}/s at {}",
                 static_cast<double>(ends[i].first) / seconds, ends[i].second);
    }
}
}

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)},
      upload_batch{std::make_unique<UploadBatch>()} {

    if (Common::DkbFrameTrace::Enabled()) {
        const auto physical = device.GetPhysical();
        trace_timestamp_bits =
            physical.GetQueueFamilyProperties()[device.GetGraphicsFamily()].timestampValidBits;
        trace_timestamp_period = physical.GetProperties().limits.timestampPeriod;
        if (trace_timestamp_bits != 0 && trace_timestamp_bits <= 64 && trace_timestamp_period > 0) {
            try {
                trace_query_pool = device.GetLogical().CreateQueryPool({
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = TraceMaxSubmissions * 2,
                    .pipelineStatistics = 0,
                });
                trace_submissions.resize(TraceMaxSubmissions);
                trace_work_pool = device.GetLogical().CreateQueryPool({
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = TraceMaxWork * 2,
                    .pipelineStatistics = 0,
                });
                trace_work_items.resize(TraceMaxWork);
            } catch (const vk::Exception& error) {
                LOG_WARNING(Render_Vulkan, "DKB gpu_timestamp unavailable: {}", error.what());
            }
        } else {
            LOG_INFO(Render_Vulkan, "DKB gpu_timestamp unsupported: valid_bits={} period_ns={}",
                     trace_timestamp_bits, trace_timestamp_period);
        }
    }

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
    // FRITCHII_SLOW_GPU: the hang report and a device loss print the newest submissions.
    Common::Nx2Hitch::SetGpuBreadcrumbs(&Scheduler::LogGpuBreadcrumbs, this);
}

Scheduler::~Scheduler() {
    // No breadcrumb read after this returns (the hang watch and a device loss read slow_gpu).
    // FRITCHII_SLOW_GPU's stamp pool waits in ~SlowGpu, after worker_thread (declared after it)
    // has joined.
    Common::Nx2Hitch::ClearGpuBreadcrumbs(this);
    if (trace_query_pool || budget_pool) {
        WaitWorker();
        worker_thread.request_stop();
        worker_thread.join();
        if (trace_gpu_tick != 0) {
            master_semaphore->Wait(trace_gpu_tick);
        }
        if (budget_last_tick != 0) {
            master_semaphore->Wait(budget_last_tick);
        }
    }
}

void Scheduler::LogGpuBreadcrumbs(const void* self_, const char* prefix) {
    const auto* const self = static_cast<const Scheduler*>(self_);
    self->slow_gpu.LogBreadcrumbs(prefix, *self->master_semaphore);
}

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                     std::source_location location) {
    SeamHook::Watch(SeamHook::Use::Flush, location);
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
    }
    CountSubmitSite(location);
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Flush");
    }
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    submit_site = location;
    submit_is_finish = false;
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    submitted_serial = record_serial; // what AllocateNewContext recorded counts as submitted
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    return signal_value;
}

bool Scheduler::DeferFlush() {
    // Fences come one per compute release: ~11,500 a second in Hyrule Warriors: Age of
    // Imprisonment fights, each a queue submission (~22,000 a second with the others), which
    // kept the Vulkan worker busy submitting. Batched within 500 us: ~10-15% more fps there.
    // Command list copies after dispatches were ~9,000 more a second, of which the front waited
    // for about 5%.
    static const s64 interval_us = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_US");
        if (value != nullptr) {
            return static_cast<s64>(std::atoll(value));
        }
        // 0 by default (a ported flash fix): Ocarina of Time flashed white with the 500 us batch
        // (sword boulder with preflush: 31 whole-screen flashes in 30 s, 0 without), and ran
        // faster without it (Lake Hylia 52.3 -> 55.4 fps). EDEN_NX2_FENCE_FLUSH_US=500 restores it.
        return VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FenceFlush) ? s64{5000} : s64{0};
    }();
    if (interval_us <= 0 || !VideoCommon::GpuSplit::OnBackThread() ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{interval_us}) {
        return false;
    }
    flush_pending = true;
    return true;
}

void Scheduler::FlushDeferred() {
    if (flush_pending) {
        Flush();
    }
}

bool Scheduler::DeferFlushUntilWanted() {
    if (!VideoCommon::GpuSplit::OnBackThread()) {
        return false;
    }
    flush_pending = true;
    return true;
}

bool Scheduler::WaitSubmitted(u64 tick) {
    if (IsSubmitted(tick)) {
        return true;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.RequestIdleHook();
    // No time limit: the back can spend seconds on one entry (a large shader's translation),
    // and a fence reported as submitted before it is would release its downloads too early.
    const auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds{2};
    for (u32 spins = 0; !IsSubmitted(tick); ++spins) {
        if (!split.BackRunning()) {
            return IsSubmitted(tick);
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_log) {
            next_log = now + std::chrono::seconds{2};
            // The back serves the request between entries: one that never does is blocked in
            // an entry. Name the wait when it is a tracked one (BackWaitScope).
            const char* const back_site = VideoCommon::back_wait_site.load(std::memory_order_relaxed);
            const char* const stage_site =
                VideoCommon::stage_wait_site.load(std::memory_order_relaxed);
            LOG_WARNING(Render_Vulkan,
                        "Waited {} s for the GPU thread to submit tick {} (submitted up to {}); "
                        "GPU back thread blocked in: {}; GPUMem blocked in: {}",
                        std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                        tick, master_semaphore->CurrentTick() - 1,
                        back_site != nullptr ? back_site
                                             : "no tracked wait (an untracked lock or wait, or "
                                               "a long entry)",
                        stage_site != nullptr ? stage_site : "nothing tracked");
            split.RequestIdleHook();
        }
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }
    return true;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                       std::source_location location) {
    SeamHook::Watch(SeamHook::Use::Finish, location);
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
    }
    CountSubmitSite(location);
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool profile = VideoCommon::Nx2Profile::Enabled();
    const u64 start = profile ? VideoCommon::Nx2Profile::Now() : 0;
    submit_site = location;
    submit_is_finish = true;
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
    submitted_serial = record_serial;
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    if (profile) {
        g_profile_finishes.fetch_add(1, std::memory_order_relaxed);
        g_profile_finish_ns.fetch_add(VideoCommon::Nx2Profile::Now() - start,
                                      std::memory_order_relaxed);
    }
}

void Scheduler::WaitWorker() {
    const Common::LowEndSim::CpuWaitScope sim_wait;
    DispatchWork();

    // Waits for the Vulkan worker (tools/nx2/critical_path.py's table): the queue, then the
    // chunk it is running.
    Common::Nx2Timeline::Span timeline_span{"wait for Vulkan worker"};
    const auto wait_start = std::chrono::steady_clock::now();
    size_t queued_chunks = 0;
    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        queued_chunks = work_queue.size();
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
    // The 40-90 ms full drains at the night market (2026-11-11) had the back blocked outside its
    // zones while the worker sat in "record commands + submit": say when this wait is long.
    const auto waited = std::chrono::steady_clock::now() - wait_start;
    if (waited > std::chrono::milliseconds{4}) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 200) {
            LOG_INFO(Render_Vulkan, "Slow worker wait: {} us for {} queued chunks",
                     std::chrono::duration_cast<std::chrono::microseconds>(waited).count(),
                     queued_chunks);
        }
    }
}

void Scheduler::MarkFrameEnd(u64 frame) {
    if (Common::DkbFrameTrace::Enabled()) {
        Record([this, frame](vk::CommandBuffer) { trace_frame_end = frame; });
    }
}

void Scheduler::DispatchWork() {
    SeamHook::Watch(SeamHook::Use::DispatchWork);
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    if (uploads_pending) {
        // The worker must receive everything recorded so far.
        FlushUploads();
    }
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        // FRITCHII_WORKER_SPIN: a spinning worker has worker_waiting false too, so it is not
        // woken either; it sees dispatch_serial move, or looks at the queue under the lock
        // before it sleeps (WorkerThread).
        bool wake;
        bool spinning;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            // Only written under queue_mutex: a plain increment, no locked add.
            dispatch_serial.store(dispatch_serial.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_release);
            wake = worker_waiting;
            spinning = worker_spinning;
        }
        if (wake) {
            event_cv.notify_all();
        }
        NoteDispatchWake(wake, spinning);
        AcquireNewChunk();
    }
}

bool Scheduler::SpinForWork(u64 seen, int spin_us, const std::stop_token& stop_token) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::microseconds{spin_us};
    bool moved = false;
    for (u32 spins = 0;; ++spins) {
        if (dispatch_serial.load(std::memory_order_acquire) != seen) {
            moved = true;
            break;
        }
        // The clock and the stop token every 16th look (a clock read is ~20 ns).
        if ((spins & 15) == 15 &&
            (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)) {
            break;
        }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }
    worker_spin_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count()),
                             std::memory_order_relaxed);
    return moved;
}

void Scheduler::NoteDispatchWake(bool notified, bool spinning) {
    WakeStats& s = wake_stats;
    ++s.dispatches;
    if (notified) {
        ++s.notified;
    } else if (spinning) {
        ++s.skipped_spinning;
    } else {
        ++s.skipped_busy;
    }
    // The clock every 256th dispatch; one line every 10 s.
    if ((s.dispatches & 255) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (s.since == std::chrono::steady_clock::time_point{}) {
        s = WakeStats{.since = now};
        return;
    }
    if (now - s.since < std::chrono::seconds{10}) {
        return;
    }
    const double window = std::chrono::duration<double>(now - s.since).count();
    const u64 spins = worker_spins.exchange(0, std::memory_order_relaxed);
    const u64 hits = worker_spin_hits.exchange(0, std::memory_order_relaxed);
    const u64 spun_ns = worker_spin_ns.exchange(0, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "Worker wake (FRITCHII_WORKER_SPIN={}) {:.0f} s: dispatches {:.0f}/s, notified "
             "{:.0f}/s, skipped {:.0f}/s busy + {:.0f}/s spinning; worker spins {:.0f}/s, {:.0f}/s "
             "found work, spinning {:.1f} ms/s",
             VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_worker_spin,
                                              "FRITCHII_WORKER_SPIN"),
             window, static_cast<double>(s.dispatches) / window,
             static_cast<double>(s.notified) / window, static_cast<double>(s.skipped_busy) / window,
             static_cast<double>(s.skipped_spinning) / window, static_cast<double>(spins) / window,
             static_cast<double>(hits) / window, static_cast<double>(spun_ns) / 1e6 / window);
    s = WakeStats{.since = now};
}

void Scheduler::RecordUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies) {
    SeamHook::Watch(SeamHook::Use::RecordUpload);
    if (VideoCommon::GpuSplit::check_owner) {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::RecordUpload");
    }
    // Check each region so even a single upload larger than the capacity stays bounded, and
    // overlapping regions within that upload cannot enter the same batch.
    for (const VkBufferCopy& copy : copies) {
        const std::span<const VkBufferCopy> region{&copy, 1};
        if (uploads_pending && !upload_batch->CanAdd(src, dst, region)) {
            FlushUploads();
        }
        upload_batch->Add(src, dst, region);
        uploads_pending = true;
    }
}

void Scheduler::FlushUploads() {
    // Copy the vectors so the recording thread retains their capacity while the worker uses them.
    auto command = [copies = upload_batch->copies, groups = upload_batch->groups](
                       vk::CommandBuffer cmdbuf) { UploadBatch::Record(cmdbuf, copies, groups); };
    upload_batch->copies.clear();
    upload_batch->groups.clear();
    uploads_pending = false;
    Record(std::move(command));
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    if (VideoCommon::Nx2Profile::Enabled()) {
        g_profile_renderpasses.fetch_add(1, std::memory_order_relaxed);
    }
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    // Delta draws: always counted (Fable plan D1, row 4), so the delta path can rely on it.
    ++render_pass_serial;
    state.renderpass = renderpass;
    state.clear_variant = false;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    const bool after_compute = std::exchange(compute_writes_pending, false);
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count,
            after_compute](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        if (after_compute) {
            // Draws read compute output as indirect arguments, vertices and resources; nothing
            // else orders those reads after the dispatch.
            static constexpr VkMemoryBarrier COMPUTE_TO_GRAPHICS{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, COMPUTE_TO_GRAPHICS);
        }
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
    // Without discards, the variant only clears instead of loading: its store ops, layouts and
    // attachments are the framebuffer's own.
    state.clear_variant = color_discard_mask == 0 && !depth_stencil_discard;
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer, std::source_location location) {
    SeamHook::Watch(SeamHook::Use::RenderPass, location);
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    // A deferred clear's pass serves the draws after its first one too: ending it there and
    // beginning the framebuffer's own pass stored the attachments only to load them back (about
    // 9 render passes a frame in Ocarina of Time, at GraphicsPipeline::ConfigureDraw).
    if (state.renderpass != VK_NULL_HANDLE &&
        (renderpass == state.renderpass || state.clear_variant) &&
        framebuffer_handle == state.framebuffer && render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestOutsideRenderPassOperationContext(std::source_location location) {
    SeamHook::Watch(SeamHook::Use::OutsideRenderPass, location);
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
    }
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling, bool mesh_layout) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling &&
        mesh_layout == state.mesh_layout) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    state.mesh_layout = mesh_layout;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    const Common::DkbCpuTrace::Registration trace_thread{Common::DkbCpuTrace::Role::VulkanWorker};
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    int siblings_free_applied = 0; ///< FRITCHII_GPU_SIBLINGS_FREE as this thread applied it
    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // FRITCHII_WORKER_SPIN (live, value = microseconds, default 0 = off): with an empty
            // queue the worker first spins outside the lock for up to that long. worker_waiting
            // stays false, so a dispatch meanwhile skips the notify system call. No wakeup is
            // lost: after the spin the worker takes the lock again and the wait below tests the
            // queue under it before sleeping, so a chunk pushed during the spin is popped there;
            // a chunk pushed after that test finds worker_waiting true (set under the same lock
            // in the predicate) and notifies, as without the spin.
            if (const int spin_us = VideoCore::Nx2Debug::SwitchValue(
                    VideoCore::Nx2Debug::g_worker_spin, "FRITCHII_WORKER_SPIN");
                spin_us > 0 && work_queue.empty() && !stop_token.stop_requested()) {
                const u64 seen = dispatch_serial.load(std::memory_order_relaxed);
                worker_spinning = true;
                lk.unlock();
                const bool moved = SpinForWork(seen, spin_us, stop_token);
                lk.lock();
                worker_spinning = false;
                worker_spins.fetch_add(1, std::memory_order_relaxed);
                if (moved) {
                    worker_spin_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Wait for work.
            {
                Common::Nx2Timeline::Span wait_span{"worker: wait for work"};
                event_cv.wait(lk, stop_token, [&] {
                    worker_waiting = !TryPopQueue(work);
                    return !worker_waiting;
                });
            }
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // FRITCHII_SIBLING_CENSUS (dry) and FRITCHII_GPU_SIBLINGS_FREE (live, off the GPU
            // threads' cores): one flag test and one relaxed load while both are off.
            Common::NoteCensusCpu(Common::CensusThread::VulkanWorker);
            Common::KeepOffGpuCores(siblings_free_applied);

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            Common::Nx2Timeline::Span timeline_span{has_submit ? "record commands + submit"
                                                               : "record commands"};
            // fable_stage_levelpoints 19.4 (GPU profile on): the worker's busy time, its chunks'
            // recording (and submit) and the command buffer reallocation ("Price Vulkan worker").
            namespace Price = VideoCommon::FritchiiPrice;
            const u64 price_start = Price::On() ? Price::Ticks() : 0;
            // FRITCHII_SLOW_GPU: the command buffer wrappers count this submission's commands
            // (null while off; the submission's SlowGpu::End clears it).
            vk::t_cmd_stats = slow_gpu.Counting();
            // FRITCHII_SIM_CPU_SLOW: recording and submitting this chunk.
            Common::LowEndSim::CpuWorkBegin(Common::LowEndSim::CpuClass::Worker);
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
            Common::LowEndSim::CpuWorkEnd();
            if (price_start != 0) [[unlikely]] {
                const u64 executed = Price::Ticks();
                const size_t kind = has_submit ? 1 : 0;
                Price::Add(Price::g_worker.chunks[kind], 1);
                Price::Add(Price::g_worker.busy[kind], executed - price_start);
                // FRITCHII_DELAY_WORKER_US's spin, apart from the busy time.
                VideoCommon::GpuSplit::DelayWorkerIf();
                Price::Add(Price::g_worker.delay_ticks, Price::Ticks() - executed);
            } else {
                // FRITCHII_DELAY_WORKER_US (live, 0 = off; fable_stage_levelpoints 19.3/19.4): n
                // us after each chunk, inside the execution lock, as if the chunk took that much
                // longer. Off: one relaxed load and a branch a chunk.
                VideoCommon::GpuSplit::DelayWorkerIf();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    if (trace_query_pool && !trace_gpu_failed) {
        TraceGpuSubmission();
    }
    // FRITCHII_SUBMIT_TIMES (diagnostic): the next submission's start stamp. Off: one load.
    if (SubmitTimes::Enabled()) [[unlikely]] {
        submit_times.Begin(device, current_upload_cmdbuf);
    }
    // Performance overlay time rows (common/frame_budget.h); off: one relaxed load.
    if ((budget_pending != 0 || Common::FrameBudget::Wanted()) && !budget_failed) [[unlikely]] {
        BudgetGpuSubmission();
    }
    // FRITCHII_SLOW_GPU (live, default 1): reads finished submissions back, stamps this one's
    // start, counts its commands from the next chunk on. Last, so the stamps above go uncounted.
    slow_gpu.Begin(device, current_upload_cmdbuf, *master_semaphore);
    // FRITCHII_SIM_GPU_SLOW (test): reads finished submissions back, stamps this one's start.
    if (sim_gpu_slow.Wanted()) [[unlikely]] {
        sim_gpu_slow.Begin(device, current_upload_cmdbuf, *master_semaphore);
    }
}

void Scheduler::BudgetGpuSubmission() {
    if (!budget_pool) {
        const auto physical = device.GetPhysical();
        budget_timestamp_bits =
            physical.GetQueueFamilyProperties()[device.GetGraphicsFamily()].timestampValidBits;
        budget_timestamp_period = physical.GetProperties().limits.timestampPeriod;
        if (budget_timestamp_bits == 0 || budget_timestamp_bits > 64 ||
            budget_timestamp_period <= 0) {
            LOG_INFO(Render_Vulkan, "Overlay GPU time: the graphics queue has no timestamps");
            budget_failed = true;
            Common::FrameBudget::SetGpuUnsupported();
            return;
        }
        try {
            budget_pool = device.GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = BudgetSlots * 2,
                .pipelineStatistics = 0,
            });
        } catch (const vk::Exception& error) {
            LOG_WARNING(Render_Vulkan, "Overlay GPU time unavailable: {}", error.what());
            budget_failed = true;
            Common::FrameBudget::SetGpuUnsupported();
            return;
        }
    }
    ReadBudgetResults();
    if (budget_failed || !Common::FrameBudget::Wanted() || budget_pending == BudgetSlots) {
        budget_recording = false;
        return;
    }
    // The slot's previous use has been read. Queries can't be reset inside a render pass, so the
    // reset goes in the upload buffer, which runs first in the same submission.
    const u32 slot = budget_next_slot % BudgetSlots;
    current_upload_cmdbuf.ResetQueryPool(*budget_pool, slot * 2, 2);
    current_upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *budget_pool,
                                         slot * 2);
    budget_recording = true;
}

void Scheduler::ReadBudgetResults() {
    if (budget_pending == 0) {
        return;
    }
    const u32 first = budget_next_slot - budget_pending;
    const u32 first_slot = first % BudgetSlots;
    if (!master_semaphore->IsFree(budget_ticks[first_slot])) {
        master_semaphore->Refresh();
    }
    struct Result {
        u64 timestamp;
        u64 available;
    };
    std::array<Result, 64> results{};
    // The finished run of pending slots, up to the end of the ring (one query call).
    const u32 limit = (std::min)({budget_pending, BudgetSlots - first_slot,
                                  static_cast<u32>(results.size() / 2)});
    u32 ready = 0;
    while (ready < limit && master_semaphore->IsFree(budget_ticks[first_slot + ready])) {
        ++ready;
    }
    if (ready == 0) {
        return;
    }
    const VkResult result = device.GetLogical().GetQueryResults(
        *budget_pool, first_slot * 2, ready * 2, ready * 2 * sizeof(Result), results.data(),
        sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        LOG_WARNING(Render_Vulkan, "Overlay GPU time off: query result {}",
                    static_cast<int>(result));
        budget_failed = true;
        budget_pending = 0;
        Common::FrameBudget::SetGpuUnsupported();
        return;
    }
    const u64 mask =
        budget_timestamp_bits == 64 ? ~u64{0} : (u64{1} << budget_timestamp_bits) - 1;
    u32 consumed = 0;
    double busy_ticks = 0;
    for (; consumed < ready; ++consumed) {
        const Result& start = results[consumed * 2];
        const Result& end = results[consumed * 2 + 1];
        if (start.available == 0 || end.available == 0) {
            // Done on the timeline but not visible yet: the next submission reads it.
            break;
        }
        const u64 length = (end.timestamp - start.timestamp) & mask;
        // Submissions run in queue order but may overlap at their edges: count each GPU tick
        // once (the union of the submissions' spans).
        u64 counted = length;
        if (budget_has_end) {
            const u64 gap = (start.timestamp - budget_last_end) & mask;
            if (gap > mask / 2) { // started before the previous one ended
                const u64 overlap = (budget_last_end - start.timestamp) & mask;
                counted = overlap >= length ? 0 : length - overlap;
            }
        }
        if (!budget_has_end || counted != 0) {
            budget_last_end = end.timestamp;
            budget_has_end = true;
        }
        busy_ticks += static_cast<double>(counted);
    }
    budget_pending -= consumed;
    const double busy_ns = busy_ticks * static_cast<double>(budget_timestamp_period);
    // Over a second per submission is a broken reading, not GPU work.
    if (consumed != 0 && busy_ns < 1e9 * consumed) {
        Common::FrameBudget::AddGpuBusy(static_cast<u64>(busy_ns));
    }
}

void Scheduler::TraceShader(u64 hash, u32 stage) {
    if (hash == 0) {
        return;
    }
    for (u32 i = 0; i < trace_work.num_shaders; ++i) {
        if (trace_work.shaders[i].hash == hash && trace_work.shaders[i].stage == stage) {
            return;
        }
    }
    if (trace_work.num_shaders == trace_work.shaders.size()) {
        ++trace_work.untracked_uses;
        return;
    }
    trace_work.shaders[trace_work.num_shaders++] = {hash, stage};
}

void Scheduler::TraceGraphicsPipeline(const std::array<u64, 6>& hashes) {
    if (!trace_query_pool) {
        return;
    }
    Record([this, hashes](vk::CommandBuffer) {
        if (!trace_gpu_recording) {
            return;
        }
        ++trace_work.draw_attempts;
        for (u32 stage = 0; stage < hashes.size(); ++stage) {
            TraceShader(hashes[stage], stage);
        }
    });
}

void Scheduler::TraceComputePipeline(u64 hash) {
    if (!trace_query_pool) {
        return;
    }
    Record([this, hash](vk::CommandBuffer) {
        if (!trace_gpu_recording) {
            return;
        }
        ++trace_work.dispatch_attempts;
        TraceShader(hash, 6);
    });
}

void Scheduler::ReportWorkTimes() {
    const u32 count = trace_work_count;
    trace_work_count = 0;
    if (count == 0) {
        return;
    }
    struct Result {
        u64 timestamp;
        u64 available;
    };
    std::vector<Result> results(count * 2);
    const auto result = device.GetLogical().GetQueryResults(
        *trace_work_pool, 0, count * 2, results.size() * sizeof(Result), results.data(),
        sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS) {
        return;
    }
    struct Totals {
        double total_ms{};
        double max_ms{};
        u32 count{};
        std::array<u32, 3> max_size{};
    };
    std::map<std::pair<u64, u64>, Totals> by_shaders;
    double total_ms{};
    for (u32 i = 0; i < count; ++i) {
        if (results[i * 2].available == 0 || results[i * 2 + 1].available == 0) {
            continue;
        }
        const double ms = static_cast<double>(Common::DkbFrameTrace::TimestampDelta(
                              results[i * 2].timestamp, results[i * 2 + 1].timestamp,
                              trace_timestamp_bits)) *
                          trace_timestamp_period / 1e6;
        const WorkTrace& item = trace_work_items[i];
        Totals& totals = by_shaders[{item.hash, item.fragment_hash}];
        totals.total_ms += ms;
        ++totals.count;
        if (ms > totals.max_ms) {
            totals.max_ms = ms;
            totals.max_size = item.size;
        }
        total_ms += ms;
    }
    std::vector<std::pair<std::pair<u64, u64>, Totals>> sorted(by_shaders.begin(),
                                                               by_shaders.end());
    std::ranges::sort(sorted, std::greater<>{},
                      [](const auto& entry) { return entry.second.total_ms; });
    if (sorted.size() > 8) {
        sorted.resize(8);
    }
    std::string slowest;
    for (const auto& [shaders, totals] : sorted) {
        const auto& [hash, fragment_hash] = shaders;
        slowest += fragment_hash == 0
                       ? fmt::format(" s6:{:016x}", hash)
                       : fmt::format(" s1:{:016x}+s5:{:016x}", hash, fragment_hash);
        slowest += fmt::format("={:.3f}ms/{}x(max {:.3f}ms size {}x{}x{})", totals.total_ms,
                               totals.count, totals.max_ms, totals.max_size[0],
                               totals.max_size[1], totals.max_size[2]);
    }
    LOG_INFO(Render_Vulkan,
             "DKB gpu_work renderer_frame={} timed={} timed_sum_ms={:.3f} slowest:{}",
             trace_sample_frame, count, total_ms, slowest);
}

void Scheduler::TraceGpuSubmission() {
    if (trace_gpu_pending) {
        master_semaphore->Refresh();
        if (!master_semaphore->IsFree(trace_gpu_tick)) {
            return;
        }
        struct Result {
            u64 timestamp;
            u64 available;
        };
        std::array<Result, TraceMaxSubmissions * 2> results{};
        const auto result = device.GetLogical().GetQueryResults(
            *trace_query_pool, 0, trace_gpu_count * 2, trace_gpu_count * 2 * sizeof(Result),
            results.data(), sizeof(Result),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_NOT_READY) {
            return;
        }
        if (result != VK_SUCCESS) {
            LOG_WARNING(Render_Vulkan, "DKB gpu_timestamp disabled: query result {}",
                        static_cast<int>(result));
            trace_gpu_failed = true;
            return;
        }
        double total_ms{};
        std::array<std::pair<double, u32>, 4> slowest{};
        for (u32 i = 0; i < trace_gpu_count * 2; i += 2) {
            if (results[i].available == 0 || results[i + 1].available == 0) {
                return;
            }
            const auto ticks = Common::DkbFrameTrace::TimestampDelta(
                results[i].timestamp, results[i + 1].timestamp, trace_timestamp_bits);
            const double duration_ms = static_cast<double>(ticks) * trace_timestamp_period / 1e6;
            total_ms += duration_ms;
            if (duration_ms >= 10.0 && duration_ms > slowest.back().first) {
                slowest.back() = {duration_ms, i / 2};
                std::ranges::sort(slowest, std::greater<>{});
            }
        }
        const double span_ms = trace_gpu_skipped == 0
            ? static_cast<double>(Common::DkbFrameTrace::TimestampDelta(
                  results[0].timestamp, results[trace_gpu_count * 2 - 1].timestamp,
                  trace_timestamp_bits)) * trace_timestamp_period / 1e6
            : -1.0;
        LOG_INFO(Render_Vulkan,
                 "DKB gpu_frame renderer_frame={} last_submission_tick={} "
                 "gpu_frame_span_ms={:.3f} gpu_submission_sum_ms={:.3f} "
                 "measured_submissions={} dropped_submissions={} "
                 "valid_bits={} period_ns={}",
                 trace_sample_frame, trace_gpu_tick, span_ms, total_ms, trace_gpu_count,
                 trace_gpu_skipped, trace_timestamp_bits, trace_timestamp_period);
        for (const auto& [duration_ms, index] : slowest) {
            if (duration_ms < 10.0) {
                continue;
            }
            const auto& work = trace_submissions[index];
            std::string shaders;
            for (u32 i = 0; i < work.num_shaders; ++i) {
                const auto& shader = work.shaders[i];
                shaders += fmt::format("{}s{}:{:016x}", shaders.empty() ? "" : ",",
                                       shader.stage, shader.hash);
            }
            LOG_INFO(Render_Vulkan,
                     "DKB gpu_submission renderer_frame={} tick={} gpu_ms={:.3f} "
                     "draw_attempts={} dispatch_attempts={} shaders={} untracked_uses={}",
                     trace_sample_frame, work.tick, duration_ms, work.draw_attempts,
                     work.dispatch_attempts, shaders, work.untracked_uses);
        }
        ReportWorkTimes();
        trace_gpu_tick = 0;
        trace_gpu_count = 0;
        trace_gpu_skipped = 0;
        trace_gpu_pending = false;
        trace_gpu_rate.next = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
    if (!trace_gpu_sampling || trace_gpu_count == TraceMaxSubmissions) {
        return;
    }
    current_upload_cmdbuf.ResetQueryPool(*trace_query_pool, trace_gpu_count * 2, 2);
    if (trace_gpu_count == 0 && trace_work_pool) {
        // Draws are timed inside render passes, where queries cannot be reset.
        current_upload_cmdbuf.ResetQueryPool(*trace_work_pool, 0, TraceMaxWork * 2);
    }
    current_upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *trace_query_pool,
                                        trace_gpu_count * 2);
    trace_gpu_recording = true;
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (uploads_pending) {
        // Keep the copies in the tick that protects their staging regions and buffer lifetimes.
        FlushUploads();
    }
    if (VideoCommon::Nx2Profile::Enabled()) {
        g_profile_submits.fetch_add(1, std::memory_order_relaxed);
        ReportSubmitCounts();
    }
    EndPendingOperations();
    InvalidateState();

    // FRITCHII_MEMSTAGE_TICKS: everything this tick holds is recorded; the memory stage
    // publishes which packet numbers it covers (vk_mem_stage.cpp, "Packet ticks").
    SeamHook::TickEnd(master_semaphore->CurrentTick());
    const u64 signal_value = master_semaphore->NextTick();
    // The recording thread's hand-off of this tick to the worker (and the GPU).
    if (Common::Nx2Timeline::Active()) {
        Common::Nx2Timeline::Instant("submit", {"tick", signal_value});
    }
    // FRITCHII_SLOW_GPU: who submits, from where (a slow line's and a breadcrumb's "from").
    const SlowGpu::Origin origin{
        .thread = Common::Nx2Hitch::t_thread_name,
        .file = submit_site.file_name(),
        .line = submit_site.line(),
        .finish = submit_is_finish,
        .signals = signal_semaphore != VK_NULL_HANDLE,
        .waits = wait_semaphore != VK_NULL_HANDLE,
    };
    submit_site = {};
    submit_is_finish = false;
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value, origin,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        // FRITCHII_SLOW_GPU: the diagnostic stamps below are not the submission's work.
        vk::t_cmd_stats = nullptr;
        // FRITCHII_SIM_GPU_SLOW: the added load, before the stamps below, so the overlay's GPU
        // time and the slow submission lines show the simulated card. Nothing when off.
        sim_gpu_slow.End(cmdbuf, signal_value);
        if (trace_gpu_recording) {
            cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_query_pool,
                                 trace_gpu_count * 2 + 1);
            trace_gpu_tick = signal_value;
            trace_work.tick = signal_value;
            trace_submissions[trace_gpu_count] = trace_work;
            ++trace_gpu_count;
            trace_gpu_recording = false;
        } else if (trace_gpu_sampling) {
            ++trace_gpu_skipped;
        }
        if (budget_recording) {
            const u32 slot = budget_next_slot % BudgetSlots;
            cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *budget_pool,
                                  slot * 2 + 1);
            budget_ticks[slot] = signal_value;
            budget_last_tick = signal_value;
            ++budget_next_slot;
            ++budget_pending;
            budget_recording = false;
        }
        if (trace_query_pool) {
            trace_work = {};
        }
        submit_times.End(cmdbuf, signal_value); // FRITCHII_SUBMIT_TIMES; nothing when off
        // FRITCHII_SLOW_GPU: the end stamp, the contents kept; stops the counting.
        slow_gpu.End(cmdbuf, signal_value, origin);
        const auto end_start = std::chrono::steady_clock::now();
        cmdbuf.End();
        const auto end_done = std::chrono::steady_clock::now();

        if (on_submit) {
            on_submit();
        }

        // fable52_answer_stalls: Swapchain::Present holds submit_mutex across vkQueuePresentKHR,
        // so the wait for it is timed apart from the submit itself.
        const auto lock_start = std::chrono::steady_clock::now();
        const auto lock_stall = Common::Nx2Hitch::StallEnter("worker: wait for the submit lock");
        std::scoped_lock lock{submit_mutex};
        Common::Nx2Hitch::StallLeave(lock_stall);
        const auto submit_start = std::chrono::steady_clock::now();
        // Slow submits (residency work for new allocations?) with what was made since the last
        // one; images are counted only with EDEN_NX2_IMAGE_ALLOC_TIMING=1.
        SCOPE_EXIT {
            static u64 images_at_last_submit = 0;
            const u64 images = MemoryAllocator::ReadImageTiming().images;
            const auto submit_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - submit_start)
                                       .count();
            const auto end_us =
                std::chrono::duration_cast<std::chrono::microseconds>(end_done - end_start)
                    .count();
            const auto lock_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     submit_start - lock_start)
                                     .count();
            if (submit_us > 4000 || end_us > 4000 || lock_us > 4000) {
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 200) {
                    LOG_INFO(Render_Vulkan,
                             "Slow submit: vkQueueSubmit {} us, waiting for the submit lock "
                             "(present) {} us, command buffer End {} us, tick {}; {} images made "
                             "since the previous submit",
                             submit_us, lock_us, end_us, signal_value,
                             images - images_at_last_submit);
                }
            }
            images_at_last_submit = images;
        };
        // Lines up with DkbFrameTrace's begin stamp and the driver's queue packets.
        if (Common::Nx2Timeline::Active()) {
            Common::Nx2Timeline::Instant("vkQueueSubmit", {"tick", signal_value});
        }
        const Common::Nx2Hitch::StallScope submit_stall{"worker: vkQueueSubmit"};
        const u64 slow_call_ns = SlowGpu::NowNs();
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // FRITCHII_SLOW_GPU: the CPU side of the queue wait estimate; one compare when off.
            slow_gpu.Submitted(signal_value, slow_call_ns);
            if (VideoCore::Nx2Debug::g_test_device_loss.load(std::memory_order_relaxed) != 0)
                [[unlikely]] {
                device.ReportLoss();
            }
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
        if (trace_frame_end != 0) {
            if (trace_gpu_sampling) {
                trace_gpu_pending = true;
                trace_gpu_sampling = false;
            }
            if (trace_query_pool && !trace_gpu_failed && !trace_gpu_pending &&
                trace_gpu_rate.Ready(std::chrono::steady_clock::now())) {
                trace_sample_frame = trace_frame_end + 1;
                trace_gpu_sampling = true;
            }
            trace_frame_end = 0;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    SeamHook::Watch(SeamHook::Use::InvalidateState);
    ++recording_epoch;
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }
        ++recording_epoch;
        fragment_barrier.Reset();
        render_pass_has_stores = false;

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        state.renderpass = VkRenderPass{};
        state.clear_variant = false;
        num_renderpass_images = 0;
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
