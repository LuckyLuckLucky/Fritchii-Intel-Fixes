// Runs two SPIR-V builds of the same compute shader (software HMMA and cooperative matrices) on
// identical pseudo-random inputs, times them on the GPU and compares their output images.
// Usage: hotrun.exe a.spv b.spv  (bindings: 0 UBO 64 KiB, 1 SSBO, 2 sampler2D, 3 storage image)
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("FAILED %s = %d (line %d)\n", #x, r_, __LINE__);                           \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

constexpr uint32_t kTexW = 2560, kTexH = 1440;
constexpr uint32_t kOutW = 4096, kOutH = 2048;
constexpr VkDeviceSize kUboSize = 64 * 1024, kSsboSize = 64ull << 20;

static uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    const uint32_t mant = (x >> 13) & 0x3ff;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7bff);
    return static_cast<uint16_t>(sign | (exp << 10) | mant);
}
static float FromHalf(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ff;
    uint32_t x;
    if (exp == 0) {
        x = sign;
    } else if (exp == 31) {
        x = sign | 0x7f800000 | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

struct Gpu {
    VkPhysicalDevice phys{};
    VkDevice dev{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties mem{};
    float period{};
    uint32_t Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        std::printf("no memory type\n");
        std::exit(1);
    }
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* map{};
};
static Buffer MakeBuffer(Gpu& g, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    CHECK(vkCreateBuffer(g.dev, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g.Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(g.dev, &ai, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(g.dev, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(g.dev, b.memory, 0, size, 0, &b.map));
    return b;
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
};
static Image MakeImage(Gpu& g, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Image im;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    CHECK(vkCreateImage(g.dev, &ci, nullptr, &im.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.dev, im.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g.Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(g.dev, &ai, nullptr, &im.memory));
    CHECK(vkBindImageMemory(g.dev, im.image, im.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(g.dev, &vi, nullptr, &im.view));
    return im;
}

static std::vector<uint32_t> ReadSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    std::vector<uint32_t> w(static_cast<size_t>(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(w.data()), w.size() * 4);
    return w;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: hotrun a.spv b.spv\n");
        return 1;
    }
    Gpu g;
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
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID == 0x8086) {
            g.phys = p;
            g.period = props.limits.timestampPeriod;
            std::printf("%s\n", props.deviceName);
        }
    }
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &coop;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(g.phys, &features);
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE; // as the emulator
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
    CHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.dev));
    vkGetDeviceQueue(g.dev, 0, 0, &g.queue);

    // Inputs: the constant buffer stays zero (offsets, sizes); the storage buffer and the texture
    // get small pseudo-random halves.
    Buffer ubo = MakeBuffer(g, kUboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memset(ubo.map, 0, kUboSize);
    if (argc > 3) {
        // argv[3]: every constant buffer word set to this value (hex), or "rand" for small values.
        auto* cb = static_cast<uint32_t*>(ubo.map);
        uint32_t s = 777;
        for (size_t i = 0; i < kUboSize / 4; ++i) {
            s = s * 1664525u + 1013904223u;
            cb[i] = std::string{argv[3]} == "rand" ? ((s >> 16) & 0x7ff) : static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
        }
    }
    Buffer ssbo = MakeBuffer(g, kSsboSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint32_t seed = 12345;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) * (1.0f / 16777216.0f);
    };
    auto* words = static_cast<uint16_t*>(ssbo.map);
    for (VkDeviceSize i = 0; i < kSsboSize / 2; ++i) {
        words[i] = Half(rnd() * 2.0f - 1.0f);
    }
    Buffer staging = MakeBuffer(g, VkDeviceSize{kTexW} * kTexH * 8,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* texels = static_cast<uint16_t*>(staging.map);
    for (size_t i = 0; i < size_t{kTexW} * kTexH * 4; ++i) {
        texels[i] = Half(rnd());
    }
    Buffer readback = MakeBuffer(g, VkDeviceSize{kOutW} * kOutH * 8,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Image tex = MakeImage(g, kTexW, kTexH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Image out = MakeImage(g, kOutW, kOutH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};
    CHECK(vkCreateSampler(g.dev, &sci, nullptr, &sampler));

    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 4;
    lci.pBindings = bindings;
    VkDescriptorSetLayout set_layout{};
    CHECK(vkCreateDescriptorSetLayout(g.dev, &lci, nullptr, &set_layout));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(g.dev, &plci, nullptr, &layout));
    VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(g.dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set{};
    CHECK(vkAllocateDescriptorSets(g.dev, &dsai, &set));
    VkDescriptorBufferInfo ubo_info{ubo.buffer, 0, kUboSize};
    VkDescriptorBufferInfo ssbo_info{ssbo.buffer, 0, kSsboSize};
    VkDescriptorImageInfo tex_info{sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out_info{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                     bindings[i].descriptorType};
    }
    writes[0].pBufferInfo = &ubo_info;
    writes[1].pBufferInfo = &ssbo_info;
    writes[2].pImageInfo = &tex_info;
    writes[3].pImageInfo = &out_info;
    vkUpdateDescriptorSets(g.dev, 4, writes, 0, nullptr);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cpool{};
    CHECK(vkCreateCommandPool(g.dev, &cpci, nullptr, &cpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(vkAllocateCommandBuffers(g.dev, &cbai, &cmd));
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    VkQueryPool queries{};
    CHECK(vkCreateQueryPool(g.dev, &qpci, nullptr, &queries));

    const auto submit_wait = [&] {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(g.queue));
    };
    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        barrier(tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kTexW, kTexH, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        barrier(tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        CHECK(vkEndCommandBuffer(cmd));
        submit_wait();
    }

    std::vector<std::vector<uint16_t>> results;
    std::vector<double> times;
    for (int which = 1; which <= 2; ++which) {
        const auto code = ReadSpv(argv[which]);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule module{};
        CHECK(vkCreateShaderModule(g.dev, &smci, nullptr, &module));
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req,
                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        ci.layout = layout;
        VkPipeline pipeline{};
        CHECK(vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
        std::vector<double> runs;
        for (int run = 0; run < 7; ++run) {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(cmd, &bi));
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            barrier(out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                    nullptr);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            vkCmdDispatch(cmd, 64, 36, 1);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            barrier(out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kOutW, kOutH, 1};
            vkCmdCopyImageToBuffer(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1,
                                   &copy);
            CHECK(vkEndCommandBuffer(cmd));
            submit_wait();
            uint64_t stamps[2]{};
            CHECK(vkGetQueryPoolResults(g.dev, queries, 0, 2, sizeof(stamps), stamps, 8,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            runs.push_back(double(stamps[1] - stamps[0]) * g.period / 1e6);
        }
        std::sort(runs.begin(), runs.end());
        times.push_back(runs[3]);
        const auto* px = static_cast<const uint16_t*>(readback.map);
        results.emplace_back(px, px + size_t{kOutW} * kOutH * 4);
        std::printf("%s: GPU %.3f ms per dispatch (median of 7)\n", argv[which], runs[3]);
    }
    size_t written = 0, differ = 0, nan_a = 0, nan_b = 0;
    double max_diff = 0, sum_diff = 0, sum_mag = 0;
    for (size_t i = 0; i < results[0].size(); ++i) {
        const float a = FromHalf(results[0][i]);
        const float b = FromHalf(results[1][i]);
        nan_a += std::isnan(a) ? 1 : 0;
        nan_b += std::isnan(b) ? 1 : 0;
        if (results[0][i] == 0 && results[1][i] == 0) {
            continue;
        }
        ++written;
        const double d = std::fabs(double(a) - double(b));
        if (!std::isfinite(d)) {
            ++differ;
            continue;
        }
        max_diff = std::max(max_diff, d);
        sum_diff += d;
        sum_mag += std::fabs(a);
        differ += d > 1e-2 * std::max(1.0, double(std::fabs(a))) ? 1 : 0;
    }
    std::printf("output: %zu channels written; differing (beyond 1%%) %zu; max |a-b| %.5f; mean "
                "|a-b| %.6f; mean |a| %.5f; NaN a %zu b %zu\n",
                written, differ, max_diff, written ? sum_diff / written : 0.0,
                written ? sum_mag / written : 0.0, nan_a, nan_b);
    std::printf("speed-up %.2fx\n", times[0] / times[1]);
    return 0;
}
