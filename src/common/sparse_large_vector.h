// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.h */
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"

namespace Common {

#ifdef _WIN32
constexpr u64 HostPageSize = 0x1000;
constexpr u64 HostPageBits = 12;
constexpr u64 HostPageMask = ~(HostPageSize - 1);
bool CommitVectorPage(uintptr_t addr) noexcept;
#else
inline const u64 HostPageSize = sysconf(_SC_PAGESIZE);
inline const u64 HostPageBits = std::countr_zero(HostPageSize);
inline const u64 HostPageMask = ~(HostPageSize - 1);
#endif

void* AllocateMemoryPages(std::size_t size) noexcept;
void FreeMemoryPages(void* base, std::size_t size) noexcept;

// Fritchii RAM work on the big page tables (the guest page table: 1 GiB reserved for a 39-bit
// address space; the GPU's small-page table: 1 GiB for 40 bits).
/// FRITCHII_RAM_PT_LAZY (boot, default 0): walks that only read the guest page table (protect,
/// block reads and writes, cache operations, the debug and cached marks) skip its uncommitted
/// pages (every entry there is unmapped) instead of committing them first.
[[nodiscard]] bool RamTableLazy() noexcept;
/// FRITCHII_RAM_PT_TRIM (boot, default 0): pages of table entries that an unmap leaves without a
/// mapped entry leave RAM (their contents are zero or unused).
[[nodiscard]] bool RamTableTrim() noexcept;
/// Gives the RAM of [base, base + bytes) (whole 4 KiB pages) back while keeping it committed: no
/// page file write, and a page that holds zeros reads zeros again. Callers make sure nobody
/// writes there meanwhile (the page table's lock).
void DiscardTablePages(void* base, std::size_t bytes) noexcept;
/// FRITCHII_MEM_CENSUS: names the table in the census and reports its pages (no-ops when off).
void CensusTagTable(const char* name, const void* key, const void* base, std::size_t bytes,
                    std::function<std::string()> reporter);
void CensusUntagTable(const void* key, const void* base);
[[nodiscard]] bool CensusOn() noexcept;
[[nodiscard]] std::vector<u8> TableResidentPages(const void* base, std::size_t bytes);
/// One call committed `pages` table pages at once (census on, 256 pages or more): logged.
void NoteTableCommit(const char* name, const char* site, std::size_t first_index,
                     std::size_t end_index, std::size_t entry_size, std::size_t pages);

/// A large page-aligned buffer that has optimized memory usage for zero-writes.
template <typename T>
    // MSVC doesn't regard structs with atomics as trivially copyable
    // requires std::is_trivially_copyable_v<T>
class SparseLargeVector final {
public:
    constexpr SparseLargeVector() = default;

    explicit SparseLargeVector(std::size_t count) noexcept
        : alloc_size{count * sizeof(T)}
    {
        base_ptr = static_cast<T*>(AllocateMemoryPages(alloc_size));

        // each item in vector holds information for 64 pages
        auto denom = HostPageSize * 64;
        committed_pages = std::vector<std::atomic<u64>>((alloc_size + denom - 1) / denom);
    }

    ~SparseLargeVector() noexcept {
        if (census_name != nullptr) {
            CensusUntagTable(this, base_ptr);
        }
        FreeMemoryPages(base_ptr, alloc_size);
    }

    /// FRITCHII_MEM_CENSUS: this table's name in the census (a string literal).
    void SetCensusName(const char* name) {
        if (!CensusOn()) {
            return;
        }
        if (census_name != nullptr) {
            CensusUntagTable(this, base_ptr);
        }
        census_name = name;
        CensusTagTable(name, this, base_ptr, alloc_size, [this] { return CensusReport(); });
    }

    SparseLargeVector(const SparseLargeVector&) = delete;
    SparseLargeVector& operator=(const SparseLargeVector&) = delete;
    SparseLargeVector(SparseLargeVector&& other) = delete;
    SparseLargeVector& operator=(SparseLargeVector&& other) = delete;

    void ResizeAndClear(std::size_t count) noexcept {
        if (auto const new_size = count * sizeof(T); new_size != alloc_size) {
            if (census_name != nullptr) {
                CensusUntagTable(this, base_ptr);
            }
            FreeMemoryPages(base_ptr, alloc_size);
            alloc_size = new_size;
            base_ptr = static_cast<T*>(AllocateMemoryPages(alloc_size));

            auto denom = HostPageSize * 64;
            committed_pages = std::vector<std::atomic<u64>>((alloc_size + denom - 1) / denom);
            committed_count.store(0, std::memory_order_relaxed);
            if (census_name != nullptr) {
                CensusTagTable(census_name, this, base_ptr, alloc_size,
                               [this] { return CensusReport(); });
            }
        }
    }

    /// Returns a reference to the value of the requested index and allocates memory if needed.
    T& GetAndFault(std::size_t index) noexcept {
        if (index > alloc_size / sizeof(T)) {
            UNREACHABLE_MSG("Out of bounds RW access on SparseLargeVector @ {}", index);
        }

        if (!IsCommittedPage(index) && !CommitPage(index)) {
            UNREACHABLE_MSG("Cannot access SparseLargeVector index {} with RW permission", index);
        }
        return base_ptr[index];
    }

    /// Returns a reference to the value of the requested index if initialized, or will otherwise return a zero-initialized object.
    const T& GetOrDefault(std::size_t index) const {
#ifdef _WIN32
        if (!IsCommittedPage(index)) {
            return *reinterpret_cast<const T*>(&default_val);
        }
#endif
        // On non-Windows, OS page table should optimize this by pointing to a zero page if unallocated.
        return base_ptr[index];
    }

    void Set(std::size_t index, const T& value) noexcept {
        if (index > alloc_size / sizeof(T)) {
            LOG_CRITICAL(Common_Memory, "Out of bounds write on SparseLargeVector @ {}", index);
            return;
        }
        if (IsCommittedPage(index) || CommitPage(index))
            base_ptr[index] = value;
    }

    void ZeroRegion(std::size_t start, std::size_t end_) noexcept {
        u64 base = reinterpret_cast<u64>(&base_ptr[start]);
        const u64 end = reinterpret_cast<u64>(&base_ptr[end_]);

        const u64 end_page = AlignUp(base, HostPageSize);
        const u64 first_size = (std::min)(end_page, end) - base;

        if (IsCommittedPage(start)) {
            std::memset(reinterpret_cast<void*>(base), 0, first_size);
        }

        if (end <= end_page)
            return;

        base = end_page;

        for (u64 page = base; page < end; page += HostPageSize) {
            if (!IsCommittedPage((page - reinterpret_cast<u64>(base_ptr)) / sizeof(T))) {
                continue;
            }

            std::memset(reinterpret_cast<void*>(page), 0, (std::min)(HostPageSize, end - page));
        }
    }

    /// `site` names the caller in the census's log of big commits.
    constexpr void CommitRegion(size_t index, size_t end_, const char* site = nullptr) {
        const u64 base = static_cast<u64>(index) * sizeof(T);
        const u64 end = static_cast<u64>(end_) * sizeof(T);

        size_t fresh = 0;
        for (u64 page = AlignDown(base, HostPageSize); page < end; page += HostPageSize) {
            if (!IsCommittedPage(page / sizeof(T))) {
                fresh += CommitPage(page / sizeof(T)) ? 1 : 0;
            }
        }
        if (fresh >= 256 && census_name != nullptr) {
            NoteTableCommit(census_name, site, index, end_, sizeof(T), fresh);
        }
    }

    /// Whether the page holding entry `index` is committed (an uncommitted one reads as zeros).
    [[nodiscard]] bool IsCommittedAt(std::size_t index) const noexcept {
        if (index >= alloc_size / sizeof(T)) {
            return false;
        }
        const auto page = (index * sizeof(T)) >> HostPageBits;
        return (committed_pages[page >> 6].load(std::memory_order_acquire) >> (page & 63)) & 1;
    }

    /// Entries in one 4 KiB page.
    [[nodiscard]] static constexpr std::size_t EntriesPerPage() noexcept {
        return 4096 / sizeof(T);
    }

    /// FRITCHII_RAM_PT_TRIM: the committed pages of entries [start, end) (whole pages around it)
    /// that hold only zeros leave RAM. The caller holds the lock every writer of a zero entry takes
    /// (the page table's), so no write lands between the check and the discard.
    void DiscardZeroPages(std::size_t start, std::size_t end_) noexcept {
        const std::size_t count = alloc_size / sizeof(T);
        end_ = (std::min)(end_, count);
        if (start >= end_ || HostPageSize != 4096) {
            return;
        }
        constexpr std::size_t per_page = 4096 / sizeof(T);
        std::size_t run_begin = 0;
        std::size_t run_pages = 0;
        const auto flush = [&] {
            if (run_pages != 0) {
                DiscardTablePages(reinterpret_cast<u8*>(base_ptr) + run_begin * 4096,
                                  run_pages * 4096);
                run_pages = 0;
            }
        };
        for (std::size_t page = start / per_page; page * per_page < end_; ++page) {
            if (!IsCommittedAt(page * per_page) || !PageIsZero(page)) {
                flush();
                continue;
            }
            if (run_pages == 0) {
                run_begin = page;
            }
            ++run_pages;
        }
        flush();
    }

    constexpr T& GetUnchecked(size_t index) {
        return base_ptr[index];
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept {
        return GetOrDefault(index);
    }

    [[nodiscard]] constexpr const T* data() const noexcept {
        return base_ptr;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return alloc_size / sizeof(T);
    }

private:
    [[nodiscard]] constexpr bool IsCommittedPage(std::size_t index) const noexcept {
        if (index > alloc_size / sizeof(T)) {
            LOG_CRITICAL(Common_Memory, "Out of bounds access on large vector @ {}", index);
            return false;
        }

        auto page = (index * sizeof(T)) >> HostPageBits;
        auto val = committed_pages[page >> 6].load(std::memory_order_acquire);
        return (val >> (page & 63)) & 1;
    }

    constexpr bool CommitPage(std::size_t index) noexcept {
        auto page_index = (index * sizeof(T)) >> HostPageBits;
        auto page = reinterpret_cast<uintptr_t>(base_ptr + index) & HostPageMask;
#if defined(_WIN32)
        if (!CommitVectorPage(page)) {
            return false;
        }
#else
        if (mprotect(reinterpret_cast<void*>(page), HostPageSize, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR(Common_Memory, "Failed to commit large buffer region at index {}, error {}", index, strerror(errno));
            return false;
        }
#endif

        const u64 bit = 1ULL << (page_index & 63);
        if ((committed_pages[page_index >> 6].fetch_or(bit, std::memory_order_release) & bit) == 0) {
            committed_count.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    /// 4 KiB page `page` of the table (committed) holds only zeros.
    [[nodiscard]] bool PageIsZero(std::size_t page) const noexcept {
        const auto* words = reinterpret_cast<const volatile u64*>(
            reinterpret_cast<const u8*>(base_ptr) + page * 4096);
        u64 any = 0;
        for (std::size_t i = 0; i < 4096 / sizeof(u64); ++i) {
            any |= words[i];
        }
        return any == 0;
    }

    /// FRITCHII_MEM_CENSUS: committed pages, how many are resident, and how many of those hold
    /// only zeros (what FRITCHII_RAM_PT_TRIM or _LAZY can give back). Reads resident pages only.
    std::string CensusReport() const {
        if (HostPageSize != 4096) {
            return {};
        }
        const std::size_t pages = (alloc_size + 4095) / 4096;
        const std::vector<u8> resident = TableResidentPages(base_ptr, alloc_size);
        std::size_t committed = 0;
        std::size_t in_ram = 0;
        std::size_t zero = 0;
        for (std::size_t page = 0; page < pages; ++page) {
            if (!((committed_pages[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1)) {
                continue;
            }
            ++committed;
            if (page < resident.size() && resident[page] != 0) {
                ++in_ram;
                zero += PageIsZero(page) ? 1 : 0;
            }
        }
        return std::string{census_name} + " " + std::to_string(alloc_size >> 20) +
               " MiB reserved, " + std::to_string(committed >> 8) + " committed, " +
               std::to_string(in_ram >> 8) + " resident, " + std::to_string(zero >> 8) +
               " of it all zero";
    }

    std::size_t alloc_size{};
    T* base_ptr{};

    std::vector<std::atomic<u64>> committed_pages{};
    std::atomic<std::size_t> committed_count{}; ///< Pages
    const char* census_name{};
#ifdef _WIN32
    const std::array<u8, sizeof(T)> default_val{};
#endif
};

} // namespace Common
