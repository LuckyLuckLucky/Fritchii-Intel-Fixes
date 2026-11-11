// Standalone Vulkan memory model coherence pass (vulkan_memory_model_pass.cpp, next to this file;
// not in the emulator: on the B580 it changed nothing in Intel's compiled code).
// Usage: spvcoherent in.spv out.spv
#include <cstdio>
#include <fstream>
#include <vector>

#include "vulkan_memory_model_pass.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    std::vector<unsigned> w(static_cast<size_t>(in.tellg()) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(w.data()), w.size() * 4);
    const auto stats = Shader::Backend::SPIRV::MakeVulkanMemoryModelCoherent(w);
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(w.data()), w.size() * 4);
    std::printf("loads %u, stores %u, image accesses %u, barrier/atomic semantics %u, new constants %u\n",
                stats.loads, stats.stores, stats.images, stats.semantics, stats.constants);
    return 0;
}
