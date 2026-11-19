// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Intel fork (2026-11-19): INTEL_FORK_REPORT_READ_GUARD=1 (default off, read once).
//
// With the Default GPU fence behaviour the guest CPU passes its syncpoint 1-3 frames before the
// host GPU ran the work, and reads ZPassPixelCount64 sample-count report slots whose
// ReportOperation (query_cache.h, run at the fence pop) has not written them yet: OoT then drops
// an object for a frame (sky through stones/trees while the camera turns). Balanced fences hide
// it but cost ~50 % fps in big areas. This guard keeps Default fences and makes only a guest CPU
// READ of a page that still owes a sample-count report wait until those reports landed.
//
//  - RasterizerVulkan::Query (once per report, on the thread that takes the query in stream
//    order, before any lend) counts the report as expected for its 4 KiB page, keeps the page
//    rasterizer-cached (UpdatePagesCachedCount +1 once, so guest reads of it reach
//    GPU::OnCPURead) and arms a read trap on it (which also bumps the cores' read-area
//    generation, so a cached read area does not skip the next read).
//  - Every way a sample-count report ends (its operation written or discarded or dropped,
//    EXP119's run entries, the immediate payload write without a streamer) counts it landed.
//  - GPU::OnCPURead first clears the page's trap bit, then waits while the page owes reports:
//    2 ms on a condition variable, then forces progress (FlushQueryResults on the GPU thread),
//    then 50 ms more; still pending counts a timeout and re-balances the page (never hangs).
//
// Counters are per page hash (collisions only make a read wait for a neighbour's report too).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCore::IntelReportGuard {

inline constexpr u64 PageBits = 12;
inline constexpr u64 PageSize = u64{1} << PageBits;
inline constexpr u64 Slots = u64{1} << 16;

[[nodiscard]] inline bool Enabled() {
    static const bool on = [] {
        const char* const value = std::getenv("INTEL_FORK_REPORT_READ_GUARD");
        const bool result = value != nullptr && value[0] == '1';
        if (result) {
            LOG_INFO(HW_GPU, "Intel fork report read guard on (INTEL_FORK_REPORT_READ_GUARD=1)");
        }
        return result;
    }();
    return on;
}

struct State {
    std::array<std::atomic<u32>, Slots> expected{};
    std::array<std::atomic<u32>, Slots> landed{};
    std::array<std::atomic<u64>, Slots> armed{};   ///< page + 1 whose trap is armed, 0 none
    std::array<std::atomic<u64>, Slots> watched{}; ///< page + 1 known watched (fast check)
    std::mutex watch_mutex;
    std::unordered_set<u64> watched_pages; ///< pages holding our UpdatePagesCachedCount +1
    std::atomic<u64> watched_count{};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::atomic<u32> waiters{};
    // Stats (since the last log line, except watched_count).
    std::atomic<u64> arms{};
    std::atomic<u64> trapped_reads{};
    std::atomic<u64> waited{};
    std::atomic<u64> wait_ns{};
    std::atomic<u64> wait_max_ns{};
    std::atomic<u64> forced{};
    std::atomic<u64> timeouts{};
    std::atomic<s64> log_since{};
};

[[nodiscard]] inline State& Get() {
    static State state;
    return state;
}

[[nodiscard]] inline u64 SlotOf(u64 page) {
    return page & (Slots - 1);
}

[[nodiscard]] inline u32 Pending(State& s, u64 slot) {
    const s32 diff = static_cast<s32>(s.expected[slot].load(std::memory_order_seq_cst) -
                                      s.landed[slot].load(std::memory_order_seq_cst));
    return diff > 0 ? static_cast<u32>(diff) : 0;
}

[[nodiscard]] inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Any thread: one log line every 10 s, from whichever thread passes the time first.
inline void MaybeLog() {
    State& s = Get();
    const s64 now = NowNs();
    s64 since = s.log_since.load(std::memory_order_relaxed);
    if (since == 0) {
        s.log_since.compare_exchange_strong(since, now, std::memory_order_relaxed);
        return;
    }
    if (now - since < 10'000'000'000 ||
        !s.log_since.compare_exchange_strong(since, now, std::memory_order_relaxed)) {
        return;
    }
    u64 pending = 0;
    for (u64 slot = 0; slot < Slots; ++slot) {
        pending += Pending(s, slot);
    }
    const u64 wait_ns = s.wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 wait_max_ns = s.wait_max_ns.exchange(0, std::memory_order_relaxed);
    LOG_INFO(HW_GPU,
             "Intel fork report read guard 10 s: watched pages {}, armed {}, trapped reads {}, "
             "waited {} ({:.2f} ms total, max {:.2f} ms), forced {}, timeouts {} (must be 0), "
             "pending now {}",
             s.watched_count.load(std::memory_order_relaxed),
             s.arms.exchange(0, std::memory_order_relaxed),
             s.trapped_reads.exchange(0, std::memory_order_relaxed),
             s.waited.exchange(0, std::memory_order_relaxed), static_cast<double>(wait_ns) / 1e6,
             static_cast<double>(wait_max_ns) / 1e6,
             s.forced.exchange(0, std::memory_order_relaxed),
             s.timeouts.exchange(0, std::memory_order_relaxed), pending);
}

/// RasterizerVulkan::Query, once per sample-count report (before any lend, no cache lock held).
/// `watch(page_address)`: UpdatePagesCachedCount(+1) for a page not watched yet;
/// `arm(page_address)`: ArmReadTraps for a page whose trap bit is clear.
template <typename Watch, typename Arm>
inline void NoteExpected(u64 device_address, Watch&& watch, Arm&& arm) {
    State& s = Get();
    const u64 page = device_address >> PageBits;
    const u64 slot = SlotOf(page);
    s.expected[slot].fetch_add(1, std::memory_order_seq_cst);
    if (s.watched[slot].load(std::memory_order_relaxed) != page + 1) {
        bool fresh = false;
        {
            std::scoped_lock lock{s.watch_mutex};
            fresh = s.watched_pages.insert(page).second;
            s.watched[slot].store(page + 1, std::memory_order_relaxed);
        }
        if (fresh) {
            s.watched_count.fetch_add(1, std::memory_order_relaxed);
            watch(page << PageBits);
        }
    }
    if (s.armed[slot].exchange(page + 1, std::memory_order_acq_rel) != page + 1) {
        s.arms.fetch_add(1, std::memory_order_relaxed);
        arm(page << PageBits);
    }
    MaybeLog();
}

/// A sample-count report counted by NoteExpected was written or dropped (any thread).
inline void NoteLanded(u64 device_address) {
    if (!Enabled()) {
        return;
    }
    State& s = Get();
    s.landed[SlotOf(device_address >> PageBits)].fetch_add(1, std::memory_order_seq_cst);
    if (s.waiters.load(std::memory_order_seq_cst) != 0) {
        std::scoped_lock lock{s.wait_mutex};
        s.wait_cv.notify_all();
    }
}

/// RasterizerVulkan::UnmapMemory: gives back the page watches in the range.
template <typename Unwatch>
inline void UnwatchRange(u64 address, u64 size, Unwatch&& unwatch) {
    if (!Enabled() || size == 0) {
        return;
    }
    State& s = Get();
    const u64 first = address >> PageBits;
    const u64 last = (address + size - 1) >> PageBits;
    std::vector<u64> pages;
    {
        std::scoped_lock lock{s.watch_mutex};
        for (auto it = s.watched_pages.begin(); it != s.watched_pages.end();) {
            if (*it >= first && *it <= last) {
                pages.push_back(*it);
                u64 expected = *it + 1;
                s.watched[SlotOf(*it)].compare_exchange_strong(expected, 0,
                                                               std::memory_order_relaxed);
                it = s.watched_pages.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const u64 page : pages) {
        s.watched_count.fetch_sub(1, std::memory_order_relaxed);
        unwatch(page << PageBits);
    }
}

[[nodiscard]] inline bool WaitSettled(State& s, u64 slot, std::chrono::nanoseconds limit) {
    s.waiters.fetch_add(1, std::memory_order_seq_cst);
    bool settled;
    {
        std::unique_lock lock{s.wait_mutex};
        settled = s.wait_cv.wait_for(lock, limit, [&] { return Pending(s, slot) == 0; });
    }
    s.waiters.fetch_sub(1, std::memory_order_seq_cst);
    return settled;
}

/// GPU::OnCPURead, first thing (a guest CPU thread). `force(page_address)` must make the GPU
/// side land the page's reports (a sync operation running FlushQueryResults).
template <typename Force>
inline void BeforeRead(u64 address, u64 size, Force&& force) {
    if (size == 0) {
        return;
    }
    State& s = Get();
    MaybeLog();
    const u64 first = address >> PageBits;
    const u64 last = (address + size - 1) >> PageBits;
    for (u64 page = first; page <= last; ++page) {
        const u64 slot = SlotOf(page);
        // The trap bit first (cleared before the check): a report counted after this re-arms.
        u64 armed_page = page + 1;
        if (s.armed[slot].compare_exchange_strong(armed_page, 0, std::memory_order_acq_rel)) {
            s.trapped_reads.fetch_add(1, std::memory_order_relaxed);
        }
        if (Pending(s, slot) == 0) {
            continue;
        }
        const s64 start = NowNs();
        s.waited.fetch_add(1, std::memory_order_relaxed);
        if (!WaitSettled(s, slot, std::chrono::milliseconds{2})) {
            s.forced.fetch_add(1, std::memory_order_relaxed);
            force(page << PageBits);
            if (!WaitSettled(s, slot, std::chrono::milliseconds{50})) {
                // Never hang: count it and re-balance the slot (a report counted but lost, e.g.
                // its page remapped between Query and the report, would stall every read).
                s.timeouts.fetch_add(1, std::memory_order_relaxed);
                const u32 owed = Pending(s, slot);
                s.landed[slot].fetch_add(owed, std::memory_order_seq_cst);
            }
        }
        const u64 waited_ns = static_cast<u64>(NowNs() - start);
        s.wait_ns.fetch_add(waited_ns, std::memory_order_relaxed);
        u64 max = s.wait_max_ns.load(std::memory_order_relaxed);
        while (waited_ns > max &&
               !s.wait_max_ns.compare_exchange_weak(max, waited_ns, std::memory_order_relaxed)) {
        }
    }
}

/// GPU::OnCPURead's answer is kept by the reading core as its read area (reads inside it skip
/// OnCPURead until the read-trap generation moves). An area reaching over another watched page
/// would let that page's later reads skip the guard: the area is cut to the read's own pages
/// then (it is only that cache's key; the flush already covered the whole area).
template <typename Area>
inline void ClampArea(Area& area, u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    State& s = Get();
    if (s.watched_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const u64 read_begin = (address >> PageBits) << PageBits;
    const u64 read_end = (((address + size - 1) >> PageBits) + 1) << PageBits;
    if (area.start_address >= read_begin && area.end_address <= read_end) {
        return;
    }
    bool cut = area.end_address - area.start_address > (u64{1} << 20);
    for (u64 page = area.start_address >> PageBits;
         !cut && area.end_address != 0 && page <= ((area.end_address - 1) >> PageBits); ++page) {
        if ((page << PageBits) >= read_begin && (page << PageBits) < read_end) {
            continue;
        }
        cut = s.watched[SlotOf(page)].load(std::memory_order_relaxed) == page + 1;
    }
    if (cut) {
        area.start_address = (std::max)(static_cast<u64>(area.start_address), read_begin);
        area.end_address = (std::min)(static_cast<u64>(area.end_address), read_end);
    }
}

} // namespace VideoCore::IntelReportGuard
