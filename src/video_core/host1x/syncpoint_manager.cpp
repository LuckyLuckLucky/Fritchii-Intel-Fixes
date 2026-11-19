// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/nx2_timeline.h"
#include "common/thread.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::scoped_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, ++last_action_id, std::move(action))->id;
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    if (handle == 0) {
        return;
    }
    std::scoped_lock lk(guard);

    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it->id == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        LOG_ERROR(HW_GPU, "Ignoring increment of invalid syncpoint {:#x}", syncpoint_id);
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id],
              "syncpoint: guest increment", syncpoint_id, true);
    if (const auto hook = guest_increment_hook.load(std::memory_order_acquire)) {
        hook(guest_increment_context.load(std::memory_order_relaxed));
    }
}

void SyncpointManager::SetGuestMirror(const void* owner, GuestMirror mirror) {
    std::scoped_lock lk(guard);
    guest_mirror = std::move(mirror);
    guest_mirror_owner = guest_mirror ? owner : nullptr;
    has_guest_mirror.store(static_cast<bool>(guest_mirror), std::memory_order_relaxed);
}

void SyncpointManager::ClearGuestMirror(const void* owner) {
    std::scoped_lock lk(guard);
    if (guest_mirror_owner != owner) {
        return;
    }
    guest_mirror = {};
    guest_mirror_owner = nullptr;
    has_guest_mirror.store(false, std::memory_order_relaxed);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id],
              "syncpoint: host increment", syncpoint_id);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value, syncpoint_id, true);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value, syncpoint_id, false);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage,
                                 const char* timeline_name, u32 syncpoint_id, bool guest) {
    std::unique_lock lk(guard, std::defer_lock);
    u32 new_value;
    const bool cpu_done_on = CpuSyncDone();
    if (guest && cpu_done_on) {
        // INTEL_FORK_CPU_SYNC_DONE: the page mirrors "done" (CpuDoneUpdateLocked), not this
        // value; under the lock so done is raised in order.
        lk.lock();
        new_value = syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else if (guest && has_guest_mirror.load(std::memory_order_relaxed)) {
        // Mirrored before the value is published: the lock-free ready checks (RegisterAction,
        // Wait, IsReadyGuest) must not see a value the client's page does not hold yet. Guest
        // increments take the lock first while a mirror is set, so they mirror in order.
        lk.lock();
        new_value = syncpoint.load(std::memory_order_relaxed) + 1;
        if (guest_mirror) {
            guest_mirror(syncpoint_id, new_value);
        }
        new_value = syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        new_value = syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    // Before the actions it runs: what ends the waits on this value (critical_path.py).
    if (Common::Nx2Timeline::Active()) {
        Common::Nx2Timeline::Instant(timeline_name, {"id", syncpoint_id}, {"value", new_value});
    }

    if (!lk.owns_lock()) {
        lk.lock();
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
    }
    wait_cv.notify_all();
    if (cpu_done_on) {
        if (guest) {
            CpuDoneUpdateLocked(syncpoint_id, 0);
        } else {
            RunCpuActionsLocked(syncpoint_id, std::chrono::steady_clock::now());
            cpu_cv.notify_all();
        }
    }
}

// ---- INTEL_FORK_CPU_SYNC_DONE ---------------------------------------------------------------

namespace {
constexpr auto CpuDoneFallbackAfter = std::chrono::milliseconds{100};
constexpr auto CpuDoneWatchPeriod = std::chrono::milliseconds{2};
} // namespace

int SyncpointManager::CpuSyncDoneMode() {
    static const int mode = [] {
        const char* value = std::getenv("INTEL_FORK_CPU_SYNC_DONE");
        if (value == nullptr || value[0] == '\0') {
            return 0;
        }
        return std::clamp(std::atoi(value), 0, 2);
    }();
    return mode;
}

SyncpointManager::SyncpointManager() {
    if (CpuSyncDone()) {
        LOG_INFO(HW_GPU,
                 "Intel fork CPU sync done (INTEL_FORK_CPU_SYNC_DONE={}): guest CPU syncpoint "
                 "waits complete once the fence was released{}; fallback to the host value after "
                 "{} ms held",
                 CpuSyncDoneMode(), CpuSyncDoneMode() == 2 ? " (fences made real)" : "",
                 CpuDoneFallbackAfter.count());
        cpu_watchdog = std::jthread([this](std::stop_token token) { CpuWatchdog(token); });
    }
}

SyncpointManager::~SyncpointManager() = default;

void SyncpointManager::KickGpuBack() {
    // A deferred submission (Scheduler::DeferFlush) is submitted by the back's idle hook.
    auto& split = VideoCommon::GpuSplit::Get();
    if (split.BackRunning()) {
        split.RequestIdleHook();
    }
}

void SyncpointManager::CpuDoneBegin(u32 syncpoint_id) {
    if (!CpuSyncDone() || syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    cpu_outstanding[syncpoint_id].fetch_add(1, std::memory_order_acq_rel);
}

void SyncpointManager::CpuDoneEnd(u32 syncpoint_id) {
    if (!CpuSyncDone() || syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    {
        std::scoped_lock lk(guard);
        cpu_outstanding[syncpoint_id].fetch_sub(1, std::memory_order_acq_rel);
        CpuDoneUpdateLocked(syncpoint_id, 0);
    }
    if (const auto hook = guest_increment_hook.load(std::memory_order_acquire)) {
        hook(guest_increment_context.load(std::memory_order_relaxed));
    }
}

void SyncpointManager::CpuDoneUpdateLocked(u32 syncpoint_id, u32 floor_value) {
    const u32 guest_value = syncpoints_guest[syncpoint_id].load(std::memory_order_acquire);
    const u32 outstanding = cpu_outstanding[syncpoint_id].load(std::memory_order_acquire);
    u32 target = guest_value >= outstanding ? guest_value - outstanding : 0;
    target = (std::max)(target, floor_value);
    if (target > cpu_done[syncpoint_id].load(std::memory_order_relaxed)) {
        // The client's page first (see SetGuestMirror), then the value, then its waits.
        if (guest_mirror) {
            guest_mirror(syncpoint_id, target);
        }
        cpu_done[syncpoint_id].store(target, std::memory_order_release);
    }
    RunCpuActionsLocked(syncpoint_id, std::chrono::steady_clock::now());
    cpu_cv.notify_all();
}

void SyncpointManager::NoteCpuHeldLocked(std::chrono::steady_clock::duration held) {
    const u64 ns = static_cast<u64>(
        (std::max)(std::chrono::duration_cast<std::chrono::nanoseconds>(held).count(),
                   std::chrono::nanoseconds::rep{0}));
    ++cpu_stats.held;
    cpu_stats.held_ns += ns;
    cpu_stats.max_held_ns = (std::max)(cpu_stats.max_held_ns, ns);
}

void SyncpointManager::RunCpuActionsLocked(u32 syncpoint_id,
                                           std::chrono::steady_clock::time_point now) {
    auto& actions = cpu_action_storage[syncpoint_id];
    if (actions.empty()) {
        return;
    }
    const u32 done = cpu_done[syncpoint_id].load(std::memory_order_acquire);
    const u32 host = syncpoints_host[syncpoint_id].load(std::memory_order_acquire);
    const u32 guest = syncpoints_guest[syncpoint_id].load(std::memory_order_acquire);
    for (auto it = actions.begin(); it != actions.end();) {
        const u32 value = it->host ? (std::min)(host, done) : done;
        if (value >= it->expected_value) {
            if (it->held) {
                NoteCpuHeldLocked(now - it->held_since);
            }
            it->action();
            it = actions.erase(it);
            continue;
        }
        const u32 raw = it->host ? host : guest;
        if (!it->held && raw >= it->expected_value) {
            it->held = true;
            it->held_since = now;
        }
        ++it;
    }
}

SyncpointManager::ActionHandle SyncpointManager::RegisterCpuAction(u32 syncpoint_id,
                                                                   u32 expected_value, bool host,
                                                                   std::function<void()>&& action) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        action();
        return {};
    }
    std::unique_lock lk(guard);
    ++cpu_stats.waits;
    const u32 done = cpu_done[syncpoint_id].load(std::memory_order_acquire);
    const u32 host_value = syncpoints_host[syncpoint_id].load(std::memory_order_acquire);
    const u32 value = host ? (std::min)(host_value, done) : done;
    if (value >= expected_value) {
        lk.unlock();
        action();
        return {};
    }
    const u32 raw =
        host ? host_value : syncpoints_guest[syncpoint_id].load(std::memory_order_acquire);
    const bool held = raw >= expected_value;
    const ActionHandle handle = ++last_action_id;
    cpu_action_storage[syncpoint_id].push_back(CpuAction{expected_value, handle, host, held,
                                                         std::chrono::steady_clock::now(),
                                                         std::move(action)});
    lk.unlock();
    if (held) {
        KickGpuBack();
    }
    return handle;
}

void SyncpointManager::DeregisterCpuAction(u32 syncpoint_id, const ActionHandle& handle,
                                           bool host) {
    if (handle == 0 || syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    std::scoped_lock lk(guard);
    auto& actions = cpu_action_storage[syncpoint_id];
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        if (it->id == handle && it->host == host) {
            actions.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterCpuGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!CpuSyncDone()) {
        DeregisterGuestAction(syncpoint_id, handle);
        return;
    }
    DeregisterCpuAction(syncpoint_id, handle, false);
}

void SyncpointManager::DeregisterCpuHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!CpuSyncDone()) {
        DeregisterHostAction(syncpoint_id, handle);
        return;
    }
    DeregisterCpuAction(syncpoint_id, handle, true);
}

void SyncpointManager::WaitCpuHost(u32 syncpoint_id, u32 expected_value) {
    if (!CpuSyncDone() || syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        WaitHost(syncpoint_id, expected_value);
        return;
    }
    const auto ready = [&] {
        return (std::min)(syncpoints_host[syncpoint_id].load(std::memory_order_acquire),
                          cpu_done[syncpoint_id].load(std::memory_order_acquire)) >=
               expected_value;
    };
    std::unique_lock lk(guard);
    ++cpu_stats.waits;
    if (ready()) {
        return;
    }
    const int token = 0;
    blocked_waits.push_back(
        {&token, syncpoint_id, expected_value, false, std::chrono::steady_clock::now()});
    bool held = false;
    std::chrono::steady_clock::time_point held_since{};
    while (!ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (!held &&
            syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value) {
            held = true;
            held_since = now;
            lk.unlock();
            KickGpuBack();
            lk.lock();
            continue;
        }
        if (held && now - held_since >= CpuDoneFallbackAfter) {
            ++cpu_stats.fallbacks;
            CpuDoneUpdateLocked(syncpoint_id, expected_value);
            break;
        }
        cpu_cv.wait_for(lk, CpuDoneWatchPeriod);
    }
    if (held) {
        NoteCpuHeldLocked(std::chrono::steady_clock::now() - held_since);
    }
    std::erase_if(blocked_waits, [&token](const BlockedWait& wait) { return wait.token == &token; });
}

void SyncpointManager::CpuWatchdog(std::stop_token token) {
    Common::SetCurrentThreadName("CpuSyncDoneWatch");
    auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    {
        std::scoped_lock lk(guard);
        cpu_seen_since.fill(std::chrono::steady_clock::now());
    }
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(CpuDoneWatchPeriod);
        const auto now = std::chrono::steady_clock::now();
        bool kick = false;
        std::unique_lock lk(guard);
        for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
            auto& actions = cpu_action_storage[id];
            if (!actions.empty()) {
                // Marks the waits the raw value reached since, fires those "done" reached.
                RunCpuActionsLocked(id, now);
                u32 floor_value = 0;
                u64 expired = 0;
                for (const CpuAction& action : actions) {
                    if (!action.held) {
                        continue;
                    }
                    kick = true;
                    if (now - action.held_since >= CpuDoneFallbackAfter) {
                        floor_value = (std::max)(floor_value, action.expected_value);
                        ++expired;
                    }
                }
                if (expired != 0) {
                    // Fallback to the raw value: "done" is raised to what the waits name.
                    cpu_stats.fallbacks += expired;
                    CpuDoneUpdateLocked(id, floor_value);
                }
            }
            // Waits the guest makes without a registration (it polls the nx2:gsv page): "done"
            // left behind the guest value and not moving for 100 ms catches up (counted).
            const u32 guest = syncpoints_guest[id].load(std::memory_order_acquire);
            const u32 done = cpu_done[id].load(std::memory_order_acquire);
            if (guest <= done || done != cpu_seen_done[id]) {
                cpu_seen_done[id] = done;
                cpu_seen_since[id] = now;
                continue;
            }
            if (now - cpu_seen_since[id] >= CpuDoneFallbackAfter) {
                ++cpu_stats.stuck_fallbacks;
                CpuDoneUpdateLocked(id, guest);
                cpu_seen_done[id] = cpu_done[id].load(std::memory_order_acquire);
                cpu_seen_since[id] = now;
            }
        }
        if (now >= next_log) {
            next_log = now + std::chrono::seconds{10};
            u64 lag = 0;
            for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
                const u32 guest = syncpoints_guest[id].load(std::memory_order_acquire);
                const u32 done = cpu_done[id].load(std::memory_order_acquire);
                lag += guest > done ? guest - done : 0;
            }
            const CpuStats stats = cpu_stats;
            cpu_stats = {};
            lk.unlock();
            LOG_INFO(HW_GPU,
                     "Intel fork CPU sync done (INTEL_FORK_CPU_SYNC_DONE={}) 10 s: guest waits {}, "
                     "of them held for the GPU {} ({:.1f} ms total, max {:.1f} ms), fallbacks to "
                     "the host value {} (must stay ~0), done lag now {} (fallbacks: {} held waits, "
                     "{} stalled syncpoints)",
                     CpuSyncDoneMode(), stats.waits, stats.held,
                     static_cast<double>(stats.held_ns) / 1e6,
                     static_cast<double>(stats.max_held_ns) / 1e6,
                     stats.fallbacks + stats.stuck_fallbacks, lag, stats.fallbacks,
                     stats.stuck_fallbacks);
        } else {
            lk.unlock();
        }
        if (kick) {
            KickGpuBack();
        }
    }
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value, u32 syncpoint_id, bool guest) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    if (pred()) {
        return;
    }
    // Only a wait that blocks is listed (for the hang report), under the lock it takes anyway.
    const int token = 0;
    blocked_waits.push_back(
        {&token, syncpoint_id, expected_value, guest, std::chrono::steady_clock::now()});
    wait_cv.wait(lk, pred);
    std::erase_if(blocked_waits, [&token](const BlockedWait& wait) { return wait.token == &token; });
}

std::string SyncpointManager::DescribeWaits() {
    std::unique_lock lk(guard, std::defer_lock);
    for (int attempt = 0; !lk.try_lock(); ++attempt) {
        if (attempt >= 200) {
            return "lock held for 200 ms (an increment or a registration in progress)";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto now = std::chrono::steady_clock::now();
    std::string text;
    size_t listed = 0;
    for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
        const auto& guest_actions = guest_action_storage[id];
        const auto& host_actions = host_action_storage[id];
        const bool blocked = std::ranges::any_of(
            blocked_waits, [id](const BlockedWait& wait) { return wait.syncpoint_id == id; });
        if (guest_actions.empty() && host_actions.empty() && !blocked) {
            continue;
        }
        if (++listed > 24) {
            text += "; ...";
            break;
        }
        std::string waits;
        const auto add_actions = [&waits](const char* side,
                                          const std::list<RegisteredAction>& actions) {
            if (actions.empty()) {
                return;
            }
            waits += fmt::format("{}{} actions at", waits.empty() ? "" : ", ", side);
            size_t shown = 0;
            for (const RegisteredAction& action : actions) {
                if (++shown > 6) {
                    waits += fmt::format(" ... ({} in all)", actions.size());
                    break;
                }
                waits += fmt::format(" {:#x}", action.expected_value);
            }
        };
        add_actions("guest", guest_actions);
        add_actions("host", host_actions);
        for (const BlockedWait& wait : blocked_waits) {
            if (wait.syncpoint_id == id) {
                waits += fmt::format("{}a thread blocked in Wait{} for {:#x} ({:.1f} s)",
                                     waits.empty() ? "" : ", ", wait.guest ? "Guest" : "Host",
                                     wait.expected_value,
                                     std::chrono::duration<double>(now - wait.since).count());
            }
        }
        text += fmt::format("{}syncpoint {} guest {:#x} host {:#x}: {}", text.empty() ? "" : "; ",
                            id, syncpoints_guest[id].load(std::memory_order_relaxed),
                            syncpoints_host[id].load(std::memory_order_relaxed), waits);
    }
    return text.empty() ? std::string{"none"} : text;
}

} // namespace Host1x

} // namespace Tegra
