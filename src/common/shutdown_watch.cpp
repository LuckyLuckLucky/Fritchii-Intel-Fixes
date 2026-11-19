// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_hitch.h"
#include "common/shutdown_progress.h"
#include "common/shutdown_watch.h"
#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::ShutdownWatch {

namespace {

using namespace std::chrono_literals;

constexpr size_t MaxSteps = 48;
constexpr u64 DumpAfterReportNs = 20'000'000'000ULL;

u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

int SecondsSwitch(const char* name, int fallback) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return (std::max)(std::atoi(value), 0);
}

int WatchSeconds() {
    static const int seconds = SecondsSwitch("FRITCHII_SHUTDOWN_WATCH_S", 5);
    return seconds;
}

int ExitSeconds() {
    static const int seconds = SecondsSwitch("FRITCHII_SHUTDOWN_EXIT_S", 0);
    return seconds;
}

struct StepEntry {
    const char* name{};
    u64 ns{};
};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool armed{};
    bool exiting{};
    bool thread_started{};
    u64 begin_ns{};
    u64 epoch{}; ///< bumps at every step
    unsigned long main_thread{};
    std::array<StepEntry, MaxSteps> steps{};
    size_t step_count{}; ///< all steps since Begin; the ring keeps the last MaxSteps
};

/// Never freed: the watch thread keeps running while the process's statics are destroyed, which
/// is one of the places it watches.
State& GetState() {
    static State* const state = new State;
    return *state;
}

const char* CurrentStep(const State& state) {
    return state.step_count == 0 ? "?" : state.steps[(state.step_count - 1) % MaxSteps].name;
}

void AddStep(State& state, const char* step) {
    state.steps[state.step_count % MaxSteps] = {step, Now()};
    ++state.step_count;
    ++state.epoch;
}

std::string StepList(const State& state) {
    std::string text;
    const size_t first = state.step_count > MaxSteps ? state.step_count - MaxSteps : 0;
    if (first != 0) {
        text = fmt::format("({} earlier) ", first);
    }
    for (size_t i = first; i < state.step_count; ++i) {
        const StepEntry& entry = state.steps[i % MaxSteps];
        text += fmt::format("{}{} +{:.2f} s", i == first ? "" : ", ", entry.name,
                            static_cast<double>(entry.ns - state.begin_ns) / 1e9);
    }
    return text;
}

[[noreturn]] void EndProcess() {
    Common::Log::Stop(); // flushes the log
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 2);
#endif
    std::_Exit(2);
}

void Run() {
    Common::SetCurrentThreadName("FritchiiShutdownWatch");
    State& state = GetState();
    u64 seen_epoch = ~u64{0};
    u64 seen_progress = 0;
    u64 quiet_since = 0;
    u64 reported_at = 0;
    bool reported = false;
    bool dumped = false;
    std::unique_lock lock{state.mutex};
    for (;;) {
        if (!state.armed) {
            state.cv.wait(lock, [&state] { return state.armed; });
            seen_epoch = ~u64{0};
        }
        state.cv.wait_for(lock, 250ms);
        if (!state.armed) {
            continue;
        }
        const u64 now = Now();
        const u64 progress = Common::ShutdownProgress();
        if (state.epoch != seen_epoch || progress != seen_progress) {
            if (reported && seen_epoch != ~u64{0}) {
                LOG_CRITICAL(Frontend,
                             "Shutdown watch: moving again after {:.1f} s (now in step '{}')",
                             static_cast<double>(now - quiet_since) / 1e9, CurrentStep(state));
            }
            seen_epoch = state.epoch;
            seen_progress = progress;
            quiet_since = now;
            reported = false;
            dumped = false;
            continue;
        }
        const u64 quiet = now - quiet_since;
        const int seconds = WatchSeconds();
        if (!reported && quiet >= static_cast<u64>(seconds) * 1'000'000'000ULL) {
            reported = true;
            reported_at = now;
            const std::string step = CurrentStep(state);
            const std::string steps = StepList(state);
            const double since_begin = static_cast<double>(now - state.begin_ns) / 1e9;
            const bool exiting = state.exiting;
            const unsigned long main_thread = state.main_thread;
            lock.unlock();
            LOG_CRITICAL(Frontend,
                         "Shutdown watch: no progress for {:.1f} s in step '{}' ({:.1f} s since "
                         "the stop began{}, teardown counter {}; FRITCHII_SHUTDOWN_WATCH_S={}, 0 "
                         "turns it off). One report per stuck step, read only",
                         static_cast<double>(quiet) / 1e9, step, since_begin,
                         exiting ? ", the frontend's close is done" : "", progress, seconds);
            LOG_CRITICAL(Frontend, "Shutdown watch: steps: {}", steps);
            Common::Nx2Hitch::LogThreadReport("Shutdown watch", true, main_thread);
            lock.lock();
            continue;
        }
        if (reported && !dumped && now - reported_at >= DumpAfterReportNs) {
            dumped = true;
            if (Common::Nx2Hitch::HangDumpSwitch()) {
                lock.unlock();
                LOG_CRITICAL(Frontend, "Shutdown watch: still stuck after {:.1f} s, minidump "
                                       "(FRITCHII_HANG_DUMP=1) {}",
                             static_cast<double>(now - quiet_since) / 1e9,
                             Common::Nx2Hitch::WriteHangDumpNow());
                lock.lock();
                continue;
            }
        }
        const int exit_seconds = ExitSeconds();
        if (reported && state.exiting && exit_seconds > 0 &&
            quiet >= static_cast<u64>(exit_seconds) * 1'000'000'000ULL) {
            LOG_CRITICAL(Frontend,
                         "Shutdown watch: process exit stuck for {:.1f} s in step '{}', ending "
                         "the process (FRITCHII_SHUTDOWN_EXIT_S={})",
                         static_cast<double>(quiet) / 1e9, CurrentStep(state), exit_seconds);
            EndProcess();
        }
    }
}

} // Anonymous namespace

void Begin(const char* step) {
    if (WatchSeconds() <= 0) {
        return;
    }
    State& state = GetState();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.armed) {
            state.armed = true;
            state.exiting = false;
            state.begin_ns = Now();
            state.step_count = 0;
#ifdef _WIN32
            state.main_thread = GetCurrentThreadId();
#endif
        }
        AddStep(state, step);
        if (!state.thread_started) {
            state.thread_started = true;
            // Detached: it runs until the process ends, through the statics' destruction.
            std::thread{Run}.detach();
        }
    }
    state.cv.notify_all();
}

void Step(const char* step) {
    if (WatchSeconds() <= 0) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.armed) {
        AddStep(state, step);
    }
}

void Exiting(const char* step) {
    if (WatchSeconds() <= 0) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.armed) {
        state.exiting = true;
        AddStep(state, step);
    }
}

void End() {
    if (WatchSeconds() <= 0) {
        return;
    }
    State& state = GetState();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.armed) {
            return;
        }
        state.armed = false;
        state.exiting = false;
        ++state.epoch;
    }
    state.cv.notify_all();
}

void EndProcessNow() {
    EndProcess();
}

void EndProcessAfter(std::chrono::seconds delay) {
    try {
        std::thread([delay] {
            std::this_thread::sleep_for(delay);
            // No log flush here: the log may be what is stuck.
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 2);
#endif
            std::_Exit(2);
        }).detach();
    } catch (const std::system_error&) {
        // No thread to be had: the caller goes on without the safety net.
    }
}

} // namespace Common::ShutdownWatch
