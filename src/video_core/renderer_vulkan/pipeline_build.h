// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
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

/// Intel fork (2026-11-22): INTEL_FORK_ASYNC_MODE (read once), Lichii build-386's draw skip.
/// off (unset) = as before: asynchronous shaders off on Intel, every draw waits.
/// dry = every draw waits as before; each lookup of an unbuilt pipeline is classified with
///       Lichii's rule and the held time counted by class ("Intel fork Lichii skip dry").
/// lichii = asynchronous shaders on; Lichii's native_async_draws = Skip rule only.
/// lichii-bounded = as lichii, but a draw Lichii may skip first waits what is left of a per-frame
///       budget (INTEL_FORK_ASYNC_BUDGET_MS, default 8, Lichii's native_pipeline_wait_ms).
enum class IntelForkAsyncMode : int { Off, Dry, Lichii, LichiiBounded };

inline IntelForkAsyncMode IntelForkAsyncModeSetting() {
    static const IntelForkAsyncMode mode = [] {
        const char* const v = std::getenv("INTEL_FORK_ASYNC_MODE");
        // Default (IntelFix10, 2026-11-23, T2/T2b/T2c): lichii. "off" turns it off.
        if (v == nullptr || v[0] == '\0') {
            return IntelForkAsyncMode::Lichii;
        }
        const std::string_view value{v};
        if (value == "dry") {
            return IntelForkAsyncMode::Dry;
        }
        if (value == "lichii") {
            return IntelForkAsyncMode::Lichii;
        }
        if (value == "lichii-bounded") {
            return IntelForkAsyncMode::LichiiBounded;
        }
        return IntelForkAsyncMode::Off;
    }();
    return mode;
}

inline const char* IntelForkAsyncModeName(IntelForkAsyncMode mode) {
    switch (mode) {
    case IntelForkAsyncMode::Dry:
        return "dry";
    case IntelForkAsyncMode::Lichii:
        return "lichii";
    case IntelForkAsyncMode::LichiiBounded:
        return "lichii-bounded";
    default:
        return "off";
    }
}

/// INTEL_FORK_ASYNC_BUDGET_MS (read once, default 8): lichii-bounded's wait budget per frame, in us.
inline s64 IntelForkAsyncBudgetUs() {
    static const s64 budget = [] {
        const char* const v = std::getenv("INTEL_FORK_ASYNC_BUDGET_MS");
        const int ms = v != nullptr && v[0] != '\0' ? std::atoi(v) : 8;
        return static_cast<s64>(ms > 0 ? ms : 0) * 1000;
    }();
    return budget;
}

/// INTEL_FORK_ASYNC_MESH_SKIP (read once, default 0), with INTEL_FORK_ASYNC_MODE: 0 = mesh draws wait
/// (Lichii's default); 1 = a mesh draw is judged by the same rule as other draws (depth-tested,
/// colour only into targets written in an earlier frame); 2 = Lichii's skip_unbuilt_mesh_draws
/// (every unbuilt native mesh draw is skipped).
inline int IntelForkAsyncMeshSkip() {
    static const int mode = [] {
        const char* const v = std::getenv("INTEL_FORK_ASYNC_MESH_SKIP");
        // Default (IntelFix10, 2026-11-23, T2): 1, mesh draws follow the same rule as others.
        const int m = v != nullptr && v[0] != '\0' ? std::atoi(v) : 1;
        return m >= 0 && m <= 2 ? m : 1;
    }();
    return mode;
}

/// INTEL_FORK_ASYNC_MODE: hands the class of the draw just looked up to the next
/// PipelineBuild::WaitFor on this thread (PipelineCache passes it through
/// GraphicsPipeline::WaitBuiltFor with a zero timeout, so no shader-stamped file changes).
/// 0 = nothing to hand over, else 1 << LichiiDrawClass.
inline thread_local u32 t_lichii_attach = 0;

/// INTEL_FORK_ASYNC_MODE counts, one line per 10 s window. Lookups are noted on the thread that
/// looks pipelines up (GPU thread), holds where the draw waits (the recording thread, WaitForDraw).
class LichiiSkipStats {
public:
    enum class Lookup { Waited, Skipped, BoundedBuilt };

    static LichiiSkipStats& Get() {
        static LichiiSkipStats stats;
        return stats;
    }

    void SetMode(IntelForkAsyncMode value) {
        mode.store(static_cast<int>(value), std::memory_order_relaxed);
    }

    bool Enabled() const {
        return mode.load(std::memory_order_relaxed) != 0;
    }

    /// A lookup of an unbuilt pipeline: its class and what was done with it.
    void NoteLookup(LichiiDrawClass verdict, Lookup what) {
        std::scoped_lock lock{mutex};
        ++lookups[static_cast<size_t>(verdict)];
        if (what == Lookup::Skipped) {
            ++skipped;
            ++session_skipped;
        } else if (what == Lookup::BoundedBuilt) {
            ++bounded_built;
        }
        MaybeLog();
    }

    /// lichii-bounded: one wait within the frame's budget.
    void NoteBoundedWait(s64 used_us) {
        std::scoped_lock lock{mutex};
        ++bounded_waits;
        bounded_us += used_us;
    }

    /// A draw held for its pipeline (WaitForDraw), by the class its pipeline's lookups gave.
    void NoteHeld(LichiiDrawClass verdict, s64 held_ns) {
        std::scoped_lock lock{mutex};
        ++holds[static_cast<size_t>(verdict)];
        held_ns_by[static_cast<size_t>(verdict)] += held_ns > 0 ? held_ns : 0;
        MaybeLog();
    }

private:
    void MaybeLog() {
        const s64 now = BuildClockNs();
        if (window_start == 0) {
            window_start = now;
        }
        if (now - window_start < 10'000'000'000LL) {
            return;
        }
        using C = LichiiDrawClass;
        const auto n = [&](C c) { return lookups[static_cast<size_t>(c)]; };
        const auto ms = [&](C c) { return held_ns_by[static_cast<size_t>(c)] / 1e6; };
        u64 total = 0;
        for (const u64 count : lookups) {
            total += count;
        }
        const u64 wait_lookups = n(C::NoDepthTest) + n(C::Mesh) + n(C::NoColour) +
                                 n(C::NewTarget) + n(C::Other);
        const double wait_ms =
            ms(C::NoDepthTest) + ms(C::Mesh) + ms(C::NoColour) + ms(C::NewTarget) + ms(C::Other);
        const double seconds = (now - window_start) / 1e9;
        const auto current = static_cast<IntelForkAsyncMode>(mode.load(std::memory_order_relaxed));
        if (current == IntelForkAsyncMode::Dry) {
            LOG_INFO(Render_Vulkan,
                     "Intel fork Lichii skip dry ({:.1f} s): {} unbuilt draws | would skip {} "
                     "({:.0f} ms held) | would wait {} ({:.0f} ms held): no depth test {}, mesh "
                     "{}, no colour {}, new target {}, other {} | held ms by reason: no depth "
                     "test {:.0f}, mesh {:.0f}, no colour {:.0f}, new target {:.0f}, other {:.0f} "
                     "| holds no lookup classified {} ({:.0f} ms)",
                     seconds, total, n(C::Skip), ms(C::Skip), wait_lookups, wait_ms,
                     n(C::NoDepthTest), n(C::Mesh), n(C::NoColour), n(C::NewTarget), n(C::Other),
                     ms(C::NoDepthTest), ms(C::Mesh), ms(C::NoColour), ms(C::NewTarget),
                     ms(C::Other), holds[static_cast<size_t>(C::Unclassified)],
                     ms(C::Unclassified));
        } else {
            LOG_INFO(Render_Vulkan,
                     "Intel fork Lichii skip {} ({:.1f} s): {} unbuilt draws | skipped {} | built "
                     "within the budget {} ({} waits, {:.1f} ms) | waited {} ({:.0f} ms held): no "
                     "depth test {}, mesh {}, no colour {}, new target {}, other {} | held ms by "
                     "reason: no depth test {:.0f}, mesh {:.0f}, no colour {:.0f}, new target "
                     "{:.0f}, other {:.0f} | holds of skippable-only pipelines {} ({:.0f} ms), "
                     "no lookup classified {} ({:.0f} ms) | {} skipped this session",
                     IntelForkAsyncModeName(current), seconds, total, skipped, bounded_built,
                     bounded_waits, bounded_us / 1e3, wait_lookups, wait_ms, n(C::NoDepthTest),
                     n(C::Mesh), n(C::NoColour), n(C::NewTarget), n(C::Other), ms(C::NoDepthTest),
                     ms(C::Mesh), ms(C::NoColour), ms(C::NewTarget), ms(C::Other),
                     holds[static_cast<size_t>(C::Skip)], ms(C::Skip),
                     holds[static_cast<size_t>(C::Unclassified)], ms(C::Unclassified),
                     session_skipped);
        }
        lookups = {};
        holds = {};
        held_ns_by = {};
        skipped = bounded_built = bounded_waits = 0;
        bounded_us = 0;
        window_start = now;
    }

    std::atomic<int> mode{};
    std::mutex mutex;
    s64 window_start{};
    std::array<u64, LichiiDrawClassCount> lookups{};
    std::array<u64, LichiiDrawClassCount> holds{};
    std::array<s64, LichiiDrawClassCount> held_ns_by{};
    u64 skipped{};
    u64 bounded_built{};
    u64 bounded_waits{};
    s64 bounded_us{};
    u64 session_skipped{};
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
        /// INTEL_FORK_ASYNC_MODE: the classes (bits, 1 << LichiiDrawClass) of the lookups that
        /// found this build unfinished, and the first wait reason among them (class + 1, 0 = none).
        std::atomic<u32> lichii_classes{};
        std::atomic<u32> lichii_first_wait{};
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
        if (auto& lichii = LichiiSkipStats::Get(); lichii.Enabled()) [[unlikely]] {
            // Held for a wait-class lookup if any lookup of this build was one (Lichii would wait
            // there too); "skip" only when every lookup that found it unbuilt was skippable.
            const u32 first_wait = shared->lichii_first_wait.load(std::memory_order_relaxed);
            const u32 classes = shared->lichii_classes.load(std::memory_order_relaxed);
            const LichiiDrawClass verdict =
                first_wait != 0 ? static_cast<LichiiDrawClass>(first_wait - 1)
                : (classes & 1u) != 0 ? LichiiDrawClass::Skip
                                      : LichiiDrawClass::Unclassified;
            lichii.NoteHeld(verdict, end - begin);
        }
    }

    /// Waits at most `timeout` for the build, never running it on this thread (one build can
    /// take seconds). True once built.
    bool WaitFor(std::chrono::nanoseconds timeout) const {
        const auto shared = state;
        if (const u32 attach = t_lichii_attach; attach != 0) [[unlikely]] {
            // INTEL_FORK_ASYNC_MODE: a lookup hands over its draw's class (see t_lichii_attach).
            t_lichii_attach = 0;
            shared->lichii_classes.fetch_or(attach, std::memory_order_relaxed);
            if (const u32 wait_bits = attach & ~1u; wait_bits != 0) {
                u32 none = 0;
                shared->lichii_first_wait.compare_exchange_strong(
                    none, static_cast<u32>(std::countr_zero(wait_bits)) + 1,
                    std::memory_order_relaxed);
            }
            if (timeout.count() <= 0) {
                return shared->complete.load(std::memory_order_acquire);
            }
        }
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
