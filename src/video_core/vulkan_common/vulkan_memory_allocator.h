// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/vulkan_common/vma.h"

namespace Vulkan {

    class Device;

/// Hints and requirements for the backing memory type of a commit
    enum class MemoryUsage {
        DeviceLocal, ///< Requests device local host visible buffer, falling back to device local memory.
        Upload,      ///< Requires a host visible memory type optimized for CPU to GPU uploads
        Download,    ///< Requires a host visible memory type optimized for GPU to CPU readbacks
        Stream,      ///< Requests device local host visible buffer, falling back host memory.
    };

    template<typename F>
    void ForEachDeviceLocalHostVisibleHeap(const Device &device, F &&f) {
        auto memory_props = device.GetPhysical().GetMemoryProperties().memoryProperties;
        for (size_t i = 0; i < memory_props.memoryTypeCount; i++) {
            auto &memory_type = memory_props.memoryTypes[i];
            if ((memory_type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                (memory_type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                f(memory_type.heapIndex, memory_props.memoryHeaps[memory_type.heapIndex]);
            }
        }
    }

/// Ownership handle of a memory commitment (real VMA allocation).
    class MemoryCommit {
    public:
        MemoryCommit() noexcept = default;

        MemoryCommit(VmaAllocator allocator, VmaAllocation allocation,
                     const VmaAllocationInfo &info) noexcept;

        ~MemoryCommit();

        MemoryCommit(const MemoryCommit &) = delete;

        MemoryCommit &operator=(const MemoryCommit &) = delete;

        MemoryCommit(MemoryCommit &&) noexcept;

        MemoryCommit &operator=(MemoryCommit &&) noexcept;

        [[nodiscard]] std::span<u8> Map();

        [[nodiscard]] std::span<const u8> Map() const;

        void Unmap();

        explicit operator bool() const noexcept { return allocation != nullptr; }

        VkDeviceMemory Memory() const noexcept { return memory; }

        VkDeviceSize Offset() const noexcept { return offset; }

        VkDeviceSize Size() const noexcept { return size; }

        VmaAllocation Allocation() const noexcept { return allocation; }

    private:
        void Release();

        VmaAllocator allocator{};   ///< VMA allocator
        VmaAllocation allocation{};  ///< VMA allocation handle
        VkDeviceMemory memory{};      ///< Underlying VkDeviceMemory chosen by VMA
        VkDeviceSize offset{};      ///< Offset of this allocation inside VkDeviceMemory
        VkDeviceSize size{};        ///< Size of the allocation
        void *mapped_ptr{};  ///< Optional persistent mapped pointer
    };

/// Memory allocator container.
/// Allocates and releases memory allocations on demand.
    class MemoryAllocator {
    public:
        /**
         * Construct memory allocator
         *
         * @param device_  Device to allocate from
         * @param buffers  Use the device's buffer allocator (Device::GetBufferAllocator)
         *
         * @throw vk::Exception on failure
         */
        explicit MemoryAllocator(const Device &device_, bool buffers = false);

        ~MemoryAllocator();

        MemoryAllocator &operator=(const MemoryAllocator &) = delete;

        MemoryAllocator(const MemoryAllocator &) = delete;

        /// `dedicated`: own device memory, returned to the driver on destruction (short-lived
        /// images would otherwise leave holes in the shared blocks).
        vk::Image CreateImage(const VkImageCreateInfo &ci, bool dedicated = false) const;

        /// CreateImage, but a null image instead of an exception when there is no memory for it.
        vk::Image TryCreateImage(const VkImageCreateInfo &ci, bool dedicated = false) const;

        /// A sparse resident image with nothing bound (reads return zero, writes are dropped), or
        /// a null image when the device can't make one.
        /// aliased: its sparse blocks may share memory (VK_IMAGE_CREATE_SPARSE_ALIASED_BIT).
        vk::Image CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased = false) const;

        /// Device memory to bind to part of a sparse resource. Null when there is none left.
        /// Device-local buffers in their own VMA pool; false when it doesn't apply or failed.
        bool CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                        const VmaAllocationCreateInfo& alloc_ci, VkBuffer& handle,
                                        VmaAllocation& allocation,
                                        VmaAllocationInfo& alloc_info) const;

        VmaAllocation AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                           VkDeviceMemory &memory, VkDeviceSize &offset) const;

        void FreeSparseMemory(VmaAllocation allocation) const;

        /// Counts the images that did not fit the device memory budget (made anyway, in host
        /// memory or with nothing bound): the texture cache collects harder when it grows.
        u32 MemoryPressureEvents() const noexcept {
            return pressure_events.load(std::memory_order_relaxed);
        }

        /// Counts the images that could not be made at all (no memory left anywhere): the
        /// texture cache collects aggressively when it grows.
        u32 FailedImages() const noexcept {
            return failed_images.load(std::memory_order_relaxed);
        }

        /// EDEN_NX2_IMAGE_ALLOC_TIMING=1 (boot, off by default): images are made in vmaCreateImage's
        /// three steps, each timed (ns, all images so far), so "Slow clear" lines can say where a
        /// new image's create time went (NPC-pass dips: 0.15-1.5 ms per per-object depth target).
        struct ImageTiming {
            u64 images{};
            u64 create_ns{}; ///< vkCreateImage
            u64 alloc_ns{};  ///< vmaAllocateMemoryForImage (requirements, block search, new blocks)
            u64 bind_ns{};   ///< vmaBindImageMemory
            /// Of the images: those whose create added a VMA memory object (a new block or
            /// dedicated memory; VMA's count across the create, so another thread's allocation
            /// in between can count too), their whole create time, and the dedicated ones.
            u64 new_block_images{};
            u64 new_block_ns{};
            u64 dedicated_images{};
        };
        static ImageTiming ReadImageTiming() noexcept;
        static bool ImageTimingEnabled() noexcept;

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const;

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                VkDeviceSize min_alignment) const;

        /// CreateBuffer without the memory budget, for a buffer the game can't go on without once
        /// what could be freed was freed: the driver pages rather than the emulator stopping.
        vk::Buffer CreateBufferOverBudget(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                          VkDeviceSize min_alignment = 1) const;

        /**
         * Commits a memory with the specified requirements.
         *
         * @param requirements Requirements returned from a Vulkan call.
         * @param usage        Indicates how the memory will be used.
         *
         * @returns A memory commit.
         */
        MemoryCommit Commit(const VkMemoryRequirements &requirements, MemoryUsage usage);

        /// Commits memory required by the buffer and binds it (for buffers created outside VMA).
        MemoryCommit Commit(const vk::Buffer &buffer, MemoryUsage usage);

    private:
        static bool IsAutoUsage(VmaMemoryUsage u) noexcept {
            switch (u) {
                case VMA_MEMORY_USAGE_AUTO:
                case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
                case VMA_MEMORY_USAGE_AUTO_PREFER_HOST:
                    return true;
                default:
                    return false;
            }
        }

        /// The buffer through VMA's aligned create, within the memory budget or not.
        vk::Buffer CreateAlignedBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                       VkDeviceSize min_alignment, bool within_budget) const;

        /// CreateImage's fallbacks; the image is left null when the result is an error.
        /// may_fail: the caller handles VK_ERROR_OUT_OF_DEVICE_MEMORY (see UsesUnbackedFallback).
        VkResult AllocateImage(const VkImageCreateInfo &ci, bool dedicated, bool may_fail,
                               vk::Image &image) const;

        const Device &device;                              ///< Device handle.
        VmaAllocator allocator;                           ///< VMA allocator.
        const VkPhysicalDeviceMemoryProperties properties; ///< Physical device memory properties.
        VkDeviceSize buffer_image_granularity;            ///< Adjacent buffer/image granularity
        u32 valid_memory_types{~0u};
        mutable std::atomic<u32> pressure_events{0};
        mutable std::atomic<u32> failed_images{0};
        mutable std::mutex buffer_type_mutex;
        /// Memory type of device-local buffers by usage and flags (CreateBufferInSeparatePool).
        mutable std::unordered_map<u64, u32> buffer_memory_types;
    };

    /// Logs VMA's blocks and allocations per memory type, and each heap's budget.
    void LogMemoryStatistics(const Device& device);

    /// One line each for the process's memory, what DXGI counts for the GPU (Windows), and VMA's
    /// heaps and memory types against what the driver reports (EDEN_NX2_MEM_TRACE).
    void LogMemoryTrace(const Device& device);

    /// VMA's heaps summed by kind, for the "Memory owners" line: host heaps (system RAM) and
    /// device-local ones (video memory), each the driver's usage and budget and VMA's blocks.
    struct HeapTotals {
        u64 host_usage{};
        u64 host_budget{};
        u64 host_blocks{};
        u64 local_usage{};
        u64 local_budget{};
        u64 local_blocks{};
    };
    [[nodiscard]] HeapTotals GetHeapTotals(const Device& device);

    /// FRITCHII_VRAM_OWNERS: where the unused space in `owner`'s blocks on `heap` lies: the
    /// memory types' default blocks, then each custom pool (depth, colour, sparse, buffer), each
    /// with its blocks, holes and largest hole. Walks every block: only the allocator's user may
    /// call it (the back for the main allocator; MemoryStageLink::IsCacheUser for the buffer one).
    [[nodiscard]] std::string VramSlackText(const Device& device, VmaAllocator owner, u32 heap);

    /// FRITCHII_VRAM_TRIM: gives back the empty block VMA keeps in each of `owner`'s block
    /// vectors on `heap` (default blocks and custom pools). Returns the bytes given back. Only the
    /// allocator's user may call it.
    u64 TrimEmptyBlocks(const Device& device, VmaAllocator owner, u32 heap);

    /// FRITCHII_DEPTH_POOL_FIT: what it placed and the padding its live images hold; empty while
    /// it is off and placed nothing.
    [[nodiscard]] std::string DepthFitText();

    /// FRITCHII_VRAM_HOLES: per block vector of `owner` on `heap` (default blocks per memory type,
    /// each named pool), the holes and the allocations by size, and what holds the blocks that
    /// are at least half free (kind, size, age). Builds VMA's detailed map: several ms with
    /// thousands of allocations, so only on request; only the allocator's user may call it.
    [[nodiscard]] std::string VramHolesText(VmaAllocator owner, u32 heap);

} // namespace Vulkan
