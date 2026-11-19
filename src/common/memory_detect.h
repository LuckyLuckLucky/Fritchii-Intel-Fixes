// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/// Page faults of this process so far, soft and hard (0 where unknown).
[[nodiscard]] u64 GetProcessPageFaults();

/// This process's memory and the system's commit, for the memory log line (zeros where unknown;
/// only Windows fills them in).
/// Commit the system can still hand out, in bytes: its commit limit (RAM plus the page files)
/// minus what is committed. No value where the host reports no such limit (anything but Windows).
/// Ported.
[[nodiscard]] std::optional<u64> GetSystemCommitRoom();

#ifdef _WIN32
/// Commits reserved pages read-write, waiting at Windows' commit limit (RAM plus the page files):
/// a refusal there is often over in moments, while a system-managed page file grows or the texture
/// cache gives memory back (HostCriticalMemory). Tries again every 50 ms for up to 5 s, only for
/// out-of-memory errors. quiet: logs nothing (exception handlers). False if it still failed, with
/// the last error set. FRITCHII_TEST_COMMIT_REFUSE (boot, ms, default 0) refuses commits for that
/// long from the first one on, as a test of this path.
[[nodiscard]] bool CommitPagesWaiting(void* address, u64 bytes, bool quiet = false);
/// Commits and C++ allocations given up at the commit limit so far, for the crash report.
[[nodiscard]] u64 CommitRefusals();
#endif
/// `operator new` refused with little commit left waits and tries again for up to 5 s on that
/// thread before std::bad_alloc, as CommitPagesWaiting does (with more left the refusal is no
/// commit limit, a size gone wrong: std::bad_alloc at once, as without this). Windows only.
void InstallCommitLimitNewHandler();

/// FRITCHII_VRAM_BUDGET_MODE=os: the OS's count of dedicated video memory in use on one adapter
/// (by every process) and this process's part of it.
struct AdapterVideoMemory {
    u64 adapter_dedicated{};
    u64 process_dedicated{};
};
/// The last sample (taken once a second on a low priority thread of its own, PDH's GPU Adapter
/// Memory and GPU Process Memory counters; the first call starts it) for the adapter with this
/// LUID. No value before the first sample, when it is older than 5 s, and off Windows.
[[nodiscard]] std::optional<AdapterVideoMemory> GetAdapterVideoMemory(u32 luid_low, s32 luid_high);

struct ProcessMemoryUsage {
    u64 working_set{};
    u64 peak_working_set{};
    u64 private_bytes{}; ///< Private commit (mapped sections like the guest memory are not in it)
    u64 peak_private_bytes{};
    /// Resident pages not shareable with other mappings: the working set minus the guest memory
    /// section and mapped files (0 where Windows does not report it: older builds).
    u64 private_working_set{};
    u64 system_commit{}; ///< Commit charge of the whole system
    u64 system_commit_limit{};
    u64 available_physical{};
};
[[nodiscard]] ProcessMemoryUsage GetProcessMemoryUsage();

/// This process's Win32 heaps (the C runtime's malloc heap and every other HeapCreate heap: Qt's,
/// drivers' that use one), summed with HeapSummary. Zeros elsewhere.
struct HeapUsage {
    u64 crt_committed{};   ///< The C runtime's heap (malloc, new)
    u64 crt_allocated{};
    u64 other_committed{}; ///< Every other heap of the process
    u64 other_allocated{};
    u32 heaps{};           ///< Heaps summed, the C runtime's included
    u32 micros{};          ///< What the pass took
};
/// Locks each heap for its summary: call it off the busy threads. Also kept for LastHeapUsage.
HeapUsage SampleHeapUsage();
/// The last SampleHeapUsage result (zeros before the first).
[[nodiscard]] HeapUsage LastHeapUsage();

/// Committed memory of zeros that takes RAM only where it is touched (VirtualAlloc, anonymous
/// mmap); null on failure. For big tables mostly left zero.
[[nodiscard]] void* AllocateZeroPages(u64 bytes);
void FreeZeroPages(void* memory, u64 bytes);

/// FRITCHII_MEM_CENSUS (boot, default 0; docs/fritchii/switches.md): every 30 s, and whenever the
/// working set climbs 512 MiB past the last census (a load peak), walks this process's address
/// space (VirtualQuery and one working set query on Windows, /proc/self/smaps on Linux) and logs
/// committed and resident MiB by owner: tagged ranges (guest memory, VMA's host maps), images
/// (DLLs, drivers), heaps, stacks, executable private memory (the JIT code caches), and the rest
/// of the private memory by reservation size with its largest allocations. Read-only.
/// 1 = committed and resident, 2 = also every heap's summary (locks each heap: can hitch),
/// 3 = committed only (no working set query).
namespace MemCensus {
[[nodiscard]] int Mode();
/// A named range for the census (`name` must outlive it: a string literal). No-op when off.
void Tag(const char* name, const void* base, u64 size);
void Untag(const void* base);
/// Text the census adds to its log (guest residency, JIT compiles); called on the census thread.
void SetGuestSampler(std::function<std::string()> sampler);
/// Sizes of memory some library allocates where the census can't see its address (the driver's
/// copies of Vulkan device memory): private allocations of the same size (64 KiB granules) are
/// counted under `label` ("size match"). `label` must be a string literal. Called on the census
/// thread; null removes it.
void SetSizeSource(const char* label, std::function<std::vector<u64>()> source);
/// More text for the census's "more" line from an owner of a big table (`key` is the owner; one
/// reporter per key). Called on the census thread under a lock that RemoveReporter also takes,
/// so the owner can remove it in its destructor and the call is never running past that.
void AddReporter(const void* key, std::function<std::string()> reporter);
void RemoveReporter(const void* key);
/// For reporters: which 4 KiB pages of [base, base + bytes) are in the working set (1 per page;
/// all 1 where the query is not available). Never touches the pages.
[[nodiscard]] std::vector<u8> ResidentPages(const void* base, u64 bytes);
/// Starts the census thread when Mode() is not 0 (once per Start/Stop pair).
void Start();
/// Stops and joins it (before what the sampler reads goes away).
void Stop();
} // namespace MemCensus

} // namespace Common
