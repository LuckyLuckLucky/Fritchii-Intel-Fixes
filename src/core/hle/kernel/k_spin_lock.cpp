// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_spin_lock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/logging.h"
#include "common/lowend_sim.h"
#include "video_core/nx2_debug_control.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Kernel {

namespace {

// Spins of about 30-140 cycles each before a contended Lock sleeps (a few microseconds).
constexpr int SpinCount = 128;

// EDEN_NX2_KLOCK_STATS=1 (read once): contended locks, how they ended (spun, slept) and the
// time the slept ones waited, every 5 s.
bool StatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_KLOCK_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}
std::atomic<u64> g_contended{};
std::atomic<u64> g_spun{};
std::atomic<u64> g_slept{};
std::atomic<u64> g_slept_ns{};
std::atomic<s64> g_next_report{};
void MaybeReport(s64 now) {
    s64 next = g_next_report.load(std::memory_order_relaxed);
    if (next == 0) {
        g_next_report.compare_exchange_strong(next, now + 5'000'000'000);
        return;
    }
    if (now < next || !g_next_report.compare_exchange_strong(next, now + 5'000'000'000)) {
        return;
    }
    const u64 contended = g_contended.exchange(0, std::memory_order_relaxed);
    const u64 spun = g_spun.exchange(0, std::memory_order_relaxed);
    const u64 slept = g_slept.exchange(0, std::memory_order_relaxed);
    const u64 slept_ns = g_slept_ns.exchange(0, std::memory_order_relaxed);
    LOG_INFO(Kernel,
             "Kernel spin lock (5 s, per second, EXP97={}): contended {:.0f}, got by spinning {:.0f}, "
             "slept {:.0f} ({:.1f} ms/s, {:.1f} us each)",
             VideoCore::Nx2Debug::Experiment(97), contended / 5.0, spun / 5.0, slept / 5.0,
             slept_ns / 5e6, slept != 0 ? slept_ns / 1e3 / slept : 0.0);
}
s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Pause() {
    Common::LowEndSim::CpuPauseNote();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

} // Anonymous namespace

void KSpinLock::Lock() {
    u32 expected = 0;
    if (m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
    }
    const bool stats = StatsEnabled();
    if (stats) [[unlikely]] {
        g_contended.fetch_add(1, std::memory_order_relaxed);
    }
    // EXP97 (live): the spin count, 0 = SpinCount.
    const int exp97 = VideoCore::Nx2Debug::Experiment(97);
    const int spin_count = exp97 > 0 ? exp97 : SpinCount;
    for (int spin = 0; spin < spin_count; ++spin) {
        Pause();
        if (m_state.load(std::memory_order_relaxed) == 0) {
            expected = 0;
            if (m_state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                if (stats) [[unlikely]] {
                    g_spun.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        }
    }
    // Mark the lock contended and sleep until it is released; taken this way it stays marked,
    // so its Unlock wakes the next sleeper.
    const s64 sleep_begin = stats ? NowNs() : 0;
    while (m_state.exchange(2, std::memory_order_acquire) != 0) {
        m_state.wait(2, std::memory_order_relaxed);
    }
    if (stats) [[unlikely]] {
        const s64 now = NowNs();
        g_slept.fetch_add(1, std::memory_order_relaxed);
        g_slept_ns.fetch_add(static_cast<u64>(now - sleep_begin), std::memory_order_relaxed);
        MaybeReport(now);
    }
}

void KSpinLock::Unlock() {
    if (m_state.exchange(0, std::memory_order_release) == 2) {
        m_state.notify_one();
    }
}

bool KSpinLock::TryLock() {
    u32 expected = 0;
    return m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed);
}

} // namespace Kernel
