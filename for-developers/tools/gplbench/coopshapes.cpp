// Lists the VK_KHR_cooperative_matrix shapes of every GPU.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <vector>

static const char* Type(VkComponentTypeKHR t) {
    switch (t) {
    case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
    case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
    case VK_COMPONENT_TYPE_SINT8_KHR: return "s8";
    case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
    case VK_COMPONENT_TYPE_SINT32_KHR: return "s32";
    case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
    default: return "other";
    }
}

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        return 1;
    }
    auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> phys(count);
    vkEnumeratePhysicalDevices(instance, &count, phys.data());
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        uint32_t n = 0;
        if (!get || get(p, &n, nullptr) != VK_SUCCESS) {
            std::printf("%s: no cooperative matrices\n", props.deviceName);
            continue;
        }
        std::vector<VkCooperativeMatrixPropertiesKHR> shapes(
            n, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
        get(p, &n, shapes.data());
        std::printf("%s: %u shapes\n", props.deviceName, n);
        for (const auto& s : shapes) {
            std::printf("  M%u N%u K%u  A %s B %s C %s Result %s  saturating %u scope %d\n",
                        s.MSize, s.NSize, s.KSize, Type(s.AType), Type(s.BType), Type(s.CType),
                        Type(s.ResultType), s.saturatingAccumulation, s.scope);
        }
    }
    return 0;
}
