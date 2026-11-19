// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "common/fritchii_lock_census.h"
#include "common/lowend_sim.h"
#include "common/nx2_hitch.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Common {

/// A recursive mutex that spins briefly before sleeping. The GPU caches' locks are taken tens of
/// thousands of times a second by the GPU thread, the fencing thread and CPU write traps, and
/// held for microseconds: std::recursive_mutex put a waiting thread to sleep at once, and its
/// wake-up took longer than the critical section (title 1's GPU thread spent ~8% asleep there).
class SpinRecursiveMutex {
public:
    void lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            LockContended();
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }

    bool try_lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return false;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }

    void unlock() {
        if (--depth != 0) {
            return;
        }
        // Still held here: the hook runs as the holder's last act (the buffer cache's page
        // mirrors close their sequence counter, EXP86). Null on every other mutex.
        if (on_release != nullptr) [[unlikely]] {
            on_release(on_release_context);
        }
        owner.store(std::thread::id{}, std::memory_order_relaxed);
        if (state.exchange(0, std::memory_order_seq_cst) == 2) {
            state.notify_one();
        }
        // A hooked waiter sleeps on wake_epoch, not on state (LockContendedUntimed).
        if (hooked_waiters.load(std::memory_order_seq_cst) != 0) [[unlikely]] {
            WakeWaiters();
        }
    }

    /// Wakes the waiters that sleep with a contended hook (they ask their hook and the lock
    /// again). The holder calls it when it starts to wait for such a waiter (it posted it work),
    /// since a waiter that read a stale owner went to sleep without knowing. Spurious wakes are
    /// harmless.
    void WakeWaiters() {
        wake_epoch.fetch_add(1, std::memory_order_seq_cst);
        wake_epoch.notify_all();
    }

    /// Called by the holder at each final unlock (depth back to 0), before the mutex is given up.
    /// Set once, before the mutex is shared between threads; the hook must not lock this mutex.
    void SetReleaseHook(void (*hook)(void*), void* context) {
        on_release = hook;
        on_release_context = context;
    }

    /// Whether the calling thread holds the mutex.
    [[nodiscard]] bool IsHeldByCurrentThread() const {
        return owner.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    /// FRITCHII_LOCK_CENSUS: the lock's name in the census line ("buffer cache", "texture
    /// cache"). Set once, before the mutex is shared.
    void SetCensusName(const char* name) {
        census_name = name;
    }

    /// FRITCHII_FENCE_YIELD_E: whether E (GPUBack) waits for this lock now (only marked while
    /// the switch is on).
    [[nodiscard]] bool BackWaiting() const {
        return back_waiters.load(std::memory_order_relaxed) != 0;
    }

    /// The holding thread, or a default id when free (a watchdog's snapshot, racy by nature).
    [[nodiscard]] std::thread::id Owner() const {
        return owner.load(std::memory_order_relaxed);
    }

    /// The holding thread's: the mutex stays held, by `thread` from now on (a thread that works
    /// for the holder while the holder waits). The caller orders the two threads.
    void HandOver(std::thread::id thread) {
        owner.store(thread, std::memory_order_relaxed);
    }

    /// A thread that may have to work for the holder while it waits for the lock (the memory
    /// stage's back serves GPUMem's syncs, and GPUMem can hold the buffer cache's lock while it
    /// waits for one: FRITCHII_MEMSTAGE_AHEAD). Set per thread; while set, a contended lock calls
    /// it between tries with the holder's id (`first`: the first try of this wait). While it
    /// answers true (the holder is the thread it may have to work for) the wait only spins and
    /// yields; on false it spins, then sleeps on a word every unlock and WakeWaiters bump, and
    /// asks again after each wake. Null everywhere else.
    using ContendedHook = bool (*)(std::thread::id holder, bool first);
    static inline thread_local ContendedHook t_contended_hook = nullptr;

private:
    // About 10 microseconds of spinning before sleeping.
    static constexpr int SpinCount = 1024;

    static void Pause() {
        LowEndSim::CpuPauseNote();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    // Timed per thread role for the drain sites line (fable52_answer_13 section 3).
    // FRITCHII_LOCK_CENSUS (helper CP): the holder at the wait's start, named per waiter.
    void LockContended() {
        const std::uint64_t begin = Nx2Hitch::NowNs();
        const int census = LockCensus::Mode();
        LockCensus::WaitToken token{};
        if (census > 0) [[unlikely]] {
            token = LockCensus::BeginWait(census, owner.load(std::memory_order_relaxed),
                                          census_name, &owner);
        }
        // FRITCHII_FENCE_YIELD_E: E marks its wait for the fencing thread's pops.
        const bool back_mark = Nx2Hitch::t_role == Nx2Hitch::Role::GpuBack &&
                               LockCensus::FenceYieldE() != 0;
        if (back_mark) [[unlikely]] {
            back_waiters.fetch_add(1, std::memory_order_relaxed);
        }
        // FRITCHII_FENCE_COPY_UNLOCKED's line: E's waits that began behind the fencing thread.
        const bool fence_watch = Nx2Hitch::t_role == Nx2Hitch::Role::GpuBack &&
                                 LockCensus::g_fence_copy_watch.load(std::memory_order_relaxed);
        std::thread::id holder_at_start{};
        if (fence_watch) [[unlikely]] {
            holder_at_start = owner.load(std::memory_order_relaxed);
        }
        LockContendedUntimed();
        if (back_mark) [[unlikely]] {
            back_waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        const std::uint64_t waited = Nx2Hitch::NowNs() - begin;
        if (fence_watch && holder_at_start != std::thread::id{} &&
            holder_at_start == LockCensus::g_fence_thread.load(std::memory_order_relaxed))
            [[unlikely]] {
            LockCensus::g_back_behind_fence_ns.fetch_add(waited, std::memory_order_relaxed);
            LockCensus::g_back_behind_fence_waits.fetch_add(1, std::memory_order_relaxed);
        }
        Nx2Hitch::AddLockWait(waited);
        if (census > 0) [[unlikely]] {
            LockCensus::EndWait(token, waited);
        }
    }

    void LockContendedUntimed() {
        if (t_contended_hook != nullptr) [[unlikely]] {
            bool first = true;
            for (int spin = 0;; ++spin) {
                if (state.load(std::memory_order_relaxed) == 0) {
                    std::uint32_t expected = 0;
                    if (state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                                    std::memory_order_relaxed)) {
                        return;
                    }
                }
                const bool relevant =
                    t_contended_hook(owner.load(std::memory_order_relaxed), first);
                first = false;
                if (relevant) {
                    // The holder may need this thread: never sleep while it holds the lock.
                    if (spin < SpinCount) {
                        Pause();
                    } else {
                        std::this_thread::yield();
                    }
                    continue;
                }
                if (spin < SpinCount) {
                    Pause();
                    continue;
                }
                // Sleep on wake_epoch, not on state: every unlock wakes it while it sleeps, and
                // so does a holder that posts this thread work (WakeWaiters), which matters
                // because the holder can change to that thread without a wake reaching a state
                // sleeper, and the owner read here can be stale. After a wake: the lock and the
                // hook again.
                hooked_waiters.fetch_add(1, std::memory_order_seq_cst);
                const std::uint32_t epoch = wake_epoch.load(std::memory_order_seq_cst);
                if (state.load(std::memory_order_seq_cst) != 0 &&
                    !t_contended_hook(owner.load(std::memory_order_relaxed), false)) {
                    wake_epoch.wait(epoch, std::memory_order_seq_cst);
                }
                hooked_waiters.fetch_sub(1, std::memory_order_relaxed);
                spin = 0;
            }
        }
        for (int spin = 0; spin < SpinCount; ++spin) {
            Pause();
            if (state.load(std::memory_order_relaxed) == 0) {
                std::uint32_t expected = 0;
                if (state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return;
                }
            }
        }
        // 2: held with (possible) sleepers; taken this way it stays marked, so its unlock wakes
        // the next sleeper.
        while (state.exchange(2, std::memory_order_acquire) != 0) {
            state.wait(2, std::memory_order_relaxed);
        }
    }

    std::atomic<std::uint32_t> state{0}; ///< 0 free, 1 held, 2 held with sleepers
    std::atomic<std::thread::id> owner{};
    /// Waiters with a contended hook sleep on wake_epoch (bumped by every unlock while any sleeps,
    /// and by WakeWaiters), so they wake for an unlock and for a holder that needs them.
    std::atomic<std::uint32_t> hooked_waiters{0};
    std::atomic<std::uint32_t> wake_epoch{0};
    std::uint32_t depth{}; ///< Recursion depth, touched only by the owner
    void (*on_release)(void*){};
    void* on_release_context{};
    const char* census_name = nullptr; ///< FRITCHII_LOCK_CENSUS
    std::atomic<std::uint32_t> back_waiters{0}; ///< FRITCHII_FENCE_YIELD_E
};

} // namespace Common
