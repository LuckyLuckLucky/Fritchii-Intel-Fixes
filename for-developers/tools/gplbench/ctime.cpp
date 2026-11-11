// Batch compile timer for compute SPIR-V on the Intel GPU: for each "<spv> <layout>" line of a
// list file, builds the shader as the emulator would (requiredSubgroupSize 32 when it uses
// subgroup operations, full subgroups with cooperative matrices) and prints one CSV line:
// name, build ms, SIMD width, instructions, scratch bytes, spill/fill counts when reported.
// Usage: ctime.exe list.txt > out.csv   (layout lines as in difftest.cpp)
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 2;
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
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    exec.pNext = &coop;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &exec;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    const char* exts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                          VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = exts;
    VkDevice dev{};
    if (vkCreateDevice(gpu, &dci, nullptr, &dev) != VK_SUCCESS) return 3;
    auto get_stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR"));
    auto get_props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR"));

    std::printf("name,build_ms,simd,instructions,scratch,spills,fills,error\n");
    std::ifstream list(argv[1]);
    for (std::string line; std::getline(list, line);) {
        std::istringstream ls(line);
        std::string spv_path, layout_path;
        if (!(ls >> spv_path >> layout_path)) continue;
        std::ifstream f(spv_path, std::ios::binary | std::ios::ate);
        if (!f) continue;
        std::vector<uint32_t> code(static_cast<size_t>(f.tellg()) / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
        // Capabilities: subgroup operations (61-66 GroupNonUniform*), cooperative matrices.
        bool subgroup = false, coop_matrix = false;
        for (size_t i = 5; i < code.size();) {
            const uint32_t count = code[i] >> 16, op = code[i] & 0xffff;
            if (count == 0) break;
            if (op == 17 && count == 2) {
                const uint32_t cap = code[i + 1];
                subgroup |= cap >= 62 && cap <= 66;
                coop_matrix |= cap == 6022;
            } else if (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16 &&
                       op != 3 && op != 5 && op != 6 && op != 7) {
                break;
            }
            i += count;
        }
        std::map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> sets;
        std::ifstream lf(layout_path);
        for (std::string l; std::getline(lf, l);) {
            std::istringstream in(l);
            uint32_t set{}, binding{}, type{}, cnt{};
            if (in >> set >> binding >> type >> cnt) {
                sets[set].push_back({binding, static_cast<VkDescriptorType>(type), cnt,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            }
        }
        std::vector<VkDescriptorSetLayout> layouts;
        const uint32_t max_set = sets.empty() ? 0 : sets.rbegin()->first + 1;
        for (uint32_t s = 0; s < max_set; ++s) {
            auto& b = sets[s];
            VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            lci.bindingCount = static_cast<uint32_t>(b.size());
            lci.pBindings = b.data();
            VkDescriptorSetLayout l{};
            vkCreateDescriptorSetLayout(dev, &lci, nullptr, &l);
            layouts.push_back(l);
        }
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = max_set;
        plci.pSetLayouts = layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        VkPipelineLayout layout{};
        vkCreatePipelineLayout(dev, &plci, nullptr, &layout);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule module{};
        vkCreateShaderModule(dev, &smci, nullptr, &module);
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = 32;
        // CTIME_SUBGROUP=16: shaders without subgroup operations are built with 16 lanes.
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req16{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req16.requiredSubgroupSize = 16;
        const bool force16 = std::getenv("CTIME_SUBGROUP") && std::atoi(std::getenv("CTIME_SUBGROUP")) == 16;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    (subgroup || coop_matrix) ? &req : (force16 ? &req16 : nullptr),
                    coop_matrix ? VkPipelineShaderStageCreateFlags{VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT} : 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        ci.layout = layout;
        const auto begin = std::chrono::steady_clock::now();
        VkPipeline pipeline{};
        const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::string name = spv_path.substr(spv_path.find_last_of("/\\") + 1);
        if (r != VK_SUCCESS) {
            std::printf("%s,%.1f,,,,,,%d\n", name.c_str(), ms, r);
        } else {
            uint32_t simd = 0;
            long long instructions = -1, scratch = -1, spills = -1, fills = -1;
            VkPipelineInfoKHR pinfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
            pinfo.pipeline = pipeline;
            uint32_t ne = 0;
            get_props(dev, &pinfo, &ne, nullptr);
            std::vector<VkPipelineExecutablePropertiesKHR> props(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
            get_props(dev, &pinfo, &ne, props.data());
            if (ne) {
                simd = props[0].subgroupSize;
                VkPipelineExecutableInfoKHR einfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
                einfo.pipeline = pipeline;
                uint32_t ns = 0;
                get_stats(dev, &einfo, &ns, nullptr);
                std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                get_stats(dev, &einfo, &ns, st.data());
                for (const auto& s : st) {
                    const long long v = s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR
                                            ? static_cast<long long>(s.value.u64)
                                            : s.value.i64;
                    if (std::strstr(s.name, "Instruction Count")) instructions = v;
                    else if (std::strstr(s.name, "Scratch")) scratch = v;
                    else if (std::strstr(s.name, "Spill")) spills = v;
                    else if (std::strstr(s.name, "Fill")) fills = v;
                }
            }
            std::printf("%s,%.1f,%u,%lld,%lld,%lld,%lld,0\n", name.c_str(), ms, simd, instructions,
                        scratch, spills, fills);
            vkDestroyPipeline(dev, pipeline, nullptr);
        }
        std::fflush(stdout);
        vkDestroyShaderModule(dev, module, nullptr);
        vkDestroyPipelineLayout(dev, layout, nullptr);
        for (auto l : layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    return 0;
}
