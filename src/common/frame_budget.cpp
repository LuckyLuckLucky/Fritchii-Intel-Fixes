// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/lowend_sim.h"
#include "common/frame_budget.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#define FRITCHII_BUDGET_CYCLES 1
#endif
#elif defined(__linux__)
#include <pthread.h>
#include <time.h>
#define FRITCHII_BUDGET_CLOCKID 1
#endif

namespace Common::FrameBudget {
namespace {

constexpr size_t MaxThreads = 32;

struct Entry {
    bool used{};
    bool fresh{}; ///< registered since the last Take: its first reading is only a baseline
    Group group{};
    u64 last{};
#ifdef _WIN32
    HANDLE handle{};
#elif defined(FRITCHII_BUDGET_CLOCKID)
    clockid_t clock{};
#endif
};

std::mutex g_mutex;
std::array<Entry, MaxThreads> g_threads;

struct Window {
    bool started{};
    std::chrono::steady_clock::time_point wall{};
    u64 gpu_ns{};
    u64 process_ns{};
#ifdef FRITCHII_BUDGET_CYCLES
    u64 tsc{};
#endif
};
Window g_window;
#ifdef FRITCHII_BUDGET_CYCLES
/// Thread cycle counts follow the TSC on x64 Windows; if a share ever comes out well over 1 they
/// don't on this machine, and the thread times are used instead (15.6 ms steps, fine over 1 s).
bool g_cycles_bad{};
#endif

bool GroupOf(std::string_view name, Group& group) {
    if (name == "GPU") {
        group = Group::Front;
    } else if (name == "GPUBack" || name == "GPUBackE2") {
        group = Group::Back;
    } else if (name == "GPURender") {
        group = Group::Render;
    } else if (name.starts_with("CPUCore") || name == "CPUThread") {
        group = Group::Game;
    } else {
        return false;
    }
    return true;
}

#ifdef _WIN32
u64 FileTimeValue(const FILETIME& time) {
    return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}
#endif

/// The thread's CPU time (cycles or ns, see Take), false once it has exited.
bool ReadThread(const Entry& entry, u64& value) {
#ifdef _WIN32
    if (WaitForSingleObject(entry.handle, 0) == WAIT_OBJECT_0) {
        return false;
    }
#ifdef FRITCHII_BUDGET_CYCLES
    if (!g_cycles_bad) {
        ULONG64 cycles{};
        if (!QueryThreadCycleTime(entry.handle, &cycles)) {
            return false;
        }
        value = cycles;
        return true;
    }
#endif
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(entry.handle, &created, &exited, &kernel, &user)) {
        return false;
    }
    value = (FileTimeValue(kernel) + FileTimeValue(user)) * 100;
    return true;
#elif defined(FRITCHII_BUDGET_CLOCKID)
    timespec time{};
    if (clock_gettime(entry.clock, &time) != 0) {
        return false;
    }
    value = static_cast<u64>(time.tv_sec) * 1'000'000'000ULL + static_cast<u64>(time.tv_nsec);
    return true;
#else
    static_cast<void>(entry);
    static_cast<void>(value);
    return false;
#endif
}

void Release(Entry& entry) {
#ifdef _WIN32
    CloseHandle(entry.handle);
    entry.handle = nullptr;
#endif
    entry.used = false;
}

u64 ProcessCpuNs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return (FileTimeValue(kernel) + FileTimeValue(user)) * 100;
    }
    return 0;
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) == 0) {
        return static_cast<u64>(time.tv_sec) * 1'000'000'000ULL + static_cast<u64>(time.tv_nsec);
    }
    return 0;
#else
    return 0;
#endif
}

void StartWindow(Window& window) {
    window.started = true;
    window.wall = std::chrono::steady_clock::now();
    window.gpu_ns = Detail::g_gpu_busy_ns.load(std::memory_order_relaxed);
    window.process_ns = ProcessCpuNs();
#ifdef FRITCHII_BUDGET_CYCLES
    window.tsc = __rdtsc();
#endif
}

} // Anonymous namespace

void RegisterThread(const char* name) {
    Group group{};
    if (!GroupOf(name, group)) {
        return;
    }
    Entry entry{.used = true, .fresh = true, .group = group};
#ifdef _WIN32
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &entry.handle, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                         0)) {
        return;
    }
#elif defined(FRITCHII_BUDGET_CLOCKID)
    if (pthread_getcpuclockid(pthread_self(), &entry.clock) != 0) {
        return;
    }
#endif
    std::scoped_lock lock{g_mutex};
    for (Entry& slot : g_threads) {
        u64 ignored{};
        if (slot.used && !ReadThread(slot, ignored)) {
            // A thread of an earlier game session.
            Release(slot);
        }
        if (!slot.used) {
            slot = entry;
            return;
        }
    }
#ifdef _WIN32
    CloseHandle(entry.handle);
#endif
}

void Reset() {
    std::scoped_lock lock{g_mutex};
    g_window.started = false;
}

Sample Take() {
    std::scoped_lock lock{g_mutex};
    Sample sample;
    const u8 gpu_state = Detail::g_gpu_state.load(std::memory_order_relaxed);
    sample.gpu_unsupported = gpu_state == 2;

    Window now;
    StartWindow(now);
    const bool had_window = g_window.started;
    const Window last = g_window;
    g_window = now;

    const double wall_ns =
        std::chrono::duration<double, std::nano>(now.wall - last.wall).count();
    double thread_scale = wall_ns; // thread readings per wall time
#ifdef FRITCHII_BUDGET_CYCLES
    if (!g_cycles_bad) {
        thread_scale = static_cast<double>(now.tsc - last.tsc);
    }
#endif
    const bool window_ok = had_window && wall_ns > 1e8; // at least 0.1 s

    double max_share = 0;
    for (Entry& entry : g_threads) {
        if (!entry.used) {
            continue;
        }
        u64 value{};
        if (!ReadThread(entry, value)) {
            Release(entry);
            continue;
        }
        const u64 previous = entry.last;
        const bool fresh = entry.fresh;
        entry.last = value;
        entry.fresh = false;
        if (fresh || !window_ok || value < previous || thread_scale <= 0) {
            continue;
        }
        const double share = static_cast<double>(value - previous) / thread_scale;
        max_share = (std::max)(max_share, share);
        double& group = sample.busy[static_cast<size_t>(entry.group)];
        group = (std::max)(group, share);
    }
#ifdef FRITCHII_BUDGET_CYCLES
    if (max_share > 1.05 && !g_cycles_bad) {
        // Cycle counts don't follow the TSC here: thread times from the next window on.
        g_cycles_bad = true;
        for (Entry& entry : g_threads) {
            entry.fresh = true;
        }
        sample.busy.fill(-1);
    }
#endif
    for (double& share : sample.busy) {
        share = share < 0 ? share : (std::min)(share, 1.0);
    }

    if (!window_ok) {
        return sample;
    }
    sample.valid = true;
    sample.seconds = wall_ns / 1e9;
    if (gpu_state == 1 && now.gpu_ns >= last.gpu_ns) {
        sample.gpu = (std::min)(static_cast<double>(now.gpu_ns - last.gpu_ns) / wall_ns, 1.0);
    }
    const unsigned cpus = Common::LowEndSim::LogicalProcessors();
    if (now.process_ns != 0 && now.process_ns >= last.process_ns && cpus != 0) {
        sample.cpu = (std::min)(
            static_cast<double>(now.process_ns - last.process_ns) / (wall_ns * cpus), 1.0);
    }
    return sample;
}

} // namespace Common::FrameBudget
