// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/thread_worker.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"

namespace Vulkan {

/// Intel fork (2026-11-22, lead 2 "priority lane"): INTEL_FORK_BUILD_PRIORITY.
/// 0 = old behaviour (a draw waits for its build in the builders' FIFO).
/// 1 = a draw that waits for an unbuilt pipeline moves its build to the front of the builder queue.
/// 2 = as 1, and the waiting thread also runs a still-queued worker-only build itself.
/// The wait statistics line ("Intel fork build waits") is printed in every mode.
inline int BuildPriorityMode() {
    static const int mode = [] {
        const char* const v = std::getenv("INTEL_FORK_BUILD_PRIORITY");
        if (v == nullptr || v[0] < '0' || v[0] > '2') {
            return 0;
        }
        return v[0] - '0';
    }();
    return mode;
}

inline s64 BuildClockNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Per-window statistics of draws held for a pipeline build: was the build still queued or
/// already running in the driver when the draw had to wait, how long it sat in the queue and how
/// long it took to build.
class BuildWaitStats {
public:
    static BuildWaitStats& Get() {
        static BuildWaitStats stats;
        return stats;
    }

    void Note(bool was_queued, bool ran_inline, s64 held_ns, s64 queue_ns, s64 build_ns) {
        std::scoped_lock lock{mutex};
        ++(was_queued ? queued : running);
        (was_queued ? queued_held_ns : running_held_ns) += held_ns;
        inline_runs += ran_inline ? 1 : 0;
        queue_total_ns += queue_ns > 0 ? queue_ns : 0;
        build_total_ns += build_ns > 0 ? build_ns : 0;
        max_held_ns = std::max(max_held_ns, held_ns);
        const s64 now = BuildClockNs();
        if (window_start == 0) {
            window_start = now;
        }
        if (now - window_start < 10'000'000'000LL) {
            return;
        }
        const u64 n = queued + running;
        LOG_INFO(Render_Vulkan,
                 "Intel fork build waits ({:.1f} s, mode {}): {} held draws | still queued {} "
                 "({:.0f} ms held) | already building {} ({:.0f} ms held) | run by the waiter {} | "
                 "avg queue {:.1f} ms, avg build {:.1f} ms, worst hold {:.1f} ms",
                 (now - window_start) / 1e9, BuildPriorityMode(), n, queued,
                 queued_held_ns / 1e6, running, running_held_ns / 1e6, inline_runs,
                 queue_total_ns / 1e6 / static_cast<double>(n),
                 build_total_ns / 1e6 / static_cast<double>(n), max_held_ns / 1e6);
        queued = running = inline_runs = 0;
        queued_held_ns = running_held_ns = queue_total_ns = build_total_ns = max_held_ns = 0;
        window_start = now;
    }

private:
    std::mutex mutex;
    s64 window_start{};
    u64 queued{};
    u64 running{};
    u64 inline_runs{};
    s64 queued_held_ns{};
    s64 running_held_ns{};
    s64 queue_total_ns{};
    s64 build_total_ns{};
    s64 max_held_ns{};
};

class EmissionBudget {
public:
    class Reservation {
    public:
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        ~Reservation() {
            if (pending) {
                pending->fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        friend class EmissionBudget;
        Reservation() = default;
        std::shared_ptr<std::atomic<size_t>> pending;
    };

    explicit EmissionBudget(size_t capacity_) : capacity{capacity_} {}

    std::unique_ptr<Reservation> TryAcquire(bool enabled, bool native) {
        size_t count = pending->load(std::memory_order_relaxed);
        if (!DefersShaderEmission(enabled, native, count, capacity)) {
            return nullptr;
        }
        auto reservation = std::unique_ptr<Reservation>{new Reservation};
        while (DefersShaderEmission(enabled, native, count, capacity)) {
            if (pending->compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
                reservation->pending = pending;
                return reservation;
            }
        }
        return nullptr;
    }

    size_t Pending() const {
        return pending->load(std::memory_order_relaxed);
    }

private:
    const size_t capacity;
    std::shared_ptr<std::atomic<size_t>> pending = std::make_shared<std::atomic<size_t>>(0);
};

class PipelineBuild {
    struct State {
        explicit State(std::function<void()> task_) : task{std::move(task_)} {}

        void Run() noexcept {
            if (claimed.exchange(true)) {
                return;
            }
            start_ns.store(BuildClockNs(), std::memory_order_relaxed);
            try {
                task();
            } catch (const std::exception& exception) {
                failed = true;
                LOG_ERROR(Render_Vulkan, "Graphics pipeline worker failed: {}", exception.what());
            } catch (...) {
                failed = true;
                LOG_ERROR(Render_Vulkan,
                          "Graphics pipeline worker failed with an unknown exception");
            }
            task = {};
            end_ns.store(BuildClockNs(), std::memory_order_relaxed);
            std::scoped_lock lock{mutex};
            complete.store(true, std::memory_order_release);
            condition.notify_all();
        }

        std::function<void()> task;
        bool failed{};
        std::atomic_bool claimed{};
        std::atomic_bool complete{};
        std::atomic_bool promoted{};
        Common::ThreadWorker* workers{};
        s64 queued_ns{};
        std::atomic<s64> start_ns{};
        std::atomic<s64> end_ns{};
        std::mutex mutex;
        std::condition_variable condition;
    };

public:
    PipelineBuild(Common::ThreadWorker* workers, std::function<void()> task)
        : state{std::make_shared<State>(std::move(task))} {
        state->workers = workers;
        state->queued_ns = BuildClockNs();
        if (workers) {
            workers->QueueWork([state = state] { state->Run(); });
        } else {
            state->Run();
        }
    }

    ~PipelineBuild() {
        Wait(true);
    }

    bool IsBuilt() const {
        return state->complete.load(std::memory_order_acquire);
    }

    bool Failed() const {
        return IsBuilt() && state->failed;
    }

    void Wait(bool may_build = false) const {
        const auto shared = state;
        if (may_build) {
            shared->Run();
        }
        std::unique_lock lock{shared->mutex};
        shared->condition.wait(lock,
                               [&] { return shared->complete.load(std::memory_order_acquire); });
    }

    /// INTEL_FORK_BUILD_PRIORITY >= 1: a draw needs this build now; if no builder has taken it
    /// yet, queue it once more at the front (the FIFO entry then finds it claimed and returns).
    void Promote() const {
        const auto& shared = state;
        if (BuildPriorityMode() == 0 || shared->workers == nullptr ||
            shared->claimed.load(std::memory_order_relaxed) ||
            shared->promoted.exchange(true)) {
            return;
        }
        shared->workers->QueueUrgent([shared] { shared->Run(); });
    }

    /// The wait of a draw that cannot go on without this pipeline: promotes the build, waits
    /// (mode 2 also runs a still-queued worker-only build here) and counts the wait.
    void WaitForDraw(bool may_build) const {
        const auto shared = state;
        const s64 begin = BuildClockNs();
        const bool was_queued = !shared->claimed.load(std::memory_order_relaxed);
        if (was_queued && !may_build && BuildPriorityMode() >= 2) {
            may_build = true;
        }
        bool ran_inline = false;
        if (was_queued) {
            if (may_build) {
                ran_inline = !shared->claimed.load(std::memory_order_relaxed);
                shared->Run();
            } else {
                Promote();
            }
        }
        Wait(false);
        const s64 end = BuildClockNs();
        const s64 start = shared->start_ns.load(std::memory_order_relaxed);
        const s64 finish = shared->end_ns.load(std::memory_order_relaxed);
        BuildWaitStats::Get().Note(was_queued, ran_inline, end - begin, start - shared->queued_ns,
                                   finish - start);
    }

    /// Waits at most `timeout` for the build, never running it on this thread (one build can
    /// take seconds). True once built.
    bool WaitFor(std::chrono::nanoseconds timeout) const {
        const auto shared = state;
        std::unique_lock lock{shared->mutex};
        return shared->condition.wait_for(
            lock, timeout, [&] { return shared->complete.load(std::memory_order_acquire); });
    }

    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;

private:
    std::shared_ptr<State> state;
};

} // namespace Vulkan
