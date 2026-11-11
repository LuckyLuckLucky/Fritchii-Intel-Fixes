// Rewrites a SPIR-V module from the Vulkan memory model to GLSL450: OpMemoryModel's model operand
// and the VulkanMemoryModel / VulkanMemoryModelDeviceScope capabilities (replaced by duplicate
// Shader capabilities so the word count stays the same). Usage: spvmodel in.spv out.spv
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    std::vector<unsigned> w(static_cast<size_t>(in.tellg()) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(w.data()), w.size() * 4);
    size_t models = 0, caps = 0;
    for (size_t i = 5; i < w.size();) {
        const unsigned count = w[i] >> 16, opcode = w[i] & 0xffff;
        if (count == 0) {
            break;
        }
        if (argc < 4 && opcode == 17 && count == 2 && (w[i + 1] == 5345 || w[i + 1] == 5346)) {
            w[i + 1] = 1; // OpCapability Shader
            ++caps;
        }
        if (opcode == 14 && count == 3 && w[i + 2] == 3) {
            w[i + 2] = 1; // GLSL450
            ++models;
        }
        i += count;
    }
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(w.data()), w.size() * 4);
    std::printf("memory models changed %zu, capabilities replaced %zu\n", models, caps);
    return 0;
}
