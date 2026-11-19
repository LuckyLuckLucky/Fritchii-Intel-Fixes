// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.cpp */
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <windows.h>
#include <mutex>
#else
#include <sys/mman.h>
#endif

#include <cstdlib>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/sparse_large_vector.h"

namespace Common {

#ifdef _WIN32
static std::vector<std::pair<u64, u64>> vector_regions {};
static std::mutex vector_regions_mutex {};

// Workaround for handling non-commited memory accessed by Dynarmic; usually result of an error
static LONG WINAPI FakePageFaultHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    u64 exception_addr = static_cast<u64>(info->ExceptionRecord->ExceptionInformation[1]);

    if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionInformation[0] == 1) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    u64 addr = 0, addr2 = 0;

    {
        std::lock_guard lock(vector_regions_mutex);
        for (auto region: vector_regions) {
            auto addr_shifted = exception_addr >> HostPageBits;
            if (region.first <= addr_shifted && addr_shifted < region.second) {
                addr = addr_shifted;
            }

            // Page-boundary accesses
            if (auto addr_ = (exception_addr + 0x40) >> HostPageBits; addr_ != addr_shifted && region.first <= addr_ && addr_ < region.second) {
                addr2 = addr_;
            }

            if (addr != 0 || addr2 != 0) {
                break;
            }
        }
    }

    if (addr == 0 && addr2 == 0) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LOG_ERROR(HW_Memory, "Accessing an unallocated region of a SparseLargeVector at {:#x}; this shouldn't happen and is likely a Dynarmic error!", exception_addr);

    // Commit this region
    if (addr != 0) {
        if (!CommitVectorPage(addr << HostPageBits)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    // Commit next region if needed
    if (addr2 != 0) {
        if (!CommitVectorPage(addr2 << HostPageBits)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// Pages are always committed writable: the fault handler can run after another thread committed
// the same page and is storing entries into it, so it must not take write access away.
bool CommitVectorPage(uintptr_t addr) noexcept {
    MEMORY_BASIC_INFORMATION info {};
    auto res = VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info));
    if (res == 0) {
        LOG_CRITICAL(HW_Memory, "Failed to query large buffer region at {:#x} with error {}, will try committing anyway", addr, GetLastError());
    } else if (info.State == MEM_COMMIT) {
        return true;
    } else if (info.State != MEM_RESERVE) {
        LOG_ERROR(HW_Memory, "Tried to commit an unreserved large buffer region at {:#x} that is not mapped (state {:#x})", addr, info.State);
        return false;
    }

    // Quiet: this runs in the fault handler too. The failure below is logged once it is final.
    if (!CommitPagesWaiting(reinterpret_cast<void*>(addr), HostPageSize, true)) {
        LOG_ERROR(HW_Memory, "Failed to commit large buffer region at {:#x}, error {}", addr, GetLastError());
        return false;
    }

    return true;
}
#endif

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

void* AllocateMemoryPages(std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        LOG_WARNING(HW_Memory, "Allocating unaligned large vector with size {:#x}; aligning to {} page size", size, page);
        size = AlignUp(size, page);
    }

#ifdef _WIN32
    // We will never use this memory entirely so instead of committing it up front let's just reserve it and commit each page individually
    void* base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);

    if (base != nullptr) {
        std::lock_guard lock(vector_regions_mutex);
        vector_regions.emplace_back(reinterpret_cast<u64>(base) >> HostPageBits, (reinterpret_cast<u64>(base) + size) >> HostPageBits);

        static std::once_flag flag;
        std::call_once(flag, []() { AddVectoredExceptionHandler(1, FakePageFaultHandler); });
    } else {
        // Try committing everything instead??
        LOG_WARNING(HW_Memory, "Failed to reserve large vector region with error {}, trying to commit instead..", GetLastError());
        base = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
    }
    ASSERT_MSG(base, "Failed to reserve {:#x} sized region with error {}", size, GetLastError());
#else
    void* base = mmap(nullptr, size, PROT_READ, MAP_ANON | MAP_PRIVATE | MAP_NOCORE, -1, 0);
    if (base == MAP_FAILED)
        base = nullptr;
    ASSERT_MSG(base, "Failed to allocate {:#x} sized region with error {}", size, strerror(errno));
#endif
    return base;
}

namespace {
bool SwitchOn(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}
} // namespace

bool RamTableLazy() noexcept {
    static const bool on = [] {
        const bool value = SwitchOn("FRITCHII_RAM_PT_LAZY");
        if (value) {
            LOG_INFO(HW_Memory, "FRITCHII_RAM_PT_LAZY=1: page table walks skip uncommitted pages");
        }
        return value;
    }();
    return on;
}

bool RamTableTrim() noexcept {
    static const bool on = [] {
        const bool value = SwitchOn("FRITCHII_RAM_PT_TRIM");
        if (value) {
            LOG_INFO(HW_Memory, "FRITCHII_RAM_PT_TRIM=1: unmaps give back page table pages left "
                                "without a mapped entry");
        }
        return value;
    }();
    return on;
}

void DiscardTablePages(void* base, std::size_t bytes) noexcept {
    if (base == nullptr || bytes == 0) {
        return;
    }
#ifdef _WIN32
    // MEM_RESET: the contents need not be kept (no page file write); still committed. Unlocking
    // pages that are not locked takes them out of the working set, so the RAM goes now, not at the
    // next trim. A page read again holds zeros (the old zeros, or a fresh zero page).
    if (VirtualAlloc(base, bytes, MEM_RESET, PAGE_READWRITE) == nullptr) {
        return;
    }
    VirtualUnlock(base, bytes);
#else
    madvise(base, bytes, MADV_DONTNEED);
#endif
}

bool CensusOn() noexcept {
    return MemCensus::Mode() != 0;
}

void CensusTagTable(const char* name, const void* key, const void* base, std::size_t bytes,
                    std::function<std::string()> reporter) {
    if (!CensusOn()) {
        return;
    }
    MemCensus::Tag(name, base, bytes);
    MemCensus::AddReporter(key, std::move(reporter));
}

void CensusUntagTable(const void* key, const void* base) {
    if (!CensusOn()) {
        return;
    }
    MemCensus::RemoveReporter(key);
    MemCensus::Untag(base);
}

std::vector<u8> TableResidentPages(const void* base, std::size_t bytes) {
    return MemCensus::ResidentPages(base, bytes);
}

void NoteTableCommit(const char* name, const char* site, std::size_t first_index,
                     std::size_t end_index, [[maybe_unused]] std::size_t entry_size,
                     std::size_t pages) {
    // Rare (a big map or a walk over a wide range): the first 64, then every 64th. Entries are
    // per 4 KiB page of address space.
    static std::atomic<u32> count{};
    const u32 n = count.fetch_add(1, std::memory_order_relaxed);
    if (n >= 64 && n % 64 != 0) {
        return;
    }
    LOG_INFO(HW_Memory,
             "Page table commit ({}): {} by {} committed {} KiB of entries for {:#x}..{:#x} ({} "
             "MiB of address space){}",
             n + 1, name, site != nullptr ? site : "?", pages * 4, u64{first_index} << 12,
             u64{end_index} << 12, (u64{end_index - first_index} << 12) >> 20,
             n >= 64 ? " (every 64th logged)" : "");
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        size = AlignUp(size, page);
    }
    if (!base)
        return;
#ifdef _WIN32
    std::lock_guard lock(vector_regions_mutex);
    // Regions are kept in host pages.
    std::erase_if(vector_regions, [base](const auto& r) {return r.first == reinterpret_cast<u64>(base) >> HostPageBits; });
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common
