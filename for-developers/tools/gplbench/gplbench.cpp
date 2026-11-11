// Standalone Vulkan benchmark (no emulator, no game): how Intel's driver compiles and runs
// monolithic pipelines vs graphics pipeline libraries (fast-linked and optimized).
// Build: build_gplbench.cmd. Run: gplbench.exe [device index] (shaders: vs.spv, fs*.spv).
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("FAILED %s = %d (line %d)\n", #x, r_, __LINE__);                           \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static std::vector<uint32_t> ReadSpv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::printf("missing %s\n", path.c_str());
        std::exit(1);
    }
    const size_t n = static_cast<size_t>(f.tellg());
    std::vector<uint32_t> words(n / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(words.data()), n);
    return words;
}

struct Ctx {
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice dev{};
    VkQueue queue{};
    uint32_t family{};
    VkPipelineLayout layout{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkQueryPool queries{};
    float timestamp_period{};
};

static VkShaderModule Module(Ctx& c, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m{};
    CHECK(vkCreateShaderModule(c.dev, &ci, nullptr, &m));
    return m;
}

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kWidth = 2560, kHeight = 1440;

struct States {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkViewport viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    States(int variant) {
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        att.colorWriteMask = 0xF;
        // Variants differ only in fixed-function state, as the precompiled records do.
        att.blendEnable = (variant & 1) ? VK_TRUE : VK_FALSE;
        att.srcColorBlendFactor = (variant & 2) ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        att.dstColorBlendFactor = (variant & 4) ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                                : VK_BLEND_FACTOR_ZERO;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &kFormat;
    }
};

static VkPipeline Monolithic(Ctx& c, VkShaderModule vs, VkShaderModule fs, int variant) {
    States s(variant);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &s.rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &s.vi;
    ci.pInputAssemblyState = &s.ia;
    ci.pViewportState = &s.vp;
    ci.pRasterizationState = &s.rs;
    ci.pMultisampleState = &s.ms;
    ci.pDepthStencilState = &s.ds;
    ci.pColorBlendState = &s.cb;
    ci.layout = c.layout;
    VkPipeline p{};
    CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

static VkPipeline Library(Ctx& c, VkGraphicsPipelineLibraryFlagsEXT part, VkShaderModule module,
                          int variant, bool retain) {
    States s(variant);
    VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    lib.flags = part;
    lib.pNext = &s.rendering;
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.pName = "main";
    stage.module = module;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &lib;
    ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
               (retain ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
    ci.layout = c.layout;
    switch (part) {
    case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
        ci.pVertexInputState = &s.vi;
        ci.pInputAssemblyState = &s.ia;
        break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pViewportState = &s.vp;
        ci.pRasterizationState = &s.rs;
        break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pMultisampleState = &s.ms;
        ci.pDepthStencilState = &s.ds;
        break;
    default:
        ci.pMultisampleState = &s.ms;
        ci.pColorBlendState = &s.cb;
        break;
    }
    VkPipeline p{};
    CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

static VkPipeline Link(Ctx& c, const VkPipeline (&parts)[4], bool optimized) {
    VkPipelineLibraryCreateInfoKHR libs{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    libs.libraryCount = 4;
    libs.pLibraries = parts;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &libs;
    ci.flags = optimized ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    ci.layout = c.layout;
    VkPipeline p{};
    CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

// GPU time of `draws` full-screen triangles with `pipeline`, in ms (median of 5 runs).
static double GpuTime(Ctx& c, VkPipeline pipeline, int draws) {
    std::vector<double> runs;
    for (int run = 0; run < 5; ++run) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = c.pool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        CHECK(vkAllocateCommandBuffers(c.dev, &ai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        vkCmdResetQueryPool(cmd, c.queries, 0, 2);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.image = c.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = c.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kWidth, kHeight}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c.queries, 0);
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const float seed = 0.25f;
        vkCmdPushConstants(cmd, c.layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 4, &seed);
        for (int i = 0; i < draws; ++i) {
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c.queries, 1);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(c.queue));
        uint64_t stamps[2]{};
        CHECK(vkGetQueryPoolResults(c.dev, c.queries, 0, 2, sizeof(stamps), stamps, 8,
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        vkFreeCommandBuffers(c.dev, c.pool, 1, &cmd);
        runs.push_back(double(stamps[1] - stamps[0]) * c.timestamp_period / 1e6);
    }
    std::sort(runs.begin(), runs.end());
    return runs[2];
}

// Submit + fence round trip of an empty command buffer, in microseconds (median of 200).
static double SubmitRoundTrip(Ctx& c) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = c.pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(vkAllocateCommandBuffers(c.dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    CHECK(vkEndCommandBuffer(cmd));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    VkSemaphore timeline{};
    CHECK(vkCreateSemaphore(c.dev, &sci, nullptr, &timeline));
    std::vector<double> times;
    double submit_only = 0;
    for (uint64_t value = 1; value <= 200; ++value) {
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.signalSemaphoreValueCount = 1;
        ts.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ts;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline;
        const auto a = Clock::now();
        CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
        const auto b = Clock::now();
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &value;
        CHECK(vkWaitSemaphores(c.dev, &wi, UINT64_MAX));
        const auto d = Clock::now();
        submit_only += Ms(a, b) * 1000.0;
        times.push_back(Ms(a, d) * 1000.0);
    }
    std::sort(times.begin(), times.end());
    std::printf("submit + timeline wait round trip: median %.0f us, p90 %.0f us; vkQueueSubmit "
                "alone %.0f us on average\n",
                times[100], times[180], submit_only / 200);
    vkDestroySemaphore(c.dev, timeline, nullptr);
    vkFreeCommandBuffers(c.dev, c.pool, 1, &cmd);
    return times[100];
}

int main(int argc, char** argv) {
    const int device_index = argc > 1 ? std::atoi(argv[1]) : -1;
    Ctx c;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "gplbench";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &c.instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> phys(count);
    vkEnumeratePhysicalDevices(c.instance, &count, phys.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(phys[i], &p);
        std::printf("device %u: %s (vendor %04x)\n", i, p.deviceName, p.vendorID);
        if ((device_index < 0 && p.vendorID == 0x8086) || int(i) == device_index) {
            c.phys = phys[i];
            c.timestamp_period = p.limits.timestampPeriod;
        }
    }
    if (!c.phys) {
        std::printf("no device\n");
        return 1;
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    gpl.graphicsPipelineLibrary = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.pNext = &gpl;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.pNext = &f13;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* exts[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                          VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(c.phys, &dci, nullptr, &c.dev));
    vkGetDeviceQueue(c.dev, 0, 0, &c.queue);

    VkPushConstantRange range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 4};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(c.dev, &lci, nullptr, &c.layout));

    VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imci.imageType = VK_IMAGE_TYPE_2D;
    imci.format = kFormat;
    imci.extent = {kWidth, kHeight, 1};
    imci.mipLevels = 1;
    imci.arrayLayers = 1;
    imci.samples = VK_SAMPLE_COUNT_1_BIT;
    imci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(vkCreateImage(c.dev, &imci, nullptr, &c.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.dev, c.image, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &c.memory));
    CHECK(vkBindImageMemory(c.dev, c.image, c.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = c.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(c.dev, &vci, nullptr, &c.view));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHECK(vkCreateCommandPool(c.dev, &pci, nullptr, &c.pool));
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    CHECK(vkCreateQueryPool(c.dev, &qpci, nullptr, &c.queries));

    const VkShaderModule vs = Module(c, ReadSpv("vs.spv"));
    std::vector<VkShaderModule> fs;
    for (int i = 0; i < 4; ++i) {
        fs.push_back(Module(c, ReadSpv("fs" + std::to_string(i) + ".spv")));
    }

    std::printf("\n== 1. Monolithic pipelines (same shaders, other fixed-function state)\n");
    std::vector<VkPipeline> mono;
    // argv[2]: first state variant, so a second run asks the disk cache for states it never saw.
    const int first_variant = argc > 2 ? std::atoi(argv[2]) : 0;
    for (int variant = first_variant; variant < first_variant + 6; ++variant) {
        const auto a = Clock::now();
        mono.push_back(Monolithic(c, vs, fs[0], variant));
        std::printf("  VS + FS0, state %d: %.1f ms\n", variant, Ms(a, Clock::now()));
    }
    {
        const auto a = Clock::now();
        mono.push_back(Monolithic(c, vs, fs[1], 0));
        std::printf("  VS + FS1 (new fragment shader), state 0: %.1f ms\n", Ms(a, Clock::now()));
    }

    std::printf("\n== 2. Graphics pipeline libraries\n");
    auto a = Clock::now();
    const VkPipeline vi = Library(c, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
                                  VK_NULL_HANDLE, 0, false);
    const VkPipeline fo = Library(c, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
                                  VK_NULL_HANDLE, 0, false);
    std::printf("  vertex input + fragment output parts: %.1f ms\n", Ms(a, Clock::now()));
    a = Clock::now();
    const VkPipeline pre = Library(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
                                   vs, 0, true);
    std::printf("  pre-rasterization library (VS): %.1f ms\n", Ms(a, Clock::now()));
    std::vector<VkPipeline> frag;
    for (int i = 2; i < 4; ++i) {
        a = Clock::now();
        frag.push_back(Library(c, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, fs[i], 0,
                               true));
        std::printf("  fragment library (FS%d): %.1f ms\n", i, Ms(a, Clock::now()));
    }
    const VkPipeline parts2[4]{vi, pre, frag[0], fo};
    const VkPipeline parts3[4]{vi, pre, frag[1], fo};
    a = Clock::now();
    const VkPipeline fast = Link(c, parts2, false);
    std::printf("  fast link (VS + FS2): %.2f ms\n", Ms(a, Clock::now()));
    a = Clock::now();
    const VkPipeline fast2 = Link(c, parts3, false);
    std::printf("  fast link (VS + FS3): %.2f ms\n", Ms(a, Clock::now()));
    a = Clock::now();
    const VkPipeline optimized = Link(c, parts2, true);
    std::printf("  optimized link (VS + FS2): %.1f ms\n", Ms(a, Clock::now()));
    a = Clock::now();
    const VkPipeline mono2 = Monolithic(c, vs, fs[2], 0);
    std::printf("  monolithic VS + FS2 for comparison: %.1f ms\n", Ms(a, Clock::now()));
    (void)fast2;

    std::printf("\n== 3. GPU time, 40 full-screen triangles at %ux%u\n", kWidth, kHeight);
    for (int warm = 0; warm < 2; ++warm) {
        GpuTime(c, mono2, 40);
        GpuTime(c, fast, 40);
        GpuTime(c, optimized, 40);
    }
    const double t_mono = GpuTime(c, mono2, 40);
    const double t_fast = GpuTime(c, fast, 40);
    const double t_opt = GpuTime(c, optimized, 40);
    std::printf("  monolithic %.2f ms, fast-linked %.2f ms (%+.1f%%), optimized link %.2f ms "
                "(%+.1f%%)\n",
                t_mono, t_fast, (t_fast / t_mono - 1) * 100, t_opt, (t_opt / t_mono - 1) * 100);

    std::printf("\n== 4. Queue round trip\n");
    SubmitRoundTrip(c);

    // 5. Parallel compiles: par0..par23.spv are distinct fragment shaders; 12 on one thread,
    // then 12 others on 12 threads. Equal wall times would mean the driver serializes compiles.
    std::vector<VkShaderModule> par;
    for (int i = 0; i < 24; ++i) {
        par.push_back(Module(c, ReadSpv("par" + std::to_string(i) + ".spv")));
    }
    std::printf("\n== 5. Parallel compiles (12 distinct fragment shaders each)\n");
    a = Clock::now();
    for (int i = 0; i < 12; ++i) {
        Monolithic(c, vs, par[i], 0);
    }
    const double serial = Ms(a, Clock::now());
    a = Clock::now();
    {
        std::vector<std::thread> threads;
        for (int i = 12; i < 24; ++i) {
            threads.emplace_back([&c, vs, module = par[i]] { Monolithic(c, vs, module, 0); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    const double parallel = Ms(a, Clock::now());
    std::printf("  1 thread: %.0f ms (%.0f ms each); 12 threads: %.0f ms; speed-up %.1fx\n",
                serial, serial / 12, parallel, serial / parallel);

    // 6. GPU cost of splitting a frame: N small draws as (a) one submit, one render pass,
    // (b) one submit, N render passes, (c) N submits of one render pass each. GPU span from the
    // first timestamp to the last, median of 5.
    std::printf("\n== 6. GPU cost of render pass and submission splits (light draws, %ux%u)\n",
                kWidth, kHeight);
    const VkPipeline light = Monolithic(c, vs, Module(c, ReadSpv("flat.spv")), 0);
    VkQueryPoolCreateInfo spci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    spci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    spci.queryCount = 2;
    VkQueryPool span_pool{};
    CHECK(vkCreateQueryPool(c.dev, &spci, nullptr, &span_pool));
    const auto run_split = [&](int draws, int passes, int submits) {
        std::vector<double> runs;
        for (int run = 0; run < 5; ++run) {
            std::vector<VkCommandBuffer> cmds(submits);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = c.pool;
            ai.commandBufferCount = static_cast<uint32_t>(submits);
            CHECK(vkAllocateCommandBuffers(c.dev, &ai, cmds.data()));
            const int passes_per_cmd = passes / submits;
            const int draws_per_pass = draws / passes;
            for (int s = 0; s < submits; ++s) {
                VkCommandBuffer cmd = cmds[s];
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                CHECK(vkBeginCommandBuffer(cmd, &bi));
                if (s == 0) {
                    vkCmdResetQueryPool(cmd, span_pool, 0, 2);
                    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    barrier.image = c.image;
                    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                                         nullptr, 0, nullptr, 1, &barrier);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, span_pool, 0);
                }
                for (int p = 0; p < passes_per_cmd; ++p) {
                    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    color.imageView = c.view;
                    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    color.loadOp = (s == 0 && p == 0) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                      : VK_ATTACHMENT_LOAD_OP_LOAD;
                    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    ri.renderArea = {{0, 0}, {kWidth, kHeight}};
                    ri.layerCount = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &color;
                    vkCmdBeginRendering(cmd, &ri);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, light);
                    const float seed = 0.5f;
                    vkCmdPushConstants(cmd, c.layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 4, &seed);
                    for (int d = 0; d < draws_per_pass; ++d) {
                        // A small draw: one triangle covering a sliver of the target.
                        VkViewport vp{0, 0, 256, 144, 0, 1};
                        vkCmdDraw(cmd, 3, 1, 0, 0);
                        (void)vp;
                    }
                    vkCmdEndRendering(cmd);
                }
                if (s == submits - 1) {
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, span_pool, 1);
                }
                CHECK(vkEndCommandBuffer(cmd));
            }
            for (int s = 0; s < submits; ++s) {
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmds[s];
                CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
            }
            CHECK(vkQueueWaitIdle(c.queue));
            uint64_t stamps[2]{};
            CHECK(vkGetQueryPoolResults(c.dev, span_pool, 0, 2, sizeof(stamps), stamps, 8,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            vkFreeCommandBuffers(c.dev, c.pool, static_cast<uint32_t>(submits), cmds.data());
            runs.push_back(double(stamps[1] - stamps[0]) * c.timestamp_period / 1e6);
        }
        std::sort(runs.begin(), runs.end());
        return runs[2];
    };
    for (const int n : {35, 140}) {
        const double one = run_split(n, 1, 1);
        const double passes = run_split(n, n, 1);
        const double subs = run_split(n, n, n);
        std::printf("  %3d draws: 1 submit/1 pass %.2f ms; 1 submit/%d passes %.2f ms (+%.1f us "
                    "per pass); %d submits %.2f ms (+%.1f us per submit beyond the pass)\n",
                    n, one, n, passes, (passes - one) * 1000 / n, n, subs,
                    (subs - passes) * 1000 / n);
    }
    vkDeviceWaitIdle(c.dev);
    return 0;
}
