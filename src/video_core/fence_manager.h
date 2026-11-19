// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

#include "common/common_types.h"
#include "common/fritchii_lock_census.h"
#include "common/logging.h"
#include "common/nx2_timeline.h"
#include "common/settings.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_fastdraw_count.h"
#include "common/thread.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/fence_release_stats.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/gpu_wait_site.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_stage_link.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

class FenceBase {
public:
    explicit FenceBase(bool is_stubbed_) : is_stubbed{is_stubbed_} {}

    bool IsStubbed() const {
        return is_stubbed;
    }

protected:
    bool is_stubbed;
};

template <typename Traits>
class FenceManager {
    using TFence = typename Traits::FenceType;
    using TTextureCache = typename Traits::TextureCacheType;
    using TBufferCache = typename Traits::BufferCacheType;
    using TQueryCache = typename Traits::QueryCacheType;
    static constexpr bool can_async_check = Traits::HAS_ASYNC_CHECK;

public:
    /// Notify the fence manager about a new frame
    void TickFrame() {
        std::unique_lock lock(ring_guard);
        delayed_destruction_ring.Tick();
    }

    // Unlike other fences, this one doesn't
    void SignalOrdering() {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        // One follower (FRITCHII_ONEF_POST_IDLE): a posted wait for idle's grouping ran on E.
        if (skip_buffer_half) [[unlikely]] {
            return;
        }
        // FRITCHII_MEMSTAGE_PASS (fable_stage_levelpoints 6.5): served, this is a passed entry's
        // flush grouping. MARKS_AHEAD off: GPUMem passed it only with no uncommitted flush (rule
        // C): nothing to group, and the back stays out of the buffer cache; a set note there is a
        // must-be-0 (it then groups as today). MARKS_AHEAD on (14.7): GPUMem does not test rule C;
        // a played packet's marks may still queue a download (AheadDue, as CommitCacheFlushes),
        // so the back groups here under the lock at its own place, and that is no failure.
        if (MemoryStageLink::BackServed()) [[unlikely]] {
            if (!buffer_cache.MayHaveUncommittedFlushes() && !buffer_cache.AheadDue()) {
                return;
            }
            if (!buffer_cache.MarksAhead()) {
                MemoryStageLink::Note(MemoryStageLink::Check::PassFlushes,
                                      "FenceManager::SignalOrdering");
            }
        }
        std::scoped_lock lock{buffer_cache.mutex};
        // FRITCHII_MEMSTAGE_MARKS_AHEAD: on the back, under the lock, also while served
        // (AccumulateFlushes promotes the due marks first).
        const VideoCommon::MemoryStageLink::BackCacheScope back_cache;
        buffer_cache.AccumulateFlushes();
    }

    void SignalReference() {
        std::function<void()> do_nothing([] {});
        SignalFence(std::move(do_nothing));
    }

    /// One follower (FRITCHII_ONEF_POST_FENCE): set on R around a posted fence, whose buffer
    /// half E owns (and found empty when it posted).
    bool skip_buffer_half{};
    /// One follower (fable_b 19.6): with skip_buffer_half, E committed the buffer half's
    /// downloads at the fence's place (their copies in an op packet before it): this fence
    /// carries them (made, flushed, popped at its release as today).
    bool onef_buffer_committed{};

    /// One follower (FRITCHII_ONEF_SEM_E, fable_b 20.3), E at the sync point's place, fences not
    /// delayed: SignalSyncPoint's guest increment (false past the table, as it logs), then the
    /// host increment its fence's callback runs at once. R then makes an empty fence.
    bool OneFollowerSyncPointGuest(u32 value) {
        if (value >= Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS) {
            LOG_ERROR(HW_GPU, "Signal of syncpoint {} past the table, ignored", value);
            return false;
        }
        if (Tegra::g_nx2_render_clock_wanted.load(std::memory_order_relaxed)) {
            Tegra::Nx2NoteSyncpointSignal(
                value, static_cast<u32>(syncpoint_manager.GetGuestSyncpointValue(value)) + 1);
        }
        // INTEL_FORK_CPU_SYNC_DONE: R's fence for this place carries the matching CpuDoneEnd
        // (CpuDoneOp, vk_rasterizer.cpp's posted fence kind 3).
        syncpoint_manager.CpuDoneBegin(value);
        syncpoint_manager.IncrementGuest(value);
        return true;
    }

    /// INTEL_FORK_CPU_SYNC_DONE: the operation a fence carries for a CpuDoneBegin (empty when
    /// off). It runs where delayed fences run their callbacks: at the fence's release.
    std::function<void()> CpuDoneOp(u32 value) {
        if (!Tegra::Host1x::SyncpointManager::CpuSyncDone()) {
            return {};
        }
        return [this, value] { syncpoint_manager.CpuDoneEnd(value); };
    }
    void OneFollowerSyncPointHost(u32 value) {
        syncpoint_manager.IncrementHost(value);
    }

    void SyncOperation(std::function<void()>&& func) {
        // Not only the GPU thread queues these (query cache work runs elsewhere too), while
        // SignalFence moves the list into pending_operations: unguarded, the list was corrupted
        // and the fencing thread crashed walking it. Its own lock, held for no callbacks.
        {
            std::scoped_lock lock{uncommitted_guard};
            uncommitted_operations.emplace_back(std::move(func));
            ++operation_stamp;
        }
        work_epoch.fetch_add(1, std::memory_order_release);
    }

    /// EXP119 (fritchii_report_runs.h): SyncOperation, returning the stamp that names `func` as
    /// the last operation queued. Every other queued operation and every hand-over of the list
    /// to a fence (TakeUncommittedOperations, FoldStubbedFence) moves the stamp on.
    u64 SyncOperationStamped(std::function<void()>&& func) {
        u64 stamp;
        {
            std::scoped_lock lock{uncommitted_guard};
            uncommitted_operations.emplace_back(std::move(func));
            stamp = ++operation_stamp;
        }
        work_epoch.fetch_add(1, std::memory_order_release);
        return stamp;
    }

    /// EXP119: append(context) under the list's lock while `stamp` is still current: the
    /// operation it names is then the last one in the uncommitted list and has not run, so what
    /// it appends runs where an operation queued now would. No epoch bump: the list was not
    /// empty and stays so (FencesQuiet's answer can't change).
    bool AppendToLastOperation(u64 stamp, void (*append)(void*), void* context) {
        std::scoped_lock lock{uncommitted_guard};
        if (stamp == 0 || stamp != operation_stamp) {
            return false;
        }
        append(context);
        return true;
    }

    /// EXP119=2: SyncOperation that leaves the stamp as it is (the verify's own operations).
    void SyncOperationKeepStamp(std::function<void()>&& func) {
        {
            std::scoped_lock lock{uncommitted_guard};
            uncommitted_operations.emplace_back(std::move(func));
        }
        work_epoch.fetch_add(1, std::memory_order_release);
    }

    /// `done_op` (INTEL_FORK_CPU_SYNC_DONE, CpuDoneOp): runs at this fence's release whatever
    /// the fence mode (an empty fence hands it to the newest queued fence, or runs it at once
    /// when none is queued or being released). Empty: as before.
    void SignalFence(std::function<void()>&& func, std::function<void()>&& done_op = {}) {
        Nx2Profile::Scope profile_scope{Nx2Profile::Zone::Fence};
        const bool has_done = static_cast<bool>(done_op);
        // INTEL_FORK_CPU_SYNC_DONE=2: the fence is made real (never skipped, never stubbed).
        const bool done_real =
            has_done && Tegra::Host1x::SyncpointManager::CpuSyncDoneMode() == 2;
        // The accuracy can change while the game runs (settings dialog, Settings::UpdateGPUAccuracy
        // at each command list end): read once, every decision of this fence uses one value.
        // EXP33: a fence that writes a fence-flagged query report is never
        // skipped as empty, and its operation waits for the fence: a skipped fence runs its
        // operation at once, before the host GPU has run the work the report follows, and
        // Ocarina of Time then reads a wrong report (white flashes on leaves and walls at low GPU
        // accuracy). Only set by the query cache while EXP33 is on.
        const bool delay_fence = DelayFences() || VideoCore::g_signaling_query_report_fence;
        // EDEN_NX2_EXP28 (the original's FENCE_TRIM, FENCE_FOLD), one value per fence.
        const int fence_trims = VideoCore::Nx2Debug::Experiment(28);
        const bool trim = (fence_trims & 1) != 0;
        // EDEN_NX2_BACK_FENCE_STATS (fable52_answer_fences2 4): this fence's parts, timed.
        namespace BackFence = FenceReleaseStats::BackFence;
        BackFence::Timer stats_timer{VideoCore::Nx2Debug::BackFenceStats()};
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        stats_timer.BeginCommit();
        auto [should_flush, committed_flushes] = CommitCacheFlushes();
        stats_timer.EndCommit();
        if (Nx2Profile::Enabled()) [[unlikely]] {
            NoteFenceDecision(delay_fence, committed_flushes, trim);
        }
        if (!delay_fence && !done_real && committed_flushes == 0 && SkipEmptyFence(trim)) {
            Nx2Profile::Scope skipped_scope{Nx2Profile::Zone::FenceSkipped};
            stats_timer.Skip();
            func();
            if (has_done) {
                PlaceDoneOp(std::move(done_op));
            }
            stats_timer.Mark(BackFence::Operation);
            rasterizer.InvalidateGPUCache();
            stats_timer.Mark(BackFence::Invalidate);
            stats_timer.Done(BackFence::Kind::Skipped, true, false);
            return;
        }
        skipped_fences = 0;
        // FRITCHII_DELAY_BACK_KIND=3 ("F", fable_stage_levelpoints 18.3): every made fence,
        // after the skip test, on the back thread. Off: one relaxed load and a branch.
        VideoCommon::GpuSplit::DelayBackIf(VideoCommon::GpuSplit::BackDelayKind::Fences);
        should_flush |= query_cache.HasUncommittedFlushes();
        should_flush |= done_real;
        query_cache.CommitAsyncFlushes();
        if constexpr (can_async_check) {
            // EXP28 & 2: a fence with nothing to wait for on the host GPU carries only queued
            // work (FoldStubbedFence).
            if ((fence_trims & 2) != 0 && !should_flush && committed_flushes == 0) {
                // As the made fence below: its callback runs now, before its queued work.
                if (!delay_fence) {
                    stats_timer.Skip();
                    func();
                    stats_timer.Mark(BackFence::Operation);
                }
                FoldStubbedFence(delay_fence, func, done_op);
                if (delay_fence) {
                    stats_timer.Skip();
                    SubmitDeferredWork();
                    stats_timer.Mark(BackFence::SubmitDeferred);
                }
                stats_timer.Skip();
                rasterizer.InvalidateGPUCache();
                stats_timer.Mark(BackFence::Invalidate);
                stats_timer.Done(BackFence::Kind::Folded, true, false);
                return;
            }
        }
        TFence new_fence = CreateFence(!should_flush);
        if constexpr (can_async_check) {
            stats_timer.Skip();
            guard.lock();
            stats_timer.Mark(BackFence::Guard);
        }
        {
            std::scoped_lock lock{uncommitted_guard};
            if (delay_fence) {
                uncommitted_operations.emplace_back(std::move(func));
            }
            if (has_done) {
                uncommitted_operations.emplace_back(std::move(done_op));
            }
            pending_operations.emplace_back(TakeUncommittedOperations());
        }
        pending_flushes.push_back(committed_flushes);
        if constexpr (can_async_check) {
            pending_query_pops.push_back(1);
        }
        QueueFence(new_fence);
        if (!delay_fence) {
            stats_timer.Skip();
            func();
            stats_timer.Mark(BackFence::Operation);
        }
        fences.push(std::move(new_fence));
        fences_in_flight.fetch_add(1, std::memory_order_relaxed);
        work_epoch.fetch_add(1, std::memory_order_release);
        if (should_flush) {
            stats_timer.Skip();
            rasterizer.FlushCommands();
            stats_timer.Mark(BackFence::FlushCommands);
        }
        if constexpr (can_async_check) {
            guard.unlock();
            cv.notify_all();
        }
        if (delay_fence) {
            // Its callback runs on the fencing thread once the fence's work is done, and the
            // guest or WaitPendingFences may block on that callback: a submission left for
            // later (Scheduler::DeferFlush) would wait for the back to go idle, which it then
            // never does (GPU accuracy switched to High while the game ran, 24 s and more).
            stats_timer.Skip();
            SubmitDeferredWork();
            stats_timer.Mark(BackFence::SubmitDeferred);
        }
        stats_timer.Skip();
        rasterizer.InvalidateGPUCache();
        stats_timer.Mark(BackFence::Invalidate);
        stats_timer.Done(BackFence::Kind::Made, committed_flushes == 0, should_flush);
    }

    void SignalSyncPoint(u32 value) {
        // The id comes from a command list (the 3D engine's sync method, the puller's fence
        // action). Past the syncpoint table, the increment would walk memory after it as an
        // action list; command words read wrong carry any id (Splatoon Raiders on Linux).
        if (value >= Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS) {
            LOG_ERROR(HW_GPU, "Signal of syncpoint {} past the table, ignored", value);
            return;
        }
        // The render-frame clock (video_core/nx2.h), before the increment: a flip its actions
        // wake must already see the bump.
        if (Tegra::g_nx2_render_clock_wanted.load(std::memory_order_relaxed)) {
            Tegra::Nx2NoteSyncpointSignal(
                value, static_cast<u32>(syncpoint_manager.GetGuestSyncpointValue(value)) + 1);
        }
        // INTEL_FORK_CPU_SYNC_DONE: counted as not done for the guest CPU until this fence's
        // release (CpuDoneOp). The host value is still raised as before.
        syncpoint_manager.CpuDoneBegin(value);
        syncpoint_manager.IncrementGuest(value);
        std::function<void()> func([this, value] { syncpoint_manager.IncrementHost(value); });
        SignalFence(std::move(func), CpuDoneOp(value));
    }

    void WaitPendingFences([[maybe_unused]] bool force) {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<true>();
        } else {
            if (!force) {
                return;
            }
            std::mutex wait_mutex;
            std::condition_variable wait_cv;
            std::atomic<bool> wait_finished{};
            std::function<void()> func([&] {
                std::scoped_lock lk(wait_mutex);
                wait_finished.store(true, std::memory_order_relaxed);
                wait_cv.notify_all();
            });
            SignalFence(std::move(func));
            // The fence's commands can be held back by a deferred flush that only this thread
            // submits. The fencing thread waits for that submission before releasing the fence,
            // so waiting here without submitting deadlocks (Ocarina of Time entering gameplay).
            SubmitDeferredWork();
            BackWaitScope wait_scope{"FenceManager::WaitPendingFences (fencing thread callback)"};
            std::unique_lock lk(wait_mutex);
            wait_cv.wait(
                lk, [&wait_finished] { return wait_finished.load(std::memory_order_relaxed); });
        }
    }

    /// GPU thread (the split's back between entries): whether WaitPendingFences(true) would only
    /// invalidate the GPU cache, as no download, query, fence operation or (`strict`, or with
    /// delayed fences) pending fence is left for it. A lock held elsewhere counts as work.
    /// `epoch`: FenceWorkEpoch() as of the check.
    bool FencesQuiet(bool strict, u64& epoch) {
        epoch = work_epoch.load(std::memory_order_acquire);
        if ((strict || DelayFences()) && fences_in_flight.load(std::memory_order_acquire) != 0) {
            return false;
        }
        {
            std::unique_lock lock{uncommitted_guard, std::try_to_lock};
            if (!lock || !uncommitted_operations.empty()) {
                return false;
            }
        }
        {
            std::unique_lock lock{texture_cache.mutex, std::try_to_lock};
            if (!lock || texture_cache.HasAsyncFlushWork()) {
                return false;
            }
        }
        {
            std::unique_lock lock{buffer_cache.mutex, std::try_to_lock};
            if (lock) {
                buffer_cache.PromoteAhead(); // FRITCHII_MEMSTAGE_MARKS_AHEAD
            }
            if (!lock || buffer_cache.HasUncommittedFlushes()) {
                return false;
            }
        }
        if (query_cache.HasUncommittedFlushes()) {
            return false;
        }
        if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            if (query_cache.HasPendingFenceWork()) {
                return false;
            }
        }
        return true;
    }

    /// Any thread: changes whenever a fence or a fence operation is added.
    u64 FenceWorkEpoch() const {
        return work_epoch.load(std::memory_order_acquire);
    }

protected:
    explicit FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                          TTextureCache& texture_cache_, TBufferCache& buffer_cache_,
                          TQueryCache& query_cache_)
        : rasterizer{rasterizer_}, gpu{gpu_}, syncpoint_manager{gpu.Host1x().GetSyncpointManager()},
          texture_cache{texture_cache_}, buffer_cache{buffer_cache_}, query_cache{query_cache_} {
        if constexpr (can_async_check) {
            fence_thread =
                std::jthread([this](std::stop_token token) { ReleaseThreadFunc(token); });
        }
    }

    virtual ~FenceManager() {
        if constexpr (can_async_check) {
            fence_thread.request_stop();
            cv.notify_all();
            fence_thread.join();
        }
    }

    /// Creates a Fence Interface, does not create a backend fence if 'is_stubbed' is
    /// true
    virtual TFence CreateFence(bool is_stubbed) = 0;
    /// Queues a fence into the backend if the fence isn't stubbed.
    virtual void QueueFence(TFence& fence) = 0;
    /// Notifies that the backend fence has been signaled/reached in host GPU.
    virtual bool IsFenceSignaled(TFence& fence) const = 0;
    /// Waits until a fence has been signalled by the host GPU.
    virtual void WaitFence(TFence& fence) = 0;
    /// Submits work whose submission the backend deferred (called before blocking on a fence).
    virtual void SubmitDeferredWork() {}

    VideoCore::RasterizerInterface& rasterizer;
    Tegra::GPU& gpu;
    Tegra::Host1x::SyncpointManager& syncpoint_manager;
    TTextureCache& texture_cache;
    TBufferCache& buffer_cache;
    TQueryCache& query_cache;

private:
    /// Vectors, not deques: MSVC's deque of std::function allocated a block per operation.
    using OperationList = std::vector<std::function<void()>>;
    static constexpr size_t MaxSpareOperationLists = 32;

    template <bool force_wait>
    void TryReleasePendingFences() {
        while (!fences.empty()) {
            TFence& current_fence = fences.front();
            if (ShouldWait() && !IsFenceSignaled(current_fence)) {
                if constexpr (force_wait) {
                    WaitFence(current_fence);
                } else {
                    return;
                }
            }
            VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::Fence);
            PopAsyncFlushes(pending_flushes.front());
            pending_flushes.pop_front();
            auto operations = std::move(pending_operations.front());
            pending_operations.pop_front();
            for (auto& operation : operations) {
                operation();
            }
            operations.clear();
            {
                std::scoped_lock lock{uncommitted_guard};
                KeepSpareOperations(operations);
            }
            {
                std::unique_lock lock(ring_guard);
                delayed_destruction_ring.Push(std::move(current_fence));
            }
            fences.pop();
            fences_in_flight.fetch_sub(1, std::memory_order_release);
        }
    }

    void ReleaseThreadFunc(std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPUFencingThread");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        Common::SetCurrentThreadToPerformanceCores();
        // FRITCHII_FENCE_COPY_UNLOCKED's line: E's lock waits behind this thread.
        Common::LockCensus::g_fence_thread.store(std::this_thread::get_id(),
                                                 std::memory_order_relaxed);

        // Every fence the host GPU has already passed is released together: Switch 2 titles
        // signal thousands a second and the host GPU completes them in bursts, so each cache's
        // lock is taken once per burst instead of once per fence (the GPU thread, which holds
        // them for nearly every command, spun, slept and woke this thread for each one).
        // EDEN_NX2_FENCE_BATCH=0: one fence at a time (for comparisons).
        static const size_t MaxBatch = [] {
            const char* value = std::getenv("EDEN_NX2_FENCE_BATCH");
            return value != nullptr && value[0] == '0' ? size_t{1} : size_t{64};
        }();
        struct Released {
            TFence fence;
            OperationList operations;
            u8 flushes;
            u32 query_pops; ///< Query cache entries: one per fence, folded ones included
        };
        std::vector<Released> batch;
        batch.reserve(MaxBatch);
        const auto take_front = [&] {
            batch.push_back({std::move(fences.front()), std::move(pending_operations.front()),
                             pending_flushes.front(), pending_query_pops.front()});
            fences.pop();
            pending_operations.pop_front();
            pending_flushes.pop_front();
            pending_query_pops.pop_front();
        };
        int siblings_free_applied = 0; ///< FRITCHII_GPU_SIBLINGS_FREE as this thread applied it
        while (!stop_token.stop_requested()) {
            // FRITCHII_GPU_SIBLINGS_FREE (live): off the GPU threads' cores before the wait.
            Common::KeepOffGpuCores(siblings_free_applied);
            {
                std::unique_lock lock(guard);
                // Work the GPU thread runs inline (FoldStubbedFence) goes before any fence
                // queued meanwhile.
                {
                    // Fences come from SignalFence (the split's back).
                    Common::Nx2Timeline::Span wait_span{"fence: wait for work"};
                    cv.wait(lock, [&] {
                        return stop_token.stop_requested() || (!fences.empty() && !inline_running);
                    });
                }
                if (stop_token.stop_requested()) [[unlikely]] {
                    return;
                }
                take_front();
                fence_thread_busy = true;
            }
            // FRITCHII_SIBLING_CENSUS (dry): where this thread woke up to work.
            Common::NoteCensusCpu(Common::CensusThread::Fencing);
            if (!batch.front().fence->IsStubbed()) {
                // The host GPU reaching the fence's submission.
                Common::Nx2Timeline::Span wait_span{"fence: wait for fence"};
                WaitFence(batch.front().fence);
            }
            // The batch's landings, waiter latches and operations (host syncpoint increments,
            // which signal the guest's GPU events): the host GPU -> guest hand-off.
            Common::Nx2Timeline::ArgSpan release_span{"fence: release"};
            {
                std::unique_lock lock(guard);
                while (batch.size() < MaxBatch && !fences.empty() &&
                       (fences.front()->IsStubbed() || IsFenceSignaled(fences.front()))) {
                    take_front();
                }
            }
            if (VideoCommon::Nx2FastDrawCount::g_on && !batch.empty()) [[unlikely]] {
                VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::Fence);
            }
            release_span.Set(0, {"fences", batch.size()});
            // EDEN_NX2_FENCE_RELEASE_STATS (fable52_answer_chicken 5.1): split this release.
            const bool stats = FenceReleaseStats::g_enabled;
            const FenceReleaseStats::Clock::time_point release_start =
                stats ? FenceReleaseStats::Clock::now() : FenceReleaseStats::Clock::time_point{};
            size_t texture_pops = 0;
            size_t buffer_pops = 0;
            for (const Released& released : batch) {
                texture_pops += (released.flushes & TextureFlushes) != 0 ? 1 : 0;
                buffer_pops += (released.flushes & BufferFlushes) != 0 ? 1 : 0;
            }
            // EDEN_NX2_EXP74 (live, us, 0 = off; fable52_answer_perdraw 1): a batch of 20-60
            // fences held the cache lock 1-13 ms, and the back waited for it in its draws and
            // fences (boss wave: "cache lock waits: GPU back" 24 -> 72 ms/s). After this long
            // under the lock the thread lets it go between two pops, so the back can take it;
            // the same pops in the same order, each whole under the lock.
            const int hand_back_us = VideoCore::Nx2Debug::Experiment(74);
            // FRITCHII_FENCE_YIELD_E (helper CP, common/fritchii_lock_census.h): between two
            // pops, the lock goes to E if E waits for it (1), or that chance is counted (3).
            const int yield_e = Common::LockCensus::FenceYieldE();
            const auto pop_all = [hand_back_us, yield_e](auto& mutex, size_t pops, auto&& pop) {
                std::unique_lock lock{mutex};
                auto held_since = std::chrono::steady_clock::now();
                u64 chances = 0;
                u64 yield_ns = 0;
                for (size_t i = 0; i < pops; ++i) {
                    pop();
                    if (yield_e != 0 && i + 1 < pops && mutex.BackWaiting()) [[unlikely]] {
                        ++chances;
                        if (yield_e == 1) {
                            // E spins for the lock: it takes it at the unlock. Taken back once E
                            // has it (its mark is gone) or after 50 us; the next pop then waits
                            // for E's hold to end, as E waited for the pops before.
                            const auto yield_start = std::chrono::steady_clock::now();
                            lock.unlock();
                            while (mutex.BackWaiting() &&
                                   std::chrono::steady_clock::now() - yield_start <
                                       std::chrono::microseconds{50}) {
                            }
                            lock.lock();
                            held_since = std::chrono::steady_clock::now();
                            yield_ns += static_cast<u64>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    held_since - yield_start)
                                    .count());
                            continue;
                        }
                    }
                    if (hand_back_us > 0 && i + 1 < pops) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - held_since >= std::chrono::microseconds{hand_back_us}) {
                            lock.unlock();
                            std::this_thread::yield();
                            lock.lock();
                            held_since = std::chrono::steady_clock::now();
                        }
                    }
                }
                if (yield_e != 0) [[unlikely]] {
                    Common::LockCensus::NoteFencePops(yield_e, pops, chances, mutex.BackWaiting(),
                                                      yield_ns);
                }
            };
            // pop_all with the stats: each pop described and timed, the mutex's waits and holds.
            const auto pop_all_stats = [hand_back_us](auto& mutex, size_t pops, bool texture,
                                                      auto&& describe, auto&& pop) {
                namespace Stats = FenceReleaseStats;
                Stats::Totals& totals = Stats::Get();
                u64& wait_ns = texture ? totals.texture_wait_ns : totals.buffer_wait_ns;
                u64& held_ns = texture ? totals.texture_held_ns : totals.buffer_held_ns;
                auto wait_start = Stats::Clock::now();
                std::unique_lock lock{mutex};
                auto held_since = Stats::Clock::now();
                wait_ns += Stats::NsBetween(wait_start, held_since);
                auto hand_back_since = std::chrono::steady_clock::now();
                for (size_t i = 0; i < pops; ++i) {
                    Stats::PopLanding landing;
                    describe(landing);
                    const auto pop_start = Stats::Clock::now();
                    pop();
                    Stats::Pop(texture, Stats::NsSince(pop_start), landing);
                    if (hand_back_us > 0 && i + 1 < pops) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - hand_back_since >= std::chrono::microseconds{hand_back_us}) {
                            held_ns += Stats::NsSince(held_since);
                            lock.unlock();
                            std::this_thread::yield();
                            wait_start = Stats::Clock::now();
                            lock.lock();
                            held_since = Stats::Clock::now();
                            wait_ns += Stats::NsBetween(wait_start, held_since);
                            hand_back_since = std::chrono::steady_clock::now();
                        }
                    }
                }
                held_ns += Stats::NsSince(held_since);
            };
            if (texture_pops != 0) {
                if (stats) [[unlikely]] {
                    pop_all_stats(
                        texture_cache.mutex, texture_pops, true,
                        [this](FenceReleaseStats::PopLanding& landing) {
                            texture_cache.Nx2DescribeNextPop(landing);
                        },
                        [this] { texture_cache.PopAsyncFlushes(); });
                } else {
                    pop_all(texture_cache.mutex, texture_pops,
                            [this] { texture_cache.PopAsyncFlushes(); });
                }
            }
            if (buffer_pops != 0) {
                if (stats) [[unlikely]] {
                    pop_all_stats(
                        buffer_cache.mutex, buffer_pops, false,
                        [this](FenceReleaseStats::PopLanding& landing) {
                            buffer_cache.Nx2DescribeNextPop(landing);
                        },
                        [this] { buffer_cache.PopAsyncFlushes(); });
                } else if (const int copy_unlocked = Common::LockCensus::FenceCopyUnlocked();
                           copy_unlocked != 0) {
                    // FRITCHII_FENCE_COPY_UNLOCKED (helper CQ): the same pops in the same order,
                    // each landing's copy out of the lock (BufferCache::PopAsyncBuffersUnlocked).
                    Common::LockCensus::g_fence_copy_watch.store(true, std::memory_order_relaxed);
                    Common::LockCensus::FenceCopyBatch copy_batch{};
                    copy_batch.mode = copy_unlocked;
                    buffer_cache.PopAsyncBuffersUnlocked(buffer_pops, copy_unlocked, copy_batch);
                    Common::LockCensus::NoteFenceCopy(copy_batch);
                } else {
                    pop_all(buffer_cache.mutex, buffer_pops,
                            [this] { buffer_cache.PopAsyncFlushes(); });
                    // Once the switch was on, its line goes on at 0 (E's waits for both arms).
                    if (Common::LockCensus::g_fence_copy_watch.load(std::memory_order_relaxed))
                        [[unlikely]] {
                        Common::LockCensus::FenceCopyBatch copy_batch{};
                        copy_batch.pops = buffer_pops;
                        Common::LockCensus::NoteFenceCopy(copy_batch);
                    }
                }
            }
            // Queries and operations keep their per-fence order.
            for (Released& released : batch) {
                FenceReleaseStats::Clock::time_point stats_mark{};
                if (stats) [[unlikely]] {
                    stats_mark = FenceReleaseStats::Clock::now();
                }
                for (u32 i = 0; i < released.query_pops; ++i) {
                    query_cache.PopAsyncFlushes();
                }
                if (stats) [[unlikely]] {
                    FenceReleaseStats::Totals& totals = FenceReleaseStats::Get();
                    const auto now = FenceReleaseStats::Clock::now();
                    totals.query_pops += released.query_pops;
                    totals.query_ns += FenceReleaseStats::NsBetween(stats_mark, now);
                    totals.operations += released.operations.size();
                    stats_mark = now;
                }
                for (auto& operation : released.operations) {
                    operation();
                }
                if (stats) [[unlikely]] {
                    FenceReleaseStats::Get().operations_ns +=
                        FenceReleaseStats::NsSince(stats_mark);
                }
                released.operations.clear();
            }
            if (VideoCommon::Nx2FastDrawCount::g_on && !batch.empty()) [[unlikely]] {
                VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::Fence);
            }
            {
                std::scoped_lock lock{uncommitted_guard};
                for (Released& released : batch) {
                    KeepSpareOperations(released.operations);
                }
            }
            {
                std::unique_lock lock(ring_guard);
                for (Released& released : batch) {
                    delayed_destruction_ring.Push(std::move(released.fence));
                }
            }
            // Only once their operations ran: FencesQuiet must not see a fence as gone before.
            fences_in_flight.fetch_sub(static_cast<u32>(batch.size()), std::memory_order_release);
            if (stats) [[unlikely]] {
                FenceReleaseStats::Release(batch.size(), FenceReleaseStats::NsSince(release_start));
            }
            batch.clear();
            {
                std::scoped_lock lock(guard);
                fence_thread_busy = false;
            }
        }
    }

    /// EDEN_NX2_EXP28 & 2 (the original's FENCE_FOLD): a fence with nothing to wait for on
    /// the host GPU (no downloads, no query results) carries only queued work: the query cache's
    /// slot cleanup, operations, and with delayed fences its callback (a syncpoint's host
    /// increment). The fencing thread runs that work as soon as every fence before it is
    /// released, so instead of a fence of its own (an allocation, a queue entry and a wake of the
    /// fencing thread):
    /// - with fences queued, the work joins the newest one, released with it in order;
    /// - with nothing queued or being released, the split's back runs it at once (same order);
    /// - otherwise (the fencing thread is releasing fences it took) it gets a fence as before.
    /// `done_op` (INTEL_FORK_CPU_SYNC_DONE): placed after `func`, wherever the work goes.
    void FoldStubbedFence(bool delay_fence, std::function<void()>& func,
                          std::function<void()>& done_op) {
        const bool has_done = static_cast<bool>(done_op);
        std::unique_lock lock{guard};
        if (!fences.empty()) {
            {
                std::scoped_lock ops_lock{uncommitted_guard};
                auto& operations = pending_operations.back();
                for (auto& operation : uncommitted_operations) {
                    operations.emplace_back(std::move(operation));
                }
                uncommitted_operations.clear();
                ++operation_stamp; // EXP119: handed to a fence
                if (delay_fence) {
                    operations.emplace_back(std::move(func));
                }
                if (has_done) {
                    operations.emplace_back(std::move(done_op));
                }
            }
            // This fence's query cache entry (no queries: CommitAsyncFlushes found none).
            ++pending_query_pops.back();
            work_epoch.fetch_add(1, std::memory_order_release);
            fold_counts[0].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!fence_thread_busy && !inline_running && GpuSplit::OnBackThread()) {
            inline_running = true;
            OperationList operations;
            {
                std::scoped_lock ops_lock{uncommitted_guard};
                operations = TakeUncommittedOperations();
            }
            lock.unlock();
            query_cache.PopAsyncFlushes();
            for (auto& operation : operations) {
                operation();
            }
            if (delay_fence) {
                func();
            }
            if (has_done) {
                done_op();
            }
            operations.clear();
            {
                std::scoped_lock ops_lock{uncommitted_guard};
                KeepSpareOperations(operations);
            }
            lock.lock();
            inline_running = false;
            const bool queued = !fences.empty();
            work_epoch.fetch_add(1, std::memory_order_release);
            lock.unlock();
            if (queued) {
                cv.notify_all();
            }
            fold_counts[1].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        TFence new_fence = CreateFence(true);
        {
            std::scoped_lock ops_lock{uncommitted_guard};
            if (delay_fence) {
                uncommitted_operations.emplace_back(std::move(func));
            }
            if (has_done) {
                uncommitted_operations.emplace_back(std::move(done_op));
            }
            pending_operations.emplace_back(TakeUncommittedOperations());
        }
        pending_flushes.push_back(0);
        pending_query_pops.push_back(1);
        QueueFence(new_fence);
        fences.push(std::move(new_fence));
        fences_in_flight.fetch_add(1, std::memory_order_relaxed);
        work_epoch.fetch_add(1, std::memory_order_release);
        lock.unlock();
        cv.notify_all();
        fold_counts[2].fetch_add(1, std::memory_order_relaxed);
    }

    /// INTEL_FORK_CPU_SYNC_DONE: a skipped (empty) fence's done operation runs where a stubbed
    /// fence's operations would: after every fence queued or being released before it.
    void PlaceDoneOp(std::function<void()>&& done_op) {
        if constexpr (can_async_check) {
            std::unique_lock lock{guard};
            if (!fences.empty()) {
                {
                    std::scoped_lock ops_lock{uncommitted_guard};
                    pending_operations.back().emplace_back(std::move(done_op));
                }
                work_epoch.fetch_add(1, std::memory_order_release);
                return;
            }
            if (!fence_thread_busy && !inline_running) {
                lock.unlock();
                done_op();
                return;
            }
            // The fencing thread releases fences it took: a stubbed fence of its own, after them.
            TFence new_fence = CreateFence(true);
            {
                std::scoped_lock ops_lock{uncommitted_guard};
                OperationList operations;
                operations.emplace_back(std::move(done_op));
                pending_operations.emplace_back(std::move(operations));
            }
            pending_flushes.push_back(0);
            pending_query_pops.push_back(0);
            QueueFence(new_fence);
            fences.push(std::move(new_fence));
            fences_in_flight.fetch_add(1, std::memory_order_relaxed);
            work_epoch.fetch_add(1, std::memory_order_release);
            lock.unlock();
            cv.notify_all();
        } else {
            if (!fences.empty()) {
                pending_operations.back().emplace_back(std::move(done_op));
                return;
            }
            done_op();
        }
    }

    /// Caller holds uncommitted_guard. Hands the uncommitted list over and puts a spare list in
    /// its place, keeping its capacity (else a new one reserved for 64): a list that started
    /// empty grew again at every fence, a report per draw adding one operation each.
    OperationList TakeUncommittedOperations() {
        ++operation_stamp; // EXP119: handed to a fence
        OperationList taken = std::move(uncommitted_operations);
        if (!spare_operations.empty()) {
            uncommitted_operations = std::move(spare_operations.back());
            spare_operations.pop_back();
        } else {
            uncommitted_operations = OperationList{}; // moved-from: make it empty explicitly
            uncommitted_operations.reserve(64);
        }
        return taken;
    }

    /// Caller holds uncommitted_guard. `list`, its operations run and cleared (outside the lock:
    /// destroying one can run code), is kept for a later fence while few are spare.
    void KeepSpareOperations(OperationList& list) {
        if (spare_operations.size() < MaxSpareOperationLists && list.capacity() != 0) {
            spare_operations.push_back(std::move(list));
        }
    }

    /// The accuracy can change while the game runs (settings dialog, Settings::UpdateGPUAccuracy
    /// at each command list end): callers read it once per decision.
    static bool DelayFences() {
        return VideoCore::Nx2Debug::DelayedFences();
    }

    bool ShouldWait() const {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return texture_cache.ShouldWaitAsyncFlushes() || buffer_cache.ShouldWaitAsyncFlushes() ||
               query_cache.ShouldWaitAsyncFlushes();
    }

    static constexpr u8 TextureFlushes = 1;
    static constexpr u8 BufferFlushes = 2;

    /// committed: what CommitAsyncFlushes returned for this fence.
    void PopAsyncFlushes(u8 committed) {
        if (committed & TextureFlushes) {
            std::scoped_lock lock{texture_cache.mutex};
            texture_cache.PopAsyncFlushes();
        }
        if (committed & BufferFlushes) {
            std::scoped_lock lock{buffer_cache.mutex};
            buffer_cache.PopAsyncFlushes();
        }
        query_cache.PopAsyncFlushes();
    }

    struct CacheFlushes {
        bool should_flush;  ///< Downloads to wait for: the fence must reach the host GPU
        u8 committed;       ///< TextureFlushes, BufferFlushes: the caches that committed downloads
    };

    /// Commits the texture and buffer caches' downloads for a new fence (the query cache's are
    /// committed once a fence is made). Switch 2 titles signal a fence per compute dispatch
    /// (thousands a second), mostly with nothing or only buffer data to download: each cache's
    /// lock is taken once, alone, and the fencing thread later takes only the lock of the cache
    /// with work, instead of holding both while it copies (the GPU thread's draws and uploads
    /// waited for it).
    //
    // FRITCHII_FENCE_COMMIT_FAST (live, default 0; fable_front_drains 8.3 row 3): a half whose
    // cache's note says it has nothing is skipped, lock and seam bracket included. Exact: the
    // note is raised in the locked section that queues the work (TextureCache: a download or
    // its buffer; BufferCache: QueueFenceDownload's adds) and set from the containers only
    // under the lock, so a lowered note means the containers HasAsyncFlushWork and
    // HasUncommittedFlushes read were empty, and the half would only have taken its lock and
    // opened an empty bracket. Work queued on this thread is seen in program order; work queued
    // on a thread with no chain of synchronization to this fence is ordered after it, as when
    // that thread takes the lock second (coherence: a raise that happens before this load is
    // seen). The notes are kept up whatever the switch says.
    CacheFlushes CommitCacheFlushes() {
        const int fast = VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_fence_commit_fast,
                                                          "FRITCHII_FENCE_COMMIT_FAST");
        const bool texture_idle = fast != 0 && !texture_cache.MayHaveAsyncFlushWork();
        // FRITCHII_MEMSTAGE_MARKS_AHEAD: a played packet's marks still to promote may queue a
        // download: the half runs then.
        bool buffer_idle = fast != 0 && !buffer_cache.MayHaveUncommittedFlushes() &&
                           !buffer_cache.AheadDue();
        // FRITCHII_MEMSTAGE_PASS bit 2 (fable_stage_p3 5.2): served, the fence of an entry GPUMem
        // passed with P3's record (a release closure, a 3D fence report). The buffer half is
        // skipped on the note whatever FRITCHII_FENCE_COMMIT_FAST says, so the back stays out of
        // the buffer cache: GPUMem passed the entry only with no uncommitted flush (rule C), so a
        // set note there is a must-be-0 (PassFlushes; with marks ahead the back commits here under
        // the lock, levelpoints 14.7). It then commits as today. Any other served fence (a read
        // flush's own) runs as before.
        if (MemoryStageLink::BackServed()) [[unlikely]] {
            const auto* const record = MemoryStageLink::Get().BackCurrentPassRecord();
            if (record != nullptr && record->sync) {
                if (!buffer_cache.MayHaveUncommittedFlushes() && !buffer_cache.AheadDue()) {
                    buffer_idle = true;
                } else if (!buffer_cache.MarksAhead()) {
                    MemoryStageLink::Note(MemoryStageLink::Check::PassFlushes,
                                          "FenceManager::CommitCacheFlushes");
                }
            }
        }
        // One follower (FRITCHII_ONEF_POST_FENCE): a posted fence's buffer half is E's, and E
        // posted it only with nothing to commit there.
        if (skip_buffer_half) [[unlikely]] {
            buffer_idle = true;
        }
        if (fast != 0) [[unlikely]] {
            NoteFenceCommitFast(fast, texture_idle, buffer_idle);
        }
        CacheFlushes flushes{};
        if (!texture_idle) {
            std::scoped_lock lock{texture_cache.mutex};
            flushes.should_flush = texture_cache.HasUncommittedFlushes();
            if (texture_cache.HasAsyncFlushWork()) {
                texture_cache.CommitAsyncFlushes();
                flushes.committed |= TextureFlushes;
            }
            texture_cache.RefreshAsyncFlushWorkNote();
        }
        if (!buffer_idle) {
            std::scoped_lock lock{buffer_cache.mutex};
            // FRITCHII_MEMSTAGE_MARKS_AHEAD (fable_stage_compute 2.4): the commit runs on the back
            // under the lock, also while served (the cache's user here); the real sets hold the
            // writes the back recorded before this fence and none after, once the due marks are
            // promoted.
            const VideoCommon::MemoryStageLink::BackCacheScope back_cache;
            buffer_cache.PromoteAhead();
            [[maybe_unused]] auto seam = [&] {
                if constexpr (requires { buffer_cache.runtime.FenceCommitBracket(); }) {
                    return buffer_cache.runtime.FenceCommitBracket();
                } else {
                    return 0;
                }
            }();
            if (buffer_cache.HasUncommittedFlushes()) {
                buffer_cache.CommitAsyncFlushes();
                flushes.should_flush = true;
                flushes.committed |= BufferFlushes;
            }
            buffer_cache.RefreshUncommittedFlushesNote();
        }
        if (skip_buffer_half && onef_buffer_committed) [[unlikely]] {
            flushes.should_flush = true;
            flushes.committed |= BufferFlushes;
        }
        return flushes;
    }

    /// FRITCHII_FENCE_COMMIT_FAST's counts, one line every 10 s (the clock is read every 1,024th
    /// commit).
    static void NoteFenceCommitFast(int mode, bool texture_idle, bool buffer_idle) {
        static std::atomic<u64> commits{};
        static std::atomic<u64> texture_skips{};
        static std::atomic<u64> buffer_skips{};
        static std::atomic<s64> since{};
        if (texture_idle) {
            texture_skips.fetch_add(1, std::memory_order_relaxed);
        }
        if (buffer_idle) {
            buffer_skips.fetch_add(1, std::memory_order_relaxed);
        }
        if ((commits.fetch_add(1, std::memory_order_relaxed) & 1023) != 0) {
            return;
        }
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = since.load(std::memory_order_relaxed);
        if (last == 0) {
            since.compare_exchange_strong(last, now, std::memory_order_relaxed);
            commits.store(0, std::memory_order_relaxed);
            texture_skips.store(0, std::memory_order_relaxed);
            buffer_skips.store(0, std::memory_order_relaxed);
            return;
        }
        if (now - last < 10'000 ||
            !since.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return;
        }
        const double window = static_cast<double>(now - last) / 1000.0;
        const auto rate = [window](std::atomic<u64>& count) {
            return static_cast<double>(count.exchange(0, std::memory_order_relaxed)) / window;
        };
        const double commit_rate = rate(commits);
        const double texture_rate = rate(texture_skips);
        const double buffer_rate = rate(buffer_skips);
        LOG_INFO(HW_GPU,
                 "Fence commit fast (FRITCHII_FENCE_COMMIT_FAST={}) {:.0f} s: commits {:.0f}/s, "
                 "texture half skipped {:.0f}/s, buffer half skipped {:.0f}/s",
                 mode, window, commit_rate, texture_rate, buffer_rate);
    }

    /// Whether a fence with no downloads would do nothing, so need not be made: with fast GPU
    /// fences its callback runs at once anyway, and most of the per-dispatch fences of Switch 2
    /// titles have no queries or queued operations either. Making, queueing and retiring them
    /// cost the GPU thread ~5% and woke the fencing thread for each. One in 64 is still made, so
    /// the operations fences carry along (query slot cleanup) keep running.
    // With the GPU profile on: why fences are made rather than skipped, logged every 5 s.
    // Called before SkipEmptyFence, which it mirrors without changing its state.
    void NoteFenceDecision(bool delay_fence, size_t committed_flushes, bool trim) {
        static std::array<u64, 6> counts{};
        static auto since = std::chrono::steady_clock::now();
        size_t reason = 5; // would be skipped
        if (delay_fence) {
            reason = 0;
        } else if (committed_flushes != 0) {
            reason = 1;
        } else if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            if (skipped_fences >= 64) {
                reason = 2;
            } else if (query_cache.HasPendingFenceWork(trim)) {
                reason = 3;
            } else {
                std::scoped_lock lock{uncommitted_guard};
                reason = uncommitted_operations.empty() ? 5 : 4;
            }
        } else {
            reason = 2;
        }
        ++counts[reason];
        const auto now = std::chrono::steady_clock::now();
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        // EXP28 & 2: what became of the made fences that had nothing to wait for.
        std::array<double, 3> folds{};
        for (size_t i = 0; i < folds.size(); ++i) {
            folds[i] =
                static_cast<double>(fold_counts[i].exchange(0, std::memory_order_relaxed)) / window;
        }
        LOG_INFO(HW_GPU,
                 "Fence decisions/s: {:.0f} made (delayed fences {:.0f}, committed flushes {:.0f}, "
                 "64-skip cap {:.0f}, query cache fence work {:.0f}, uncommitted operations "
                 "{:.0f}), {:.0f} skipped; EXP28 trim {}, fold: {:.0f} joined a queued fence, "
                 "{:.0f} run inline, {:.0f} fenced",
                 (counts[0] + counts[1] + counts[2] + counts[3] + counts[4]) / window,
                 counts[0] / window, counts[1] / window, counts[2] / window, counts[3] / window,
                 counts[4] / window, counts[5] / window, trim ? 1 : 0, folds[0], folds[1],
                 folds[2]);
        counts = {};
        since = now;
    }

    bool SkipEmptyFence(bool trim) {
        if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            // EDEN_NX2_EXP28 & 1 (the original's FENCE_TRIM): payload releases (QMD
            // releases, ~4,300 a second in Ocarina) wrote guest memory at once anyway; their host
            // buffer syncs (a render pass end and two barriers each) go with the next fence made
            // or wait for idle.
            if (skipped_fences >= 64 || query_cache.HasPendingFenceWork(trim)) {
                return false;
            }
            {
                std::scoped_lock lock{uncommitted_guard};
                if (!uncommitted_operations.empty()) {
                    return false;
                }
            }
            if (trim) {
                query_cache.NotePayloadSyncDeferred();
            }
            ++skipped_fences;
            return true;
        } else {
            return false;
        }
    }

    std::queue<TFence> fences;
    OperationList uncommitted_operations;
    std::deque<OperationList> pending_operations;
    /// Lists whose operations ran, kept with their capacity for the next fences.
    std::vector<OperationList> spare_operations; ///< Guarded by uncommitted_guard
    std::deque<u8> pending_flushes; ///< Per fence: the caches that committed downloads
    std::deque<u32> pending_query_pops; ///< Async check, per fence: query cache entries to pop
    bool fence_thread_busy{}; ///< Guarded by guard: the fencing thread releases fences it took
    bool inline_running{};    ///< Guarded by guard: FoldStubbedFence runs a fence's work inline
    /// EXP28 & 2 (FoldStubbedFence): fences joined to a queued one, run inline, made anyway.
    std::array<std::atomic<u64>, 3> fold_counts{};
    u32 skipped_fences{}; ///< Empty fences skipped since the last one made (GPU thread only)
    std::atomic<u32> fences_in_flight{}; ///< Fences made whose operations have not run yet
    std::atomic<u64> work_epoch{};       ///< Bumped by every fence and fence operation added

    std::mutex guard;
    std::mutex ring_guard;
    std::mutex uncommitted_guard; ///< Guards uncommitted_operations
    /// EXP119: names the last operation queued (SyncOperationStamped). Guarded by uncommitted_guard.
    u64 operation_stamp{};
    std::condition_variable cv;

    std::jthread fence_thread;

    DelayedDestructionRing<TFence, 8> delayed_destruction_ring;
};

} // namespace VideoCommon
