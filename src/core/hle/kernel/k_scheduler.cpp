// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <bit>
#include <chrono>

#include "common/assert.h"
#include "common/lowend_sim.h"
#include "common/bit_util.h"
#include "common/fiber.h"
#include "common/logging.h"
#include "common/nx2_timeline.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_interrupt_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "core/hle/kernel/physical_core.h"
#include "video_core/nx2_debug_control.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Kernel {

namespace {

// EXP98 (live; Fable fable52_answer_yield): an exact early return for guest yields, per core.
// g_yield_gen[c] moves whenever the scheduling of a thread whose affinity mask (or active core)
// holds core c changes. A yield that found nothing to do stamps its core and that core's
// counter; while the counter is unchanged, the same yield would find nothing again.
struct alignas(64) YieldGen {
    std::atomic<u64> value{1};
};
std::array<YieldGen, Core::Hardware::NUM_CPU_CORES> g_yield_gen;

// Set under the scheduler lock by the first yield that runs with EXP98 != 0, before it can stamp
// anything, and never cleared. Until then nothing reads the counters, so they are not raised.
std::atomic<bool> g_yield_gens_live{false};

// Scheduler lock held: the only writers. Plain stores, their visibility rides on the lock word.
void BumpYieldGens(u64 mask) {
    while (mask != 0) {
        const auto core = static_cast<size_t>(std::countr_zero(mask));
        mask &= mask - 1;
        if (core < g_yield_gen.size()) {
            auto& gen = g_yield_gen[core].value;
            gen.store(gen.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }
}

void NoteYieldGens(u64 mask) {
    if (g_yield_gens_live.load(std::memory_order_relaxed)) [[unlikely]] {
        BumpYieldGens(mask);
    }
}

// The scheduling of `thread` changed: raise the counters of the cores it can sit on.
void NoteScheduleChange(const KThread* thread) {
    if (!g_yield_gens_live.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    if (thread->IsDummyThread()) {
        return; // never in the queues
    }
    u64 mask = thread->GetAffinityMask().GetAffinityMask();
    if (const s32 core = thread->GetActiveCore(); core >= 0) {
        mask |= u64{1} << core;
    }
    BumpYieldGens(mask);
}

u64 YieldGenOf(s32 core) {
    return g_yield_gen[static_cast<size_t>(core)].value.load(std::memory_order_relaxed);
}

// Scheduler lock held: the thread's stamp still describes the queues (the verify prediction).
bool YieldStampMatches(const KThread& cur, u8 kind) {
    const s32 core = cur.GetYieldCore();
    return core >= 0 && cur.GetYieldKind() >= kind && YieldGenOf(core) == cur.GetYieldGen();
}

void CpuPause() {
    Common::LowEndSim::CpuPauseNote();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// EXP98 counters, one padded block per core (written by that core's host thread), summed every
// 5 s into the "Guest yields (EXP98" line.
struct alignas(64) YieldStats {
    std::atomic<u64> forms[3]{};       // svcSleepThread 0, -1, -2
    std::atomic<u64> early{};          // early returns
    std::atomic<u64> held_waited{};    // early check found the lock held, waited it out
    std::atomic<u64> held_fell_back{}; // ... still held after 64 pauses, took the lock
    std::atomic<u64> stamped{};        // locked, found nothing to do
    std::atomic<u64> recheck{};
    std::atomic<u64> rotated{};
    std::atomic<u64> migrated{};
    std::atomic<u64> not_runnable{};
    std::atomic<u64> predicted{};      // mode 2
    std::atomic<u64> mismatches{};     // mode 2
};
std::array<YieldStats, Core::Hardware::NUM_CPU_CORES> g_yield_stats;
std::atomic<s64> g_yield_next_report{};
std::atomic<u64> g_yield_mismatch_total{};
std::atomic<u32> g_yield_mismatch_logged{};

void Add(std::atomic<u64>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}

YieldStats& YieldStatsOf(s32 core) {
    return g_yield_stats[static_cast<size_t>(core) % g_yield_stats.size()];
}

void MaybeReportYields(int mode) {
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 next = g_yield_next_report.load(std::memory_order_relaxed);
    if (next == 0) {
        g_yield_next_report.compare_exchange_strong(next, now + 5'000'000'000);
        return;
    }
    if (now < next || !g_yield_next_report.compare_exchange_strong(next, now + 5'000'000'000)) {
        return;
    }
    u64 forms[3]{}, early{}, waited{}, fell_back{}, stamped{}, recheck{}, rotated{}, migrated{},
        not_runnable{}, predicted{}, mismatches{};
    for (auto& s : g_yield_stats) {
        for (size_t i = 0; i < 3; ++i) {
            forms[i] += s.forms[i].exchange(0, std::memory_order_relaxed);
        }
        early += s.early.exchange(0, std::memory_order_relaxed);
        waited += s.held_waited.exchange(0, std::memory_order_relaxed);
        fell_back += s.held_fell_back.exchange(0, std::memory_order_relaxed);
        stamped += s.stamped.exchange(0, std::memory_order_relaxed);
        recheck += s.recheck.exchange(0, std::memory_order_relaxed);
        rotated += s.rotated.exchange(0, std::memory_order_relaxed);
        migrated += s.migrated.exchange(0, std::memory_order_relaxed);
        not_runnable += s.not_runnable.exchange(0, std::memory_order_relaxed);
        predicted += s.predicted.exchange(0, std::memory_order_relaxed);
        mismatches += s.mismatches.exchange(0, std::memory_order_relaxed);
    }
    const u64 mismatch_total =
        g_yield_mismatch_total.fetch_add(mismatches, std::memory_order_relaxed) + mismatches;
    if (mode == 2) {
        LOG_INFO(Kernel,
                 "Guest yields (EXP98=2, 5 s, per second): forms 0 {:.0f}, -1 {:.0f}, -2 {:.0f}; "
                 "early returns {:.0f}; lock held at the check: waited out {:.0f}, fell back "
                 "{:.0f}; locked: stamped {:.0f}, recheck {:.0f}, rotated {:.0f}, migrated "
                 "{:.0f}, not runnable {:.0f}; predicted {:.0f}, mismatches {} in 5 s ({} total, "
                 "must be 0)",
                 forms[0] / 5.0, forms[1] / 5.0, forms[2] / 5.0, early / 5.0, waited / 5.0,
                 fell_back / 5.0, stamped / 5.0, recheck / 5.0, rotated / 5.0, migrated / 5.0,
                 not_runnable / 5.0, predicted / 5.0, mismatches, mismatch_total);
    } else {
        LOG_INFO(Kernel,
                 "Guest yields (EXP98={}, 5 s, per second): forms 0 {:.0f}, -1 {:.0f}, -2 {:.0f}; "
                 "early returns {:.0f}; lock held at the check: waited out {:.0f}, fell back "
                 "{:.0f}; locked: stamped {:.0f}, recheck {:.0f}, rotated {:.0f}, migrated "
                 "{:.0f}, not runnable {:.0f}",
                 mode, forms[0] / 5.0, forms[1] / 5.0, forms[2] / 5.0, early / 5.0, waited / 5.0,
                 fell_back / 5.0, stamped / 5.0, recheck / 5.0, rotated / 5.0, migrated / 5.0,
                 not_runnable / 5.0);
    }
}

// Counts a yield of form `form` (0, 1 = -1, 2 = -2); checks the report clock every 256th.
void CountYieldForm(YieldStats& stats, size_t form, int mode) {
    const u64 n = stats.forms[form].fetch_add(1, std::memory_order_relaxed);
    if ((n & 255) == 255) [[unlikely]] {
        MaybeReportYields(mode);
    }
}

// Mode 2: the stamp said "nothing to do" but the locked path did something. `what` is rotated,
// migrated, recheck or core (the thread's core differs from its stamp).
void NoteYieldMismatch(YieldStats& stats, const KThread& cur, s32 core, const char* what,
                       const KThread* other) {
    Add(stats.mismatches);
    if (g_yield_mismatch_logged.fetch_add(1, std::memory_order_relaxed) >= 16) {
        return;
    }
    LOG_WARNING(Kernel,
                "EXP98 mismatch: thread {} core {} (stamp core {}) priority {}: {}; other thread "
                "{} priority {} mask {:#x}",
                cur.GetThreadId(), core, cur.GetYieldCore(), cur.GetPriority(), what,
                other != nullptr ? other->GetThreadId() : u64{0},
                other != nullptr ? other->GetPriority() : -1,
                other != nullptr ? other->GetAffinityMask().GetAffinityMask() : u64{0});
}

// Mode 1: true when the locked path would find nothing to do. The lock word is read first: if it
// is free, every completed critical section is visible; one that starts later began after this
// yield entered the kernel. If it is held, wait for it to be free without taking it.
bool YieldIsKnownNoOp(KernelCore& kernel, const KThread& cur, u8 kind, YieldStats& stats) {
    const s32 core = cur.GetYieldCore();
    if (core < 0 || cur.GetYieldKind() < kind) {
        return false;
    }
    const auto& lock = kernel.GlobalSchedulerContext().SchedulerLock();
    if (lock.IsFree()) [[likely]] {
        return YieldGenOf(core) == cur.GetYieldGen();
    }
    for (int spin = 0; spin < 64; ++spin) {
        CpuPause();
        if (lock.IsFree()) {
            Add(stats.held_waited);
            return YieldGenOf(core) == cur.GetYieldGen();
        }
    }
    Add(stats.held_fell_back);
    return false; // busy for long: take the lock as today
}

} // Anonymous namespace

static void IncrementScheduledCount(Kernel::KThread* thread) {
    NoteScheduleChange(thread); // EXP98: every process, and threads without one
    if (auto process = thread->GetOwnerProcess(); process) {
        process->IncrementScheduledCount();
    }
}

KScheduler::KScheduler(KernelCore& kernel) {
    m_switch_fiber = std::make_shared<Common::Fiber>([this, &kernel] {
        while (true) {
            ScheduleImplFiber(kernel);
        }
    });
    m_state.needs_scheduling = true;
}

KScheduler::~KScheduler() = default;

void KScheduler::SetInterruptTaskRunnable(KernelCore& kernel) {
    m_state.interrupt_task_runnable = true;
    m_state.needs_scheduling = true;
}

void KScheduler::RequestScheduleOnInterrupt(KernelCore& kernel) {
    m_state.needs_scheduling = true;
    if (CanSchedule(kernel)) {
        ScheduleOnInterrupt(kernel);
    }
}

void KScheduler::DisableScheduling(KernelCore& kernel) {
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() >= 0);
    GetCurrentThread(kernel).DisableDispatch(kernel);
}

void KScheduler::EnableScheduling(KernelCore& kernel, u64 cores_needing_scheduling) {
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() >= 1);

    auto* scheduler{kernel.CurrentScheduler()};

    if (!scheduler || kernel.IsPhantomModeForSingleCore()) {
        KScheduler::RescheduleCores(kernel, cores_needing_scheduling);
        KScheduler::RescheduleCurrentHLEThread(kernel);
        return;
    }

    scheduler->RescheduleOtherCores(kernel, cores_needing_scheduling);

    if (GetCurrentThread(kernel).GetDisableDispatchCount() > 1) {
        GetCurrentThread(kernel).EnableDispatch(kernel);
    } else {
        scheduler->RescheduleCurrentCore(kernel);
    }
}

void KScheduler::RescheduleCurrentHLEThread(KernelCore& kernel) {
    // HACK: we cannot schedule from this thread, it is not a core thread
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() == 1);

    // Ensure dummy threads that are waiting block.
    GetCurrentThread(kernel).DummyThreadBeginWait(kernel);

    ASSERT(GetCurrentThread(kernel).GetState() != ThreadState::Waiting);
    GetCurrentThread(kernel).EnableDispatch(kernel);
}

u64 KScheduler::UpdateHighestPriorityThreads(KernelCore& kernel) {
    if (IsSchedulerUpdateNeeded(kernel)) {
        return UpdateHighestPriorityThreadsImpl(kernel);
    } else {
        return 0;
    }
}

void KScheduler::Schedule(KernelCore& kernel) {
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() == 1);
    ASSERT(m_core_id == GetCurrentCoreId(kernel));

    ScheduleImpl(kernel);
}

void KScheduler::ScheduleOnInterrupt(KernelCore& kernel) {
    GetCurrentThread(kernel).DisableDispatch(kernel);
    Schedule(kernel);
    GetCurrentThread(kernel).EnableDispatch(kernel);
}

void KScheduler::PreemptSingleCore(KernelCore& kernel) {
    GetCurrentThread(kernel).DisableDispatch(kernel);

    auto* thread = GetCurrentThreadPointer(kernel);
    auto& previous_scheduler = kernel.Scheduler(thread->GetCurrentCore());
    previous_scheduler.Unload(kernel, thread);

    Common::Fiber::YieldTo(thread->GetHostContext(), *m_switch_fiber);

    GetCurrentThread(kernel).EnableDispatch(kernel);
}

void KScheduler::RescheduleCurrentCore(KernelCore& kernel) {
    ASSERT(!kernel.IsPhantomModeForSingleCore());
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() == 1);

    GetCurrentThread(kernel).EnableDispatch(kernel);

    if (m_state.needs_scheduling.load()) {
        // Disable interrupts, and then check again if rescheduling is needed.
        // KScopedInterruptDisable intr_disable;

        kernel.CurrentScheduler()->RescheduleCurrentCoreImpl(kernel);
    }
}

void KScheduler::RescheduleCurrentCoreImpl(KernelCore& kernel) {
    // Check that scheduling is needed.
    if (m_state.needs_scheduling.load()) [[likely]] {
        GetCurrentThread(kernel).DisableDispatch(kernel);
        Schedule(kernel);
        GetCurrentThread(kernel).EnableDispatch(kernel);
    }
}

void KScheduler::Initialize(KernelCore& kernel, KThread* main_thread, KThread* idle_thread, s32 core_id) {
    // Set core ID/idle thread/interrupt task manager.
    m_core_id = core_id;
    m_idle_thread = idle_thread;
    // m_state.idle_thread_stack = m_idle_thread->GetStackTop();
    // m_state.interrupt_task_manager = std::addressof(kernel.GetInterruptTaskManager());

    // Insert the main thread into the priority queue.
    // {
    //     KScopedSchedulerLock lk{kernel};
    //     GetPriorityQueue(kernel).PushBack(GetCurrentThreadPointer(kernel));
    //     SetSchedulerUpdateNeeded(kernel);
    // }

    // Bind interrupt handler.
    // kernel.GetInterruptManager().BindHandler(
    //     GetSchedulerInterruptHandler(kernel), KInterruptName::Scheduler, m_core_id,
    //     KInterruptController::PriorityLevel::Scheduler, false, false);

    // Set the current thread.
    m_current_thread = main_thread;
}

void KScheduler::Activate(KernelCore& kernel) {
    ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() == 1);

    // m_state.should_count_idle = KTargetSystem::IsDebugMode();
    m_is_active = true;
    RescheduleCurrentCore(kernel);
}

void KScheduler::OnThreadStart(KernelCore& kernel) {
    GetCurrentThread(kernel).EnableDispatch(kernel);
}

u64 KScheduler::UpdateHighestPriorityThread(KernelCore& kernel, KThread* highest_thread) {
    if (KThread* prev_highest_thread = m_state.highest_priority_thread;
        prev_highest_thread != highest_thread) [[likely]] {
        if (prev_highest_thread != nullptr) [[likely]] {
            IncrementScheduledCount(prev_highest_thread);
            prev_highest_thread->SetLastScheduledTick(
                kernel.System().CoreTiming().GetClockTicks());
        }
        if (m_state.should_count_idle) {
            if (highest_thread != nullptr) [[likely]] {
                if (KProcess* process = highest_thread->GetOwnerProcess(); process != nullptr) {
                    process->SetRunningThread(m_core_id, highest_thread, m_state.idle_count, 0);
                }
            } else {
                m_state.idle_count++;
            }
        }

        m_state.highest_priority_thread = highest_thread;
        m_state.needs_scheduling = true;
        return (1ULL << m_core_id);
    } else {
        return 0;
    }
}

u64 KScheduler::UpdateHighestPriorityThreadsImpl(KernelCore& kernel) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));

    // Clear that we need to update.
    ClearSchedulerUpdateNeeded(kernel);

    u64 cores_needing_scheduling = 0, idle_cores = 0;
    KThread* top_threads[Core::Hardware::NUM_CPU_CORES];
    auto& priority_queue = GetPriorityQueue(kernel);

    // We want to go over all cores, finding the highest priority thread and determining if
    // scheduling is needed for that core.
    for (size_t core_id = 0; core_id < Core::Hardware::NUM_CPU_CORES; core_id++) {
        KThread* top_thread = priority_queue.GetScheduledFront(static_cast<s32>(core_id));
        if (top_thread != nullptr) {
            // We need to check if the thread's process has a pinned thread.
            if (KProcess* parent = top_thread->GetOwnerProcess()) {
                // Check that there's a pinned thread other than the current top thread.
                if (KThread* pinned = parent->GetPinnedThread(static_cast<s32>(core_id));
                    pinned != nullptr && pinned != top_thread) {
                    // We need to prefer threads with kernel waiters to the pinned thread.
                    if (top_thread->GetNumKernelWaiters() ==
                        0 /* && top_thread != parent->GetExceptionThread() */) {
                        // If the pinned thread is runnable, use it.
                        if (pinned->GetRawState() == ThreadState::Runnable) {
                            top_thread = pinned;
                        } else {
                            top_thread = nullptr;
                        }
                    }
                }
            }
        } else {
            idle_cores |= (1ULL << core_id);
        }

        top_threads[core_id] = top_thread;
        cores_needing_scheduling |= kernel.Scheduler(core_id).UpdateHighestPriorityThread(kernel, top_threads[core_id]);
    }

    // Idle cores are bad. We're going to try to migrate threads to each idle core in turn.
    while (idle_cores != 0) {
        const s32 core_id = static_cast<s32>(std::countr_zero(idle_cores));

        if (KThread* suggested = priority_queue.GetSuggestedFront(core_id); suggested != nullptr) {
            s32 migration_candidates[Core::Hardware::NUM_CPU_CORES];
            size_t num_candidates = 0;

            // While we have a suggested thread, try to migrate it!
            while (suggested != nullptr) {
                // Check if the suggested thread is the top thread on its core.
                const s32 suggested_core = suggested->GetActiveCore();
                if (KThread* top_thread =
                        (suggested_core >= 0) ? top_threads[suggested_core] : nullptr;
                    top_thread != suggested) {
                    // Make sure we're not dealing with threads too high priority for migration.
                    if (top_thread != nullptr &&
                        top_thread->GetPriority() < HighestCoreMigrationAllowedPriority) {
                        break;
                    }

                    // The suggested thread isn't bound to its core, so we can migrate it!
                    suggested->SetActiveCore(core_id);
                    priority_queue.ChangeCore(suggested_core, suggested);
                    NoteScheduleChange(suggested); // EXP98
                    top_threads[core_id] = suggested;
                    cores_needing_scheduling |= kernel.Scheduler(core_id).UpdateHighestPriorityThread(kernel, top_threads[core_id]);
                    break;
                }

                // Note this core as a candidate for migration.
                ASSERT(num_candidates < Core::Hardware::NUM_CPU_CORES);
                migration_candidates[num_candidates++] = suggested_core;
                suggested = priority_queue.GetSuggestedNext(core_id, suggested);
            }

            // If suggested is nullptr, we failed to migrate a specific thread. So let's try all our
            // candidate cores' top threads.
            if (suggested == nullptr) {
                for (size_t i = 0; i < num_candidates; i++) {
                    // Check if there's some other thread that can run on the candidate core.
                    const s32 candidate_core = migration_candidates[i];
                    suggested = top_threads[candidate_core];
                    if (KThread* next_on_candidate_core =
                            priority_queue.GetScheduledNext(candidate_core, suggested);
                        next_on_candidate_core != nullptr) {
                        // The candidate core can run some other thread! We'll migrate its current
                        // top thread to us.
                        top_threads[candidate_core] = next_on_candidate_core;
                        cores_needing_scheduling |= kernel.Scheduler(candidate_core).UpdateHighestPriorityThread(kernel, top_threads[candidate_core]);

                        // Perform the migration.
                        suggested->SetActiveCore(core_id);
                        priority_queue.ChangeCore(candidate_core, suggested);
                        NoteScheduleChange(suggested); // EXP98
                        top_threads[core_id] = suggested;
                        cores_needing_scheduling |= kernel.Scheduler(core_id).UpdateHighestPriorityThread(kernel, top_threads[core_id]);
                        break;
                    }
                }
            }
        }

        idle_cores &= ~(1ULL << core_id);
    }

    // HACK: any waiting dummy threads can wake up now.
    kernel.GlobalSchedulerContext().WakeupWaitingDummyThreads(kernel);

    // HACK: if we are a dummy thread, and we need to go sleep, indicate
    // that for when the lock is released.
    KThread* const cur_thread = GetCurrentThreadPointer(kernel);
    if (cur_thread->IsDummyThread() && cur_thread->GetState() != ThreadState::Runnable) {
        cur_thread->RequestDummyThreadWait(kernel);
    }

    return cores_needing_scheduling;
}

void KScheduler::SwitchThread(KernelCore& kernel, KThread* next_thread) {
    KProcess* const cur_process = GetCurrentProcessPointer(kernel);
    KThread* const cur_thread = GetCurrentThreadPointer(kernel);

    // We never want to schedule a null thread, so use the idle thread if we don't have a next.
    if (next_thread == nullptr) {
        next_thread = m_idle_thread;
    }

    if (next_thread->GetCurrentCore() != m_core_id) {
        next_thread->SetCurrentCore(m_core_id);
    }

    // If we're not actually switching thread, there's nothing to do.
    if (next_thread == cur_thread) {
        return;
    }

    // Next thread is now known not to be nullptr, and must not be dispatchable.
    ASSERT(next_thread->GetDisableDispatchCount() == 1);
    ASSERT(!next_thread->IsDummyThread());

    // Update the CPU time tracking variables.
    const s64 prev_tick = m_last_context_switch_time;
    const s64 cur_tick = kernel.System().CoreTiming().GetClockTicks();
    const s64 tick_diff = cur_tick - prev_tick;
    cur_thread->AddCpuTime(m_core_id, tick_diff);
    if (cur_process != nullptr) {
        cur_process->AddCpuTime(tick_diff);
    }
    m_last_context_switch_time = cur_tick;
    if (Nx2ThreadStats::g_enabled) {
        Nx2ThreadStats::OnSwitch(cur_thread, next_thread, cur_tick, tick_diff);
    }

    // Update our previous thread.
    if (cur_process != nullptr) {
        if (!cur_thread->IsTerminationRequested() && cur_thread->GetActiveCore() == m_core_id)
            [[likely]] {
            m_state.prev_thread = cur_thread;
        } else {
            m_state.prev_thread = nullptr;
        }
    }

    // Switch the current process, if we're switching processes.
    // if (KProcess *next_process = next_thread->GetOwnerProcess(); next_process != cur_process) {
    //     KProcess::Switch(cur_process, next_process);
    // }

    // Set the new thread.
    SetCurrentThread(kernel, next_thread);
    m_current_thread = next_thread;

    // The only place a core's guest thread changes: the timeline's guest layer (thread ids, 0 =
    // the core's idle thread) follows a guest thread across cores from these.
    if (Common::Nx2Timeline::Guest()) [[unlikely]] {
        Common::Nx2Timeline::Instant(
            "guest switch", {"core", m_core_id},
            {"to", next_thread == m_idle_thread ? u64{0} : next_thread->GetThreadId()},
            {"from", cur_thread == m_idle_thread ? u64{0} : cur_thread->GetThreadId()});
    }

    // Set the new Thread Local region.
    // cpu::SwitchThreadLocalRegion(GetInteger(next_thread->GetThreadLocalRegionAddress()));

    // Update the thread's cpu time differential in TLS, if relevant.
    next_thread->UpdateTlsThreadCpuTime(kernel, cur_tick);
}

void KScheduler::ScheduleImpl(KernelCore& kernel) {
    // First, clear the needs scheduling bool.
    m_state.needs_scheduling.store(false, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Load the appropriate thread pointers for scheduling.
    KThread* const cur_thread{GetCurrentThreadPointer(kernel)};
    KThread* highest_priority_thread{m_state.highest_priority_thread};

    // Check whether there are runnable interrupt tasks.
    if (m_state.interrupt_task_runnable) {
        // The interrupt task is runnable.
        // We want to switch to the interrupt task/idle thread.
        highest_priority_thread = nullptr;
    }

    // If there aren't, we want to check if the highest priority thread is the same as the current
    // thread.
    if (highest_priority_thread == cur_thread) {
        // If they're the same, then we can just issue a memory barrier and return.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }

    // The highest priority thread is not the same as the current thread.
    // Jump to the switcher and continue executing from there.
    m_switch_cur_thread = cur_thread;
    m_switch_highest_priority_thread = highest_priority_thread;
    m_switch_from_schedule = true;
    Common::Fiber::YieldTo(cur_thread->m_host_context, *m_switch_fiber);

    // Returning from ScheduleImpl occurs after this thread has been scheduled again.
}

void KScheduler::ScheduleImplFiber(KernelCore& kernel) {
    KThread* const cur_thread{m_switch_cur_thread};
    KThread* highest_priority_thread{m_switch_highest_priority_thread};

    // If we're not coming from scheduling (i.e., we came from SC preemption),
    // we should restart the scheduling loop directly. Not accurate to HOS.
    if (!m_switch_from_schedule) {
        goto retry;
    }

    // Mark that we are not coming from scheduling anymore.
    m_switch_from_schedule = false;

    // Save the original thread context.
    Unload(kernel, cur_thread);

    // The current thread's context has been entirely taken care of.
    // Now we want to loop until we successfully switch the thread context.
    while (true) {
        // We're starting to try to do the context switch.
        // Check if the highest priority thread is null.
        if (!highest_priority_thread) {
            // The next thread is nullptr!

            // Switch to the idle thread. Note: HOS treats idling as a special case for
            // performance. This is not *required* for the emulator's purposes, and for singlecore
            // compatibility, we can just move the logic that would go here into the execution
            // of the idle thread. If we ever remove singlecore, we should implement this
            // accurately to HOS.
            highest_priority_thread = m_idle_thread;
        }

        // We want to try to lock the highest priority thread's context.
        // Try to take it.
        while (!highest_priority_thread->m_context_guard.try_lock()) {
            // The highest priority thread's context is already locked.
            // Check if we need scheduling. If we don't, we can retry directly.
            if (m_state.needs_scheduling.load(std::memory_order_seq_cst)) {
                // If we do, another core is interfering, and we must start again.
                goto retry;
            }
        }

        // It's time to switch the thread.
        // Switch to the highest priority thread.
        SwitchThread(kernel, highest_priority_thread);

        // Check if we need scheduling. If we do, then we can't complete the switch and should
        // retry.
        if (m_state.needs_scheduling.load(std::memory_order_seq_cst)) {
            // Some libc++ lazily init mutex
            [[maybe_unused]] auto const can_lock = highest_priority_thread->m_context_guard.try_lock();
            DEBUG_ASSERT(!can_lock);

            // Our switch failed.
            // We should unlock the thread context, and then retry.
            highest_priority_thread->m_context_guard.unlock();
            goto retry;
        } else {
            break;
        }

    retry:

        // We failed to successfully do the context switch, and need to retry.
        // Clear needs_scheduling.
        m_state.needs_scheduling.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Refresh the highest priority thread.
        highest_priority_thread = m_state.highest_priority_thread;
    }

    // Reload the guest thread context.
    Reload(kernel, highest_priority_thread);

    // Reload the host thread.
    Common::Fiber::YieldTo(m_switch_fiber, *highest_priority_thread->m_host_context);
}

void KScheduler::Unload(KernelCore& kernel, KThread* thread) {
    kernel.PhysicalCore(m_core_id).SaveContext(thread);

    // Check if the thread is terminated by checking the DPC flags.
    if ((thread->GetStackParameters().dpc_flags & static_cast<u32>(DpcFlag::Terminated)) == 0) {
        // Some libc++ lazily init mutex
        [[maybe_unused]] auto const can_lock = thread->m_context_guard.try_lock();
        DEBUG_ASSERT(!can_lock);

        // The thread isn't terminated, so we want to unlock it.
        thread->m_context_guard.unlock();
    }
}

void KScheduler::Reload(KernelCore& kernel, KThread* thread) {
    kernel.PhysicalCore(m_core_id).LoadContext(thread);
}

void KScheduler::ClearPreviousThread(KernelCore& kernel, KThread* thread) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));
    for (size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; ++i) {
        // Get an atomic reference to the core scheduler's previous thread.
        auto& prev_thread{kernel.Scheduler(i).m_state.prev_thread};

        // Atomically clear the previous thread if it's our target.
        KThread* compare = thread;
        prev_thread.compare_exchange_strong(compare, nullptr, std::memory_order_seq_cst);
    }
}

void KScheduler::OnThreadStateChanged(KernelCore& kernel, KThread* thread, ThreadState old_state) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));
    // Check if the state has changed, because if it hasn't there's nothing to do.
    if (const ThreadState cur_state = thread->GetRawState(); cur_state != old_state) {
        if (Nx2ThreadStats::g_enabled) {
            Nx2ThreadStats::OnStateChanged(
                thread, kernel.System().CoreTiming().GetClockTicks(),
                (old_state & ThreadState::Mask) == ThreadState::Waiting,
                cur_state == ThreadState::Runnable,
                (cur_state & ThreadState::Mask) == ThreadState::Waiting);
        }
        // Update the priority queues.
        if (old_state == ThreadState::Runnable) {
            // If we were previously runnable, then we're not runnable now, and we should remove.
            GetPriorityQueue(kernel).Remove(thread);
            IncrementScheduledCount(thread);
            SetSchedulerUpdateNeeded(kernel);
            // HACK: if this is a dummy thread, it should no longer wake up when the
            // scheduler lock is released.
            if (thread->IsDummyThread())
                kernel.GlobalSchedulerContext().UnregisterDummyThreadForWakeup(thread);
        } else if (cur_state == ThreadState::Runnable) {
            // If we're now runnable, then we weren't previously, and we should add.
            GetPriorityQueue(kernel).PushBack(thread);
            IncrementScheduledCount(thread);
            SetSchedulerUpdateNeeded(kernel);
            // HACK: if this is a dummy thread, it should wake up when the scheduler
            // lock is released.
            if (thread->IsDummyThread())
                kernel.GlobalSchedulerContext().RegisterDummyThreadForWakeup(thread);
        }
    }
}

void KScheduler::OnThreadPriorityChanged(KernelCore& kernel, KThread* thread, s32 old_priority) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));

    // If the thread is runnable, we want to change its priority in the queue.
    if (thread->GetRawState() == ThreadState::Runnable) {
        GetPriorityQueue(kernel).ChangePriority(old_priority,
                                                thread == GetCurrentThreadPointer(kernel), thread);
        IncrementScheduledCount(thread);
        SetSchedulerUpdateNeeded(kernel);
    }
}

void KScheduler::OnThreadAffinityMaskChanged(KernelCore& kernel, KThread* thread,
                                             const KAffinityMask& old_affinity, s32 old_core) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));

    // If the thread is runnable, we want to change its affinity in the queue.
    if (thread->GetRawState() == ThreadState::Runnable) {
        GetPriorityQueue(kernel).ChangeAffinityMask(old_core, old_affinity, thread);
        IncrementScheduledCount(thread);
        // EXP98: the cores the thread left too (IncrementScheduledCount raised the new ones).
        NoteYieldGens(old_affinity.GetAffinityMask() |
                      (old_core >= 0 ? u64{1} << old_core : u64{0}));
        SetSchedulerUpdateNeeded(kernel);
    }
}

void KScheduler::RotateScheduledQueue(KernelCore& kernel, s32 core_id, s32 priority) {
    ASSERT(IsSchedulerLockedByCurrentThread(kernel));

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Rotate the front of the queue to the end.
    KThread* top_thread = priority_queue.GetScheduledFront(core_id, priority);
    KThread* next_thread = nullptr;
    if (top_thread != nullptr) {
        next_thread = priority_queue.MoveToScheduledBack(top_thread);
        if (next_thread != top_thread) {
            IncrementScheduledCount(top_thread);
            IncrementScheduledCount(next_thread);
        }
    }

    // While we have a suggested thread, try to migrate it!
    {
        KThread* suggested = priority_queue.GetSuggestedFront(core_id, priority);
        while (suggested != nullptr) {
            // Check if the suggested thread is the top thread on its core.
            const s32 suggested_core = suggested->GetActiveCore();
            if (KThread* top_on_suggested_core =
                    (suggested_core >= 0) ? priority_queue.GetScheduledFront(suggested_core)
                                          : nullptr;
                top_on_suggested_core != suggested) {
                // If the next thread is a new thread that has been waiting longer than our
                // suggestion, we prefer it to our suggestion.
                if (top_thread != next_thread && next_thread != nullptr &&
                    next_thread->GetLastScheduledTick() < suggested->GetLastScheduledTick()) {
                    suggested = nullptr;
                    break;
                }

                // If we're allowed to do a migration, do one.
                // NOTE: Unlike migrations in UpdateHighestPriorityThread, this moves the suggestion
                // to the front of the queue.
                if (top_on_suggested_core == nullptr ||
                    top_on_suggested_core->GetPriority() >= HighestCoreMigrationAllowedPriority) {
                    suggested->SetActiveCore(core_id);
                    priority_queue.ChangeCore(suggested_core, suggested, true);
                    IncrementScheduledCount(suggested);
                    break;
                }
            }

            // Get the next suggestion.
            suggested = priority_queue.GetSamePriorityNext(core_id, suggested);
        }
    }

    // Now that we might have migrated a thread with the same priority, check if we can do better.
    {
        KThread* best_thread = priority_queue.GetScheduledFront(core_id);
        if (best_thread == GetCurrentThreadPointer(kernel)) {
            best_thread = priority_queue.GetScheduledNext(core_id, best_thread);
        }

        // If the best thread we can choose has a priority the same or worse than ours, try to
        // migrate a higher priority thread.
        if (best_thread != nullptr && best_thread->GetPriority() >= priority) {
            KThread* suggested = priority_queue.GetSuggestedFront(core_id);
            while (suggested != nullptr) {
                // If the suggestion's priority is the same as ours, don't bother.
                if (suggested->GetPriority() >= best_thread->GetPriority()) {
                    break;
                }

                // Check if the suggested thread is the top thread on its core.
                const s32 suggested_core = suggested->GetActiveCore();
                if (KThread* top_on_suggested_core =
                        (suggested_core >= 0) ? priority_queue.GetScheduledFront(suggested_core)
                                              : nullptr;
                    top_on_suggested_core != suggested) {
                    // If we're allowed to do a migration, do one.
                    // NOTE: Unlike migrations in UpdateHighestPriorityThread, this moves the
                    // suggestion to the front of the queue.
                    if (top_on_suggested_core == nullptr ||
                        top_on_suggested_core->GetPriority() >=
                            HighestCoreMigrationAllowedPriority) {
                        suggested->SetActiveCore(core_id);
                        priority_queue.ChangeCore(suggested_core, suggested, true);
                        IncrementScheduledCount(suggested);
                        break;
                    }
                }

                // Get the next suggestion.
                suggested = priority_queue.GetSuggestedNext(core_id, suggested);
            }
        }
    }

    // After a rotation, we need a scheduler update.
    SetSchedulerUpdateNeeded(kernel);
}

void KScheduler::YieldWithoutCoreMigration(KernelCore& kernel) {
    // EXP98 1 or 2: the per core early return (below); 0 = the code as it was.
    if (const int exp98 = VideoCore::Nx2Debug::Experiment(98); exp98 != 0) [[unlikely]] {
        YieldWithoutCoreMigrationExp98(kernel, exp98);
        return;
    }

    // Validate preconditions.
    ASSERT(CanSchedule(kernel));
    ASSERT(GetCurrentProcessPointer(kernel) != nullptr);

    // Get the current thread and process.
    KThread& cur_thread = GetCurrentThread(kernel);
    KProcess& cur_process = GetCurrentProcess(kernel);

    // If the thread's yield count matches, there's nothing for us to do.
    if (cur_thread.GetYieldScheduleCount() == cur_process.GetScheduledCount()) {
        return;
    }

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Perform the yield.
    {
        KScopedSchedulerLock sl{kernel};

        const auto cur_state = cur_thread.GetRawState();
        if (cur_state == ThreadState::Runnable) {
            // Put the current thread at the back of the queue.
            KThread* next_thread = priority_queue.MoveToScheduledBack(std::addressof(cur_thread));
            IncrementScheduledCount(std::addressof(cur_thread));

            // If the next thread is different, we have an update to perform.
            if (next_thread != std::addressof(cur_thread)) {
                SetSchedulerUpdateNeeded(kernel);
            } else {
                // Otherwise, set the thread's yield count so that we won't waste work until the
                // process is scheduled again.
                cur_thread.SetYieldScheduleCount(cur_process.GetScheduledCount());
            }
        }
    }
}

void KScheduler::YieldWithCoreMigration(KernelCore& kernel) {
    // EXP98 1 or 2: the per core early return (below); 0 = the code as it was.
    if (const int exp98 = VideoCore::Nx2Debug::Experiment(98); exp98 != 0) [[unlikely]] {
        YieldWithCoreMigrationExp98(kernel, exp98);
        return;
    }

    // Validate preconditions.
    ASSERT(CanSchedule(kernel));
    ASSERT(GetCurrentProcessPointer(kernel) != nullptr);

    // Get the current thread and process.
    KThread& cur_thread = GetCurrentThread(kernel);
    KProcess& cur_process = GetCurrentProcess(kernel);

    // If the thread's yield count matches, there's nothing for us to do.
    if (cur_thread.GetYieldScheduleCount() == cur_process.GetScheduledCount()) {
        return;
    }

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Perform the yield.
    {
        KScopedSchedulerLock sl{kernel};

        const auto cur_state = cur_thread.GetRawState();
        if (cur_state == ThreadState::Runnable) {
            // Get the current active core.
            const s32 core_id = cur_thread.GetActiveCore();

            // Put the current thread at the back of the queue.
            KThread* next_thread = priority_queue.MoveToScheduledBack(std::addressof(cur_thread));
            IncrementScheduledCount(std::addressof(cur_thread));

            // While we have a suggested thread, try to migrate it!
            bool recheck = false;
            KThread* suggested = priority_queue.GetSuggestedFront(core_id);
            while (suggested != nullptr) {
                // Check if the suggested thread is the thread running on its core.
                const s32 suggested_core = suggested->GetActiveCore();

                if (KThread* running_on_suggested_core =
                        (suggested_core >= 0)
                            ? kernel.Scheduler(suggested_core).m_state.highest_priority_thread
                            : nullptr;
                    running_on_suggested_core != suggested) {
                    // If the current thread's priority is higher than our suggestion's we prefer
                    // the next thread to the suggestion. We also prefer the next thread when the
                    // current thread's priority is equal to the suggestions, but the next thread
                    // has been waiting longer.
                    if ((suggested->GetPriority() > cur_thread.GetPriority()) ||
                        (suggested->GetPriority() == cur_thread.GetPriority() &&
                         next_thread != std::addressof(cur_thread) &&
                         next_thread->GetLastScheduledTick() < suggested->GetLastScheduledTick())) {
                        suggested = nullptr;
                        break;
                    }

                    // If we're allowed to do a migration, do one.
                    // NOTE: Unlike migrations in UpdateHighestPriorityThread, this moves the
                    // suggestion to the front of the queue.
                    if (running_on_suggested_core == nullptr ||
                        running_on_suggested_core->GetPriority() >=
                            HighestCoreMigrationAllowedPriority) {
                        suggested->SetActiveCore(core_id);
                        priority_queue.ChangeCore(suggested_core, suggested, true);
                        IncrementScheduledCount(suggested);
                        break;
                    } else {
                        // We couldn't perform a migration, but we should check again on a future
                        // yield.
                        recheck = true;
                    }
                }

                // Get the next suggestion.
                suggested = priority_queue.GetSuggestedNext(core_id, suggested);
            }

            // If we still have a suggestion or the next thread is different, we have an update to
            // perform.
            if (suggested != nullptr || next_thread != std::addressof(cur_thread)) {
                SetSchedulerUpdateNeeded(kernel);
            } else if (!recheck) {
                // Otherwise if we don't need to re-check, set the thread's yield count so that we
                // won't waste work until the process is scheduled again.
                cur_thread.SetYieldScheduleCount(cur_process.GetScheduledCount());
            }
        }
    }
}

void KScheduler::YieldToAnyThread(KernelCore& kernel) {
    // EXP98 leaves this form as it is (OoT does not use it); it only counts it.
    if (const int exp98 = VideoCore::Nx2Debug::Experiment(98); exp98 != 0) [[unlikely]] {
        CountYieldForm(YieldStatsOf(GetCurrentCoreId(kernel)), 2, exp98);
    }

    // Validate preconditions.
    ASSERT(CanSchedule(kernel));
    ASSERT(GetCurrentProcessPointer(kernel) != nullptr);

    // Get the current thread and process.
    KThread& cur_thread = GetCurrentThread(kernel);
    KProcess& cur_process = GetCurrentProcess(kernel);

    // If the thread's yield count matches, there's nothing for us to do.
    if (cur_thread.GetYieldScheduleCount() == cur_process.GetScheduledCount()) {
        return;
    }

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Perform the yield.
    {
        KScopedSchedulerLock sl{kernel};

        const auto cur_state = cur_thread.GetRawState();
        if (cur_state == ThreadState::Runnable) {
            // Get the current active core.
            const s32 core_id = cur_thread.GetActiveCore();

            // Migrate the current thread to core -1.
            cur_thread.SetActiveCore(-1);
            priority_queue.ChangeCore(core_id, std::addressof(cur_thread));
            IncrementScheduledCount(std::addressof(cur_thread));

            // If there's nothing scheduled, we can try to perform a migration.
            if (priority_queue.GetScheduledFront(core_id) == nullptr) {
                // While we have a suggested thread, try to migrate it!
                KThread* suggested = priority_queue.GetSuggestedFront(core_id);
                while (suggested != nullptr) {
                    // Check if the suggested thread is the top thread on its core.
                    const s32 suggested_core = suggested->GetActiveCore();
                    if (KThread* top_on_suggested_core =
                            (suggested_core >= 0) ? priority_queue.GetScheduledFront(suggested_core)
                                                  : nullptr;
                        top_on_suggested_core != suggested) {
                        // If we're allowed to do a migration, do one.
                        if (top_on_suggested_core == nullptr ||
                            top_on_suggested_core->GetPriority() >=
                                HighestCoreMigrationAllowedPriority) {
                            suggested->SetActiveCore(core_id);
                            priority_queue.ChangeCore(suggested_core, suggested);
                            IncrementScheduledCount(suggested);
                        }

                        // Regardless of whether we migrated, we had a candidate, so we're done.
                        break;
                    }

                    // Get the next suggestion.
                    suggested = priority_queue.GetSuggestedNext(core_id, suggested);
                }

                // If the suggestion is different from the current thread, we need to perform an
                // update.
                if (suggested != std::addressof(cur_thread)) {
                    SetSchedulerUpdateNeeded(kernel);
                } else {
                    // Otherwise, set the thread's yield count so that we won't waste work until the
                    // process is scheduled again.
                    cur_thread.SetYieldScheduleCount(cur_process.GetScheduledCount());
                }
            } else {
                // Otherwise, we have an update to perform.
                SetSchedulerUpdateNeeded(kernel);
            }
        }
    }
}

// EXP98 (Fable fable52_answer_yield). 1: return early when the thread's stamp says the locked
// path would find nothing to do (YieldIsKnownNoOp). 2 (verify): never return early; under the
// lock, compare the stamp's prediction with what the locked path does and count mismatches.
// Both: a yield that does nothing raises no counter (the old per-process count included), and it
// stamps the thread instead. The per-process early return is not used in these modes.
void KScheduler::YieldWithoutCoreMigrationExp98(KernelCore& kernel, int mode) {
    // Validate preconditions.
    ASSERT(CanSchedule(kernel));
    ASSERT(GetCurrentProcessPointer(kernel) != nullptr);

    // Get the current thread and process.
    KThread& cur_thread = GetCurrentThread(kernel);
    KProcess& cur_process = GetCurrentProcess(kernel);
    YieldStats& stats = YieldStatsOf(GetCurrentCoreId(kernel));
    CountYieldForm(stats, 0, mode);

    if (mode == 1 && YieldIsKnownNoOp(kernel, cur_thread, 1, stats)) {
        Add(stats.early);
        // EXP102 (live, pauses; fable52_answer_yield 7): keep the old cadence of a yield
        // that took the lock, so the worker does not reach its idle wait sooner.
        for (int pause = VideoCore::Nx2Debug::Experiment(102); pause > 0; --pause) {
            CpuPause();
        }
        return;
    }

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Perform the yield.
    {
        KScopedSchedulerLock sl{kernel};

        // From here on, every scheduling change raises the per core counters.
        if (!g_yield_gens_live.load(std::memory_order_relaxed)) [[unlikely]] {
            g_yield_gens_live.store(true, std::memory_order_relaxed);
        }

        // Mode 2: the prediction, exact under the lock.
        const bool predicted = mode == 2 && YieldStampMatches(cur_thread, 1);
        if (predicted) {
            Add(stats.predicted);
        }

        const auto cur_state = cur_thread.GetRawState();
        if (cur_state == ThreadState::Runnable) {
            const s32 core_id = cur_thread.GetActiveCore();

            // Put the current thread at the back of the queue.
            KThread* next_thread = priority_queue.MoveToScheduledBack(std::addressof(cur_thread));

            if (next_thread != std::addressof(cur_thread)) {
                // The next thread is different: we have an update to perform.
                IncrementScheduledCount(std::addressof(cur_thread));
                SetSchedulerUpdateNeeded(kernel);
                Add(stats.rotated);
                if (predicted) {
                    NoteYieldMismatch(stats, cur_thread, core_id, "rotated", next_thread);
                }
            } else {
                // Nothing to do: no counter moves; stamp the thread.
                if (predicted && core_id != cur_thread.GetYieldCore()) {
                    NoteYieldMismatch(stats, cur_thread, core_id, "core", nullptr);
                }
                cur_thread.SetYieldScheduleCount(cur_process.GetScheduledCount());
                if (core_id >= 0 && core_id < static_cast<s32>(g_yield_gen.size())) {
                    cur_thread.SetYieldCache(core_id, YieldGenOf(core_id), 1);
                }
                Add(stats.stamped);
            }
        } else {
            Add(stats.not_runnable);
        }
    }
}

void KScheduler::YieldWithCoreMigrationExp98(KernelCore& kernel, int mode) {
    // Validate preconditions.
    ASSERT(CanSchedule(kernel));
    ASSERT(GetCurrentProcessPointer(kernel) != nullptr);

    // Get the current thread and process.
    KThread& cur_thread = GetCurrentThread(kernel);
    KProcess& cur_process = GetCurrentProcess(kernel);
    YieldStats& stats = YieldStatsOf(GetCurrentCoreId(kernel));
    CountYieldForm(stats, 1, mode);

    if (mode == 1 && YieldIsKnownNoOp(kernel, cur_thread, 2, stats)) {
        Add(stats.early);
        // EXP102 (live, pauses; fable52_answer_yield 7): keep the old cadence of a yield
        // that took the lock, so the worker does not reach its idle wait sooner.
        for (int pause = VideoCore::Nx2Debug::Experiment(102); pause > 0; --pause) {
            CpuPause();
        }
        return;
    }

    // Get a reference to the priority queue.
    auto& priority_queue = GetPriorityQueue(kernel);

    // Perform the yield.
    {
        KScopedSchedulerLock sl{kernel};

        // From here on, every scheduling change raises the per core counters.
        if (!g_yield_gens_live.load(std::memory_order_relaxed)) [[unlikely]] {
            g_yield_gens_live.store(true, std::memory_order_relaxed);
        }

        // Mode 2: the prediction, exact under the lock.
        const bool predicted = mode == 2 && YieldStampMatches(cur_thread, 2);
        if (predicted) {
            Add(stats.predicted);
        }

        const auto cur_state = cur_thread.GetRawState();
        if (cur_state == ThreadState::Runnable) {
            // Get the current active core.
            const s32 core_id = cur_thread.GetActiveCore();

            // Put the current thread at the back of the queue.
            KThread* next_thread = priority_queue.MoveToScheduledBack(std::addressof(cur_thread));

            // While we have a suggested thread, try to migrate it! (Unchanged, apart from
            // remembering the thread that set recheck, for the mode 2 log.)
            bool recheck = false;
            KThread* recheck_thread = nullptr;
            KThread* suggested = priority_queue.GetSuggestedFront(core_id);
            while (suggested != nullptr) {
                // Check if the suggested thread is the thread running on its core.
                const s32 suggested_core = suggested->GetActiveCore();

                if (KThread* running_on_suggested_core =
                        (suggested_core >= 0)
                            ? kernel.Scheduler(suggested_core).m_state.highest_priority_thread
                            : nullptr;
                    running_on_suggested_core != suggested) {
                    // If the current thread's priority is higher than our suggestion's we prefer
                    // the next thread to the suggestion. We also prefer the next thread when the
                    // current thread's priority is equal to the suggestions, but the next thread
                    // has been waiting longer.
                    if ((suggested->GetPriority() > cur_thread.GetPriority()) ||
                        (suggested->GetPriority() == cur_thread.GetPriority() &&
                         next_thread != std::addressof(cur_thread) &&
                         next_thread->GetLastScheduledTick() < suggested->GetLastScheduledTick())) {
                        suggested = nullptr;
                        break;
                    }

                    // If we're allowed to do a migration, do one.
                    // NOTE: Unlike migrations in UpdateHighestPriorityThread, this moves the
                    // suggestion to the front of the queue.
                    if (running_on_suggested_core == nullptr ||
                        running_on_suggested_core->GetPriority() >=
                            HighestCoreMigrationAllowedPriority) {
                        suggested->SetActiveCore(core_id);
                        priority_queue.ChangeCore(suggested_core, suggested, true);
                        IncrementScheduledCount(suggested);
                        break;
                    } else {
                        // We couldn't perform a migration, but we should check again on a future
                        // yield.
                        recheck = true;
                        recheck_thread = suggested;
                    }
                }

                // Get the next suggestion.
                suggested = priority_queue.GetSuggestedNext(core_id, suggested);
            }

            const bool rotated = next_thread != std::addressof(cur_thread);
            const bool migrated = suggested != nullptr;
            if (rotated || migrated) {
                // The yield did something: raise the current thread's counters as before, and
                // perform the update.
                IncrementScheduledCount(std::addressof(cur_thread));
                SetSchedulerUpdateNeeded(kernel);
                Add(migrated ? stats.migrated : stats.rotated);
                if (predicted) {
                    NoteYieldMismatch(stats, cur_thread, core_id, migrated ? "migrated" : "rotated",
                                      migrated ? suggested : next_thread);
                }
            } else if (!recheck) {
                // Nothing to do: no counter moves; stamp the thread.
                if (predicted && core_id != cur_thread.GetYieldCore()) {
                    NoteYieldMismatch(stats, cur_thread, core_id, "core", nullptr);
                }
                cur_thread.SetYieldScheduleCount(cur_process.GetScheduledCount());
                if (core_id >= 0 && core_id < static_cast<s32>(g_yield_gen.size())) {
                    cur_thread.SetYieldCache(core_id, YieldGenOf(core_id), 2);
                }
                Add(stats.stamped);
            } else {
                // Recheck: changed nothing, never stamped (as before).
                Add(stats.recheck);
                if (predicted) {
                    NoteYieldMismatch(stats, cur_thread, core_id, "recheck", recheck_thread);
                }
            }
        } else {
            Add(stats.not_runnable);
        }
    }
}

void KScheduler::RescheduleOtherCores(KernelCore& kernel, u64 cores_needing_scheduling) {
    if (const u64 core_mask = cores_needing_scheduling & ~(1ULL << m_core_id); core_mask != 0) {
        RescheduleCores(kernel, core_mask);
    }
}

void KScheduler::RescheduleCores(KernelCore& kernel, u64 core_mask) {
    // Send IPI
    for (size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
        if (core_mask & (1ULL << i)) {
            kernel.PhysicalCore(i).Interrupt();
        }
    }
}

} // namespace Kernel
