// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <span>
#include <string>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaPool)
struct VmaBudget;

// Define all features which may be used by the implementation here.
// Vulkan version in the macro describes the minimum version required for feature availability.
// If the Vulkan version is lower than the required version, the named extension is required.
#define FOR_EACH_VK_FEATURE_1_1(FEATURE)                                                           \
    FEATURE(KHR, 16BitStorage, 16BIT_STORAGE, bit16_storage)                                       \
    FEATURE(KHR, ShaderAtomicInt64, SHADER_ATOMIC_INT64, shader_atomic_int64)                      \
    FEATURE(KHR, ShaderDrawParameters, SHADER_DRAW_PARAMETERS, shader_draw_parameters)             \
    FEATURE(KHR, ShaderFloat16Int8, SHADER_FLOAT16_INT8, shader_float16_int8)                      \
    FEATURE(KHR, UniformBufferStandardLayout, UNIFORM_BUFFER_STANDARD_LAYOUT,                      \
            uniform_buffer_standard_layout)                                                        \
    FEATURE(KHR, VariablePointer, VARIABLE_POINTERS, variable_pointer)

#define FOR_EACH_VK_FEATURE_1_2(FEATURE)                                                           \
    FEATURE(EXT, DescriptorIndexing, DESCRIPTOR_INDEXING, descriptor_indexing)                     \
    FEATURE(EXT, HostQueryReset, HOST_QUERY_RESET, host_query_reset)                               \
    FEATURE(KHR, 8BitStorage, 8BIT_STORAGE, bit8_storage)                                          \
    FEATURE(KHR, BufferDeviceAddress, BUFFER_DEVICE_ADDRESS, buffer_device_address)                \
    FEATURE(KHR, TimelineSemaphore, TIMELINE_SEMAPHORE, timeline_semaphore)                        \
    FEATURE(KHR, VulkanMemoryModel, VULKAN_MEMORY_MODEL, vulkan_memory_model)

#define FOR_EACH_VK_FEATURE_1_3(FEATURE)                                                           \
    FEATURE(EXT, ImageRobustness, IMAGE_ROBUSTNESS, robust_image_access)                           \
    FEATURE(EXT, ShaderDemoteToHelperInvocation, SHADER_DEMOTE_TO_HELPER_INVOCATION,               \
            shader_demote_to_helper_invocation)                                                    \
    FEATURE(EXT, SubgroupSizeControl, SUBGROUP_SIZE_CONTROL, subgroup_size_control)                \
    FEATURE(KHR, Maintenance4, MAINTENANCE_4, maintenance4)                                        \
    FEATURE(KHR, Synchronization2, SYNCHRONIZATION_2, synchronization2)

#define FOR_EACH_VK_FEATURE_1_4(FEATURE)

// Define all features which may be used by the implementation and require an extension here.
#define FOR_EACH_VK_FEATURE_EXT(FEATURE)                                                           \
    FEATURE(EXT, BorderColorSwizzle, BORDER_COLOR_SWIZZLE, border_color_swizzle)                   \
    FEATURE(EXT, ColorWriteEnable, COLOR_WRITE_ENABLE, color_write_enable)                         \
    FEATURE(EXT, CustomBorderColor, CUSTOM_BORDER_COLOR, custom_border_color)                      \
    FEATURE(EXT, DepthBiasControl, DEPTH_BIAS_CONTROL, depth_bias_control)                         \
    FEATURE(EXT, DepthClipControl, DEPTH_CLIP_CONTROL, depth_clip_control)                         \
    FEATURE(EXT, DescriptorBuffer, DESCRIPTOR_BUFFER, descriptor_buffer)                           \
    FEATURE(EXT, ExtendedDynamicState, EXTENDED_DYNAMIC_STATE, extended_dynamic_state)             \
    FEATURE(EXT, ExtendedDynamicState2, EXTENDED_DYNAMIC_STATE_2, extended_dynamic_state2)         \
    FEATURE(EXT, ExtendedDynamicState3, EXTENDED_DYNAMIC_STATE_3, extended_dynamic_state3)         \
    FEATURE(EXT, 4444Formats, 4444_FORMATS, format_a4b4g4r4)                                       \
    FEATURE(EXT, IndexTypeUint8, INDEX_TYPE_UINT8, index_type_uint8)                               \
    FEATURE(EXT, LineRasterization, LINE_RASTERIZATION, line_rasterization)                        \
    FEATURE(EXT, MeshShader, MESH_SHADER, mesh_shader)                                             \
    FEATURE(EXT, PrimitiveTopologyListRestart, PRIMITIVE_TOPOLOGY_LIST_RESTART,                    \
            primitive_topology_list_restart)                                                       \
    FEATURE(EXT, ProvokingVertex, PROVOKING_VERTEX, provoking_vertex)                              \
    FEATURE(EXT, Robustness2, ROBUSTNESS_2, robustness2)                                           \
    FEATURE(EXT, ShaderAtomicFloat, SHADER_ATOMIC_FLOAT, shader_atomic_float)                      \
    FEATURE(EXT, ShaderFloat8, SHADER_FLOAT8, shader_float8)                                       \
    FEATURE(EXT, TransformFeedback, TRANSFORM_FEEDBACK, transform_feedback)                        \
    FEATURE(EXT, VertexInputDynamicState, VERTEX_INPUT_DYNAMIC_STATE, vertex_input_dynamic_state)  \
    FEATURE(KHR, CooperativeMatrix, COOPERATIVE_MATRIX, cooperative_matrix)                        \
    FEATURE(KHR, Maintenance5, MAINTENANCE_5, maintenance5)                                        \
    FEATURE(KHR, Maintenance6, MAINTENANCE_6, maintenance6)                                        \
    FEATURE(KHR, PipelineExecutableProperties, PIPELINE_EXECUTABLE_PROPERTIES,                     \
            pipeline_executable_properties)                                                        \
    FEATURE(KHR, PresentId, PRESENT_ID, present_id)                                                \
    FEATURE(KHR, PresentWait, PRESENT_WAIT, present_wait)                                          \
    FEATURE(KHR, ShaderQuadControl, SHADER_QUAD_CONTROL, shader_quad_control)                      \
    FEATURE(KHR, ShaderMaximalReconvergence, SHADER_MAXIMAL_RECONVERGENCE,                        \
            shader_maximal_reconvergence)                                                        \
    FEATURE(KHR, WorkgroupMemoryExplicitLayout, WORKGROUP_MEMORY_EXPLICIT_LAYOUT,                  \
            workgroup_memory_explicit_layout)


// Define miscellaneous extensions which may be used by the implementation here.
#define FOR_EACH_VK_EXTENSION(EXTENSION)                                                           \
    EXTENSION(EXT, ASTC_DECODE_MODE, astc_decode_mode)                                             \
    EXTENSION(EXT, CONDITIONAL_RENDERING, conditional_rendering)                                   \
    EXTENSION(EXT, CONSERVATIVE_RASTERIZATION, conservative_rasterization)                         \
    EXTENSION(EXT, DEPTH_RANGE_UNRESTRICTED, depth_range_unrestricted)                             \
    EXTENSION(EXT, MEMORY_BUDGET, memory_budget)                                                   \
    EXTENSION(EXT, ROBUSTNESS_2, robustness_2)                                                     \
    EXTENSION(EXT, SAMPLER_FILTER_MINMAX, sampler_filter_minmax)                                   \
    EXTENSION(EXT, SHADER_STENCIL_EXPORT, shader_stencil_export)                                   \
    EXTENSION(EXT, SHADER_VIEWPORT_INDEX_LAYER, shader_viewport_index_layer)                       \
    EXTENSION(EXT, TOOLING_INFO, tooling_info)                                                     \
    EXTENSION(EXT, VERTEX_ATTRIBUTE_DIVISOR, vertex_attribute_divisor)                             \
    EXTENSION(KHR, CREATE_RENDERPASS_2, create_renderpass2)                                        \
    EXTENSION(KHR, DEPTH_STENCIL_RESOLVE, depth_stencil_resolve)                                   \
    EXTENSION(KHR, DRAW_INDIRECT_COUNT, draw_indirect_count)                                       \
    EXTENSION(KHR, DRIVER_PROPERTIES, driver_properties)                                           \
    EXTENSION(KHR, PUSH_DESCRIPTOR, push_descriptor)                                               \
    EXTENSION(KHR, SAMPLER_MIRROR_CLAMP_TO_EDGE, sampler_mirror_clamp_to_edge)                     \
    EXTENSION(KHR, SHADER_FLOAT_CONTROLS, shader_float_controls)                                   \
    EXTENSION(KHR, SPIRV_1_4, spirv_1_4)                                                           \
    EXTENSION(KHR, SWAPCHAIN, swapchain)                                                           \
    EXTENSION(KHR, SWAPCHAIN_MUTABLE_FORMAT, swapchain_mutable_format)                             \
    EXTENSION(KHR, IMAGE_FORMAT_LIST, image_format_list)                                           \
    EXTENSION(KHR, MAINTENANCE_1, maintenance1)                                                    \
    EXTENSION(KHR, MAINTENANCE_2, maintenance2)                                                    \
    EXTENSION(KHR, MAINTENANCE_3, maintenance3)                                                    \
    EXTENSION(KHR, MAINTENANCE_7, maintenance7)                                                    \
    EXTENSION(KHR, MAINTENANCE_8, maintenance8)                                                    \
    EXTENSION(NV, DEVICE_DIAGNOSTICS_CONFIG, device_diagnostics_config)                            \
    EXTENSION(NV, GEOMETRY_SHADER_PASSTHROUGH, geometry_shader_passthrough)                        \
    EXTENSION(NV, VIEWPORT_ARRAY2, viewport_array2)                                                \
    EXTENSION(NV, VIEWPORT_SWIZZLE, viewport_swizzle)                                              \
    EXTENSION(EXT, FILTER_CUBIC, filter_cubic)                                                     \
    EXTENSION(IMG, FILTER_CUBIC, filter_cubic_img)                                                 \
    EXTENSION(QCOM, FILTER_CUBIC_WEIGHTS, filter_cubic_weights)

// Define extensions which must be supported.
#define FOR_EACH_VK_MANDATORY_EXTENSION(EXTENSION_NAME)                                            \
    EXTENSION_NAME(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)                                        \
    EXTENSION_NAME(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)                             \
    EXTENSION_NAME(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)

// Define extensions where the absence of the extension may result in a degraded experience.
#define FOR_EACH_VK_RECOMMENDED_EXTENSION(EXTENSION_NAME)                                          \
    EXTENSION_NAME(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)                                    \
    EXTENSION_NAME(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)                                       \
    EXTENSION_NAME(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)                                   \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)                                     \
    EXTENSION_NAME(VK_EXT_4444_FORMATS_EXTENSION_NAME)                                             \
    EXTENSION_NAME(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME)                                       \
    EXTENSION_NAME(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)                                             \
    EXTENSION_NAME(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME)                                           \
    EXTENSION_NAME(VK_NV_VIEWPORT_SWIZZLE_EXTENSION_NAME)

// Define features which must be supported.
#define FOR_EACH_VK_MANDATORY_FEATURE(FEATURE_NAME)                                                \
    FEATURE_NAME(features, depthBiasClamp)                                                         \
    FEATURE_NAME(features, depthClamp)                                                             \
    FEATURE_NAME(features, drawIndirectFirstInstance)                                              \
    FEATURE_NAME(features, dualSrcBlend)                                                           \
    FEATURE_NAME(features, fillModeNonSolid)                                                       \
    FEATURE_NAME(features, fragmentStoresAndAtomics)                                               \
    FEATURE_NAME(features, geometryShader)                                                         \
    FEATURE_NAME(features, imageCubeArray)                                                         \
    FEATURE_NAME(features, independentBlend)                                                       \
    FEATURE_NAME(features, largePoints)                                                            \
    FEATURE_NAME(features, logicOp)                                                                \
    FEATURE_NAME(features, multiDrawIndirect)                                                      \
    FEATURE_NAME(features, multiViewport)                                                          \
    FEATURE_NAME(features, occlusionQueryPrecise)                                                  \
    FEATURE_NAME(features, robustBufferAccess)                                                     \
    FEATURE_NAME(features, samplerAnisotropy)                                                      \
    FEATURE_NAME(features, sampleRateShading)                                                      \
    FEATURE_NAME(features, shaderClipDistance)                                                     \
    FEATURE_NAME(features, shaderCullDistance)                                                     \
    FEATURE_NAME(features, shaderImageGatherExtended)                                              \
    FEATURE_NAME(features, shaderStorageImageWriteWithoutFormat)                                   \
    FEATURE_NAME(features, tessellationShader)                                                     \
    FEATURE_NAME(features, vertexPipelineStoresAndAtomics)                                         \
    FEATURE_NAME(features, wideLines)                                                              \
    FEATURE_NAME(host_query_reset, hostQueryReset)                                                 \
    FEATURE_NAME(shader_demote_to_helper_invocation, shaderDemoteToHelperInvocation)               \
    FEATURE_NAME(shader_draw_parameters, shaderDrawParameters)                                     \
    FEATURE_NAME(variable_pointer, variablePointers)                                               \
    FEATURE_NAME(variable_pointer, variablePointersStorageBuffer)

// Define features where the absence of the feature may result in a degraded experience.
#define FOR_EACH_VK_RECOMMENDED_FEATURE(FEATURE_NAME)                                              \
    FEATURE_NAME(bit16_storage, storageBuffer16BitAccess)                                          \
    FEATURE_NAME(bit8_storage, storageBuffer8BitAccess)                                            \
    FEATURE_NAME(custom_border_color, customBorderColors)                                          \
    FEATURE_NAME(depth_bias_control, depthBiasControl)                                             \
    FEATURE_NAME(depth_bias_control, leastRepresentableValueForceUnormRepresentation)              \
    FEATURE_NAME(depth_bias_control, depthBiasExact)                                               \
    FEATURE_NAME(descriptor_indexing, descriptorBindingPartiallyBound)                             \
    FEATURE_NAME(descriptor_indexing, shaderSampledImageArrayNonUniformIndexing)                   \
    FEATURE_NAME(extended_dynamic_state, extendedDynamicState)                                     \
    FEATURE_NAME(format_a4b4g4r4, formatA4B4G4R4)                                                  \
    FEATURE_NAME(robust_image_access, robustImageAccess)                                           \
    FEATURE_NAME(index_type_uint8, indexTypeUint8)                                                 \
    FEATURE_NAME(primitive_topology_list_restart, primitiveTopologyListRestart)                    \
    FEATURE_NAME(provoking_vertex, provokingVertexLast)                                            \
    FEATURE_NAME(robustness2, nullDescriptor)                                                      \
    FEATURE_NAME(shader_float16_int8, shaderFloat16)                                               \
    FEATURE_NAME(shader_float16_int8, shaderInt8)                                                  \
    FEATURE_NAME(synchronization2, synchronization2)                                               \
    FEATURE_NAME(timeline_semaphore, timelineSemaphore)                                            \
    FEATURE_NAME(transform_feedback, transformFeedback)                                            \
    FEATURE_NAME(uniform_buffer_standard_layout, uniformBufferStandardLayout)                      \
    FEATURE_NAME(vertex_input_dynamic_state, vertexInputDynamicState)

// These features are not required but can be helpful for drivers that can use it.
#define FOR_EACH_VK_OPTIONAL_FEATURE(FEATURE_NAME)                                                 \
    FEATURE_NAME(bit16_storage, uniformAndStorageBuffer16BitAccess)                                \
    FEATURE_NAME(bit8_storage, uniformAndStorageBuffer8BitAccess)

namespace Vulkan {

class NsightAftermathTracker;
class KeptBlocks;

/// Format usage descriptor.
enum class FormatType { Linear, Optimal, Buffer };

/// Subgroup size of the guest emulated hardware (Nvidia has 32 threads per subgroup).
const u32 GuestWarpSize = 32;

enum class NvidiaArchitecture {
    Arch_KeplerOrOlder,
    Arch_Maxwell,
    Arch_Pascal,
    Arch_Volta,
    Arch_Turing,
    Arch_AmpereOrNewer,
};

/// Handles data specific to a physical device.
class Device {
public:
    explicit Device(VkInstance instance, vk::PhysicalDevice physical, VkSurfaceKHR surface,
                    const vk::InstanceDispatch& dld);
    ~Device();

    /**
     * Returns a format supported by the device for the passed requirements.
     * @param wanted_format The ideal format to be returned. It may not be the returned format.
     * @param wanted_usage The usage that must be fulfilled even if the format is not supported.
     * @param format_type Format type usage.
     * @returns A format supported by the device.
     */
    VkFormat GetSupportedFormat(VkFormat wanted_format, VkFormatFeatureFlags wanted_usage,
                                FormatType format_type) const;

    /// Returns true if a format is supported.
    bool IsFormatSupported(VkFormat wanted_format, VkFormatFeatureFlags wanted_usage,
                           FormatType format_type) const;

    /// Reports a device loss and ends the process. It never returns: the first thread to get here
    /// writes the report, any other waits for the end.
    void ReportLoss(std::source_location where = std::source_location::current()) const;

    /// Logs what VK_EXT_device_fault reports about a device loss.
    void ReportDeviceFault() const;

    /// Reports a shader to Nsight Aftermath.
    void SaveShader(std::span<const u32> spirv) const;

    /// Returns the name of the VkDriverId reported from Vulkan.
    std::string GetDriverName() const;

    /// Returns the dispatch loader with direct function pointers of the device.
    const vk::DeviceDispatch& GetDispatchLoader() const {
        return dld;
    }

    /// Returns the VMA allocator.
    VmaAllocator GetAllocator() const {
        return allocator;
    }

    /// FRITCHII_MEMSTAGE_SPLIT_ALLOC (boot, default on): the buffer cache's own VMA allocator, or
    /// the main one when the split is off.
    VmaAllocator GetBufferAllocator() const {
        return buffer_allocator != nullptr ? buffer_allocator : allocator;
    }

    [[nodiscard]] bool HasSplitAllocators() const noexcept {
        return buffer_allocator != nullptr;
    }

    /// EDEN_NX2_ASYNC_IMAGE_FREE (boot, default on; =0 off): the allocator was made without
    /// VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT, so VMA locks internally and images may be
    /// destroyed off the GPU thread (TextureCacheRuntime's image free queue).
    [[nodiscard]] bool IsAllocatorThreadSafe() const noexcept {
        return allocator_thread_safe;
    }

    /// Allocations kept out of the default blocks, which then hold images only: churned buffers
    /// and long-lived sparse pages left holes the texture cache's collector couldn't drain.
    enum class SeparatePool : u32 {
        SparsePages,
        DeviceBuffers,
    };
    static constexpr VkDeviceSize SeparatePoolBlockSize = 16ULL << 20;
    /// Block size of the DeviceBuffers pools; allocations over half of it take the default
    /// blocks. FRITCHII_BUFFER_POOL_BLOCK_MB (boot, default 0 = SeparatePoolBlockSize): buffer
    /// cache buffers live a few per block, so 16 MiB blocks can stay pinned mostly empty.
    static VkDeviceSize DeviceBufferBlockSize() noexcept;

    /// The pool of that kind for a memory type in `owner` (GetAllocator or GetBufferAllocator),
    /// created on first use (null if that failed).
    VmaPool GetSeparatePool(VmaAllocator owner, SeparatePool kind, u32 memory_type) const;

    /// That pool if it was made already, else null (never makes one).
    VmaPool PeekSeparatePool(VmaAllocator owner, SeparatePool kind, u32 memory_type) const;

    /// Returns the logical device.
    const vk::Device& GetLogical() const {
        return logical;
    }

    /// Returns the physical device.
    vk::PhysicalDevice GetPhysical() const {
        return physical;
    }

    /// Returns the instance the device was created from.
    VkInstance GetInstance() const {
        return instance;
    }

    VkPipelineCache StaticPipelineCache() const noexcept {
        return *static_pipeline_cache;
    }

    /// Returns the main graphics queue.
    vk::Queue GetGraphicsQueue() const {
        return graphics_queue;
    }

    /// Returns the main present queue.
    vk::Queue GetPresentQueue() const {
        return present_queue;
    }

    /// FRITCHII_READ_SIDE_COPY >= 2 (boot): a second queue of the graphics family (its index 1),
    /// for read downloads that wait only for their writer. Null without one.
    vk::Queue GetSideQueue() const {
        return side_queue;
    }
    bool HasSideQueue() const {
        return has_side_queue;
    }

    /// Whether the device is made with the side queue (the switch, and a graphics family of two
    /// queues or more). Decided once, before the device is made.
    bool WantsSideQueue() const;

    /// FRITCHII_SIM_LINK_HOG: the queue family the copy load gets. Decided once, before the
    /// device is made.
    [[nodiscard]] std::optional<u32> LinkHogFamily() const;

    /// FRITCHII_SIM_LINK_HOG (boot, test): a queue of a transfer family for the copy load.
    vk::Queue GetLinkHogQueue() const {
        return link_hog_queue;
    }
    bool HasLinkHogQueue() const {
        return has_link_hog_queue;
    }
    u32 GetLinkHogFamily() const {
        return link_hog_family;
    }

    /// FRITCHII_DESCRIPTOR_SETS (boot): draws and dispatches take descriptor sets from pools
    /// instead of push descriptors or the descriptor buffer ring. Decided once.
    [[nodiscard]] bool PrefersDescriptorSets() const;

    /// Returns main graphics queue family index.
    u32 GetGraphicsFamily() const {
        return graphics_family;
    }

    /// Returns main present queue family index.
    u32 GetPresentFamily() const {
        return present_family;
    }

    /// Returns the current Vulkan API version provided in Vulkan-formatted version numbers.
    u32 ApiVersion() const {
        return properties.properties.apiVersion;
    }

    /// Returns the current driver version provided in Vulkan-formatted version numbers.
    u32 GetDriverVersion() const {
        return properties.properties.driverVersion;
    }

    /// Returns the PCI device ID of the physical device.
    u32 GetDeviceID() const {
        return properties.properties.deviceID;
    }

    /// Returns the PCI vendor ID of the physical device.
    u32 GetVendorID() const {
        return properties.properties.vendorID;
    }

    /// Returns the pipeline cache UUID (it names the driver build that compiles pipelines).
    std::span<const u8> GetPipelineCacheUUID() const {
        return properties.properties.pipelineCacheUUID;
    }

    /// Returns the device name.
    std::string_view GetModelName() const {
        return properties.properties.deviceName;
    }

    /// Returns the driver ID.
    VkDriverIdKHR GetDriverID() const {
        return properties.driver.driverID;
    }

    /// Whether the device runs on an AMD driver: AMD's own (Windows, AMDVLK) or RADV. Gates
    /// AMD-only workarounds so other vendors (NVIDIA above all) keep their path unchanged.
    bool IsAmdDriver() const {
        const VkDriverIdKHR id = GetDriverID();
        return id == VK_DRIVER_ID_AMD_PROPRIETARY || id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
               id == VK_DRIVER_ID_MESA_RADV;
    }

    bool IsSparseBindingSupported() const {
        return features.features.sparseBinding && graphics_family_sparse_binding;
    }

    /// 2D images whose memory can be bound in parts, on the graphics queue.
    bool IsSparseResidencyImage2DSupported() const {
        return IsSparseBindingSupported() && features.features.sparseResidencyImage2D;
    }

    /// Whether a sparse image may bind the same memory to several of its blocks.
    bool IsSparseResidencyAliasedSupported() const {
        return IsSparseResidencyImage2DSupported() && features.features.sparseResidencyAliased;
    }

    /// Whether images with no memory bound may be sampled and used as attachments. AMD's drivers
    /// fault on it (the GPU hangs shortly after the first such draw), so large images there are
    /// backed normally. RADV is counted in until someone shows it doesn't need it.
    /// EDEN_NX2_SPARSE_UNBACKED=1 allows unbound images on any driver, =0 on none.
    bool CanUseUnbackedSparseImages() const {
        static const char forced = [] {
            const char* value = std::getenv("EDEN_NX2_SPARSE_UNBACKED");
            return value == nullptr ? '\0' : value[0];
        }();
        if (forced == '0' || forced == '1') {
            return forced == '1';
        }
        const VkDriverIdKHR id = GetDriverID();
        return id != VK_DRIVER_ID_AMD_PROPRIETARY && id != VK_DRIVER_ID_AMD_OPEN_SOURCE &&
               id != VK_DRIVER_ID_MESA_RADV;
    }

    /// Whether pipelines linked from graphics pipeline libraries without link time optimization
    /// draw correctly. On AMD's Windows driver such links draw white fringes around alpha blended
    /// effects (clouds, particles and glows in a test title, RX 7800 XT), while optimized links
    /// and monolithic pipelines from the same SPIR-V do not, so libraries are linked optimized
    /// there. EDEN_NX2_GPL_FAST_LINK=1 allows fast links on any driver, =0 on none.
    bool CanFastLinkGraphicsLibraries() const {
        static const char forced = [] {
            const char* value = std::getenv("EDEN_NX2_GPL_FAST_LINK");
            return value == nullptr ? '\0' : value[0];
        }();
        if (forced == '0' || forced == '1') {
            return forced == '1';
        }
        return GetDriverID() != VK_DRIVER_ID_AMD_PROPRIETARY;
    }

    /// Returns true for tile-based deferred renderers.
    bool IsTiler() const {
        switch (GetDriverID()) {
        case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:
        case VK_DRIVER_ID_ARM_PROPRIETARY:
        case VK_DRIVER_ID_SAMSUNG_PROPRIETARY:
        case VK_DRIVER_ID_MESA_TURNIP:
            return true;
        default:
            return false;
        }
    }

    bool ShouldBoostClocks() const;

    /// Returns uniform buffer alignment requirement.
    VkDeviceSize GetUniformBufferAlignment() const {
        return properties.properties.limits.minUniformBufferOffsetAlignment;
    }

    /// Returns storage alignment requirement.
    VkDeviceSize GetStorageBufferAlignment() const {
        return properties.properties.limits.minStorageBufferOffsetAlignment;
    }

    /// Returns how many samplers may exist at once.
    u32 GetMaxSamplerAllocationCount() const {
        return properties.properties.limits.maxSamplerAllocationCount;
    }

    /// Returns the maximum range for storage buffers.
    VkDeviceSize GetMaxStorageBufferRange() const {
        return properties.properties.limits.maxStorageBufferRange;
    }

    std::array<u32, 3> GetMaxComputeWorkGroupCount() const {
        const auto& count = properties.properties.limits.maxComputeWorkGroupCount;
        return {count[0], count[1], count[2]};
    }

    /// Returns the maximum size for push constants.
    VkDeviceSize GetMaxPushConstantsSize() const {
        return properties.properties.limits.maxPushConstantsSize;
    }

#define FN_MAX_LIMIT_LIST \
    FN_MAX_LIMIT_ELEM(ComputeSharedMemorySize) \
    FN_MAX_LIMIT_ELEM(PerStageDescriptorSampledImages) \
    FN_MAX_LIMIT_ELEM(PerStageDescriptorStorageBuffers) \
    FN_MAX_LIMIT_ELEM(PerStageResources) \
    FN_MAX_LIMIT_ELEM(PerStageDescriptorSamplers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetSamplers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetUniformBuffers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetUniformBuffersDynamic) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageBuffers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageBuffersDynamic) \
    FN_MAX_LIMIT_ELEM(DescriptorSetSampledImages) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageImages) \
    FN_MAX_LIMIT_ELEM(DescriptorSetInputAttachments) \
    FN_MAX_LIMIT_ELEM(ImageDimension2D) \
    FN_MAX_LIMIT_ELEM(FramebufferWidth) \
    FN_MAX_LIMIT_ELEM(FramebufferHeight) \
    FN_MAX_LIMIT_ELEM(FramebufferLayers)
#define FN_MAX_LIMIT_ELEM(name) \
    u32 GetMax##name() const { return properties.properties.limits.max##name; }
FN_MAX_LIMIT_LIST
#undef FN_MAX_LIMIT_ELEM
#undef FN_MAX_LIMIT_LIST

    /// Returns float control properties of the device.
    const VkPhysicalDeviceFloatControlsPropertiesKHR& FloatControlProperties() const {
        return properties.float_controls;
    }

    bool IsOptimalAstcSupported() const {
        return is_optimal_astc_supported;
    }

    /// Returns true if geometry shaders are supported.
    bool IsGeometryShaderSupported() const {
        return features.features.geometryShader;
    }

    /// Returns true if BCn is natively supported.
    bool IsOptimalBcnSupported() const {
        return features.features.textureCompressionBC;
    }

    /// Returns true if ETC2 is natively supported.
    bool IsOptimalEtc2Supported() const {
        return features.features.textureCompressionETC2;
    }

    /// Returns true if descriptor aliasing is natively supported.
    bool IsDescriptorAliasingSupported() const {
        return !has_broken_descriptor_aliasing;
    }

    bool IsSampledImageArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderSampledImageArrayNonUniformIndexing;
    }

    bool IsStorageImageArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderStorageImageArrayNonUniformIndexing;
    }

    bool IsUniformTexelBufferArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing;
    }

    bool IsStorageTexelBufferArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing;
    }

    /// Returns true if the device supports float64 natively.
    bool IsFloat64Supported() const {
        return features.features.shaderFloat64;
    }

    /// Returns true if the device supports float16 natively.
    bool IsFloat16Supported() const {
        return float16_math;
    }

    /// Returns true if the device can run shaders built against the Vulkan memory model.
    bool IsVulkanMemoryModelSupported() const {
        return features.vulkan_memory_model.vulkanMemoryModel;
    }

    /// Returns true if the device supports int8 natively.
    bool IsInt8Supported() const {
        return features.shader_float16_int8.shaderInt8;
    }

    /// Returns true if the device allows 8-bit integer members in uniform/storage buffers.
    bool IsUniformAndStorageBuffer8BitAccessSupported() const {
        return features.bit8_storage.uniformAndStorageBuffer8BitAccess;
    }

    /// Returns true if the device allows 16-bit integer members in uniform/storage buffers.
    bool IsUniformAndStorageBuffer16BitAccessSupported() const {
        return features.bit16_storage.uniformAndStorageBuffer16BitAccess;
    }

    /// Returns true if the device supports reading 8-bit values from a storage buffer.
    bool IsStorageBuffer8BitAccessSupported() const {
        return features.bit8_storage.storageBuffer8BitAccess;
    }

    /// Returns true if the device supports reading 16-bit values from a storage buffer.
    bool IsStorageBuffer16BitAccessSupported() const {
        return features.bit16_storage.storageBuffer16BitAccess;
    }

    /// Returns true if the device supports binding multisample images as storage images.
    bool IsStorageImageMultisampleSupported() const {
        return features.features.shaderStorageImageMultisample;
    }

    /// Returns true if the device warp size can potentially be bigger than guest's warp size.
    bool IsWarpSizePotentiallyBiggerThanGuest() const {
        return is_warp_potentially_bigger;
    }

    /// Returns true if the device can be forced to use the guest warp size.
    bool IsGuestWarpSizeSupported(VkShaderStageFlagBits stage) const {
        return properties.subgroup_size_control.requiredSubgroupSizeStages & stage;
    }

    /// Returns true if a pipeline stage can be built with requiredSubgroupSize = GuestWarpSize.
    bool CanRequireGuestWarpSize(VkShaderStageFlagBits stage) const {
        const auto& control = properties.subgroup_size_control;
        return extensions.subgroup_size_control && IsGuestWarpSizeSupported(stage) &&
               control.minSubgroupSize <= GuestWarpSize && GuestWarpSize <= control.maxSubgroupSize;
    }

    /// The subgroup size to require for compute shaders without warp-width operations, 0 for the
    /// driver's choice. Intel (Xe2) compiles large compute shaders SIMD32 with register spills
    /// (5.7 KB of scratch for Ocarina of Time's global memory shaders) after also compiling them
    /// SIMD16: 16 lanes build ~2.6x faster without spills, but run slower (Hyrule Field on a
    /// B580: ~35 instead of ~39.5 fps; f84d5909908cc5ea 0.14 instead of 0.07 ms a dispatch), so
    /// it is off unless asked for. EDEN_NX2_INTEL_COMPUTE_SIMD16: 0 (default) off, 1 shaders with
    /// global memory accesses, 2 every shader without warp operations.
    u32 NarrowComputeSubgroupSize(bool uses_global_memory) const {
        constexpr u32 narrow = 16;
        static const int mode = [] {
            const char* value = std::getenv("EDEN_NX2_INTEL_COMPUTE_SIMD16");
            return value != nullptr && value[0] >= '0' && value[0] <= '2' ? value[0] - '0' : 0;
        }();
        const auto& control = properties.subgroup_size_control;
        const bool applies = mode == 2 || (mode == 1 && uses_global_memory);
        return applies && properties.properties.vendorID == 0x8086 &&
                       extensions.subgroup_size_control &&
                       IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT) &&
                       control.minSubgroupSize <= narrow && narrow <= control.maxSubgroupSize
                   ? narrow
                   : 0;
    }

    /// Returns true if a stage may run in subgroups narrower than the guest's warp (Intel runs
    /// fragment and vertex shaders SIMD8 or SIMD16, and those cannot be given a size).
    bool IsWarpSizePotentiallySmallerThanGuest() const {
        const u32 min_size = extensions.subgroup_size_control
                                 ? properties.subgroup_size_control.minSubgroupSize
                                 : properties.subgroup_properties.subgroupSize;
        return min_size != 0 && min_size < GuestWarpSize;
    }

    /// Returns true if the device supports the provided subgroup feature.
    bool IsSubgroupFeatureSupported(VkSubgroupFeatureFlagBits feature) const {
        return properties.subgroup_properties.supportedOperations & feature;
    }

    VkShaderStageFlags GetSubgroupSupportedStages() const {
        return properties.subgroup_properties.supportedStages;
    }

    u32 GetMaxSubgroupSize() const {
        return (std::max)(properties.subgroup_properties.subgroupSize,
                          properties.subgroup_size_control.maxSubgroupSize);
    }

    /// Returns the maximum number of push descriptors.
    u32 MaxPushDescriptors() const {
        return properties.push_descriptor.maxPushDescriptors;
    }

    /// Returns true if robust buffer access is enabled on the device.
    bool IsRobustBufferAccessEnabled() const {
        return features.features.robustBufferAccess == VK_TRUE;
    }

    /// Returns true if the device supports descriptor buffers.
    bool IsExtDescriptorBufferSupported() const {
        return extensions.descriptor_buffer;
    }

    /// Returns the descriptor buffer properties of the device.
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& DescriptorBufferProperties() const {
        return properties.descriptor_buffer;
    }

    /// Returns true if the device supports buffer device address.
    bool IsBufferDeviceAddressSupported() const {
        return extensions.buffer_device_address;
    }

    /// FRITCHII_OCCL_FLAG: the buffer device address feature is enabled for the flag pipelines
    /// alone (IsBufferDeviceAddressSupported stays false for everything else).
    bool IsOcclFlagDeviceAddressEnabled() const {
        return occl_flag_device_address || extensions.buffer_device_address;
    }

    /// Returns true if formatless image load is supported.
    bool IsFormatlessImageLoadSupported() const {
        // FRITCHII_FORMATLESS_13 (boot, default 0): Vulkan 1.3 allows formatless reads per format
        // even when the global feature is false (Intel on Windows, where every typeless image
        // read became a constant 0 and scenes rendered black). Off, as before.
        static const bool formatless_13 = [] {
            const char* value = std::getenv("FRITCHII_FORMATLESS_13");
            return value != nullptr && std::atoi(value) != 0;
        }();
        return features.features.shaderStorageImageReadWithoutFormat ||
               (formatless_13 && instance_version >= VK_API_VERSION_1_3);
    }

    /// Returns true if storage images of the extended formats (e.g. R16F, RG16F, R8) are supported.
    bool IsStorageImageExtendedFormatsSupported() const {
        return features.features.shaderStorageImageExtendedFormats;
    }

    /// Returns true if shader int64 is supported.
    bool IsShaderInt64Supported() const {
        return features.features.shaderInt64;
    }

    /// Returns true if shader int16 is supported.
    bool IsShaderInt16Supported() const {
        return features.features.shaderInt16;
    }

    // Returns true if depth bounds is supported.
    bool IsDepthBoundsSupported() const {
        return features.features.depthBounds;
    }

    /// Returns true when blitting from and to D24S8 images is supported.
    bool IsBlitDepth24Stencil8Supported() const {
        return is_blit_depth24_stencil8_supported;
    }

    /// Returns true when blitting from and to D32S8 images is supported.
    bool IsBlitDepth32Stencil8Supported() const {
        return is_blit_depth32_stencil8_supported;
    }

    /// Returns true if the device supports VK_NV_viewport_swizzle.
    bool IsNvViewportSwizzleSupported() const {
        return extensions.viewport_swizzle;
    }

    /// Returns true if the device supports VK_NV_viewport_array2.
    bool IsNvViewportArray2Supported() const {
        return extensions.viewport_array2;
    }

    /// Returns true if the device supports VK_NV_geometry_shader_passthrough.
    bool IsNvGeometryShaderPassthroughSupported() const {
        return extensions.geometry_shader_passthrough;
    }

    /// Returns true if the device supports VK_KHR_uniform_buffer_standard_layout.
    bool IsKhrUniformBufferStandardLayoutSupported() const {
        return extensions.uniform_buffer_standard_layout;
    }

    /// Returns true if the device supports VK_KHR_push_descriptor.
    bool IsKhrPushDescriptorSupported() const {
        return extensions.push_descriptor;
    }

    /// Returns true if VK_KHR_pipeline_executable_properties is enabled.
    bool IsKhrPipelineExecutablePropertiesEnabled() const {
        return extensions.pipeline_executable_properties;
    }

    /// Returns true if VK_KHR_swapchain_mutable_format is enabled.
    bool IsKhrSwapchainMutableFormatEnabled() const {
        return extensions.swapchain_mutable_format;
    }

    /// Returns true if VK_KHR_shader_float_controls is enabled.
    bool IsKhrShaderFloatControlsSupported() const {
        return extensions.shader_float_controls;
    }

    /// Returns true if the device supports VK_KHR_workgroup_memory_explicit_layout.
    bool IsKhrWorkgroupMemoryExplicitLayoutSupported() const {
        return extensions.workgroup_memory_explicit_layout;
    }

    bool IsWorkgroupMemoryExplicitLayout8BitAccessSupported() const {
        return extensions.workgroup_memory_explicit_layout &&
               features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout8BitAccess &&
               features.shader_float16_int8.shaderInt8;
    }

    bool IsWorkgroupMemoryExplicitLayout16BitAccessSupported() const {
        return extensions.workgroup_memory_explicit_layout &&
               features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout16BitAccess &&
               features.features.shaderInt16;
    }

    /// Returns true if the guest's HMMA.16816.F16 can run on the host's cooperative matrices
    /// (VK_KHR_cooperative_matrix with the same 16x8x16 shape on 16-bit floats, 32-lane subgroups).
    bool IsCooperativeHmmaSupported() const {
        return cooperative_hmma_16x8x16;
    }

    /// Returns true if the guest's HMMA can run through shared memory on 16x16x16 cooperative
    /// matrices of 16-bit floats, with compute subgroups forced to the guest's 32 lanes.
    bool IsCooperativeHmma16x16x16Supported() const {
        return cooperative_hmma_16x16x16;
    }

    /// Returns true if the guest's HMMA can run through shared memory as two 8x16x16 cooperative
    /// matrix products of 16-bit floats, with compute subgroups forced to the guest's 32 lanes.
    bool IsCooperativeHmma8x16x16Supported() const {
        return cooperative_hmma_8x16x16;
    }

    /// Returns true if the device supports VK_KHR_shader_quad_control.
    bool IsKhrShaderQuadControlSupported() const {
        return extensions.shader_quad_control && features.shader_quad_control.shaderQuadControl;
    }

    /// Returns true if the device can build tessellation pipelines.
    bool IsTessellationSupported() const {
        return features.features.tessellationShader;
    }

    /// Returns the largest patch size (control points, and TCS output vertices) a pipeline allows.
    u32 GetMaxTessellationPatchSize() const {
        return properties.properties.limits.maxTessellationPatchSize;
    }

    /// Returns true if the device supports task and mesh shaders (VK_EXT_mesh_shader).
    bool IsPipelineStatisticsQuerySupported() const {
        return features.features.pipelineStatisticsQuery;
    }

    /// Whether presents can carry ids and be waited on until shown (the host frame pacer).
    bool IsKhrPresentWaitSupported() const {
        return extensions.present_id && extensions.present_wait;
    }

    /// FRITCHII_PIPELINE_BINARY is on and the device has VK_KHR_pipeline_binary (with
    /// maintenance5): Fritchii keeps the compiled pipelines itself (vk_pipeline_binary.h).
    bool IsKhrPipelineBinaryEnabled() const {
        return pipeline_binary_enabled;
    }

    /// The driver's VK_KHR_pipeline_binary properties (zero when it doesn't offer it).
    const VkPhysicalDevicePipelineBinaryPropertiesKHR& PipelineBinaryProperties() const {
        return pipeline_binary_properties;
    }

    /// EDEN_NX2_GPL=1 and the device has VK_EXT_graphics_pipeline_library with fast linking:
    /// graphics pipelines are linked from per-stage libraries.
    bool IsExtGraphicsPipelineLibrarySupported() const {
        return graphics_pipeline_library_enabled;
    }

    bool IsExtMeshShaderSupported() const {
        return extensions.mesh_shader && features.mesh_shader.taskShader &&
               features.mesh_shader.meshShader;
    }

    /// Returns true if the device supports VK_KHR_image_format_list.
    bool IsKhrImageFormatListSupported() const {
        return extensions.image_format_list || instance_version >= VK_API_VERSION_1_2;
    }

    /// Returns true if the device supports VK_EXT_primitive_topology_list_restart.
    bool IsTopologyListPrimitiveRestartSupported() const {
        return features.primitive_topology_list_restart.primitiveTopologyListRestart;
    }

    /// Returns true if the device supports VK_EXT_primitive_topology_list_restart.
    bool IsPatchListPrimitiveRestartSupported() const {
        return features.primitive_topology_list_restart.primitiveTopologyPatchListRestart;
    }

    /// Returns true if the device supports VK_EXT_index_type_uint8.
    bool IsExtIndexTypeUint8Supported() const {
        return extensions.index_type_uint8;
    }

    /// Returns true if the device supports VK_EXT_sampler_filter_minmax.
    bool IsExtSamplerFilterMinmaxSupported() const {
        return extensions.sampler_filter_minmax;
    }

    /// Returns true if the device supports VK_EXT_shader_stencil_export.
    bool IsExtShaderStencilExportSupported() const {
        return extensions.shader_stencil_export;
    }

    /// Returns true if the device supports VK_KHR_create_renderpass2.
    bool IsKhrCreateRenderPass2Supported() const {
        return extensions.create_renderpass2 || instance_version >= VK_API_VERSION_1_2;
    }

    /// Returns true if the device supports VK_KHR_depth_stencil_resolve.
    bool IsKhrDepthStencilResolveSupported() const {
        return (extensions.depth_stencil_resolve || instance_version >= VK_API_VERSION_1_2) &&
               IsKhrCreateRenderPass2Supported();
    }

    /// Returns the supported resolve modes for the depth aspect.
    VkResolveModeFlags GetDepthResolveModes() const {
        return properties.depth_stencil_resolve.supportedDepthResolveModes;
    }

    /// Returns the supported resolve modes for the stencil aspect.
    VkResolveModeFlags GetStencilResolveModes() const {
        return properties.depth_stencil_resolve.supportedStencilResolveModes;
    }

    /// Returns true if only one of the depth and stencil aspects may be resolved.
    bool SupportsIndependentResolveNone() const {
        return properties.depth_stencil_resolve.independentResolveNone == VK_TRUE;
    }

    /// Returns true if depth/stencil operations can be performed efficiently.
    /// Either through shader export or hardware blits.
    bool CanPerformDepthStencilOperations() const {
        return extensions.shader_stencil_export || is_blit_depth24_stencil8_supported ||
               is_blit_depth32_stencil8_supported;
    }

    /// Returns true if the device supports VK_EXT_depth_range_unrestricted.
    bool IsExtDepthRangeUnrestrictedSupported() const {
        return extensions.depth_range_unrestricted;
    }

    /// Returns true if the device supports VK_EXT_depth_clip_control.
    bool IsExtDepthClipControlSupported() const {
        return extensions.depth_clip_control;
    }

    /// Returns true if the device supports VK_EXT_depth_bias_control.
    bool IsExtDepthBiasControlSupported() const {
        return extensions.depth_bias_control;
    }

    /// Returns true if the device supports VK_EXT_shader_viewport_index_layer.
    bool IsExtShaderViewportIndexLayerSupported() const {
        return extensions.shader_viewport_index_layer;
    }

    /// Returns true if the device supports VK_EXT_subgroup_size_control.
    bool IsExtSubgroupSizeControlSupported() const {
        return extensions.subgroup_size_control;
    }

    /// Returns true if vkResetQueryPool (host-side query reset) is supported.
    bool IsHostQueryResetSupported() const {
        return features.host_query_reset.hostQueryReset != VK_FALSE;
    }

    /// Returns true if the device supports VK_EXT_transform_feedback.
    bool IsExtTransformFeedbackSupported() const {
        return extensions.transform_feedback;
    }

    /// Returns true if transform feedback draw commands are supported.
    bool IsTransformFeedbackDrawSupported() const {
        return extensions.transform_feedback && properties.transform_feedback.transformFeedbackDraw;
    }

    /// Returns true if transform feedback query types are supported.
    bool IsTransformFeedbackQueriesSupported() const {
        return extensions.transform_feedback &&
               properties.transform_feedback.transformFeedbackQueries;
    }

    /// Returns true if the device supports VK_EXT_transform_feedback properly.
    bool AreTransformFeedbackGeometryStreamsSupported() const {
        return features.transform_feedback.geometryStreams;
    }

    /// Returns true if custom border colors can be created without a format.
    bool IsCustomBorderColorUsable() const {
        return extensions.custom_border_color &&
               features.custom_border_color.customBorderColors &&
               features.custom_border_color.customBorderColorWithoutFormat;
    }

    /// Takes budget for samplers carrying a custom border color, false when exhausted.
    bool TryReserveCustomBorderColorSamplers(size_t count) const;

    /// Gives back budget taken by TryReserveCustomBorderColorSamplers.
    void ReleaseCustomBorderColorSamplers(size_t count) const;

    /// Returns true if the device supports VK_EXT_color_write_enable.
    bool IsExtColorWriteEnableSupported() const {
        return extensions.color_write_enable;
    }

    /// Returns true if the device supports VK_EXT_border_color_swizzle.
    bool IsExtBorderColorSwizzleSupported() const {
        return extensions.border_color_swizzle;
    }

    /// Returns true if samplers must be carried with border color swizzle mapping.
    bool NeedsBorderColorSwizzleMapping() const {
        return extensions.border_color_swizzle &&
               !features.border_color_swizzle.borderColorSwizzleFromImage;
    }

    /// Returns true if borderColorSwizzleFromImage is available.
    bool IsBorderColorSwizzleFromImageSupported() const {
        return features.border_color_swizzle.borderColorSwizzleFromImage;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state.
    bool IsExtExtendedDynamicStateSupported() const {
        return extensions.extended_dynamic_state;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state2.
    bool IsExtExtendedDynamicState2Supported() const {
        return extensions.extended_dynamic_state2;
    }

    bool IsExtExtendedDynamicState2ExtrasSupported() const {
        return features.extended_dynamic_state2.extendedDynamicState2LogicOp;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state3.
    bool IsExtExtendedDynamicState3Supported() const {
        return extensions.extended_dynamic_state3;
    }

    /// Returns true if the device supports VK_EXT_4444_formats.
    bool IsExt4444FormatsSupported() const {
        return features.format_a4b4g4r4.formatA4B4G4R4;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state3.
    bool IsExtExtendedDynamicState3BlendingSupported() const {
        return dynamic_state3_blending;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state3.
    bool IsExtExtendedDynamicState3EnablesSupported() const {
        return dynamic_state3_enables;
    }

    /// Returns true if the device supports VK_EXT_filter_cubic
    bool IsExtFilterCubicSupported() const {
        return extensions.filter_cubic;
    }

    /// Returns true if the device supports VK_QCOM_filter_cubic_weights
    bool IsQcomFilterCubicWeightsSupported() const {
        return extensions.filter_cubic_weights;
    }

    /// Returns true if the device supports VK_EXT_line_rasterization.
    bool IsExtLineRasterizationSupported() const {
        return extensions.line_rasterization;
    }

    bool SupportsRectangularLines() const {
        return features.line_rasterization.rectangularLines != VK_FALSE;
    }

    bool SupportsSmoothLines() const {
        return features.line_rasterization.smoothLines != VK_FALSE;
    }

    bool SupportsStippledRectangularLines() const {
        return features.line_rasterization.stippledRectangularLines != VK_FALSE;
    }

    bool SupportsAlphaToOne() const {
        return features.features.alphaToOne != VK_FALSE;
    }

    bool SupportsDynamicState3DepthClampEnable() const {
        return dynamic_state3_depth_clamp_enable;
    }

    bool SupportsDynamicState3LogicOpEnable() const {
        return dynamic_state3_logic_op_enable;
    }

    bool SupportsDynamicState3LineRasterizationMode() const {
        return dynamic_state3_line_raster_mode;
    }

    bool SupportsDynamicState3ConservativeRasterizationMode() const {
        return dynamic_state3_conservative_raster_mode;
    }

    bool SupportsDynamicState3LineStippleEnable() const {
        return dynamic_state3_line_stipple_enable;
    }

    bool SupportsDynamicState3AlphaToCoverageEnable() const {
        return dynamic_state3_alpha_to_coverage;
    }

    bool SupportsDynamicState3AlphaToOneEnable() const {
        return dynamic_state3_alpha_to_one;
    }

    /// Returns true if the device supports VK_EXT_vertex_input_dynamic_state.
    bool IsExtVertexInputDynamicStateSupported() const {
        return extensions.vertex_input_dynamic_state;
    }

    /// Returns true if the device supports VK_EXT_shader_demote_to_helper_invocation
    bool IsExtShaderDemoteToHelperInvocationSupported() const {
        return extensions.shader_demote_to_helper_invocation;
    }

    /// Returns true if the device supports VK_EXT_conservative_rasterization.
    bool IsExtConservativeRasterizationSupported() const {
        return extensions.conservative_rasterization;
    }

    /// Returns true if the device supports VK_EXT_provoking_vertex.
    bool IsExtProvokingVertexSupported() const {
        return extensions.provoking_vertex;
    }

    /// Returns true if first vertex provoking mode can be used.
    bool SupportsProvokingVertexFirstMode() const {
        return extensions.provoking_vertex;
    }

    /// Returns true if last vertex provoking mode can be used.
    bool SupportsProvokingVertexLastMode() const {
        return extensions.provoking_vertex && features.provoking_vertex.provokingVertexLast;
    }

    /// Returns true if transform feedback preserves provoking vertex mode semantics.
    bool SupportsTransformFeedbackProvokingVertexPreservation() const {
        return extensions.provoking_vertex &&
               features.provoking_vertex.transformFeedbackPreservesProvokingVertex;
    }

    /// Returns true if the device supports VK_KHR_shader_atomic_int64.
    bool IsExtShaderAtomicInt64Supported() const {
        return extensions.shader_atomic_int64;
    }

    bool IsSharedInt64AtomicsSupported() const {
        return extensions.shader_atomic_int64 &&
               features.shader_atomic_int64.shaderSharedInt64Atomics;
    }

    /// Returns true if storage buffer float32 atomic add runs in hardware (VK_EXT_shader_atomic_float).
    bool IsFloat32AtomicAddSupported() const {
        return extensions.shader_atomic_float &&
               features.shader_atomic_float.shaderBufferFloat32AtomicAdd;
    }

    bool IsExtConditionalRendering() const {
        return extensions.conditional_rendering;
    }

    bool IsExtAstcDecodeModeSupported() const {
        return extensions.astc_decode_mode;
    }

    /// Returns true if descriptor bindings is partially bound.
    bool IsDescriptorBindingPartiallyBoundSupported() const {
        return features.descriptor_indexing.descriptorBindingPartiallyBound;
    }

    bool HasTimelineSemaphore() const;

    /// Returns true if the device supports VK_KHR_synchronization2.
    bool HasSynchronization2() const {
        return extensions.synchronization2;
    }

    /// Returns the minimum supported version of SPIR-V.
    u32 SupportedSpirvVersion() const {
        if (instance_version >= VK_API_VERSION_1_3) {
            return 0x00010600U;
        }
        if (extensions.spirv_1_4) {
            return 0x00010400U;
        }
        return 0x00010300U;
    }

    /// Returns true for an integrated GPU (its device-local memory is system memory).
    bool IsIntegrated() const {
        return is_integrated;
    }

    /// Returns true when a known debugging tool is attached.
    bool HasDebuggingToolAttached() const {
        return has_renderdoc || has_nsight_graphics || has_radeon_gpu_profiler;
    }

    /// @returns True if compute pipelines can cause crashing.
    bool HasBrokenCompute() const {
        return has_broken_compute;
    }

    /// Returns true when the device does not properly support cube compatibility.
    bool HasBrokenCubeImageCompatibility() const {
        return has_broken_cube_compatibility;
    }

    /// Returns true for AMD GCN4 and older (Polaris and earlier, found by device ID or by the
    /// lack of float16) on AMD's own drivers (CheezyEmu 132cc46c9e). Their driver uses a lot of
    /// RAM per compile of a large Switch 2 shader, so pipelines build on fewer threads there.
    bool IsAmdLegacyGcn() const {
        return is_amd_legacy_gcn;
    }

    /// Returns true if parallel shader compiling has issues with the current driver.
    bool HasBrokenParallelShaderCompiling() const {
        return has_broken_parallel_compiling;
    }

    /// Returns the vendor name reported from Vulkan.
    std::string_view GetVendorName() const {
        return properties.driver.driverName;
    }

    /// Returns the list of available extensions.
    const std::set<std::string, std::less<>>& GetAvailableExtensions() const {
        return supported_extensions;
    }

    u64 GetDeviceLocalMemory() const {
        return device_access_memory;
    }

    bool CanReportMemoryUsage() const {
        return extensions.memory_budget;
    }

    u64 GetDeviceMemoryUsage() const;
    /// GetDeviceMemoryUsage without EXP29's reuse: always asks the driver.
    u64 QueryDeviceMemoryUsage() const;
    /// The last GetDeviceMemoryUsage() answer.
    [[nodiscard]] u64 LastDeviceMemoryUsage() const noexcept {
        return memory_usage_value.load(std::memory_order_relaxed);
    }

    /// FRITCHII_KEEP_BLOCKS (vulkan_kept_blocks.h): the most the kept blocks may hold, from the
    /// texture cache's budget check: half the room between the cache's critical use and the
    /// budget, plus what the live usage leaves under the critical use.
    void LimitKeptBlocks(u64 budget, u64 critical) const;
    /// Gives every kept block back now (memory pressure); returns the bytes.
    u64 ReleaseKeptBlocks() const;
    /// Once a frame from the texture cache runtime: trims and logs the kept blocks.
    void TickKeptBlocks() const;
    /// Bytes the kept blocks hold (counted as free by GetDeviceMemoryUsage).
    [[nodiscard]] u64 KeptBlockBytes() const noexcept;

    /// The buffer allocator's heap budgets: VMA's at FRITCHII_MEMSTAGE_THREAD=0, as its user
    /// published them at 1 (vulkan_device.cpp).
    void BufferHeapBudgets(VmaBudget* budgets) const;
    void PublishBufferHeapBudgets() const;

    /// The driver's current memory budget for this process (VK_EXT_memory_budget), in
    /// GetDeviceMemoryUsage() terms: the usage at which the main device-local heap reaches its
    /// budget. 0 without the extension. The unused part of the allocator's blocks is not
    /// subtracted: see GetDeviceMemorySlack().
    u64 GetDeviceMemoryBudget() const;

    /// Memory the allocator holds from the driver in the main device-local heap but has no
    /// allocation in (the unused part of its blocks). GetDeviceMemoryUsage() does not count it,
    /// yet the driver does: with the heap at its budget it is not available to new images.
    u64 GetDeviceMemorySlack() const;

    /// FRITCHII_VRAM_REPORT: the main device-local heap's usage and budget as the driver reports
    /// them (all of this process's memory in it, the unused part of blocks included), the budget
    /// at most the EDEN_NX2_VRAM_LIMIT_MIB limit. False without VK_EXT_memory_budget.
    bool MainHeapUsage(u64& usage, u64& budget) const;

    /// FRITCHII_VRAM_BUDGET_MODE: MainHeapUsage plus the main heap's size (at most the
    /// EDEN_NX2_VRAM_LIMIT_MIB limit). False without VK_EXT_memory_budget.
    bool MainHeapNumbers(u64& size, u64& usage, u64& budget) const;

    /// FRITCHII_VRAM_BUDGET_MODE=os: the adapter's LUID, to find its OS counters. False when the
    /// driver gives none.
    bool GetDeviceLuid(u32& low, s32& high) const;

    /// The largest device-local heap, where images live.
    size_t GetMainHeap() const {
        return main_heap;
    }

    /// Boot: one line per memory heap (size, kind, the driver's budget and usage) and one line
    /// with every memory type (flags and heap).
    void LogMemoryHeaps() const;

    u32 GetSetsPerPool() const {
        return sets_per_pool;
    }

    bool SupportsD24DepthBuffer() const {
        return supports_d24_depth;
    }

    bool MustEmulateScaledFormats() const {
        return must_emulate_scaled_formats;
    }

    bool HasNullDescriptor() const {
        return features.robustness2.nullDescriptor;
    }

    bool MustEmulateBGR565() const;

    bool HasExactDepthBiasControl() const {
        return features.depth_bias_control.depthBiasExact;
    }

    u32 GetMaxVertexInputAttributes() const {
        return properties.properties.limits.maxVertexInputAttributes;
    }

    u32 GetMaxVertexInputBindings() const {
        return properties.properties.limits.maxVertexInputBindings;
    }

    u32 GetMaxViewports() const {
        return properties.properties.limits.maxViewports;
    }

    u32 GetMaxUserClipDistances() const {
        return properties.properties.limits.maxClipDistances;
    }

    bool SupportsConditionalBarriers() const {
        return supports_conditional_barriers;
    }

    bool SupportsMultiViewport() const {
        return features2.features.multiViewport;
    }

    /// Returns true if the device supports VK_KHR_maintenance1.
    bool IsKhrMaintenance1Supported() const {
        return extensions.maintenance1;
    }

    /// Returns true if the device supports VK_KHR_maintenance2.
    bool IsKhrMaintenance2Supported() const {
        return extensions.maintenance2;
    }

    /// Returns true if the device supports VK_KHR_maintenance3.
    bool IsKhrMaintenance3Supported() const {
        return extensions.maintenance3;
    }

    /// Returns true if the device supports VK_KHR_maintenance4.
    bool IsKhrMaintenance4Supported() const {
        return extensions.maintenance4;
    }

    /// Returns true if the device supports VK_KHR_maintenance5.
    bool IsKhrMaintenance5Supported() const {
        return extensions.maintenance5;
    }

    /// Returns true if polygon mode POINT supports gl_PointSize.
    bool SupportsPolygonModePointSize() const {
        return extensions.maintenance5 && properties.maintenance5.polygonModePointSize;
    }

    /// Returns true if depth/stencil swizzle ONE is supported.
    bool SupportsDepthStencilSwizzleOne() const {
        return extensions.maintenance5 && properties.maintenance5.depthStencilSwizzleOneSupport;
    }

    /// Returns true if early fragment tests optimizations are available.
    bool SupportsEarlyFragmentTests() const {
        return extensions.maintenance5 &&
               properties.maintenance5.earlyFragmentMultisampleCoverageAfterSampleCounting &&
               properties.maintenance5.earlyFragmentSampleMaskTestBeforeSampleCounting;
    }

    /// Returns true if the device supports VK_KHR_maintenance6.
    bool IsKhrMaintenance6Supported() const {
        return extensions.maintenance6;
    }

    /// Returns true if the device supports VK_KHR_maintenance7.
    bool IsKhrMaintenance7Supported() const {
        return extensions.maintenance7;
    }

    /// Returns true if the device supports VK_KHR_maintenance8.
    bool IsKhrMaintenance8Supported() const {
        return extensions.maintenance8;
    }

    /// Returns true if the device supports UINT8 index buffer conversion via compute shader.
    bool SupportsUint8Indices() const {
        return features.bit8_storage.storageBuffer8BitAccess &&
               features.bit16_storage.storageBuffer16BitAccess;
    }

    [[nodiscard]] static constexpr bool CheckBrokenCompute(VkDriverId driver_id,
                                                           u32 driver_version) {
        if (driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
            const u32 major = VK_API_VERSION_MAJOR(driver_version);
            const u32 minor = VK_API_VERSION_MINOR(driver_version);
            const u32 patch = VK_API_VERSION_PATCH(driver_version);
            if (major == 0 && minor == 405 && patch < 286) {
                LOG_WARNING(
                    Render_Vulkan,
                    "Intel proprietary drivers 0.405.0 until 0.405.286 have broken compute");
                return true;
            }
        }
        return false;
    }

    bool IsNvidia() const noexcept {
        return properties.driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    }

    bool IsMoltenVK() const noexcept {
        return properties.driver.driverID == VK_DRIVER_ID_MOLTENVK;
    }

    /// The multiple vertex input binding strides must be. Metal needs 4 bytes, which MoltenVK
    /// reports as minVertexInputBindingStrideAlignment through VK_KHR_portability_subset.
    u32 GetMinVertexInputBindingStrideAlignment() const noexcept {
        return IsMoltenVK() ? 4 : 1;
    }

    NvidiaArchitecture GetNvidiaArch() const noexcept {
        return nvidia_arch;
    }

    /// GPU logging integration
    void InitializeGPULogging();
    void ShutdownGPULogging();

private:
    /// Checks if the physical device is suitable and configures the object state
    /// with all necessary info about its properties.
    bool GetSuitability(bool requires_swapchain);

    // Remove extensions which have incomplete feature support.
    void RemoveUnsuitableExtensions();

    void RemoveExtension(bool& extension, const std::string& extension_name);
    void RemoveExtensionIfUnsuitable(bool is_suitable, const std::string& extension_name);

    template <typename Feature>
    void RemoveExtensionFeature(bool& extension, Feature& feature,
                                const std::string& extension_name);
    template <typename Feature>
    void RemoveExtensionFeatureIfUnsuitable(bool is_suitable, Feature& feature,
                                            const std::string& extension_name);

    /// Sets up queue families.
    void SetupFamilies(VkSurfaceKHR surface);

    /// Collects information about attached tools.
    void CollectToolingInfo();

    /// Collects information about the device's local memory.
    void CollectPhysicalMemoryInfo();

    /// Returns a list of queue initialization descriptors.
    std::vector<VkDeviceQueueCreateInfo> GetDeviceQueueCreateInfos() const;

    /// Returns true if ASTC textures are natively supported.
    bool ComputeIsOptimalAstcSupported() const;

    /// Returns true if the device natively supports blitting depth stencil images.
    bool TestDepthStencilBlits(VkFormat format) const;

    void LoadStaticPipelineCache();
    void SaveStaticPipelineCache() const;

private:
    VkInstance instance;         ///< Vulkan instance.
    VmaAllocator allocator{};    ///< VMA allocator.
    VmaAllocator buffer_allocator{}; ///< See GetBufferAllocator; null when not split.
    /// BufferHeapBudgets at FRITCHII_MEMSTAGE_THREAD=1: per heap block count, allocation count,
    /// block bytes, allocation bytes, usage, budget; and the allocator's finished-operation count
    /// they were taken at. Published and read under buffer_budgets_mutex.
    void PublishBufferHeapBudgetsLocked() const;
    mutable std::mutex buffer_budgets_mutex;
    mutable std::array<std::array<u64, 6>, VK_MAX_MEMORY_HEAPS> buffer_budgets{};
    mutable u64 buffer_budget_ops{~u64{0}};
    bool allocator_thread_safe{}; ///< See IsAllocatorThreadSafe.
    mutable std::mutex separate_pools_mutex;
    /// [allocator: main, buffers][SeparatePool][memory type]
    mutable std::array<std::array<std::array<VmaPool, VK_MAX_MEMORY_TYPES>, 2>, 2> separate_pools{};
    vk::DeviceDispatch dld;      ///< Device function pointers.
    vk::PhysicalDevice physical; ///< Physical device.
    vk::Device logical;          ///< Logical device.
    vk::Queue graphics_queue;    ///< Main graphics queue.
    vk::Queue present_queue;     ///< Main present queue.
    vk::Queue side_queue;        ///< FRITCHII_READ_SIDE_COPY >= 2: graphics family, index 1.
    bool has_side_queue{};
    vk::Queue link_hog_queue;    ///< FRITCHII_SIM_LINK_HOG: a transfer family's queue.
    bool has_link_hog_queue{};
    u32 link_hog_family{};
    vk::PipelineCache static_pipeline_cache;
    bool owns_static_pipeline_cache{};
    u32 instance_version{};      ///< Vulkan instance version.
    u32 graphics_family{};       ///< Main graphics queue family index.
    u32 present_family{};        ///< Main present queue family index.
    bool graphics_family_sparse_binding{};

    struct Extensions {
#define EXTENSION(prefix, macro_name, var_name) bool var_name{};
#define FEATURE(prefix, struct_name, macro_name, var_name) bool var_name{};

        FOR_EACH_VK_FEATURE_1_1(FEATURE);
        FOR_EACH_VK_FEATURE_1_2(FEATURE);
        FOR_EACH_VK_FEATURE_1_3(FEATURE);
        FOR_EACH_VK_FEATURE_1_4(FEATURE);
        FOR_EACH_VK_FEATURE_EXT(FEATURE);
        FOR_EACH_VK_EXTENSION(EXTENSION);

#undef EXTENSION
#undef FEATURE
    };

    struct Features {
#define FEATURE_CORE(prefix, struct_name, macro_name, var_name)                                    \
    VkPhysicalDevice##struct_name##Features var_name{};
#define FEATURE_EXT(prefix, struct_name, macro_name, var_name)                                     \
    VkPhysicalDevice##struct_name##Features##prefix var_name{};

        FOR_EACH_VK_FEATURE_1_1(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_2(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_3(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_4(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_EXT(FEATURE_EXT);

#undef FEATURE_CORE
#undef FEATURE_EXT

        VkPhysicalDeviceFeatures features{};
    };

    struct Properties {
        VkPhysicalDeviceDriverProperties driver{};
        VkPhysicalDeviceSubgroupProperties subgroup_properties{};
        VkPhysicalDeviceFloatControlsProperties float_controls{};
        VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor{};
        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer{};
        VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_control{};
        VkPhysicalDeviceTransformFeedbackPropertiesEXT transform_feedback{};
        VkPhysicalDeviceMaintenance5PropertiesKHR maintenance5{};
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT graphics_pipeline_library{};
        VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve{};
        VkPhysicalDeviceCustomBorderColorPropertiesEXT custom_border_color{};
        VkPhysicalDeviceCooperativeMatrixPropertiesKHR cooperative_matrix{};
        VkPhysicalDeviceShaderIntegerDotProductProperties integer_dot_product{};

        VkPhysicalDeviceProperties properties{};
    };

    Extensions extensions{};
    Features features{};
    Properties properties{};

    /// EDEN_NX2_GPL: kept out of the extension tables so a run without the switch creates the
    /// device exactly as before.
    bool graphics_pipeline_library_requested{};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT graphics_pipeline_library_features{};
    bool graphics_pipeline_library_enabled{};

    /// FRITCHII_PIPELINE_BINARY: kept out of the extension tables like EDEN_NX2_GPL, so a run
    /// without the switch creates the device exactly as before.
    bool pipeline_binary_offered{};
    bool pipeline_binary_requested{};
    VkPhysicalDevicePipelineBinaryFeaturesKHR pipeline_binary_features{};
    VkPhysicalDevicePipelineBinaryPropertiesKHR pipeline_binary_properties{};
    bool pipeline_binary_enabled{};

    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceFaultFeaturesEXT device_fault_features{};
    /// FRITCHII_VA_REPORT=1: VK_EXT_device_address_binding_report (vulkan_debug_callback.h).
    VkPhysicalDeviceAddressBindingReportFeaturesEXT address_binding_report_features{};
    VkPhysicalDeviceProperties2 properties2{};

    // Misc features
    bool is_optimal_astc_supported{};          ///< Support for all guest ASTC formats.
    bool is_blit_depth24_stencil8_supported{}; ///< Support for blitting from and to D24S8.
    bool is_blit_depth32_stencil8_supported{}; ///< Support for blitting from and to D32S8.
    bool is_warp_potentially_bigger{};         ///< Host warp size can be bigger than guest.
    bool float16_math{};                       ///< Shaders may do float16 arithmetic.
    bool cooperative_hmma_16x8x16{};           ///< HMMA runs on the guest's matrix shape.
    bool cooperative_hmma_16x16x16{};          ///< HMMA runs on 16x16x16 matrices.
    bool cooperative_hmma_8x16x16{};           ///< HMMA runs on two 8x16x16 products.
    bool is_integrated{};                      ///< Is GPU an iGPU.
    bool is_virtual{};                         ///< Is GPU a virtual GPU.
    bool is_non_gpu{};                         ///< Is SoftwareRasterizer, FPGA, non-GPU device.
    bool has_broken_compute{};                 ///< Compute shaders can cause crashes
    bool has_broken_cube_compatibility{};      ///< Has broken cube compatibility bit
    bool has_broken_descriptor_aliasing{};     ///< Miscompiles descriptors aliased on one binding
    bool has_broken_parallel_compiling{};      ///< Has broken parallel shader compiling.
    bool is_amd_legacy_gcn{};                  ///< AMD GCN4 or older on an AMD driver.
    bool has_renderdoc{};                      ///< Has RenderDoc attached
    bool has_nsight_graphics{};                ///< Has Nsight Graphics attached
    bool has_radeon_gpu_profiler{};            ///< Has Radeon GPU Profiler attached.
    bool supports_d24_depth{};                 ///< Supports D24 depth buffers.
    bool must_emulate_scaled_formats{};        ///< Requires scaled vertex format emulation
    bool dynamic_state3_blending{};            ///< Has blending features of dynamic_state3.
    bool dynamic_state3_enables{};             ///< Has at least one enable feature of dynamic_state3.
    bool dynamic_state3_depth_clamp_enable{};
    bool dynamic_state3_logic_op_enable{};
    bool dynamic_state3_line_raster_mode{};
    bool dynamic_state3_conservative_raster_mode{};
    bool dynamic_state3_line_stipple_enable{};
    bool dynamic_state3_alpha_to_coverage{};
    bool dynamic_state3_alpha_to_one{};
    bool supports_conditional_barriers{};      ///< Allows barriers in conditional control flow.
    mutable std::atomic<size_t> custom_border_color_samplers_used{};
    mutable std::atomic<u64> memory_usage_value{}; ///< EXP29 & 1: last GetDeviceMemoryUsage()
    mutable std::atomic<s64> memory_usage_time{};  ///< When it was taken (steady clock ns)
    u64 device_access_memory{};                ///< Total size of device local memory in bytes.
    u32 sets_per_pool{};                       ///< Sets per Description Pool
    NvidiaArchitecture nvidia_arch{NvidiaArchitecture::Arch_AmpereOrNewer};

    // Telemetry parameters
    std::set<std::string, std::less<>> supported_extensions; ///< Reported Vulkan extensions.
    std::set<std::string, std::less<>> loaded_extensions;    ///< Loaded Vulkan extensions.
    bool occl_flag_device_address{}; ///< FRITCHII_OCCL_FLAG kept the BDA feature
    bool streamline_extensions{}; ///< NVIDIA DLSS's extensions are loaded.
    bool streamline_attached{};   ///< Streamline has this device.
    std::vector<size_t> valid_heap_memory;                   ///< Heaps used.
    size_t main_heap{};       ///< Largest device-local heap, where images live.
    u64 heap_size_limit{};    ///< EDEN_NX2_VRAM_LIMIT_MIB for device-local heaps, 0 if unset.

    /// Format properties dictionary.
    ::Common::unordered_map<VkFormat, VkFormatProperties> format_properties;

    /// Nsight Aftermath GPU crash tracker
    std::unique_ptr<NsightAftermathTracker> nsight_aftermath_tracker;

    /// FRITCHII_KEEP_BLOCKS: VMA's freed device-local blocks (vulkan_kept_blocks.h).
    void CreateKeptBlocks();
    std::unique_ptr<KeptBlocks> kept_blocks;
};

/// What the graphics card pick looks at for one Vulkan physical device.
struct PhysicalDeviceSummary {
    std::string name;     ///< VkPhysicalDeviceProperties::deviceName
    VkPhysicalDeviceType type{};
    u64 local_memory{};   ///< Largest device-local heap, in bytes
};

/// The card a game runs on, and why (for the log).
struct PhysicalDevicePick {
    size_t index{};
    std::string_view reason;
};

std::vector<PhysicalDeviceSummary> SummarizePhysicalDevices(
    const vk::InstanceDispatch& dld, std::span<const VkPhysicalDevice> devices);

/// Fritchii's graphics card pick. A saved name (vulkan_device_name) is the player's choice and
/// is found again by name when the device order changes. Without one, a saved index other than
/// 0 is an earlier choice too; index 0 is the default, and there the discrete card with the most
/// device-local memory wins over an integrated GPU (a 5600G + Arc B580 PC landed on the 5600G's
/// iGPU). A saved card that is gone falls back to the same automatic pick.
PhysicalDevicePick PickPhysicalDevice(std::span<const PhysicalDeviceSummary> devices,
                                      u32 saved_index, std::string_view saved_name);

} // namespace Vulkan
