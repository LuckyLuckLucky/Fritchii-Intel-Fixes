// Compute shader statistics on the host GPU: builds one SPIR-V compute shader with the driver's
// own subgroup size, with requiredSubgroupSize 32 and 16, and prints the driver's pipeline
// executable statistics (SIMD width, registers, spills) for each.
// Usage: cstats.exe shader.spv layout.txt
//   layout.txt: "push <bytes>" and "<set> <binding> <VkDescriptorType> <count>" lines.
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: cstats shader.spv layout.txt\n");
        return 1;
    }
    std::ifstream spv_file(argv[1], std::ios::binary | std::ios::ate);
    std::vector<uint32_t> code(static_cast<size_t>(spv_file.tellg()) / 4);
    spv_file.seekg(0);
    spv_file.read(reinterpret_cast<char*>(code.data()), code.size() * 4);

    uint32_t push_bytes = 0;
    std::map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> sets;
    std::ifstream layout_file(argv[2]);
    for (std::string line; std::getline(layout_file, line);) {
        std::istringstream in(line);
        std::string first;
        in >> first;
        if (first == "push") {
            in >> push_bytes;
        } else if (!first.empty()) {
            uint32_t binding{}, type{}, count{};
            in >> binding >> type >> count;
            sets[std::stoul(first)].push_back({binding, static_cast<VkDescriptorType>(type),
                                               count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> phys(count);
    vkEnumeratePhysicalDevices(instance, &count, phys.data());
    VkPhysicalDevice gpu{};
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID == 0x8086) {
            gpu = p;
            std::printf("%s\n", props.deviceName);
        }
    }
    if (!gpu) {
        return 1;
    }
    // Every feature the device has, so the emulator's capabilities are accepted.
    const bool coop = argc > 3 && std::string{argv[3]} == "coop";
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    if (coop) {
        exec.pNext = &coop_features;
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &exec;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, available.data());
    std::vector<const char*> exts;
    for (const char* wanted : {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
                               VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
                               VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME,
                               VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
                               VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
                               coop ? VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME : "none"}) {
        for (const auto& e : available) {
            if (std::string{e.extensionName} == wanted) {
                exts.push_back(wanted);
            }
        }
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    VkDevice dev{};
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
    auto get_stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR"));
    auto get_props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR"));

    std::vector<VkDescriptorSetLayout> layouts;
    const uint32_t max_set = sets.empty() ? 0 : sets.rbegin()->first + 1;
    for (uint32_t set = 0; set < max_set; ++set) {
        auto& bindings = sets[set];
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = static_cast<uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        VkDescriptorSetLayout layout{};
        CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layout));
        layouts.push_back(layout);
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = static_cast<uint32_t>(layouts.size());
    plci.pSetLayouts = layouts.data();
    plci.pushConstantRangeCount = push_bytes ? 1 : 0;
    plci.pPushConstantRanges = &range;
    VkPipelineLayout pipeline_layout{};
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout));
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    VkShaderModule module{};
    CHECK(vkCreateShaderModule(dev, &smci, nullptr, &module));

    for (const uint32_t size : {0u, 32u, 16u}) {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = size;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        if (coop && size != 32) {
            continue; // the emulator builds matrix shaders with 32 full lanes only
        }
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, size ? &req : nullptr, coop ? VkPipelineShaderStageCreateFlags{VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT} : 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        ci.layout = pipeline_layout;
        const auto begin = std::chrono::steady_clock::now();
        VkPipeline pipeline{};
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - begin).count();
        std::printf("\n-- requiredSubgroupSize %s (built in %.0f ms)\n",
                    size ? std::to_string(size).c_str() : "driver's choice", ms);
        VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        info.pipeline = pipeline;
        uint32_t executables = 0;
        get_props(dev, &info, &executables, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> props(
            executables, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        get_props(dev, &info, &executables, props.data());
        for (uint32_t e = 0; e < executables; ++e) {
            std::printf("  executable %u: %s, subgroup size %u\n", e, props[e].name,
                        props[e].subgroupSize);
            VkPipelineExecutableInfoKHR einfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            einfo.pipeline = pipeline;
            einfo.executableIndex = e;
            uint32_t n = 0;
            get_stats(dev, &einfo, &n, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> stats(
                n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            get_stats(dev, &einfo, &n, stats.data());
            for (const auto& s : stats) {
                switch (s.format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                    std::printf("    %-32s %s\n", s.name, s.value.b32 ? "yes" : "no");
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                    std::printf("    %-32s %lld\n", s.name, static_cast<long long>(s.value.i64));
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                    std::printf("    %-32s %llu\n", s.name,
                                static_cast<unsigned long long>(s.value.u64));
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                    std::printf("    %-32s %.2f\n", s.name, s.value.f64);
                    break;
                default:
                    break;
                }
            }
        }
        vkDestroyPipeline(dev, pipeline, nullptr);
    }
    vkDeviceWaitIdle(dev);
    return 0;
}
