// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <list>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    u32 GetGuestSyncpointValue(u32 id) const {
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    struct RegisteredAction {
        explicit RegisteredAction(u32 expected_value_, u64 id_, std::function<void()>&& action_)
            : expected_value{expected_value_}, id{id_}, action{std::move(action_)} {}
        u32 expected_value;
        u64 id;
        std::function<void()> action;
    };
    using ActionHandle = u64;

    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    /// Called after every guest increment, on the incrementing thread (nx2:gsv uses it to answer
    /// the driver's armed syncpoint waits at once instead of at its next 500 us poll). The hook
    /// must be cheap and thread-safe; nullptr removes it.
    using GuestIncrementHook = void (*)(void* context);
    void SetGuestIncrementHook(GuestIncrementHook hook, void* context) {
        guest_increment_context.store(context, std::memory_order_relaxed);
        guest_increment_hook.store(hook, std::memory_order_release);
    }

    /// Called with a syncpoint's new guest value on every guest increment, under the manager's
    /// lock, before the value is published and before the actions it meets run. Switch 2 clients
    /// read syncpoint values from a page the nx2:gsv session fills, and an action may wake a
    /// client that reads that page right away: the page must not be behind the value that
    /// answered its wait (nn::movie aborts on purpose with 2162-0001 when it is). The mirror
    /// runs on whichever thread increments: it must be cheap and take no other lock.
    using GuestMirror = std::function<void(u32 syncpoint_id, u32 value)>;
    /// Installs `mirror` for `owner` (replacing any other owner's).
    void SetGuestMirror(const void* owner, GuestMirror mirror);
    /// Removes the mirror if `owner` installed it. No mirror call runs after this returns.
    void ClearGuestMirror(const void* owner);

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    /// The hang report (FRITCHII_HANG_REPORT_S): every syncpoint something waits on, with its
    /// guest and host values, the values its registered actions wait for (nx2:syncpt's armed
    /// slot waits among them) and the threads blocked in WaitGuest/WaitHost. Read only; never
    /// blocks for long: says so when the lock stays held for 200 ms.
    [[nodiscard]] std::string DescribeWaits();

    // ---- INTEL_FORK_CPU_SYNC_DONE (env, read once, default 0 = off) ----------------------------
    // Fences stay Default for the GPU side (the host value is raised at once, every fast path on),
    // but the guest CPU's waits and reads of syncpoints (nvhost_ctrl events, nvcore's UpdateMin,
    // nx2:syncpt waits, the nx2:gsv value page, present fences) see a third value, "done": a
    // fenced increment (FenceManager::SignalSyncPoint) only counts once its fence was released
    // by the fencing thread (where delayed fences run their callbacks). 1: the fence as made
    // (stubbed when it carries no download, as with delayed fences); 2: the fence is made real
    // (waits for the host GPU's submission). A guest wait held for "done" longer than 100 ms falls
    // back to the raw value (counted), so "done" can never deadlock the guest against the GPU.
    // Off: every Cpu* call below is the raw call it replaces.
    static int CpuSyncDoneMode();
    static bool CpuSyncDone() {
        return CpuSyncDoneMode() != 0;
    }

    /// Fence side: before the guest increment of a fenced signal (CpuDoneBegin), and once its
    /// fence was released (CpuDoneEnd). Each Begin is matched by exactly one End.
    void CpuDoneBegin(u32 syncpoint_id);
    void CpuDoneEnd(u32 syncpoint_id);

    /// Guest CPU side: the guest value as the guest may see it.
    u32 GetCpuGuestSyncpointValue(u32 id) const {
        if (!CpuSyncDone()) {
            return GetGuestSyncpointValue(id);
        }
        return cpu_done[id].load(std::memory_order_acquire);
    }
    /// Guest CPU side: the host value as the guest may see it.
    u32 GetCpuHostSyncpointValue(u32 id) const {
        if (!CpuSyncDone()) {
            return GetHostSyncpointValue(id);
        }
        return (std::min)(GetHostSyncpointValue(id), cpu_done[id].load(std::memory_order_acquire));
    }
    bool IsReadyCpuGuest(u32 syncpoint_id, u32 expected_value) const {
        return GetCpuGuestSyncpointValue(syncpoint_id) >= expected_value;
    }

    template <typename Func>
    ActionHandle RegisterCpuGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!CpuSyncDone()) {
            return RegisterGuestAction(syncpoint_id, expected_value, std::move(action));
        }
        return RegisterCpuAction(syncpoint_id, expected_value, false, std::move(action));
    }
    template <typename Func>
    ActionHandle RegisterCpuHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!CpuSyncDone()) {
            return RegisterHostAction(syncpoint_id, expected_value, std::move(action));
        }
        return RegisterCpuAction(syncpoint_id, expected_value, true, std::move(action));
    }
    void DeregisterCpuGuestAction(u32 syncpoint_id, const ActionHandle& handle);
    void DeregisterCpuHostAction(u32 syncpoint_id, const ActionHandle& handle);
    /// Guest CPU side WaitHost: min(host, done), the raw host value after 100 ms held.
    void WaitCpuHost(u32 syncpoint_id, u32 expected_value);

    SyncpointManager();
    ~SyncpointManager();

private:
    struct CpuAction {
        u32 expected_value;
        u64 id;
        bool host; ///< Compared with min(host, done), else with done
        bool held; ///< The raw value reached it, "done" not yet
        std::chrono::steady_clock::time_point held_since;
        std::function<void()> action;
    };
    ActionHandle RegisterCpuAction(u32 syncpoint_id, u32 expected_value, bool host,
                                   std::function<void()>&& action);
    void DeregisterCpuAction(u32 syncpoint_id, const ActionHandle& handle, bool host);
    /// Under `guard`: raises done to (guest - outstanding), at least `floor_value`, mirrors it,
    /// then runs the CPU actions it (or the host value) meets.
    void CpuDoneUpdateLocked(u32 syncpoint_id, u32 floor_value);
    void RunCpuActionsLocked(u32 syncpoint_id, std::chrono::steady_clock::time_point now);
    void NoteCpuHeldLocked(std::chrono::steady_clock::duration held);
    void CpuWatchdog(std::stop_token token);
    static void KickGpuBack();

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> cpu_done{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> cpu_outstanding{};
    std::array<std::list<CpuAction>, NUM_MAX_SYNCPOINTS> cpu_action_storage; ///< Under `guard`
    std::array<u32, NUM_MAX_SYNCPOINTS> cpu_seen_done{};                      ///< Watchdog only
    std::array<std::chrono::steady_clock::time_point, NUM_MAX_SYNCPOINTS> cpu_seen_since{};
    std::condition_variable cpu_cv;
    struct CpuStats {
        u64 waits{};
        u64 held{};
        u64 held_ns{};
        u64 max_held_ns{};
        u64 fallbacks{};
        u64 stuck_fallbacks{};
    } cpu_stats; ///< Under `guard`

private:
    /// `timeline_name`: the Nx2Timeline marker of the increment (a literal), with the id.
    /// `guest`: a guest increment, mirrored first (SetGuestMirror) while a mirror is set.
    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   std::list<RegisteredAction>& action_storage, const char* timeline_name,
                   u32 syncpoint_id, bool guest = false);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(std::list<RegisteredAction>& action_storage, const ActionHandle& handle);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value,
              u32 syncpoint_id, bool guest);

    /// A thread blocked in Wait (under `guard`), for DescribeWaits.
    struct BlockedWait {
        const void* token;
        u32 syncpoint_id;
        u32 expected_value;
        bool guest;
        std::chrono::steady_clock::time_point since;
    };
    std::vector<BlockedWait> blocked_waits;

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    std::mutex guard;
    u64 last_action_id{};
    std::atomic<GuestIncrementHook> guest_increment_hook{};
    GuestMirror guest_mirror;               ///< Under `guard`
    const void* guest_mirror_owner{};       ///< Under `guard`
    std::atomic<bool> has_guest_mirror{};   ///< Whether guest_mirror is set (read without `guard`)
    std::atomic<void*> guest_increment_context{};
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
    /// INTEL_FORK_CPU_SYNC_DONE: fallbacks for held waits, the 10 s line. Last: stops first.
    std::jthread cpu_watchdog;
};

} // namespace Host1x

} // namespace Tegra
