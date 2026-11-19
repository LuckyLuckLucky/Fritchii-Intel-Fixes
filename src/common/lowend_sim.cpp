// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/lowend_sim.h"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace Common::LowEndSim {

namespace {

u64 g_cpu_mask = 0;
std::vector<std::string> g_boot_notes;

[[nodiscard]] const char* SwitchText(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0' ? value : nullptr;
}

#ifdef _WIN32

/// The physical cores of processor group 0 as masks of their logical processors, lowest first.
std::vector<u64> CoreMasks() {
    std::vector<u64> cores;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<u8> buffer(length);
    auto* const first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (length == 0 || !GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) {
        return cores;
    }
    for (DWORD offset = 0; offset < length;) {
        const auto* const entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        if (entry->Processor.GroupCount > 0 && entry->Processor.GroupMask[0].Group == 0) {
            cores.push_back(static_cast<u64>(entry->Processor.GroupMask[0].Mask));
        }
        offset += entry->Size;
    }
    std::ranges::sort(cores);
    return cores;
}

/// FRITCHII_SIM_CPU=<cores>/<threads> (6/12, 4/8, 6/6; "6" = with SMT where the CPU has it). The
/// last cores: core 0 takes most of Windows' interrupts, and what is left stays free for the rest
/// of the PC. At least one core always stays out, so a test can't take the whole machine.
void ApplyCpuSet() {
    const char* const text = SwitchText("FRITCHII_SIM_CPU");
    if (text == nullptr) {
        return;
    }
    char* end = nullptr;
    const long cores = std::strtol(text, &end, 10);
    long threads = end != nullptr && *end == '/' ? std::strtol(end + 1, nullptr, 10) : 0;
    const std::vector<u64> all = CoreMasks();
    if (cores <= 0 || all.empty() || static_cast<size_t>(cores) >= all.size()) {
        g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={} refused: this PC has {} cores and "
                                           "at least one stays free",
                                           text, all.size()));
        return;
    }
    const bool smt = std::popcount(all.back()) > 1;
    if (threads <= 0) {
        threads = smt ? cores * 2 : cores;
    }
    threads = std::clamp(threads, cores, smt ? cores * 2 : cores);
    long extra = threads - cores;
    u64 mask = 0;
    for (long index = 0; index < cores; ++index) {
        u64 core = all[all.size() - 1 - static_cast<size_t>(index)];
        const u64 first = core & (~core + 1);
        mask |= first;
        core &= ~first;
        if (extra > 0 && core != 0) {
            mask |= core & (~core + 1);
            --extra;
        }
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) {
        g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={}: SetProcessAffinityMask failed "
                                           "(error {})",
                                           text, GetLastError()));
        return;
    }
    g_cpu_mask = mask;
    // The preparation's threads follow the simulated CPU, by its switch: nx2_precompile.cpp is one
    // of the files the Preparing shaders stamp hashes.
    if (std::getenv("EDEN_NX2_PRECOMPILE_THREADS") == nullptr) {
        const int count = std::clamp(std::popcount(mask) / 4, 1, 6);
        _putenv_s("EDEN_NX2_PRECOMPILE_THREADS", std::to_string(count).c_str());
    }
    g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={}: {} cores, {} threads, processors "
                                       "{:#x} of this PC's {} cores",
                                       text, cores, threads, mask, all.size()));
}

/// FRITCHII_SIM_RAM_LIMIT_MB=<MiB>: a job object holding this process alone. It closes with the
/// process. No kill-on-close: the limit never ends the game by itself. A job's CPU rate cap is not
/// offered: measured here it stops every thread for about 300 ms at a time, nothing like a slower
/// CPU (FRITCHII_SIM_CPU_SLOW is the speed knob).
void ApplyJob() {
    const char* const ram_text = SwitchText("FRITCHII_SIM_RAM_LIMIT_MB");
    if (ram_text == nullptr) {
        return;
    }
    const u64 ram_mib = std::strtoull(ram_text, nullptr, 10);
    // Under 4 GiB the game can't even boot: that would test nothing.
    if (ram_mib < 4096) {
        g_boot_notes.push_back(
            fmt::format("FRITCHII_SIM_RAM_LIMIT_MB={} ignored (at least 4096)", ram_text));
        return;
    }
    // Never closed: the job has to outlive every thread, and it ends with the process.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        g_boot_notes.push_back(fmt::format("No job object (error {})", GetLastError()));
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
    info.JobMemoryLimit = static_cast<SIZE_T>(ram_mib << 20);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        g_boot_notes.push_back(
            fmt::format("FRITCHII_SIM_RAM_LIMIT_MB: not set (error {})", GetLastError()));
        CloseHandle(job);
        return;
    }
    g_boot_notes.push_back(fmt::format("Committed memory limit {} MiB", ram_mib));
}

struct ThreadState {
    u64 begin{};       ///< the clock at the start of the stretch, or at its last pause
    u64 sum{};         ///< ticks of work not settled yet
    u64 left_out{};    ///< ticks of waits and spins not reported yet
    u64 base_cycles{}; ///< the thread's own cycle counter at the last settling
    u64 debt{};        ///< ticks still to spin
    u64 wait_begin{};
    CpuClass work_class{CpuClass::Game};
    bool open{};
    bool after_pause{};
    bool has_base{};
};
thread_local ThreadState t_state;

constexpr size_t ClassCount = static_cast<size_t>(CpuClass::Count);
constexpr std::array<const char*, ClassCount> ClassNames{"game", "front", "back", "render",
                                                         "worker"};

/// Work is settled once this much has added up (about 20 us): settling reads the thread's cycle
/// counter, a system call, and the guest cores end a stretch at every kernel call.
constexpr u64 SettleTicks = 80'000;
/// Smaller debts wait for the next settling (about 20 us).
constexpr u64 MinSpinTicks = 80'000;
/// The longest single spin, and the most a thread may owe (about 20 ms).
constexpr u64 MaxSpinTicks = 80'000'000;
/// Two pauses closer than this (about 2 us) belong to one spin loop.
constexpr u64 SpinGapTicks = 8'000;
/// An open stretch this old (about 50 ms) was left by a switch turned off and on again.
constexpr u64 StaleTicks = 200'000'000;
/// A log line about this often (about 10 s).
constexpr u64 ReportEveryTicks = 40'000'000'000;

std::array<std::atomic<u64>, ClassCount> g_work_ticks{};
std::array<std::atomic<u64>, ClassCount> g_added_ticks{};
std::array<std::atomic<u64>, ClassCount> g_left_ticks{};
std::atomic<u64> g_next_report{0};
std::atomic<u64> g_report_ticks{0};
std::atomic<s64> g_report_ns{0};

[[nodiscard]] u64 ThreadCycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

void ReportCpuSlow(u64 now_ticks, int percent) {
    u64 next = g_next_report.load(std::memory_order_relaxed);
    if (now_ticks < next ||
        !g_next_report.compare_exchange_strong(next, now_ticks + ReportEveryTicks,
                                               std::memory_order_relaxed)) {
        return;
    }
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const u64 last_ticks = g_report_ticks.exchange(now_ticks, std::memory_order_relaxed);
    const s64 last_ns = g_report_ns.exchange(now_ns, std::memory_order_relaxed);
    std::string classes;
    u64 all_work = 0;
    u64 all_added = 0;
    const double seconds = static_cast<double>(now_ns - last_ns) / 1e9;
    // ms of thread time per second of wall time, from the clock's ticks in this window.
    const double to_ms_per_s = next != 0 && now_ticks > last_ticks
                                   ? 1000.0 / static_cast<double>(now_ticks - last_ticks)
                                   : 0.0;
    for (size_t index = 0; index < ClassCount; ++index) {
        const u64 work = g_work_ticks[index].exchange(0, std::memory_order_relaxed);
        const u64 added = g_added_ticks[index].exchange(0, std::memory_order_relaxed);
        const u64 left = g_left_ticks[index].exchange(0, std::memory_order_relaxed);
        all_work += work;
        all_added += added;
        classes += fmt::format(
            "{}{} {:.0f} + {:.0f} (x{:.2f}, left out {:.0f})", classes.empty() ? "" : ", ",
            ClassNames[index], static_cast<double>(work) * to_ms_per_s,
            static_cast<double>(added) * to_ms_per_s,
            work != 0 ? static_cast<double>(work + added) / static_cast<double>(work) : 0.0,
            static_cast<double>(left) * to_ms_per_s);
    }
    if (next != 0 && all_work != 0 && seconds > 0.0) {
        LOG_INFO(Common,
                 "Low-end simulation, CPU x{:.2f}, ms/s summed over each class's threads, work + "
                 "added (reached, waits and spins left out): {}; all x{:.2f}",
                 percent / 100.0, classes,
                 static_cast<double>(all_work + all_added) / static_cast<double>(all_work));
    }
}

/// Turns the work added up into a spin. The work is the clock's time inside the stretches, at most
/// what the thread's own cycle counter says it ran: time it was descheduled or blocked inside a
/// stretch costs no cycles. The spin only watches the clock and pauses: no system calls, nothing
/// that disturbs the caches or an SMT sibling.
void Settle(ThreadState& state) {
    const u64 cycles = ThreadCycles();
    const u64 used = (std::min)(state.sum, cycles - state.base_cycles);
    const size_t index = static_cast<size_t>(state.work_class);
    state.base_cycles = cycles;
    state.left_out += state.sum - used;
    state.sum = 0;
    g_work_ticks[index].fetch_add(used, std::memory_order_relaxed);
    g_left_ticks[index].fetch_add(std::exchange(state.left_out, 0), std::memory_order_relaxed);
    const int percent = CpuSlowPercent();
    if (percent <= 100) {
        state.debt = 0;
        return;
    }
    state.debt = (std::min)(state.debt + used / 100 * static_cast<u64>(percent - 100),
                            MaxSpinTicks);
    u64 now = __rdtsc();
    if (state.debt >= MinSpinTicks) {
        const u64 spin = state.debt;
        state.debt = 0;
        const u64 start = now;
        while (now - start < spin) {
            _mm_pause();
            now = __rdtsc();
        }
        g_added_ticks[index].fetch_add(spin, std::memory_order_relaxed);
        state.base_cycles = ThreadCycles(); // the spin is not work to count again
    }
    ReportCpuSlow(now, percent);
}

/// Ends the current piece of the stretch at `now`. The piece between two pauses of a spin loop is
/// the loop's own check, not work.
void ClosePiece(ThreadState& state, u64 now) {
    const u64 piece = now - state.begin;
    if (state.after_pause && piece < SpinGapTicks) {
        state.left_out += piece;
    } else {
        state.sum += piece;
    }
}

#endif // _WIN32

} // Anonymous namespace

void ApplyProcessLimits() {
#ifdef _WIN32
    ApplyCpuSet();
    ApplyJob();
#endif
}

void LogProcessLimits() {
    for (const std::string& note : g_boot_notes) {
        LOG_INFO(Common, "Low-end simulation: {}", note);
    }
}

u64 CpuMask() {
    return g_cpu_mask;
}

unsigned LogicalProcessors() {
    return g_cpu_mask != 0 ? static_cast<unsigned>(std::popcount(g_cpu_mask))
                           : std::thread::hardware_concurrency();
}

int FactorPercent(std::atomic<int>& value, const char* name, int max_percent) {
    int raw = value.load(std::memory_order_relaxed);
    if (raw < 0) {
        int parsed = 0;
        if (const char* const text = std::getenv(name); text != nullptr && text[0] != '\0') {
            const double number = std::strtod(text, nullptr);
            if (number >= 100.0) {
                parsed = static_cast<int>((std::min)(number, 100000.0));
            } else if (number >= 1.0) {
                parsed = static_cast<int>(std::lround(number * 100.0));
            }
        }
        int unset = raw;
        value.compare_exchange_strong(unset, parsed, std::memory_order_relaxed);
        raw = value.load(std::memory_order_relaxed);
    }
    if (raw <= 1) {
        return 100;
    }
    return (std::min)(raw < 100 ? raw * 100 : raw, max_percent);
}

namespace {

std::atomic<s64> g_link_gpu_ns{0};
std::atomic<u64> g_link_cpu_total_ns{0};
std::atomic<u64> g_link_gpu_total_ns{0};
thread_local s64 t_link_debt_ns = 0;

/// Smaller debts wait for the next write: a clock read costs about as much as they do.
constexpr s64 LinkMinSpinNs = 2'000;
/// A thread never owes more than this; a load's burst past it is dropped.
constexpr s64 LinkMaxDebtNs = 50'000'000;

void LinkSpin(s64 ns) {
    const CpuWaitScope not_work; // FRITCHII_SIM_CPU_SLOW must not multiply it
    const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds{ns};
    while (std::chrono::steady_clock::now() < end) {
#ifdef _WIN32
        _mm_pause();
#endif
    }
}

[[nodiscard]] s64 LinkNs(u64 bytes) {
    // 1 MB/s is one byte a microsecond.
    return static_cast<s64>(bytes * 1000 / static_cast<u64>((std::max)(LinkFreeMbps(), 1)));
}

[[nodiscard]] int LinkUs() {
    const int raw = g_link_us.load(std::memory_order_relaxed);
    return raw < 0 ? LinkValue(g_link_us, "FRITCHII_SIM_LINK_US") : raw;
}

void AddGpuNs(s64 ns) {
    g_link_gpu_ns.fetch_add(ns, std::memory_order_relaxed);
    g_link_gpu_total_ns.fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
}

} // Anonymous namespace

int LinkValue(std::atomic<int>& value, const char* name) {
    int raw = value.load(std::memory_order_relaxed);
    if (raw < 0) {
        const char* const text = std::getenv(name);
        const int parsed = text != nullptr ? (std::max)(std::atoi(text), 0) : 0;
        int unset = raw;
        value.compare_exchange_strong(unset, parsed, std::memory_order_relaxed);
        raw = value.load(std::memory_order_relaxed);
    }
    return (std::max)(raw, 0);
}

int LinkFreeMbps() {
    const int link = LinkMbps();
    const int raw = g_link_load_mbps.load(std::memory_order_relaxed);
    const int load = raw < 0 ? LinkValue(g_link_load_mbps, "FRITCHII_SIM_LINK_LOAD") : raw;
    return (std::max)(link - load, link / 10);
}

void LinkChargeCpu(u64 bytes) {
    if (!LinkOn()) {
        return;
    }
    t_link_debt_ns = (std::min)(t_link_debt_ns + LinkNs(bytes), LinkMaxDebtNs);
    if (t_link_debt_ns < LinkMinSpinNs) {
        return;
    }
    const s64 ns = t_link_debt_ns;
    t_link_debt_ns = 0;
    g_link_cpu_total_ns.fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
    LinkSpin(ns);
}

void LinkChargeGpu(u64 bytes) {
    if (LinkOn()) {
        AddGpuNs(LinkNs(bytes));
    }
}

void LinkChargeGpuFetch() {
    if (LinkOn()) {
        AddGpuNs(s64{LinkUs()} * 1000);
    }
}

void LinkRoundTrip() {
    if (!LinkOn()) {
        return;
    }
    if (const int us = LinkUs(); us > 0) {
        g_link_cpu_total_ns.fetch_add(u64(us) * 1000, std::memory_order_relaxed);
        LinkSpin(s64{us} * 1000);
    }
}

s64 LinkTakeGpuNs() {
    return g_link_gpu_ns.exchange(0, std::memory_order_relaxed);
}

u64 LinkCpuNsTotal() {
    return g_link_cpu_total_ns.load(std::memory_order_relaxed);
}

u64 LinkGpuNsTotal() {
    return g_link_gpu_total_ns.load(std::memory_order_relaxed);
}

void CpuWorkBeginSlow(CpuClass work_class) {
#ifdef _WIN32
    ThreadState& state = t_state;
    const u64 now = __rdtsc();
    if (state.open && now - state.begin < StaleTicks) {
        return; // inside a stretch: its time is counted once
    }
    if (!state.has_base) {
        state.has_base = true;
        state.base_cycles = ThreadCycles();
    }
    state.work_class = work_class;
    state.begin = now;
    state.open = true;
    state.after_pause = false;
#else
    static_cast<void>(work_class);
#endif
}

void CpuWorkEndSlow() {
#ifdef _WIN32
    ThreadState& state = t_state;
    if (!state.open) {
        return;
    }
    ClosePiece(state, __rdtsc());
    state.open = false;
    if (state.sum >= SettleTicks) {
        Settle(state);
    }
#endif
}

void CpuWorkCancelSlow() {
#ifdef _WIN32
    t_state.open = false;
#endif
}

void CpuPauseNoteSlow() {
#ifdef _WIN32
    ThreadState& state = t_state;
    if (!state.open) {
        return;
    }
    const u64 now = __rdtsc();
    ClosePiece(state, now);
    state.begin = now;
    state.after_pause = true;
#endif
}

bool CpuWaitBeginSlow() {
#ifdef _WIN32
    ThreadState& state = t_state;
    if (!state.open) {
        return false;
    }
    ClosePiece(state, __rdtsc());
    state.open = false;
    // Before the wait: a slower thread would reach it later.
    if (state.sum >= SettleTicks) {
        Settle(state);
    }
    state.wait_begin = __rdtsc();
    return true;
#else
    return false;
#endif
}

void CpuWaitEndSlow() {
#ifdef _WIN32
    ThreadState& state = t_state;
    const u64 now = __rdtsc();
    state.left_out += now - state.wait_begin;
    state.begin = now;
    state.open = true;
    state.after_pause = false;
#endif
}

} // namespace Common::LowEndSim
