// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <algorithm>

#include "common/assert.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/multi_level_page_table.h"
#include "common/sparse_large_vector.h"

namespace Common {

template <typename BaseAddr>
MultiLevelPageTable<BaseAddr>::MultiLevelPageTable(std::size_t address_space_bits_, std::size_t first_level_bits_, std::size_t page_bits_)
    : address_space_bits{address_space_bits_}
    , first_level_bits{first_level_bits_}
    , page_bits{page_bits_}
{
    if (page_bits == 0) {
        return;
    }
    first_level_shift = address_space_bits - first_level_bits;
    first_level_chunk_size = (1ULL << (first_level_shift - page_bits)) * sizeof(BaseAddr);
    alloc_size = (1ULL << (address_space_bits - page_bits)) * sizeof(BaseAddr);
    std::size_t first_level_size = 1ULL << first_level_bits;
    first_level_map.resize(first_level_size, nullptr);
#ifdef _WIN32
    void* base{VirtualAlloc(nullptr, alloc_size, MEM_RESERVE, PAGE_READWRITE)};
#else
    void* base{mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
    if (base == MAP_FAILED)
        base = nullptr;
#endif
    ASSERT(base);
    base_ptr = reinterpret_cast<BaseAddr*>(base);
}

template <typename BaseAddr>
MultiLevelPageTable<BaseAddr>::~MultiLevelPageTable() noexcept {
    if (!base_ptr) {
        return;
    }
    if (census_name != nullptr) {
        CensusUntagTable(this, base_ptr);
    }
#ifdef _WIN32
    ASSERT(VirtualFree(base_ptr, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base_ptr, alloc_size) == 0);
#endif
}

template <typename BaseAddr>
void MultiLevelPageTable<BaseAddr>::ReserveRange(u64 start, std::size_t size) {
    if (size == 0) {
        return;
    }
    const u64 new_start = start >> first_level_shift;
    const u64 new_end = (start + size - 1) >> first_level_shift;
    u64 fresh = 0;
    for (u64 i = new_start; i <= new_end; i++)
        if (!first_level_map[i]) {
            AllocateLevel(i);
            ++fresh;
        }
    if (census_name != nullptr && fresh * first_level_chunk_size >= (u64{1} << 20)) {
        LOG_INFO(HW_Memory, "Page table commit: {} committed {} KiB for {:#x}+{:#x} ({} MiB)",
                 census_name, (fresh * first_level_chunk_size) >> 10, start, size, size >> 20);
    }
}

template <typename BaseAddr>
void MultiLevelPageTable<BaseAddr>::SetCensusName(const char* name) {
    if (!CensusOn() || base_ptr == nullptr) {
        return;
    }
    census_name = name;
    CensusTagTable(name, this, base_ptr, alloc_size, {});
}

template <typename BaseAddr>
void MultiLevelPageTable<BaseAddr>::DiscardEntries(std::size_t first, std::size_t end) noexcept {
    if (base_ptr == nullptr || first >= end) {
        return;
    }
    constexpr std::size_t PageBytes = 4096;
    const std::size_t total = alloc_size / sizeof(BaseAddr);
    end = (std::min)(end, total);
    // Whole pages inside [first, end) only, and only in committed levels.
    const std::size_t first_byte = (first * sizeof(BaseAddr) + PageBytes - 1) & ~(PageBytes - 1);
    const std::size_t end_byte = (end * sizeof(BaseAddr)) & ~(PageBytes - 1);
    std::size_t run_begin = 0;
    std::size_t run_bytes = 0;
    const auto flush = [&] {
        if (run_bytes != 0) {
            DiscardTablePages(reinterpret_cast<u8*>(base_ptr) + run_begin, run_bytes);
            run_bytes = 0;
        }
    };
    for (std::size_t byte = first_byte; byte < end_byte; byte += PageBytes) {
        if (!IsCommittedAt(byte / sizeof(BaseAddr))) {
            flush();
            continue;
        }
        if (run_bytes == 0) {
            run_begin = byte;
        }
        run_bytes += PageBytes;
    }
    flush();
}

template <typename BaseAddr>
void MultiLevelPageTable<BaseAddr>::AllocateLevel(u64 index) {
    void* ptr = reinterpret_cast<char *>(base_ptr) + index * first_level_chunk_size;
#ifdef _WIN32
    const bool committed = CommitPagesWaiting(ptr, first_level_chunk_size);
    ASSERT(committed);
    void* base = ptr;
#else
    void* base = ptr;
#endif
    first_level_map[index] = base;
}

} // namespace Common
