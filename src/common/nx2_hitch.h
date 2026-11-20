// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__linux__)
#include <time.h>
#endif

#include "common/common_types.h"

// Hitch attribution (2026-11-10, the owner's v5.6 playtest: 0.4-1 s frames with 0 pipelines built,
// the cause unclear from 10-second totals; design fable52_answer_11/12). Every place that can hold
// the game up adds its time here; the game's present (nx2:vi2) calls OnPresent with the gap since
// the previous present, and a gap over 50 ms logs one line with its exact time, what each cause
// took inside it, page faults, memory the kernel zero-filled, and the CPU time of the busiest
// threads. EDEN_NX2_HITCH_LOG=0 turns the line off (the counters stay, a few relaxed adds).
// With the line on, it also says how much CPU the rest of the machine used, and a once-a-second
// "Pulse:" line samples the clock, a speed probe and free memory (FRITCHII_PULSE=0 turns that
// off; nx2_hitch.cpp, machine pulse).
namespace Common::Nx2Hitch {

enum class Cause : u8 {
    FullDrain,       ///< fence releases: the front waited until the back and the host GPU were idle
    ReadBack,        ///< guest reads of GPU-written memory (download + wait), any thread
    SaveData,        ///< save data writes and flushes (fsp)
    MemorySample,    ///< working-set walks for the memory lines (memory probe thread)
    ZeroTrim,        ///< zero-page passes that ended in the gap (their own thread)
    FileThrottle,    ///< IStorage::Read's sleep under 40 MB/s (game files, the one FS thread)
    FileRead,        ///< IStorage::Read's read itself
    HostMap,         ///< host Map of guest memory (placeholder lock included), amount = bytes
    HostUnmap,       ///< host Unmap, amount = bytes
    HostProtect,     ///< host Protect, amount = bytes
    Recompile,       ///< a CPU core's recompiler compiling game code no core had compiled
    RecompileRepeat, ///< the same, for code another core had compiled already (separate JITs)
    WarmCompile,     ///< EDEN_NX2_JIT_WARM: idle cores compiling what another core compiled
    ImageCreate,     ///< Vulkan images created with their memory (MemoryAllocator::CreateImage)
    TimerSpin,       ///< the timer thread spinning before a guest timer (EDEN_NX2_EXP75)
    SlowTimerEvent,  ///< timer thread callbacks of 1 ms or more (core_timing.cpp)
    // FRITCHII_HITCH_SPLIT=1 (boot) only; never counted with it off:
    RecompileDropped, ///< RecompileRepeat on a core that had compiled the block itself (dropped)
    CodeDropRange,    ///< blocks dropped by range invalidations (count = blocks, time = the drop)
    CodeDropFastmem,  ///< blocks dropped by fastmem faults to be recompiled (count = blocks)
    WarmRefusedSpace, ///< warm compiles refused: the code cache kept its free half (count only)
    StagingCreate,    ///< new staging buffers (StagingBufferPool::CreateStagingBuffer), bytes
    TextureUpload,    ///< TextureCache::UploadImageContents (read, untile, staging write), bytes
    ZeroUpload,       ///< FRITCHII_ZERO_UPLOAD: the zero check of an upload, bytes looked at
    RecompileListed,  ///< Recompile of a FRITCHII_JIT_BLOCK_LIST block the warm-up had not reached
    // Intel fork: guest WaitSynchronization of 0.5 ms or more, by the event's name (summed over
    // threads; the longest single wait says what held a frame back)
    GuestWaitGpu,     ///< syncpoint / gsv events: the guest waiting for the GPU
    GuestWaitAudio,   ///< audio renderer events
    GuestWaitOther,   ///< any other event or object
    Count,
};

/// Names in the hitch line, in Cause order.
inline constexpr std::array<const char*, static_cast<size_t>(Cause::Count)> CauseNames{
    "full drains",        "read-backs", "save data", "memory walks", "zero-page passes",
    "file read throttle", "file reads", "host map",  "host unmap",   "host protect",
    "recompiling new game code (summed over cores)",
    "recompiling code another core had (summed over cores)",
    "warm compiles on idle cores", "image creates", "timer thread spin",
    "timer events over 1 ms",
    "recompiling code this core had compiled before (dropped since)",
    "game code dropped by invalidations (blocks)", "game code dropped by fastmem faults (blocks)",
    "warm compiles refused (code cache full)", "staging buffers made",
    "texture uploads (CPU side)", "zero checks of texture uploads",
    "recompiling listed game code the warm-up had not compiled yet (summed over cores)",
    "guest waits on the GPU (summed over threads)", "guest waits on audio (summed over threads)",
    "guest waits on other events (summed over threads)"};

struct Counters {
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> count{};
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> amount{};
    /// The longest single one since the last present (reset there)
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> longest{};
    /// steady_clock ns when the running zero-page pass began, 0 while none runs
    std::atomic<u64> zero_trim_since{};
};

inline Counters g_counters;

[[nodiscard]] inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void Add(Cause cause, u64 ns, u64 amount = 0) {
    const auto i = static_cast<size_t>(cause);
    g_counters.ns[i].fetch_add(ns, std::memory_order_relaxed);
    g_counters.count[i].fetch_add(1, std::memory_order_relaxed);
    if (amount != 0) {
        g_counters.amount[i].fetch_add(amount, std::memory_order_relaxed);
    }
    u64 longest = g_counters.longest[i].load(std::memory_order_relaxed);
    while (ns > longest &&
           !g_counters.longest[i].compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
    }
}

/// `n` events of `cause` that took `ns` together (count-only causes pass ns 0).
inline void AddCount(Cause cause, u64 ns, u64 n) {
    const auto i = static_cast<size_t>(cause);
    g_counters.ns[i].fetch_add(ns, std::memory_order_relaxed);
    g_counters.count[i].fetch_add(n, std::memory_order_relaxed);
    u64 longest = g_counters.longest[i].load(std::memory_order_relaxed);
    while (ns > longest &&
           !g_counters.longest[i].compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
    }
}

/// FRITCHII_HITCH_SPLIT (boot, default 0; helper BA, the area-load hitches): 1 = the hitch line
/// also splits recompiles of code a core had itself (dropped since) from code only other cores
/// had, counts game code dropped by invalidations and fastmem faults and warm compiles the full
/// code cache refused, times new staging buffers and texture uploads, and gives the GPU threads
/// (GPU, GPUBack, GPUMem, GPUBackE2) a place in the busiest threads even past the first 48.
[[nodiscard]] bool SplitEnabled();

/// Times its scope as `cause`.
class Scope {
public:
    explicit Scope(Cause cause_, u64 amount_ = 0)
        : cause{cause_}, amount{amount_}, begin{NowNs()} {}
    ~Scope() {
        Add(cause, NowNs() - begin, amount);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Cause cause;
    u64 amount;
    u64 begin;
};

/// Times its scope as `cause` only with FRITCHII_HITCH_SPLIT=1 (one cached flag test when off).
class SplitScope {
public:
    explicit SplitScope(Cause cause_, u64 amount_ = 0)
        : cause{cause_}, amount{amount_}, begin{SplitEnabled() ? NowNs() : 0} {}
    ~SplitScope() {
        if (begin != 0) {
            Add(cause, NowNs() - begin, amount);
        }
    }
    SplitScope(const SplitScope&) = delete;
    SplitScope& operator=(const SplitScope&) = delete;

private:
    Cause cause;
    u64 amount;
    u64 begin;
};

inline void ZeroTrimBegin() {
    g_counters.zero_trim_since.store(NowNs(), std::memory_order_relaxed);
}

inline void ZeroTrimEnd() {
    const u64 since = g_counters.zero_trim_since.exchange(0, std::memory_order_relaxed);
    if (since != 0) {
        Add(Cause::ZeroTrim, NowNs() - since);
    }
}

/// Set on a CPU core's thread while it warm-compiles (PhysicalCore::Idle).
inline thread_local bool t_warming{};

/// Which emulator thread this is, from its name (RegisterThread), for the lock wait counters.
enum class Role : u8 { Other, GuestCore, GpuFront, GpuBack, GpuMem, Count };
inline thread_local Role t_role{};

/// Time spent waiting for a contended SpinRecursiveMutex (the buffer and texture cache locks),
/// per role (fable52_answer_13 section 3: the back's lock waits and the guest's 144 ms/s in
/// GetFlushAreaLocked that nothing showed). Logged with the drain sites line (GpuSplit).
struct LockWaits {
    std::array<std::atomic<u64>, static_cast<size_t>(Role::Count)> ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Role::Count)> count{};
};
inline LockWaits g_lock_waits;

inline void AddLockWait(u64 ns) {
    const auto i = static_cast<size_t>(t_role);
    g_lock_waits.ns[i].fetch_add(ns, std::memory_order_relaxed);
    g_lock_waits.count[i].fetch_add(1, std::memory_order_relaxed);
}

/// From a CPU core's recompiler before it compiles block `key` (its LocationDescriptor value):
/// the cause to time it as (new code, code another core compiled, a warm compile). Process-wide
/// key -> core mask, never forgets. With EDEN_NX2_JIT_WARM=1, an on-demand compile also queues
/// the key for every other core that hasn't compiled it (fable52_answer_12).
[[nodiscard]] Cause CompileCause(u64 key, u32 core);

/// EDEN_NX2_JIT_WARM=1 (boot): idle cores compile what other cores compiled. Off by default.
[[nodiscard]] bool JitWarmEnabled();

/// Moves up to `max` queued keys for `core` into `out` (cleared first); returns how many.
size_t TakeWarmKeys(u32 core, std::vector<u64>& out, size_t max);

/// FRITCHII_JIT_WARM_CORES (boot, default 0 = every core; docs/fritchii/switches.md): bit N set =
/// guest core N takes part in the warm-up (EDEN_NX2_JIT_WARM's queues and the
/// FRITCHII_JIT_BLOCK_LIST preload). Each core's recompiler keeps its own code and block tables,
/// so every core warmed holds a copy of every block any core ran.
[[nodiscard]] u32 JitWarmCoreMask();

/// Blocks compiled per core so far, warm and on demand, for the memory census.
[[nodiscard]] std::string JitCompileCensus();

/// From Common::SetCurrentThreadName: the thread's CPU time goes in the hitch line under `name`.
void RegisterThread(const char* name);

/// The calling thread's name as RegisterThread saw it (cut to 15 characters; empty before), for
/// diagnostics that say which thread did something (FRITCHII_SLOW_GPU's "from").
inline thread_local std::array<char, 16> t_thread_name{};

/// FRITCHII_SLOW_GPU's breadcrumbs (vk_submit_times.h SlowGpu): the newest main-queue
/// submissions, what they held and whether the GPU finished or started them. The hang report
/// logs them after its stall snapshot, Vulkan::Device::ReportLoss on a device loss. The renderer
/// installs `log` (called under a lock; it must not block for long) and removes it before it goes.
void SetGpuBreadcrumbs(void (*log)(const void* owner, const char* prefix), const void* owner);
void ClearGpuBreadcrumbs(const void* owner);
/// Logs them at Critical, each line starting with `prefix`; one line saying none without a renderer.
void LogGpuBreadcrumbs(const char* prefix);

/// From the game's present, one thread: `gap_us` since its previous present.
void OnPresent(long long gap_us);

// Hang report (2026-11-16, helper AW's boot hang: 0 frames from 17 s on, every thread idle, and
// nothing in the log said what the game waited on). Once a second (the Pulse thread, or a small
// "FritchiiHangWatch" thread where the Pulse does not run) it checks for presents. After
// FRITCHII_HANG_REPORT_S seconds (live, default 5, 0 = off) without one while the game runs (the
// hooks below say so: powered on, not paused, not stalled), it logs one "Hang report:" block at
// Critical: the stall snapshot as a present would log it, then the emulator's side through
// `report` (GPU acquires and syncpoint waits, the GPU split / one follower state, service calls
// in flight, the game's threads in kernel waits), then every thread's stack (Windows x64,
// module+offset). FRITCHII_HANG_DUMP=1 (boot) adds a minidump next to the log (Windows). One
// report per freeze: the next present re-arms it. Read only (threads are suspended only for the
// moment their registers are read). A boot or shutdown (SetHangHooks, ClearHangHooks) starts the
// count over; until the session's first present the threshold is FRITCHII_HANG_BOOT_S (live,
// default 60), as the first frame comes 16-19 s into a boot.
struct HangHooks {
    /// The report's threshold in seconds while the game runs, 0 while it does not (or the
    /// switch is 0). Called once a second under the hooks' lock.
    int (*armed_seconds)(void* context){};
    /// Logs the emulator's side of the report. Under the hooks' lock; must not block for long.
    void (*report)(void* context){};
    void* context{};
};
/// Installs the hooks (a session's start).
void SetHangHooks(const HangHooks& hooks);
/// Removes them if `context` installed them; no hook call runs after this returns.
void ClearHangHooks(const void* context);

/// The report's thread part, shared with the shutdown watch (common/shutdown_watch.h): the stall
/// snapshot (unless EDEN_NX2_STALL_SNAPSHOT=0) and every thread's stack (Windows x64), each line
/// at Critical under `prefix`. `shutdown`: the session's hooks are gone, said in the line.
/// `main_thread_id` (0 = unknown) is labelled "main" in the stacks.
void LogThreadReport(const char* prefix, bool shutdown, unsigned long main_thread_id);
/// FRITCHII_HANG_DUMP=1 (boot, default 0).
[[nodiscard]] bool HangDumpSwitch();
/// Writes the hang minidump next to the log (at most three a process); returns what to log.
[[nodiscard]] std::string WriteHangDumpNow();

// Stall snapshot (fable52_answer_stall2 section 2): on by default since 2026-11-14,
// EDEN_NX2_STALL_SNAPSHOT=0 turns it off (read as each thread is named, so nx2_switches.txt applies). A present gap of 100 ms or more then logs one
// "Stall snapshot:" line after its Hitch line: for the GPU back, GPU front, Vulkan worker, GPU
// fencing and present threads the span each is in now and for how long, and the longest span
// each spent in the gap; the master semaphore's ticks; whether the present thread is between
// vkAcquireNextImageKHR and vkQueuePresentKHR. Spans are the timeline's (Nx2Timeline::Span,
// ArgSpan), the profiler's wait zones (Nx2Profile::IsStallZone: waits, sleeps, host calls, the
// front's per-list push) and a few marks of its own (StallScope). Off, a span pays one
// thread_local load (t_stall_slot stays null on every thread). On, a span reads no clock: its
// times come from StallTickNs, a tick the OS keeps (2026-11-14: two fenced clock reads per
// coarse zone cost the front 83 ms/s, fable_front_drains 8.4).
enum class StallThread : u8 { GpuBack, GpuFront, VulkanWorker, Fencing, Present, GpuMem, Count };

/// The snapshot's clock, in ns: a coarse tick the OS keeps in memory it maps into every process,
/// so reading it is one plain load (no time stamp instruction, no fence). Windows: the kernel's
/// interrupt time (KUSER_SHARED_DATA, advanced at every timer interrupt: every 0.5 ms with the
/// timer resolution Fritchii asks for at boot, 15.6 ms at worst). Linux: CLOCK_MONOTONIC_COARSE
/// (the vDSO's tick, 1 to 4 ms). Elsewhere steady_clock. A 100 ms stall needs no better.
[[nodiscard]] inline u64 StallTickNs() {
#if defined(_WIN32) &&                                                                             \
    (defined(_M_X64) || defined(_M_ARM64) || defined(__x86_64__) || defined(__aarch64__))
    // InterruptTime at offset 8 of KUSER_SHARED_DATA (0x7FFE0000), 100 ns units. On 64-bit
    // Windows it is one aligned 64-bit word, read whole (wdm.h's KeQueryInterruptTime reads it
    // the same way).
    return *reinterpret_cast<const volatile u64*>(std::uintptr_t{0x7FFE0008}) * 100;
#elif defined(__linux__)
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000 + static_cast<u64>(ts.tv_nsec);
#else
    return NowNs();
#endif
}

struct StallSlot {
    std::atomic<const char*> zone{}; ///< The innermost span now, nullptr outside every span
    std::atomic<u64> since{};        ///< StallTickNs when it began (or the last span ended); 0 never
    std::atomic<const char*> longest_zone{}; ///< The longest span since the last present
    std::atomic<u64> longest_ns{};
};

inline std::array<StallSlot, static_cast<size_t>(StallThread::Count)> g_stall_slots;
/// Set when the first thread registers with the stall snapshot on (EDEN_NX2_STALL_SNAPSHOT not 0).
inline std::atomic<bool> g_stall_on{false};
/// This thread's slot (RegisterThread), null unless the snapshot is on and this is one of the five.
inline thread_local StallSlot* t_stall_slot{};

/// Ns between vkAcquireNextImageKHR returning an image and vkQueuePresentKHR returning; 0 outside.
inline std::atomic<u64> g_stall_acquired_since{};
/// The longest acquire to present since the last present (ns).
inline std::atomic<u64> g_stall_acquire_longest{};

[[nodiscard]] inline bool StallOn() {
    return g_stall_on.load(std::memory_order_relaxed);
}

/// What a span entered on a slot restores on exit. `slot` null: nothing entered. Trivial, so a
/// profiler Scope can hold one without writing it on its off path.
struct StallMark {
    StallSlot* slot;
    const char* prev_zone;
    u64 prev_since;
    u64 since;
};

/// A span ending (or a stretch outside every span) is the slot's longest only if it beats the
/// recorded one by 2 ms, so a wait keeps its inner name ("macro dma read") over the zone around it
/// ("split drain"): ticks are coarse, so the two can read up to two ticks apart (1 ms at 0.5 ms).
/// Only the slot's own thread writes it.
inline void StallNoteLongest(StallSlot* slot, const char* zone, u64 ns) {
    if (ns > slot->longest_ns.load(std::memory_order_relaxed) + 2'000'000) {
        slot->longest_zone.store(zone, std::memory_order_relaxed);
        slot->longest_ns.store(ns, std::memory_order_release);
    }
}

inline StallMark StallEnterSlow(StallSlot* slot, const char* zone) {
    const u64 now = StallTickNs();
    StallMark mark{slot, slot->zone.load(std::memory_order_relaxed),
                   slot->since.load(std::memory_order_relaxed), now};
    if (mark.prev_zone == nullptr && mark.prev_since != 0) {
        StallNoteLongest(slot, "(no span)", now - mark.prev_since);
    }
    slot->zone.store(zone, std::memory_order_relaxed);
    slot->since.store(now, std::memory_order_relaxed);
    return mark;
}

inline void StallLeaveSlow(const StallMark& mark) {
    StallSlot* const slot = mark.slot;
    const u64 now = StallTickNs();
    StallNoteLongest(slot, slot->zone.load(std::memory_order_relaxed), now - mark.since);
    // Back in the span around this one, with its own start; outside every span from now.
    slot->zone.store(mark.prev_zone, std::memory_order_relaxed);
    slot->since.store(mark.prev_zone != nullptr ? mark.prev_since : now,
                      std::memory_order_relaxed);
}

[[nodiscard]] inline StallMark StallEnter(const char* zone) {
    StallSlot* const slot = t_stall_slot;
    if (slot == nullptr) [[likely]] {
        return {};
    }
    return StallEnterSlow(slot, zone);
}

inline void StallLeave(const StallMark& mark) {
    if (mark.slot != nullptr) [[unlikely]] {
        StallLeaveSlow(mark);
    }
}

/// A span for the snapshot only (not on the timeline): waits the timeline has no span for.
class StallScope {
public:
    explicit StallScope(const char* zone) : mark{StallEnter(zone)} {}
    ~StallScope() {
        StallLeave(mark);
    }
    StallScope(const StallScope&) = delete;
    StallScope& operator=(const StallScope&) = delete;

private:
    StallMark mark;
};

/// The present thread got a swapchain image / presented it (Swapchain). Snapshot on only.
void StallAcquired();
void StallPresented();

/// The renderer's tick text for the line (the master semaphore): `reporter(owner)`. A null
/// reporter removes it; it is called under a mutex from the game's present, so the owner clears it
/// in its destructor.
void SetStallTickReporter(std::string (*reporter)(const void*), const void* owner);
/// Removes the reporter if `owner` set it.
void ClearStallTickReporter(const void* owner);

} // namespace Common::Nx2Hitch
