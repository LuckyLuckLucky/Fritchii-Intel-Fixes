// Differential test of two SPIR-V builds of one compute shader on the host GPU: builds both with
// the emulator's settings (32 full lanes), runs each on identical pseudo-random inputs and
// compares every storage buffer and storage image afterwards.
// Usage: difftest.exe a.spv b.spv layout.txt <groups x> <groups y> <groups z> [cbuf fill]
//   layout.txt lines: "<set> <binding> <VkDescriptorType> <count> <dim>" (dim: 1D 2D 3D Cube
//   Buffer Array2D, for images), from spirv-cross --reflect.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("FAILED %s = %d (line %d)\n", #x, r_, __LINE__);                           \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

namespace {
constexpr VkDeviceSize kBufferSize = 16ull << 20;
constexpr uint32_t kImage2D = 1024, kImage3D = 64, kLayers = 8;

struct Binding {
    uint32_t set, binding, type, count;
    std::string dim;
};
struct Resource {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* map{};
    VkImage image{};
    VkImageView view{};
    VkBufferView texel_view{};
    VkDeviceSize image_bytes{};
    VkExtent3D extent{};
    uint32_t layers{1};
    bool written{};
};

VkDevice g_dev;
VkPhysicalDeviceMemoryProperties g_mem;
uint32_t g_seed = 1;

float Rand() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) * (1.0f / 16777216.0f);
}
uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7bff);
    return static_cast<uint16_t>(sign | (exp << 10) | ((x >> 13) & 0x3ff));
}
uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    std::exit(3);
}
void Buffer(Resource& r, VkBufferUsageFlags usage, VkDeviceSize size) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    CHECK(vkCreateBuffer(g_dev, &ci, nullptr, &r.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_dev, r.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &r.memory));
    CHECK(vkBindBufferMemory(g_dev, r.buffer, r.memory, 0));
    CHECK(vkMapMemory(g_dev, r.memory, 0, size, 0, &r.map));
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::printf("usage: difftest a.spv b.spv layout.txt gx gy gz [cbuf fill hex|rand]\n");
        return 1;
    }
    std::vector<Binding> bindings;
    {
        std::ifstream f(argv[3]);
        for (std::string line; std::getline(f, line);) {
            std::istringstream in(line);
            Binding b{};
            if (in >> b.set >> b.binding >> b.type >> b.count) {
                in >> b.dim;
                bindings.push_back(b);
            }
        }
    }
    const uint32_t groups[3] = {static_cast<uint32_t>(std::atoi(argv[4])),
                                static_cast<uint32_t>(std::atoi(argv[5])),
                                static_cast<uint32_t>(std::atoi(argv[6]))};
    const std::string fill = argc > 7 ? argv[7] : "80";
    // argv[8]: scale of the random buffer and texture values (default 1: within [-1, 1]).
    const float scale = argc > 8 ? static_cast<float>(std::atof(argv[8])) : 1.0f;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(instance, &n, phys.data());
    VkPhysicalDevice gpu{};
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID == 0x8086) gpu = p;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu, &g_mem);
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &coop;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    const char* exts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &g_dev));
    VkQueue queue{};
    vkGetDeviceQueue(g_dev, 0, 0, &queue);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cpool{};
    CHECK(vkCreateCommandPool(g_dev, &cpci, nullptr, &cpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(vkAllocateCommandBuffers(g_dev, &cbai, &cmd));
    const auto run = [&](auto&& record) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        record();
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
    };
    const auto barrier_all = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    // Resources and the initial contents they get before each run.
    std::vector<Resource> res(bindings.size());
    std::vector<std::vector<uint8_t>> initial(bindings.size());
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};
    CHECK(vkCreateSampler(g_dev, &sci, nullptr, &sampler));
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        auto& r = res[i];
        if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            continue;
        }
        const bool is_image = b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                              b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                              b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (!is_image) {
            const VkBufferUsageFlags usage =
                b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                : b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                    ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
                : b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                    ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                    : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            const VkDeviceSize size = b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? 65536 : kBufferSize;
            Buffer(r, usage, size);
            initial[i].resize(size);
            auto* words = reinterpret_cast<uint32_t*>(initial[i].data());
            for (size_t w = 0; w < size / 4; ++w) {
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    words[w] = fill == "rand" ? (static_cast<uint32_t>(Rand() * 2048))
                                              : static_cast<uint32_t>(std::strtoul(fill.c_str(), nullptr, 16));
                } else {
                    words[w] = Half((Rand() * 2 - 1) * scale) | (uint32_t{Half((Rand() * 2 - 1) * scale)} << 16);
                }
            }
            if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                VkBufferViewCreateInfo vci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                vci.buffer = r.buffer;
                vci.format = VK_FORMAT_R32_UINT;
                vci.range = VK_WHOLE_SIZE;
                CHECK(vkCreateBufferView(g_dev, &vci, nullptr, &r.texel_view));
            }
            r.written = b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                        b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            continue;
        }
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
        ci.imageType = VK_IMAGE_TYPE_2D;
        r.extent = {kImage2D, kImage2D, 1};
        if (b.dim == "3D") {
            ci.imageType = VK_IMAGE_TYPE_3D;
            view_type = VK_IMAGE_VIEW_TYPE_3D;
            r.extent = {kImage3D, kImage3D, kImage3D};
        } else if (b.dim == "Array2D") {
            view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            r.layers = kLayers;
        }
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = r.extent;
        ci.mipLevels = 1;
        ci.arrayLayers = r.layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        CHECK(vkCreateImage(g_dev, &ci, nullptr, &r.image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(g_dev, r.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &r.memory));
        CHECK(vkBindImageMemory(g_dev, r.image, r.memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r.image;
        vi.viewType = view_type;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, r.layers};
        CHECK(vkCreateImageView(g_dev, &vi, nullptr, &r.view));
        r.image_bytes = VkDeviceSize{r.extent.width} * r.extent.height * r.extent.depth * r.layers * 8;
        Buffer(r, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, r.image_bytes);
        initial[i].resize(r.image_bytes);
        auto* texels = reinterpret_cast<uint16_t*>(initial[i].data());
        for (size_t t = 0; t < r.image_bytes / 2; ++t) {
            texels[t] = Half(Rand() * scale);
        }
        r.written = b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }

    // One descriptor set layout per set index.
    uint32_t max_set = 0;
    for (const auto& b : bindings) max_set = std::max(max_set, b.set + 1);
    std::vector<VkDescriptorSetLayout> layouts(max_set);
    std::vector<VkDescriptorSet> sets(max_set);
    std::vector<VkDescriptorPoolSize> sizes;
    for (uint32_t s = 0; s < max_set; ++s) {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const auto& b : bindings) {
            if (b.set == s) {
                lb.push_back({b.binding, static_cast<VkDescriptorType>(b.type), b.count,
                              VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
                sizes.push_back({static_cast<VkDescriptorType>(b.type), b.count});
            }
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = static_cast<uint32_t>(lb.size());
        lci.pBindings = lb.data();
        CHECK(vkCreateDescriptorSetLayout(g_dev, &lci, nullptr, &layouts[s]));
    }
    if (sizes.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = std::max(max_set, 1u);
    dpci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    dpci.pPoolSizes = sizes.data();
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(g_dev, &dpci, nullptr, &pool));
    for (uint32_t s = 0; s < max_set; ++s) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layouts[s];
        CHECK(vkAllocateDescriptorSets(g_dev, &dsai, &sets[s]));
    }
    std::vector<VkDescriptorBufferInfo> buffer_infos(bindings.size() * 64);
    std::vector<VkDescriptorImageInfo> image_infos(bindings.size() * 64);
    std::vector<VkBufferView> texel_views(bindings.size() * 64);
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        std::vector<VkWriteDescriptorSet> writes;
        for (uint32_t e = 0; e < std::min<uint32_t>(b.count, 64); ++e) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[b.set];
            w.dstBinding = b.binding;
            w.dstArrayElement = e;
            w.descriptorCount = 1;
            w.descriptorType = static_cast<VkDescriptorType>(b.type);
            const size_t slot = i * 64 + e;
            switch (b.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                buffer_infos[slot] = {res[i].buffer, 0, VK_WHOLE_SIZE};
                w.pBufferInfo = &buffer_infos[slot];
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                texel_views[slot] = res[i].texel_view;
                w.pTexelBufferView = &texel_views[slot];
                break;
            default:
                image_infos[slot] = {sampler, res[i].view, VK_IMAGE_LAYOUT_GENERAL};
                w.pImageInfo = &image_infos[slot];
                break;
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(g_dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = max_set;
    plci.pSetLayouts = layouts.data();
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(g_dev, &plci, nullptr, &layout));

    std::vector<std::vector<std::vector<uint8_t>>> outputs(2);
    for (int which = 0; which < 2; ++which) {
        std::ifstream f(argv[1 + which], std::ios::binary | std::ios::ate);
        std::vector<uint32_t> code(static_cast<size_t>(f.tellg()) / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule module{};
        CHECK(vkCreateShaderModule(g_dev, &smci, nullptr, &module));
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req,
                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        ci.layout = layout;
        VkPipeline pipeline{};
        CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
        // Same inputs for both runs.
        for (size_t i = 0; i < res.size(); ++i) {
            if (!res[i].image) {
                std::memcpy(res[i].map, initial[i].data(), initial[i].size());
            } else {
                std::memcpy(res[i].map, initial[i].data(), initial[i].size());
            }
        }
        run([&] {
            for (size_t i = 0; i < res.size(); ++i) {
                if (!res[i].image) continue;
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.image = res[i].image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, res[i].layers};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, res[i].layers};
                copy.imageExtent = res[i].extent;
                vkCmdCopyBufferToImage(cmd, res[i].buffer, res[i].image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            }
            barrier_all();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            if (max_set) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, max_set,
                                        sets.data(), 0, nullptr);
            }
            uint32_t push[32]{};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            vkCmdDispatch(cmd, groups[0], groups[1], groups[2]);
            barrier_all();
            for (size_t i = 0; i < res.size(); ++i) {
                if (!res[i].image) continue;
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, res[i].layers};
                copy.imageExtent = res[i].extent;
                vkCmdCopyImageToBuffer(cmd, res[i].image, VK_IMAGE_LAYOUT_GENERAL, res[i].buffer, 1, &copy);
            }
        });
        for (size_t i = 0; i < res.size(); ++i) {
            const auto* p = static_cast<const uint8_t*>(res[i].map);
            const size_t size = res[i].image ? res[i].image_bytes : initial[i].size();
            outputs[which].emplace_back(p, p + size);
        }
        vkDestroyPipeline(g_dev, pipeline, nullptr);
    }
    // Compare: words changed by either run, words that differ between the runs.
    // A value differs when its relative error, against a floor of 0.05 (the inputs are within
    // [-1, 1], so cancellations near zero don't count), is over 5 %, or when only one run gave a
    // NaN or infinity.
    const auto from_half = [](uint16_t h) {
        const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
        uint32_t x = exp == 0 ? sign : exp == 31 ? (sign | 0x7f800000 | (mant << 13))
                                                 : (sign | ((exp - 15 + 127) << 23) | (mant << 13));
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    };
    size_t changed = 0, differ = 0, total = 0, special = 0, bitwise = 0;
    double max_rel = 0, sum_rel = 0;
    int worst_binding = -1;
    size_t worst = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        if (!res[i].written) continue;
        const auto& a = outputs[0][i];
        const auto& b = outputs[1][i];
        const auto& init = initial[i];
        size_t d = 0;
        for (size_t w = 0; w + 2 <= a.size(); w += 2) {
            uint16_t va, vb, vi;
            std::memcpy(&va, &a[w], 2);
            std::memcpy(&vb, &b[w], 2);
            std::memcpy(&vi, &init[w], 2);
            ++total;
            if (va == vi && vb == vi) continue;
            ++changed;
            if (va == vb) continue;
            ++bitwise;
            const float fa = from_half(va), fb = from_half(vb);
            if (std::isfinite(fa) != std::isfinite(fb)) {
                ++special;
                ++d;
                continue;
            }
            if (!std::isfinite(fa)) continue;
            const double rel = std::fabs(double(fa) - fb) /
                               std::max({std::fabs(double(fa)), std::fabs(double(fb)), 0.05});
            max_rel = std::max(max_rel, rel);
            sum_rel += rel;
            if (rel > 0.05) ++d;
        }
        differ += d;
        if (d > worst) {
            worst = d;
            worst_binding = static_cast<int>(bindings[i].binding);
        }
    }
    std::printf("RESULT changed %zu of %zu halves, differing %zu (worst binding %d) bitwise %zu "
                "special %zu maxrel %.4f meanrel %.6f\n",
                changed, total, differ, worst_binding, bitwise, special, max_rel,
                bitwise ? sum_rel / bitwise : 0.0);
    return differ != 0 ? 10 : 0;
}
