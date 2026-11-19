// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/assert.h"
#include "common/dkb_cpu_trace.h"
#include "common/lowend_sim.h"
#include "common/nx2_timeline.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/memstage_shadow.h"
#include "video_core/memstage_stats.h"
#include "video_core/nx2_fastdraw_count.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

namespace {

// Tells a CPU thread waiting in PushCommand that its command ran.
void SignalCommand(SynchState& state, u64 fence, bool block) {
    state.signaled_fence.store(fence);
    if (block) {
        // We have to lock the write_lock to ensure that the condition_variable wait not get a
        // race between the check and the lock itself.
        std::scoped_lock lk{state.write_lock};
        state.cv.notify_all();
    }
}

// One spin-wait hint (EXP114's pending poll gap).
void PollPause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

} // Anonymous namespace

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        const Common::DkbCpuTrace::Registration cpu_trace{Common::DkbCpuTrace::Role::GpuCommand};
        Common::SetCurrentThreadName("GPU");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::PinCurrentThread(Common::PinnedRole::GpuFront);
        Common::SetCurrentThreadToPerformanceCores();
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the polling below).
        auto last_progress = std::chrono::steady_clock::now();
        // EDEN_NX2_GPU_WAKE: for PendingPoll after that the thread waits in hardware, then it
        // blocks in the OS PendingSleepTime at a time. In hardware it looks at the words the
        // channels wait on every WordCheckPeriod and polls the channels in full every
        // PendingPollPeriod.
        constexpr std::chrono::nanoseconds PendingPoll = std::chrono::microseconds{500};
        constexpr std::chrono::nanoseconds PendingPollPeriod = std::chrono::microseconds{20};
        constexpr std::chrono::nanoseconds WordCheckPeriod = std::chrono::microseconds{2};
        constexpr std::chrono::nanoseconds PendingSleepTime = std::chrono::microseconds{500};
        auto& wake = Tegra::Nx2PendingWake::Get();
        auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
        namespace Profile = VideoCommon::Nx2Profile;
        // Before the split's back thread starts, which reads it too.
        Profile::Initialize();
        auto& split = VideoCommon::GpuSplit::Get();
        while (!stop_token.stop_requested()) {
            Profile::Report();
            // FRITCHII_SIM_CPU_SLOW: the command just handled ends here.
            Common::LowEndSim::CpuWorkEnd();
            split.MaybeStart(system);
            if (scheduler.HasPendingChannels()) {
                // Suspended channels wait on semaphores the CPU releases: poll them meanwhile.
                // EDEN_NX2_GPU_WAKE: a new command or a GPU release
                // wakes the wait below at once (Nx2PendingWake), so arm it before looking.
                const bool gpu_wake = VideoCore::Nx2Debug::GpuWake();
                wake.SetEnabled(gpu_wake);
                if (gpu_wake) {
                    wake.Arm();
                }
                bool resumed = false;
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    // FRITCHII_LAGGED_DEFER: the checks this poll puts off, only.
                    Tegra::Engines::LaggedDefer::Clear();
                    resumed = scheduler.ResumePending(system.GPU());
                }
                if (!state.queue.TryPop(next)) {
                    // EDEN_NX2_EXP27 & 1: what the resumed channels sent goes to the back before
                    // this thread yields or sleeps (Commit publishes every 256 words).
                    if (split.Active()) {
                        split.PublishBeforeWait();
                    }
                    // FRITCHII_LAGGED_DEFER: lagged checks put off while the back was short of
                    // their targets wait here, where the front has nothing else to do, then
                    // the channels are polled again (the checks run with the back there).
                    if (Tegra::Engines::LaggedDefer::Pending()) {
                        Tegra::Engines::LaggedDefer::WaitBeforeSleep();
                        continue;
                    }
                    if (gpu_wake) {
                        if (resumed) {
                            last_progress = std::chrono::steady_clock::now();
                        }
                        if (!scheduler.HasPendingChannels()) {
                            continue;
                        }
                        // The CPU usually releases the semaphore soon after: for half a
                        // millisecond after the last progress wait in hardware, watching the
                        // semaphore words, then block in the OS for 500 us at a time. A full poll
                        // every 20 us catches what sends no wake and changes no word, such as a
                        // release a shader wrote to the caches.
                        Profile::Scope scope{Profile::Zone::PendingSleep};
                        const Profile::FrontWaitScope wait_mark{Profile::FrontDid::Pending};
                        const VideoCommon::OneFollower::DryStallScope dry_stall;
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_progress < PendingPoll) {
                            const auto poll_end =
                                std::min(now + PendingPollPeriod, last_progress + PendingPoll);
                            auto until = now;
                            do {
                                until = std::min(std::chrono::steady_clock::now() +
                                                     WordCheckPeriod,
                                                 poll_end);
                                if (wake.Poll(until) || waiters.WordStored()) {
                                    break;
                                }
                            } while (until < poll_end);
                        } else {
                            wake.Sleep(PendingSleepTime);
                        }
                        continue;
                    }
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond before sleeping, or every wait also waits out a host sleep
                    // (title 1 waits ~25 times a second).
                    // EDEN_NX2_EXP6: 1 = 50 us sleeps (Ocarina waits here
                    // ~330 times a second and its present waits for the work behind them; on
                    // Windows a sleep still lasts one timer tick, ~0.5 ms), 2 = always yield.
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    const Profile::FrontWaitScope wait_mark{Profile::FrontDid::Pending};
                    const VideoCommon::OneFollower::DryStallScope dry_stall;
                    const int poll = VideoCore::Nx2Debug::Experiment(6);
                    if (poll == 2 || std::chrono::steady_clock::now() - last_progress <
                                         std::chrono::microseconds{500}) {
                        // EXP114 (live; 0 = yield, N > 0 = N us): between polls, wait with PAUSE
                        // instead of a yield per poll. ResumePending ran hundreds of thousands of
                        // times a second at the market; the PAUSE wait leaves the core's SMT
                        // sibling (often the back thread or a guest core) free.
                        if (const int gap_us = VideoCore::Nx2Debug::Experiment(114); gap_us > 0) {
                            const auto until = std::chrono::steady_clock::now() +
                                               std::chrono::microseconds{gap_us};
                            do {
                                for (int i = 0; i < 16; ++i) {
                                    PollPause();
                                }
                            } while (std::chrono::steady_clock::now() < until);
                        } else {
                            std::this_thread::yield();
                        }
                    } else if (poll == 1) {
                        std::this_thread::sleep_for(std::chrono::microseconds{50});
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
                Profile::Scope scope{Profile::Zone::Idle};
                const Profile::FrontWaitScope wait_mark{Profile::FrontDid::Idle};
                state.queue.PopWait(next, stop_token);
            }
            if (stop_token.stop_requested()) {
                break;
            }
            last_progress = std::chrono::steady_clock::now();
            // FRITCHII_SIM_CPU_SLOW: the wait for this command was not work, handling it is.
            Common::LowEndSim::CpuWorkBegin(Common::LowEndSim::CpuClass::Front);
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    // EDEN_NX2_MEMSTAGE_STATS: command lists per channel.
                    VideoCommon::MemStageStats::ListSubmitted(
                        submit_list->channel, submit_list->entries.command_lists.size());
                    const bool timeline = Common::Nx2Timeline::Active();
                    if (timeline) {
                        // The list's sequence number (PushCommand's "submit list") on each
                        // stage: here, and from a one-entry marker where the back reaches it.
                        Common::Nx2Timeline::Instant("front: list begin", {"seq", next.fence},
                                                     {"channel", submit_list->channel});
                        split.Defer(
                            [seq = next.fence] {
                                Common::Nx2Timeline::Instant("back: list", {"seq", seq});
                            },
                            VideoCommon::GpuSplit::Effect::None,
                            VideoCommon::Nx2FastDrawCount::Neutral() |
                                VideoCommon::StageTagBits(VideoCommon::StageTag::Pass));
                    }
                    {
                        Profile::Scope scope{Profile::Zone::Push};
                        scheduler.Push(system.GPU(), submit_list->channel,
                                       std::move(submit_list->entries));
                    }
                    if (timeline) {
                        Common::Nx2Timeline::Instant("front: list end", {"seq", next.fence});
                    }
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    // TickWork ends delta draw reuse itself when it runs a request.
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); },
                                VideoCommon::GpuSplit::Effect::Unknown,
                                VideoCommon::Nx2FastDrawCount::Neutral());
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    if (run->before_later_reads) {
                        // GPU memory maps unmap caches (UnmapMemory downloads): the back's state.
                        split.DeferFunction(std::move(run->func));
                        split.Drain(VideoCommon::DrainSite::Map);
                    } else {
                        split.DeferFunction(std::move(run->func));
                        split.NoteMemoryWrite();
                    }
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    // Delta draw count: FlushRegion and the collected write below are neutral
                    // (a download changes no draw; a write breaks where it lands).
                    split.Defer(
                        [rasterizer_ptr, addr = flush->addr, size = flush->size] {
                            rasterizer_ptr->FlushRegion(addr, size);
                        },
                        VideoCommon::GpuSplit::Effect::Unknown,
                        VideoCommon::Nx2FastDrawCount::Neutral());
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer(
                        [rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                            rasterizer_ptr->OnCacheInvalidation(addr, size);
                        },
                        VideoCommon::GpuSplit::Effect::Unknown,
                        VideoCommon::Nx2FastDrawCount::Neutral());
                } else {
                    ASSERT(false);
                }
                split.Defer(
                    [state_ptr = &state, fence = next.fence, block = next.block] {
                        SignalCommand(*state_ptr, fence, block);
                    },
                    VideoCommon::GpuSplit::Effect::None,
                    VideoCommon::Nx2FastDrawCount::Neutral() |
                        VideoCommon::StageTagBits(VideoCommon::StageTag::Pass));
                split.Publish();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                VideoCommon::MemStageStats::ListSubmitted(
                    submit_list->channel, submit_list->entries.command_lists.size());
                const bool timeline = Common::Nx2Timeline::Active();
                if (timeline) {
                    Common::Nx2Timeline::Instant("front: list begin", {"seq", next.fence},
                                                 {"channel", submit_list->channel});
                }
                {
                    Profile::Scope scope{Profile::Zone::Push};
                    scheduler.Push(system.GPU(), submit_list->channel,
                                   std::move(submit_list->entries));
                }
                if (timeline) {
                    Common::Nx2Timeline::Instant("front: list end", {"seq", next.fence});
                }
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                Profile::Scope scope{Profile::Zone::Tick};
                system.GPU().TickWork();
            } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Run};
                run->func();
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            SignalCommand(state, next.fence, next.block);
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::Run(std::function<void()>&& func, bool is_async, bool before_later_reads) {
    PushCommand(RunCommand{std::move(func), before_later_reads}, false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (VideoCore::Nx2Debug::AccuratePart(8)) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    if (Common::Nx2Timeline::Active()) {
        // The command's sequence number, which the GPU threads' list markers repeat
        // (tools/nx2/critical_path.py follows a list through the stages with it). This is also
        // what wakes the front from "idle".
        if (const auto* list = std::get_if<SubmitListCommand>(&command_data)) {
            Common::Nx2Timeline::Instant("submit list", {"seq", fence}, {"channel", list->channel},
                                         {"entries", list->entries.command_lists.size()});
        } else {
            Common::Nx2Timeline::Instant("submit command", {"seq", fence});
        }
    }
    state.queue.EmplaceWait(std::move(command_data), fence, block);
    // EDEN_NX2_GPU_WAKE: ends the GPU thread's wait for suspended channels (returns at once
    // while the switch is off).
    Tegra::Nx2PendingWake::Get().Notify();

    if (block) {
        // With the split, SignalCommand runs on the back.
        Common::Nx2Timeline::ArgSpan timeline_span{"guest: wait for GPU command", {"seq", fence}};
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread
