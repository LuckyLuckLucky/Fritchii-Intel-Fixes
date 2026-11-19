// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
// clang-format on
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/nx2_hitch.h"
#include "common/thread.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

u64 GetProcessPageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.PageFaultCount;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<u64>(usage.ru_minflt) + static_cast<u64>(usage.ru_majflt);
#endif
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    // PROCESS_MEMORY_COUNTERS_EX2's layout (the SDK declares it only for NTDDI_WIN10_CU and up).
    // Windows builds that know it fill PrivateWorkingSetSize; older ones either refuse the size
    // (then the EX one is asked for) or leave it 0.
    struct CountersEx2 {
        PROCESS_MEMORY_COUNTERS_EX ex;
        SIZE_T PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    };
    CountersEx2 counters{};
    counters.ex.cb = sizeof(counters);
    bool ok = K32GetProcessMemoryInfo(GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                      sizeof(counters));
    if (!ok) {
        counters = {};
        counters.ex.cb = sizeof(counters.ex);
        ok = K32GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters.ex),
                                     sizeof(counters.ex));
    }
    if (ok) {
        usage.working_set = counters.ex.WorkingSetSize;
        usage.peak_working_set = counters.ex.PeakWorkingSetSize;
        usage.private_bytes = counters.ex.PrivateUsage;
        usage.peak_private_bytes = counters.ex.PeakPagefileUsage;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        usage.system_commit = status.ullTotalPageFile - status.ullAvailPageFile;
        usage.system_commit_limit = status.ullTotalPageFile;
        usage.available_physical = status.ullAvailPhys;
    }
#endif
    return usage;
}

namespace {
std::mutex g_heap_usage_mutex;
HeapUsage g_heap_usage{};
} // Anonymous namespace

HeapUsage SampleHeapUsage() {
    HeapUsage usage{};
#ifdef _WIN32
    const auto start = std::chrono::steady_clock::now();
#ifdef _MSC_VER
    const HANDLE crt_heap = reinterpret_cast<HANDLE>(_get_heap_handle());
#else
    const HANDLE crt_heap = GetProcessHeap();
#endif
    std::vector<HANDLE> heaps(64);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
        if (count <= heaps.size()) {
            heaps.resize(count);
            break;
        }
        heaps.resize(count + 16);
    }
    for (const HANDLE heap : heaps) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heap, 0, &summary)) {
            continue;
        }
        ++usage.heaps;
        if (heap == crt_heap) {
            usage.crt_committed += summary.cbCommitted;
            usage.crt_allocated += summary.cbAllocated;
        } else {
            usage.other_committed += summary.cbCommitted;
            usage.other_allocated += summary.cbAllocated;
        }
    }
    usage.micros = static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
#endif
    std::scoped_lock lock{g_heap_usage_mutex};
    g_heap_usage = usage;
    return usage;
}

HeapUsage LastHeapUsage() {
    std::scoped_lock lock{g_heap_usage_mutex};
    return g_heap_usage;
}

std::optional<u64> GetSystemCommitRoom() {
#ifdef _WIN32
    // GlobalMemoryStatusEx, not GetPerformanceInfo: the latter also counts every process, thread
    // and handle in the system and took ~3.7 ms a call (388 processes, 176k handles), on the GPU
    // back thread every 30 frames (TextureCache::TickFrame, ~8.7 ms/s at the Castle Town market).
    // GlobalMemoryStatusEx takes ~1.4 us and its ullAvailPageFile is the same number (commit limit
    // minus commit total; equal to the byte in a side-by-side read), only ever lower inside a job
    // object with a commit quota, where the quota is the tighter real bound anyway.
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullAvailPageFile;
    }
#endif
    return std::nullopt;
}

#ifdef _WIN32
namespace {
using CommitClock = std::chrono::steady_clock;
constexpr std::chrono::seconds CommitWaitLimit{5};
constexpr DWORD CommitRetryMs = 50;
/// A refused `operator new` counts as the commit limit only with less than this left.
constexpr u64 NewHandlerCommitRoom = u64{4} << 30;

std::atomic<u64> g_commit_refusals{};

/// FRITCHII_TEST_COMMIT_REFUSE: when the first commit asked for it, in clock ticks; 0: not yet.
std::atomic<CommitClock::rep> g_test_refuse_start{0};

u32 TestRefuseWindowMs() {
    static const u32 window_ms = [] {
        const char* const value = std::getenv("FRITCHII_TEST_COMMIT_REFUSE");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
    }();
    return window_ms;
}

bool TestRefusesCommit(bool is_commit) {
    const u32 window_ms = TestRefuseWindowMs();
    if (window_ms == 0) [[likely]] {
        return false;
    }
    const CommitClock::time_point now = CommitClock::now();
    CommitClock::rep start = g_test_refuse_start.load(std::memory_order_relaxed);
    if (start == 0) {
        if (!is_commit) {
            return false;
        }
        const CommitClock::rep ticks = now.time_since_epoch().count();
        start = g_test_refuse_start.compare_exchange_strong(start, ticks) ? ticks : start;
    }
    const CommitClock::time_point begin{CommitClock::duration{start}};
    return now - begin < std::chrono::milliseconds{window_ms};
}

bool IsOutOfCommit(DWORD error) {
    return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY ||
           error == ERROR_OUTOFMEMORY || error == ERROR_NO_SYSTEM_RESOURCES ||
           error == ERROR_PAGEFILE_QUOTA;
}

bool TryCommit(void* address, u64 bytes) {
    if (TestRefusesCommit(true)) {
        SetLastError(ERROR_COMMITMENT_LIMIT);
        return false;
    }
    return VirtualAlloc(address, static_cast<SIZE_T>(bytes), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

u64 CommitRoomMiB() {
    return GetSystemCommitRoom().value_or(0) >> 20;
}

void OnNewRefused() {
    // Logging allocates: a refusal inside the log line must not log again.
    thread_local bool in_log = false;
    thread_local CommitClock::time_point first{};
    thread_local CommitClock::time_point last{};
    static std::atomic<int> told{0};
    const CommitClock::time_point now = CommitClock::now();
    if (last == CommitClock::time_point{} || now - last > std::chrono::milliseconds{250}) {
        first = now;
    }
    last = now;
    const std::optional<u64> real_room = GetSystemCommitRoom();
    const std::optional<u64> room = TestRefusesCommit(false) ? std::optional<u64>{0} : real_room;
    if (!room || *room > NewHandlerCommitRoom) {
        throw std::bad_alloc{};
    }
    if (now - first >= CommitWaitLimit) {
        g_commit_refusals.fetch_add(1, std::memory_order_relaxed);
        if (!in_log) {
            in_log = true;
            LOG_CRITICAL(Common_Memory,
                         "Out of memory: a C++ allocation was refused for 5 s at Windows' commit "
                         "limit ({} MiB of commit left). Close other programs or enlarge the page "
                         "file",
                         *room >> 20);
            in_log = false;
        }
        throw std::bad_alloc{};
    }
    if (!in_log && told.fetch_add(1, std::memory_order_relaxed) < 8) {
        in_log = true;
        LOG_WARNING(Common_Memory,
                    "A C++ allocation was refused at Windows' commit limit ({} MiB of commit "
                    "left): trying again for up to 5 s",
                    *room >> 20);
        in_log = false;
    }
    Sleep(CommitRetryMs);
}
} // Anonymous namespace

bool CommitPagesWaiting(void* address, u64 bytes, bool quiet) {
    if (TryCommit(address, bytes)) [[likely]] {
        return true;
    }
    DWORD error = GetLastError();
    if (!IsOutOfCommit(error)) {
        return false;
    }
    static std::atomic<int> told{0};
    const bool tell = !quiet && told.fetch_add(1, std::memory_order_relaxed) < 8;
    if (tell) {
        LOG_WARNING(Common_Memory,
                    "Committing {} KiB was refused at Windows' commit limit (error {}, {} MiB of "
                    "commit left): trying again for up to 5 s",
                    bytes >> 10, error, CommitRoomMiB());
    }
    const CommitClock::time_point start = CommitClock::now();
    while (CommitClock::now() - start < CommitWaitLimit) {
        Sleep(CommitRetryMs);
        if (TryCommit(address, bytes)) {
            if (tell) {
                LOG_INFO(Common_Memory, "The commit went through after {} ms",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             CommitClock::now() - start)
                             .count());
            }
            return true;
        }
        error = GetLastError();
        if (!IsOutOfCommit(error)) {
            break;
        }
    }
    g_commit_refusals.fetch_add(1, std::memory_order_relaxed);
    if (!quiet) {
        LOG_CRITICAL(Common_Memory,
                     "Out of memory: committing {} KiB was refused for 5 s at Windows' commit "
                     "limit (error {}, {} MiB of commit left). Close other programs or enlarge "
                     "the page file",
                     bytes >> 10, error, CommitRoomMiB());
    }
    SetLastError(error);
    return false;
}

u64 CommitRefusals() {
    return g_commit_refusals.load(std::memory_order_relaxed);
}
#endif

void InstallCommitLimitNewHandler() {
#ifdef _WIN32
    std::set_new_handler(OnNewRefused);
    if (TestRefuseWindowMs() != 0) {
        // The test of the handler: an allocation no PC can give, while commits are refused.
        std::thread([] {
            while (g_test_refuse_start.load(std::memory_order_relaxed) == 0) {
                Sleep(CommitRetryMs);
            }
            try {
                ::operator delete(::operator new(size_t{1} << 46));
            } catch (const std::bad_alloc&) {
                LOG_INFO(Common_Memory, "FRITCHII_TEST_COMMIT_REFUSE: std::bad_alloc came through");
            }
        }).detach();
    }
#endif
}

#ifdef _WIN32
namespace {
/// FRITCHII_VRAM_BUDGET_MODE=os: the last sample of the OS's dedicated video memory counters.
struct AdapterVideoMemorySample {
    std::atomic<u64> adapter{};
    std::atomic<u64> process{};
    std::atomic<s64> time_ms{}; ///< steady clock ms of the last good sample, 0 before it
};
AdapterVideoMemorySample g_adapter_video_memory;

s64 SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string LowerAscii(const wchar_t* text) {
    std::string out;
    for (; text != nullptr && *text != L'\0'; ++text) {
        const wchar_t c = *text;
        out.push_back(c < 128 ? static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c)
                              : '?');
    }
    return out;
}

/// Sums a wildcard counter's instances whose names contain `luid` (and start with `prefix`).
std::optional<u64> SumInstances(PDH_HCOUNTER counter, const std::string& luid,
                                const std::string& prefix, std::vector<u8>& scratch) {
    DWORD bytes = 0;
    DWORD count = 0;
    PDH_STATUS status =
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0) {
        return std::nullopt;
    }
    scratch.resize(bytes);
    auto* const items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(scratch.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, items);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    u64 sum = 0;
    bool found = false;
    for (DWORD i = 0; i < count; ++i) {
        const std::string name = LowerAscii(items[i].szName);
        if (name.find(luid) == std::string::npos ||
            (!prefix.empty() && name.rfind(prefix, 0) != 0)) {
            continue;
        }
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
            sum += static_cast<u64>((std::max)(items[i].FmtValue.largeValue, LONGLONG{0}));
            found = true;
        }
    }
    return found ? std::optional<u64>{sum} : std::nullopt;
}

void AdapterVideoMemoryLoop(u32 luid_low, s32 luid_high) {
    Common::SetCurrentThreadName("Fritchii:VramOS");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    PDH_HQUERY query{};
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        LOG_WARNING(Common_Memory, "FRITCHII_VRAM_BUDGET_MODE=os: no PDH query; vk budget only");
        return;
    }
    PDH_HCOUNTER adapter_counter{};
    PDH_HCOUNTER process_counter{};
    const bool have_adapter =
        PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0,
                              &adapter_counter) == ERROR_SUCCESS;
    const bool have_process =
        PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0,
                              &process_counter) == ERROR_SUCCESS;
    if (!have_adapter || !have_process) {
        LOG_WARNING(Common_Memory,
                    "FRITCHII_VRAM_BUDGET_MODE=os: the GPU memory counters are missing "
                    "(adapter {}, process {}); vk budget only",
                    have_adapter, have_process);
        PdhCloseQuery(query);
        return;
    }
    // Instance names: "luid_0x00000000_0x0000D1E3_phys_0" and "pid_1234_luid_..._phys_0".
    const std::string luid =
        fmt::format("luid_0x{:08x}_0x{:08x}", static_cast<u32>(luid_high), luid_low);
    const std::string pid = fmt::format("pid_{}_", GetCurrentProcessId());
    std::vector<u8> scratch;
    bool logged = false;
    u32 misses = 0;
    for (;;) {
        if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
            const auto adapter = SumInstances(adapter_counter, luid, {}, scratch);
            const auto process = SumInstances(process_counter, luid, pid, scratch);
            if (adapter && process) {
                g_adapter_video_memory.adapter.store(*adapter, std::memory_order_relaxed);
                g_adapter_video_memory.process.store(*process, std::memory_order_relaxed);
                g_adapter_video_memory.time_ms.store(SteadyMs(), std::memory_order_release);
                if (!logged) {
                    logged = true;
                    LOG_INFO(Common_Memory,
                             "FRITCHII_VRAM_BUDGET_MODE=os: adapter {} dedicated use {} MiB (all "
                             "processes), this process {} MiB",
                             luid, *adapter >> 20, *process >> 20);
                }
            } else if (++misses == 5 && !logged) {
                LOG_WARNING(Common_Memory,
                            "FRITCHII_VRAM_BUDGET_MODE=os: no counter instance for {} (adapter "
                            "{}, process {}); vk budget only",
                            luid, adapter.has_value(), process.has_value());
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}
} // Anonymous namespace
#endif

std::optional<AdapterVideoMemory> GetAdapterVideoMemory(u32 luid_low, s32 luid_high) {
#ifdef _WIN32
    static std::once_flag started;
    std::call_once(started, [luid_low, luid_high] {
        // Detached: it only sleeps and reads counters, and lives as long as the process.
        std::thread(AdapterVideoMemoryLoop, luid_low, luid_high).detach();
    });
    const s64 time = g_adapter_video_memory.time_ms.load(std::memory_order_acquire);
    if (time == 0 || SteadyMs() - time > 5000) {
        return std::nullopt;
    }
    return AdapterVideoMemory{
        .adapter_dedicated = g_adapter_video_memory.adapter.load(std::memory_order_relaxed),
        .process_dedicated = g_adapter_video_memory.process.load(std::memory_order_relaxed),
    };
#else
    static_cast<void>(luid_low);
    static_cast<void>(luid_high);
    return std::nullopt;
#endif
}

void* AllocateZeroPages(u64 bytes) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
#else
    void* const memory = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
#endif
}

void FreeZeroPages(void* memory, u64 bytes) {
    if (memory == nullptr) {
        return;
    }
#ifdef _WIN32
    static_cast<void>(bytes);
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, static_cast<size_t>(bytes));
#endif
}

// FRITCHII_MEM_CENSUS (memory_detect.h): who holds the working set. The "Memory owners" line left
// ~3.7 GB of the private working set unattributed at the Castle Town market (2026-11-16); this
// walks the address space instead of estimating.
namespace MemCensus {
namespace {

struct TagEntry {
    const char* name;
    uintptr_t base;
    u64 size;
};
std::mutex g_tag_mutex;
std::vector<TagEntry> g_tags;             ///< Sorted by base
std::function<std::string()> g_sampler;   ///< Under g_tag_mutex
std::vector<std::pair<const char*, std::function<std::vector<u64>()>>> g_size_sources; ///< Same
std::mutex g_reporter_mutex; ///< Held while the reporters run, so RemoveReporter waits for them
std::vector<std::pair<const void*, std::function<std::string()>>> g_reporters; ///< Under it
std::mutex g_thread_mutex;
std::jthread g_thread;

struct Amount {
    u64 committed{};
    u64 resident{};
    u32 count{};
    void Add(u64 c, u64 r) {
        committed += c;
        resident += r;
    }
};

/// One allocation (an AllocationBase on Windows, a mapping on Linux) after the walk.
struct Allocation {
    uintptr_t base{};
    u64 span{};       ///< Reserved bytes from the base to the end of its last region
    u64 committed{};  ///< Committed and not inside a tagged range
    u64 resident{};
    u32 protect{};    ///< The protection of its largest committed region (Windows)
    std::string name; ///< Image or mapped file
};

/// Everything a census found, the same on every host.
struct Result {
    std::map<std::string, Amount> tagged;
    std::map<std::string, Amount> matched; ///< SetSizeSource matches
    std::vector<Allocation> big_blocks;    ///< Heap blocks with an allocation of their own
    Amount crt_segments;                   ///< Further segments of the CRT heap
    Amount other_segments;                 ///< Further segments of the other heaps
    std::vector<Allocation> images;
    std::vector<Allocation> mapped;
    std::vector<Allocation> exec;
    std::vector<Allocation> other;
    Amount heaps;
    Amount crt_heap;
    Amount stacks;
    std::array<Amount, 6> buckets{}; ///< Other private by reservation size
    u64 reserved{};
    u64 regions{};
    u64 ws_pages{};
    u32 walk_ms{};
    u32 query_ms{};
    std::string heap_text;
    std::string hint_text; ///< Owner hints of the biggest unnamed allocations (Windows)
};

constexpr std::array<const char*, 6> BucketNames{"<64 KiB",  "64 KiB-1 MiB", "1-16 MiB",
                                                 "16-64 MiB", "64-256 MiB",  "256 MiB+"};
size_t BucketOf(u64 span) {
    constexpr u64 KiB = 1024;
    constexpr u64 MiB = KiB * KiB;
    if (span < 64 * KiB) {
        return 0;
    }
    if (span < MiB) {
        return 1;
    }
    if (span < 16 * MiB) {
        return 2;
    }
    if (span < 64 * MiB) {
        return 3;
    }
    return span < 256 * MiB ? 4 : 5;
}

u64 MiBOf(u64 bytes) {
    return bytes >> 20;
}

std::vector<TagEntry> CopyTags() {
    std::scoped_lock lock{g_tag_mutex};
    return g_tags;
}

/// Splits [begin, end) into the tagged parts (added to result.tagged) and returns the untagged
/// bytes, with `resident_of` giving a range's resident bytes.
template <typename ResidentOf>
std::pair<u64, u64> SplitTagged(const std::vector<TagEntry>& tags, uintptr_t begin, uintptr_t end,
                                Result& result, ResidentOf&& resident_of) {
    u64 tagged_bytes = 0;
    u64 tagged_resident = 0;
    auto it = std::upper_bound(tags.begin(), tags.end(), begin,
                               [](uintptr_t value, const TagEntry& tag) { return value < tag.base; });
    if (it != tags.begin()) {
        --it;
    }
    for (; it != tags.end() && it->base < end; ++it) {
        const uintptr_t tag_end = it->base + it->size;
        if (tag_end <= begin) {
            continue;
        }
        const uintptr_t overlap_begin = (std::max)(begin, it->base);
        const uintptr_t overlap_end = (std::min)(end, tag_end);
        const u64 resident = resident_of(overlap_begin, overlap_end);
        result.tagged[it->name].Add(overlap_end - overlap_begin, resident);
        tagged_bytes += overlap_end - overlap_begin;
        tagged_resident += resident;
    }
    return {(end - begin) - tagged_bytes, resident_of(begin, end) - tagged_resident};
}

std::string ProtectText(u32 protect) {
#ifdef _WIN32
    std::string text;
    switch (protect & 0xff) {
    case PAGE_NOACCESS: text = "none"; break;
    case PAGE_READONLY: text = "r"; break;
    case PAGE_READWRITE: text = "rw"; break;
    case PAGE_WRITECOPY: text = "wc"; break;
    case PAGE_EXECUTE: text = "x"; break;
    case PAGE_EXECUTE_READ: text = "rx"; break;
    case PAGE_EXECUTE_READWRITE: text = "rwx"; break;
    case PAGE_EXECUTE_WRITECOPY: text = "xwc"; break;
    default: text = fmt::format("{:#x}", protect & 0xff); break;
    }
    if ((protect & PAGE_GUARD) != 0) {
        text += "+guard";
    }
    if ((protect & PAGE_NOCACHE) != 0) {
        text += "+nocache";
    }
    if ((protect & PAGE_WRITECOMBINE) != 0) {
        text += "+wc";
    }
    return text;
#else
    return fmt::format("{:#x}", protect);
#endif
}

/// SetSizeSource's sizes, in 64 KiB granules, taken one by one as allocations match them.
struct SizeMatcher {
    std::vector<std::pair<std::string, std::multiset<u64>>> sources;

    static u64 Granules(u64 bytes) {
        return (bytes + 0xffff) >> 16;
    }
    void Load() {
        std::vector<std::pair<const char*, std::function<std::vector<u64>()>>> copy;
        {
            std::scoped_lock lock{g_tag_mutex};
            copy = g_size_sources;
        }
        for (auto& [label, source] : copy) {
            std::multiset<u64> sizes;
            for (const u64 size : source()) {
                sizes.insert(Granules(size));
            }
            sources.emplace_back(label, std::move(sizes));
        }
    }
    /// The label of the source an allocation of `span` reserved bytes matches (taking that size).
    const std::string* Take(u64 span) {
        const u64 granules = Granules(span);
        for (auto& [label, sizes] : sources) {
            if (const auto it = sizes.find(granules); it != sizes.end()) {
                sizes.erase(it);
                return &label;
            }
        }
        return nullptr;
    }
};

#ifdef _WIN32
std::string Utf8Of(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::string BaseName(std::string path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Sorted page addresses of the working set; empty when the query failed.
std::vector<uintptr_t> WorkingSetPages() {
    std::vector<ULONG_PTR> entries(size_t{1} << 22);
    for (int attempt = 0;; ++attempt) {
        if (K32QueryWorkingSet(GetCurrentProcess(), entries.data(),
                               static_cast<DWORD>(entries.size() * sizeof(ULONG_PTR)))) {
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH || attempt == 3) {
            return {};
        }
        entries.assign((std::max)(entries.size() * 2, size_t{entries[0]} + (size_t{1} << 18)), 0);
    }
    const size_t count = (std::min)(size_t{entries[0]}, entries.size() - 1);
    std::vector<uintptr_t> pages(count);
    for (size_t i = 0; i < count; ++i) {
        pages[i] = entries[i + 1] & ~uintptr_t{0xfff};
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

/// FRITCHII_MEM_CENSUS=4, once: the CRT heap's busy blocks by size, to tell owners apart by their
/// block sizes (a hash map's nodes, a JIT table's entries, pipeline objects). HeapWalk under
/// HeapLock: every thread that allocates waits for the whole walk (~0.3-1 s for ~3 GB). Nothing is
/// allocated from the heap while it is locked: the tables are made before.
std::string CrtHeapHistogram(HANDLE heap) {
    constexpr size_t Slots = 8192; // exact sizes, open addressing
    struct Slot {
        u64 size{};
        u64 count{};
    };
    std::vector<Slot> slots(Slots);
    std::array<u64, 40> bucket_count{};
    std::array<u64, 40> bucket_bytes{};
    u64 blocks = 0;
    u64 bytes = 0;
    u64 overflow = 0;
    const auto start = std::chrono::steady_clock::now();
    if (!HeapLock(heap)) {
        return "CRT heap walk: HeapLock failed";
    }
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry)) {
        if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0) {
            continue;
        }
        const u64 size = entry.cbData;
        ++blocks;
        bytes += size;
        const size_t bucket = (std::min<size_t>)(size == 0 ? 0 : 64 - std::countl_zero(size), 39);
        ++bucket_count[bucket];
        bucket_bytes[bucket] += size;
        size_t index = static_cast<size_t>((size * 0x9e3779b97f4a7c15ULL) >> 51) % Slots;
        size_t probes = 0;
        while (slots[index].count != 0 && slots[index].size != size && probes < 64) {
            index = (index + 1) % Slots;
            ++probes;
        }
        if (slots[index].count == 0 || slots[index].size == size) {
            slots[index].size = size;
            ++slots[index].count;
        } else {
            ++overflow;
        }
    }
    HeapUnlock(heap);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::string text = fmt::format("CRT heap walk ({} ms): {} busy blocks, {} MiB; by size "
                                   "(blocks/MiB):",
                                   ms, blocks, MiBOf(bytes));
    for (size_t i = 0; i < bucket_count.size(); ++i) {
        if (bucket_count[i] != 0) {
            text += fmt::format(" <{} {}/{}", u64{1} << i, bucket_count[i], MiBOf(bucket_bytes[i]));
        }
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
        return a.size * a.count > b.size * b.count;
    });
    text += "; top exact sizes (bytes x blocks = MiB):";
    for (size_t i = 0; i < 16 && slots[i].count != 0; ++i) {
        text += fmt::format(" {}x{}={}", slots[i].size, slots[i].count,
                            MiBOf(slots[i].size * slots[i].count));
    }
    if (overflow != 0) {
        text += fmt::format(" ({} blocks of sizes not tabled)", overflow);
    }
    return text;
}

/// Owner hints: what the biggest unnamed allocations point at. Objects with a vtable or a
/// callback hold pointers into the image whose code made them (the driver's objects into
/// nvoglv64.dll / nvgpucomp64.dll, ours into the exe), and allocator free lists point into their
/// own allocation. Reads up to 512 resident pages of each (ReadProcessMemory, so a page freed
/// meanwhile fails the read instead of faulting), never a page that is not in the working set.
std::string OwnerHints(const Result& result, const std::vector<uintptr_t>& pages) {
    if (pages.empty()) {
        return {};
    }
    struct Range {
        uintptr_t begin;
        uintptr_t end;
        const std::string* name;
    };
    std::vector<Range> images;
    for (const Allocation& a : result.images) {
        images.push_back({a.base, a.base + a.span, &a.name});
    }
    std::sort(images.begin(), images.end(),
              [](const Range& a, const Range& b) { return a.begin < b.begin; });
    struct Group {
        std::string label;
        std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
        u64 resident{};
    };
    std::vector<Group> groups;
    std::vector<const Allocation*> big;
    Group bucket{"16-64 MiB reservations", {}, 0};
    for (const Allocation& a : result.other) {
        if (a.resident >= (u64{32} << 20)) {
            big.push_back(&a);
        } else if (BucketOf(a.span) == 3) {
            bucket.ranges.emplace_back(a.base, a.base + a.span);
            bucket.resident += a.resident;
        }
    }
    std::sort(big.begin(), big.end(),
              [](const Allocation* a, const Allocation* b) { return a->resident > b->resident; });
    for (size_t i = 0; i < big.size() && i < 4; ++i) {
        groups.push_back({fmt::format("{:x} span {}", big[i]->base, MiBOf(big[i]->span)),
                          {{big[i]->base, big[i]->base + big[i]->span}},
                          big[i]->resident});
    }
    if (bucket.resident >= (u64{32} << 20)) {
        groups.push_back(std::move(bucket));
    }
    const auto image_of = [&images](u64 value) -> const std::string* {
        auto it = std::upper_bound(images.begin(), images.end(), value,
                                   [](u64 v, const Range& r) { return v < r.begin; });
        if (it == images.begin()) {
            return nullptr;
        }
        --it;
        return value < it->end ? it->name : nullptr;
    };
    std::string text;
    std::array<u64, 512> words{};
    for (const Group& group : groups) {
        std::vector<uintptr_t> sample;
        for (const auto& [begin, end] : group.ranges) {
            const auto low = std::lower_bound(pages.begin(), pages.end(), begin);
            const auto high = std::lower_bound(low, pages.end(), end);
            sample.insert(sample.end(), low, high);
        }
        if (sample.empty()) {
            continue;
        }
        const size_t step = (std::max)(size_t{1}, sample.size() / 512);
        std::map<const std::string*, u64> hits;
        u64 read_pages = 0;
        u64 zero_pages = 0;
        u64 nonzero = 0;
        u64 inside = 0;
        for (size_t i = 0; i < sample.size(); i += step) {
            SIZE_T read = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(sample[i]),
                                   words.data(), sizeof(words), &read) ||
                read != sizeof(words)) {
                continue;
            }
            ++read_pages;
            bool any = false;
            for (const u64 word : words) {
                if (word == 0) {
                    continue;
                }
                any = true;
                ++nonzero;
                bool own = false;
                for (const auto& [begin, end] : group.ranges) {
                    if (word >= begin && word < end) {
                        own = true;
                        break;
                    }
                }
                if (own) {
                    ++inside;
                } else if (const std::string* name = image_of(word)) {
                    ++hits[name];
                }
            }
            zero_pages += any ? 0 : 1;
        }
        if (read_pages == 0) {
            continue;
        }
        std::vector<std::pair<u64, const std::string*>> ranked;
        for (const auto& [name, count] : hits) {
            ranked.emplace_back(count, name);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string top;
        for (size_t i = 0; i < ranked.size() && i < 4; ++i) {
            top += fmt::format("{}{} {:.1f}", i == 0 ? "" : ", ", *ranked[i].second,
                               static_cast<double>(ranked[i].first) /
                                   static_cast<double>(read_pages));
        }
        std::array<u64, 6> head{};
        SIZE_T head_read = 0;
        std::string head_text = "unreadable";
        if (ReadProcessMemory(GetCurrentProcess(),
                              reinterpret_cast<const void*>(group.ranges.front().first),
                              head.data(), sizeof(head), &head_read) &&
            head_read == sizeof(head)) {
            head_text = fmt::format("{:x} {:x} {:x} {:x} {:x} {:x}", head[0], head[1], head[2],
                                    head[3], head[4], head[5]);
        }
        text += fmt::format(
            "{}{} ({} resident, {} allocations): {} pages read, {} all zero; per page {:.0f} "
            "non-zero words, {:.1f} into itself, into images {}; first words {}",
            text.empty() ? "" : "; ", group.label, MiBOf(group.resident), group.ranges.size(),
            read_pages, zero_pages,
            static_cast<double>(nonzero) / static_cast<double>(read_pages),
            static_cast<double>(inside) / static_cast<double>(read_pages),
            top.empty() ? "none" : top, head_text);
    }
    return text;
}

void Walk(int mode, Result& result) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<uintptr_t> pages;
    if (mode != 3) {
        const Nx2Hitch::Scope timed{Nx2Hitch::Cause::MemorySample};
        pages = WorkingSetPages();
    }
    result.ws_pages = pages.size();
    // The query's own buffer is gone again; the pages vector is the census's own.
    std::vector<TagEntry> tags = CopyTags();
    if (!pages.empty()) {
        tags.push_back({"census scratch", reinterpret_cast<uintptr_t>(pages.data()),
                        pages.capacity() * sizeof(uintptr_t)});
        std::sort(tags.begin(), tags.end(),
                  [](const TagEntry& a, const TagEntry& b) { return a.base < b.base; });
    }
    result.query_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
    const auto resident_of = [&pages](uintptr_t begin, uintptr_t end) -> u64 {
        const auto low = std::lower_bound(pages.begin(), pages.end(), begin);
        const auto high = std::lower_bound(low, pages.end(), end);
        return static_cast<u64>(high - low) * 4096;
    };

#ifdef _MSC_VER
    const auto crt_heap = static_cast<uintptr_t>(_get_heap_handle());
#else
    const auto crt_heap = reinterpret_cast<uintptr_t>(GetProcessHeap());
#endif
    std::vector<HANDLE> heap_handles(256);
    const DWORD heap_count =
        GetProcessHeaps(static_cast<DWORD>(heap_handles.size()), heap_handles.data());
    heap_handles.resize((std::min)(static_cast<size_t>(heap_count), heap_handles.size()));
    std::vector<uintptr_t> heaps;
    for (const HANDLE heap : heap_handles) {
        heaps.push_back(reinterpret_cast<uintptr_t>(heap));
    }
    std::sort(heaps.begin(), heaps.end());

    SizeMatcher matcher;
    matcher.Load();

    struct Current {
        Allocation allocation;
        DWORD type{};
        bool exec{};
        bool guard{};
        bool first_rw{}; ///< Its first page is committed read/write (a heap header can be read)
        u64 largest_region{};
    };
    Current current{};
    bool have_current = false;
    const auto flush = [&] {
        if (!have_current) {
            return;
        }
        have_current = false;
        Allocation& a = current.allocation;
        if (a.committed == 0) {
            return;
        }
        if (current.type == MEM_IMAGE) {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(reinterpret_cast<HMODULE>(a.base), path, MAX_PATH) != 0) {
                a.name = BaseName(Utf8Of(path));
            }
            if (a.name.empty()) {
                a.name = fmt::format("image@{:x}", a.base);
            }
            result.images.push_back(std::move(a));
            return;
        }
        if (current.type == MEM_MAPPED) {
            wchar_t path[MAX_PATH]{};
            if (K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path,
                                      MAX_PATH) != 0) {
                a.name = BaseName(Utf8Of(path));
            }
            if (a.name.empty()) {
                a.name = "section";
            }
            result.mapped.push_back(std::move(a));
            return;
        }
        if (std::binary_search(heaps.begin(), heaps.end(), a.base)) {
            result.heaps.Add(a.committed, a.resident);
            ++result.heaps.count;
            if (a.base == crt_heap) {
                result.crt_heap.Add(a.committed, a.resident);
            }
            return;
        }
        if (current.exec) {
            result.exec.push_back(std::move(a));
            return;
        }
        if (current.guard) {
            result.stacks.Add(a.committed, a.resident);
            ++result.stacks.count;
            return;
        }
        // Windows heaps keep their memory in allocations of their own past the first segment:
        // further segments (the CRT heap's grow to 1 GiB reservations) start with a segment header
        // (signature 0xffeeffee at +0x10, the heap at +0x28), and blocks over ~508 KiB get one
        // each, starting with their size: CommitSize at +0x20, ReserveSize at +0x28 (x64 layout).
        // Read with ReadProcessMemory: an allocation freed since the walk saw it fails the read
        // instead of faulting.
        if (current.first_rw) {
            struct Header {
                u64 words[6];
            } header{};
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a.base),
                                  &header, sizeof(header), &read) &&
                read == sizeof(header)) {
                const u32 signature = static_cast<u32>(header.words[2]);
                const u64 heap = header.words[5];
                if (signature == 0xffeeffee &&
                    std::binary_search(heaps.begin(), heaps.end(), static_cast<uintptr_t>(heap))) {
                    (heap == crt_heap ? result.crt_segments : result.other_segments)
                        .Add(a.committed, a.resident);
                    ++(heap == crt_heap ? result.crt_segments : result.other_segments).count;
                    return;
                }
                const u64 commit = header.words[4];
                const u64 reserve = header.words[5];
                constexpr u64 Slack = 0x10000;
                if (commit != 0 && commit <= reserve && reserve <= a.span + Slack &&
                    reserve + Slack >= a.span && commit <= a.committed + Slack &&
                    commit + Slack >= a.committed) {
                    result.big_blocks.push_back(std::move(a));
                    return;
                }
            }
        }
        if (const std::string* label = matcher.Take(a.span)) {
            Amount& matched = result.matched[*label];
            matched.Add(a.committed, a.resident);
            ++matched.count;
            return;
        }
        Amount& bucket = result.buckets[BucketOf(a.span)];
        bucket.Add(a.committed, a.resident);
        ++bucket.count;
        result.other.push_back(std::move(a));
    };

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const auto max_address = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < max_address &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (end <= address) {
            break;
        }
        address = end;
        ++result.regions;
        if (mbi.State == MEM_FREE) {
            continue;
        }
        const auto allocation_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        if (!have_current || current.allocation.base != allocation_base) {
            flush();
            current = Current{};
            current.allocation.base = allocation_base;
            current.type = mbi.Type;
            have_current = true;
        }
        current.allocation.span = end - allocation_base;
        if (mbi.State == MEM_RESERVE) {
            result.reserved += mbi.RegionSize;
            continue;
        }
        constexpr DWORD Executable =
            PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & PAGE_GUARD) != 0) {
            current.guard = true;
        }
        if ((mbi.Protect & Executable) != 0) {
            current.exec = true;
        }
        if (begin == allocation_base && (mbi.Protect & 0xff) == PAGE_READWRITE &&
            (mbi.Protect & PAGE_GUARD) == 0) {
            current.first_rw = true;
        }
        if (mbi.RegionSize > current.largest_region) {
            current.largest_region = mbi.RegionSize;
            current.allocation.protect = mbi.Protect;
        }
        const auto [untagged, untagged_resident] =
            SplitTagged(tags, begin, end, result, resident_of);
        current.allocation.committed += untagged;
        current.allocation.resident += untagged_resident;
    }
    flush();
    result.walk_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
    result.hint_text = OwnerHints(result, pages);

    if (mode == 2) {
        // Locks each heap for its summary (EDEN_NX2_MEM_HEAPS measured ~250 ms for a 2.5 GB CRT
        // heap): only for a memory hunt.
        std::vector<std::pair<u64, std::string>> rows;
        for (const HANDLE heap : heap_handles) {
            HEAP_SUMMARY summary{};
            summary.cb = sizeof(summary);
            if (!HeapSummary(heap, 0, &summary)) {
                continue;
            }
            const auto base = reinterpret_cast<uintptr_t>(heap);
            rows.emplace_back(summary.cbCommitted,
                              fmt::format("{}{:x} {}/{}", base == crt_heap ? "CRT " : "", base,
                                          MiBOf(summary.cbCommitted),
                                          MiBOf(summary.cbAllocated)));
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < rows.size() && i < 12; ++i) {
            result.heap_text += (i == 0 ? "" : ", ") + rows[i].second;
        }
    }
    static bool histogram_done = false;
    if (mode == 4 && !histogram_done) {
        histogram_done = true;
        result.heap_text = CrtHeapHistogram(reinterpret_cast<HANDLE>(crt_heap));
    }
}
#elif defined(__linux__)
void Walk(int mode, Result& result) {
    const auto start = std::chrono::steady_clock::now();
    static_cast<void>(mode);
    const std::vector<TagEntry> tags = CopyTags();
    std::ifstream file{"/proc/self/smaps"};
    std::string line;
    struct Mapping {
        uintptr_t begin{};
        uintptr_t end{};
        std::string perms;
        std::string path;
        u64 rss{};
        bool valid{};
    };
    Mapping mapping;
    const auto flush = [&] {
        if (!mapping.valid) {
            return;
        }
        mapping.valid = false;
        ++result.regions;
        const u64 size = mapping.end - mapping.begin;
        // Rss of a mapping is not split by address: a tagged mapping goes to its tag whole.
        for (const TagEntry& tag : tags) {
            if (mapping.begin >= tag.base && mapping.begin < tag.base + tag.size) {
                result.tagged[tag.name].Add(size, mapping.rss);
                return;
            }
        }
        Allocation a{.base = mapping.begin, .span = size, .committed = size,
                     .resident = mapping.rss, .protect = 0, .name = {}};
        const std::string& path = mapping.path;
        const bool exec = mapping.perms.find('x') != std::string::npos;
        if (path.find(".so") != std::string::npos) {
            const size_t slash = path.find_last_of('/');
            a.name = slash == std::string::npos ? path : path.substr(slash + 1);
            result.images.push_back(std::move(a));
        } else if (path == "[heap]") {
            result.heaps.Add(size, mapping.rss);
            result.crt_heap.Add(size, mapping.rss);
            ++result.heaps.count;
        } else if (path.rfind("[stack", 0) == 0) {
            result.stacks.Add(size, mapping.rss);
            ++result.stacks.count;
        } else if (!path.empty() && path[0] != '[') {
            const size_t slash = path.find_last_of('/');
            a.name = slash == std::string::npos ? path : path.substr(slash + 1);
            result.mapped.push_back(std::move(a));
        } else if (exec) {
            result.exec.push_back(std::move(a));
        } else if (mapping.perms.rfind("---", 0) == 0) {
            result.reserved += size;
        } else {
            Amount& bucket = result.buckets[BucketOf(size)];
            bucket.Add(size, mapping.rss);
            ++bucket.count;
            result.other.push_back(std::move(a));
        }
    };
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const char first = line[0];
        const bool header = (first >= '0' && first <= '9') || (first >= 'a' && first <= 'f');
        const size_t dash = line.find('-');
        if (header && dash != std::string::npos && dash < line.find(' ')) {
            flush();
            std::istringstream in{line};
            std::string range, offset, device, inode;
            in >> range >> mapping.perms >> offset >> device >> inode;
            std::getline(in, mapping.path);
            mapping.path.erase(0, mapping.path.find_first_not_of(' '));
            mapping.begin = std::strtoull(range.c_str(), nullptr, 16);
            mapping.end = std::strtoull(range.c_str() + dash + 1, nullptr, 16);
            mapping.rss = 0;
            mapping.valid = true;
        } else if (line.rfind("Rss:", 0) == 0) {
            mapping.rss = std::strtoull(line.c_str() + 4, nullptr, 10) * 1024;
            result.ws_pages += mapping.rss / 4096;
        }
    }
    flush();
    result.walk_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}
#else
void Walk(int, Result&) {}
#endif

std::string Top(std::vector<Allocation>& list, size_t count, bool with_base) {
    std::sort(list.begin(), list.end(),
              [](const Allocation& a, const Allocation& b) { return a.committed > b.committed; });
    std::string text;
    for (size_t i = 0; i < list.size() && i < count; ++i) {
        const Allocation& a = list[i];
        text += i == 0 ? "" : ", ";
        if (with_base) {
            text += fmt::format("{:x} span {} {}/{} {}", a.base, MiBOf(a.span), MiBOf(a.committed),
                                MiBOf(a.resident), ProtectText(a.protect));
        } else {
            text += fmt::format("{} {}/{}", a.name, MiBOf(a.committed), MiBOf(a.resident));
        }
    }
    return text;
}

Amount Sum(const std::vector<Allocation>& list) {
    Amount sum{};
    for (const Allocation& a : list) {
        sum.Add(a.committed, a.resident);
        ++sum.count;
    }
    return sum;
}

/// Same-named images summed (a DLL loaded twice is rare; drivers load several of their own).
std::vector<Allocation> MergeByName(const std::vector<Allocation>& list) {
    std::map<std::string, Allocation> merged;
    for (const Allocation& a : list) {
        Allocation& m = merged[a.name];
        m.name = a.name;
        m.committed += a.committed;
        m.resident += a.resident;
    }
    std::vector<Allocation> out;
    for (auto& [name, a] : merged) {
        out.push_back(std::move(a));
    }
    return out;
}

void Census(int mode, const char* reason) {
    Result result;
    Walk(mode, result);
    const ProcessMemoryUsage usage = GetProcessMemoryUsage();
    std::string tagged;
    for (const auto& [name, amount] : result.tagged) {
        tagged += fmt::format("{}{} {}/{}", tagged.empty() ? "" : ", ", name,
                              MiBOf(amount.committed), MiBOf(amount.resident));
    }
    const Amount images = Sum(result.images);
    const Amount mapped = Sum(result.mapped);
    const Amount exec = Sum(result.exec);
    const Amount other = Sum(result.other);
    const Amount big_blocks = Sum(result.big_blocks);
    u64 attributed = images.resident + mapped.resident + exec.resident + other.resident +
                     result.heaps.resident + result.stacks.resident + big_blocks.resident +
                     result.crt_segments.resident + result.other_segments.resident;
    for (const auto& [name, amount] : result.tagged) {
        attributed += amount.resident;
    }
    std::string matched;
    for (const auto& [name, amount] : result.matched) {
        matched += fmt::format("{}{} {}/{} ({})", matched.empty() ? "" : ", ", name,
                               MiBOf(amount.committed), MiBOf(amount.resident), amount.count);
        attributed += amount.resident;
    }
    LOG_INFO(HW_Memory,
             "Memory census ({}, FRITCHII_MEM_CENSUS={}): {} regions walked in {} ms (working set "
             "query {} ms, {} MiB of pages); working set {} MiB (peak {}), private {}, private "
             "commit {} (peak {}), reserved {} GiB; committed/resident MiB: {}; images {}/{} ({}); "
             "mapped other {}/{} ({}); exec private {}/{} ({} allocations); heaps {}/{} ({} first "
             "segments, CRT {}/{}); heap segments CRT {}/{} ({}), other heaps {}/{} ({}); heap "
             "big blocks {}/{} ({}); size matches: {}; stacks {}/{} ({}); other private {}/{} ({} "
             "allocations); resident not in any of them {}",
             reason, mode, result.regions, result.walk_ms, result.query_ms,
             MiBOf(result.ws_pages * 4096), MiBOf(usage.working_set),
             MiBOf(usage.peak_working_set), MiBOf(usage.private_working_set),
             MiBOf(usage.private_bytes), MiBOf(usage.peak_private_bytes), result.reserved >> 30,
             tagged.empty() ? "nothing tagged" : tagged, MiBOf(images.committed),
             MiBOf(images.resident), images.count, MiBOf(mapped.committed), MiBOf(mapped.resident),
             mapped.count, MiBOf(exec.committed), MiBOf(exec.resident), exec.count,
             MiBOf(result.heaps.committed), MiBOf(result.heaps.resident), result.heaps.count,
             MiBOf(result.crt_heap.committed), MiBOf(result.crt_heap.resident),
             MiBOf(result.crt_segments.committed), MiBOf(result.crt_segments.resident),
             result.crt_segments.count, MiBOf(result.other_segments.committed),
             MiBOf(result.other_segments.resident), result.other_segments.count,
             MiBOf(big_blocks.committed), MiBOf(big_blocks.resident), big_blocks.count,
             matched.empty() ? "none" : matched, MiBOf(result.stacks.committed), MiBOf(result.stacks.resident), result.stacks.count,
             MiBOf(other.committed), MiBOf(other.resident), other.count,
             static_cast<s64>(result.ws_pages * 4096 - (std::min)(result.ws_pages * 4096, attributed)) >> 20);
    std::vector<Allocation> merged_images = MergeByName(result.images);
    std::vector<Allocation> merged_mapped = MergeByName(result.mapped);
    LOG_INFO(HW_Memory, "Memory census ({}) images, committed/resident MiB: {}", reason,
             Top(merged_images, 14, false));
    LOG_INFO(HW_Memory, "Memory census ({}) mapped other: {}", reason,
             Top(merged_mapped, 6, false));
    std::string buckets;
    for (size_t i = 0; i < result.buckets.size(); ++i) {
        const Amount& b = result.buckets[i];
        buckets += fmt::format("{}{} {}/{} ({})", i == 0 ? "" : ", ", BucketNames[i],
                               MiBOf(b.committed), MiBOf(b.resident), b.count);
    }
    LOG_INFO(HW_Memory,
             "Memory census ({}) private: exec (JIT code caches, driver code) {}; other by "
             "reservation size {}; largest other: {}; largest heap big blocks: {}",
             reason, Top(result.exec, 10, true), buckets, Top(result.other, 10, true),
             Top(result.big_blocks, 8, true));
    if (!result.hint_text.empty()) {
        LOG_INFO(HW_Memory, "Memory census ({}) owner hints: {}", reason, result.hint_text);
    }
    std::string extra = Nx2Hitch::JitCompileCensus();
    std::function<std::string()> sampler;
    {
        std::scoped_lock lock{g_tag_mutex};
        sampler = g_sampler;
    }
    if (sampler) {
        extra += "; " + sampler();
    }
    {
        std::scoped_lock lock{g_reporter_mutex};
        for (const auto& [key, reporter] : g_reporters) {
            if (std::string text = reporter(); !text.empty()) {
                extra += "; " + text;
            }
        }
    }
    if (!result.heap_text.empty()) {
        extra += "; heaps (committed/allocated MiB, or the CRT heap walk): " + result.heap_text;
    }
    LOG_INFO(HW_Memory, "Memory census ({}) more: {}", reason, extra);
}

u64 WorkingSetNow() {
#ifdef __linux__
    std::ifstream statm{"/proc/self/statm"};
    u64 size = 0;
    u64 resident = 0;
    statm >> size >> resident;
    return resident * 4096;
#else
    return GetProcessMemoryUsage().working_set;
#endif
}

void Run(std::stop_token stop, int mode) {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable_any wake;
    constexpr u64 RiseBytes = u64{512} << 20;
    auto last_regular = std::chrono::steady_clock::now();
    auto last_peak = last_regular - 1h;
    u64 last_census_ws = WorkingSetNow();
    u64 highest_ws = last_census_ws;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, 1s, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const u64 ws = WorkingSetNow();
        const bool climbing = ws > highest_ws;
        highest_ws = (std::max)(highest_ws, ws);
        // A load peak: the working set rose 512 MiB past the last census and is the highest yet
        // (one every 5 s at most, so a long climb gets a few).
        if (climbing && ws >= last_census_ws + RiseBytes && now - last_peak >= 5s) {
            last_peak = now;
            Census(mode, "peak");
            last_census_ws = WorkingSetNow();
            continue;
        }
        if (now - last_regular >= 30s) {
            last_regular = now;
            Census(mode, "30 s");
            last_census_ws = WorkingSetNow();
        }
    }
}

} // Anonymous namespace

int Mode() {
    static const int mode = [] {
        const char* const value = std::getenv("FRITCHII_MEM_CENSUS");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    return mode;
}

void Tag(const char* name, const void* base, u64 size) {
    if (Mode() == 0 || base == nullptr || size == 0) {
        return;
    }
    const TagEntry entry{name, reinterpret_cast<uintptr_t>(base), size};
    std::scoped_lock lock{g_tag_mutex};
    const auto it = std::lower_bound(
        g_tags.begin(), g_tags.end(), entry.base,
        [](const TagEntry& tag, uintptr_t value) { return tag.base < value; });
    if (it != g_tags.end() && it->base == entry.base) {
        *it = entry;
        return;
    }
    g_tags.insert(it, entry);
}

void Untag(const void* base) {
    if (Mode() == 0) {
        return;
    }
    std::scoped_lock lock{g_tag_mutex};
    std::erase_if(g_tags, [base](const TagEntry& tag) {
        return tag.base == reinterpret_cast<uintptr_t>(base);
    });
}

void SetGuestSampler(std::function<std::string()> sampler) {
    std::scoped_lock lock{g_tag_mutex};
    g_sampler = std::move(sampler);
}

void SetSizeSource(const char* label, std::function<std::vector<u64>()> source) {
    std::scoped_lock lock{g_tag_mutex};
    std::erase_if(g_size_sources, [label](const auto& entry) {
        return std::string_view{entry.first} == label;
    });
    if (source) {
        g_size_sources.emplace_back(label, std::move(source));
    }
}

void AddReporter(const void* key, std::function<std::string()> reporter) {
    if (Mode() == 0 || key == nullptr || !reporter) {
        return;
    }
    std::scoped_lock lock{g_reporter_mutex};
    std::erase_if(g_reporters, [key](const auto& entry) { return entry.first == key; });
    g_reporters.emplace_back(key, std::move(reporter));
}

void RemoveReporter(const void* key) {
    if (Mode() == 0) {
        return;
    }
    std::scoped_lock lock{g_reporter_mutex};
    std::erase_if(g_reporters, [key](const auto& entry) { return entry.first == key; });
}

std::vector<u8> ResidentPages(const void* base, u64 bytes) {
    const size_t count = static_cast<size_t>((bytes + 4095) / 4096);
    std::vector<u8> resident(count, 1);
#ifdef _WIN32
    constexpr size_t Batch = size_t{1} << 16;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info((std::min)(count, Batch));
    const auto first = reinterpret_cast<uintptr_t>(base) & ~uintptr_t{0xfff};
    for (size_t done = 0; done < count; done += Batch) {
        const size_t n = (std::min)(Batch, count - done);
        for (size_t i = 0; i < n; ++i) {
            info[i] = {};
            info[i].VirtualAddress = reinterpret_cast<void*>(first + (done + i) * 4096);
        }
        if (!K32QueryWorkingSetEx(GetCurrentProcess(), info.data(),
                                  static_cast<DWORD>(n * sizeof(info[0])))) {
            continue; // left as resident
        }
        for (size_t i = 0; i < n; ++i) {
            resident[done + i] = info[i].VirtualAttributes.Valid ? 1 : 0;
        }
    }
#endif
    return resident;
}

void Start() {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    std::scoped_lock lock{g_thread_mutex};
    if (g_thread.joinable()) {
        return;
    }
    LOG_INFO(HW_Memory,
             "Memory census on (FRITCHII_MEM_CENSUS={}): every 30 s and at working set peaks",
             mode);
    g_thread = std::jthread{[mode](std::stop_token stop) {
        Common::SetCurrentThreadName("MemCensus");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        Run(stop, mode);
    }};
}

void Stop() {
    std::jthread thread;
    {
        std::scoped_lock lock{g_thread_mutex};
        thread = std::move(g_thread);
    }
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
}

} // namespace MemCensus

} // namespace Common
