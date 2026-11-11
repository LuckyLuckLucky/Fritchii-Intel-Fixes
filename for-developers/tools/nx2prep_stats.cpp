// Read-only statistics of a precompiled.fork-v<N>.nx2prep file (layout: vk_pipeline_cache.cpp,
// PreparePrecompiled). Built against the emulator's own key headers so the key size matches.
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

using Vulkan::ComputePipelineCacheKey;
using Vulkan::GraphicsPipelineCacheKey;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: nx2prep_stats <file.nx2prep>\n");
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    char magic[8]{};
    u32 version{}, env_count{}, record_count{}, reserved{};
    in.read(magic, 8)
        .read(reinterpret_cast<char*>(&version), 4)
        .read(reinterpret_cast<char*>(&env_count), 4)
        .read(reinterpret_cast<char*>(&record_count), 4)
        .read(reinterpret_cast<char*>(&reserved), 4);
    std::printf("magic %.8s version %u environments %u records %u\n", magic, version, env_count,
                record_count);
    std::printf("sizeof graphics key %zu, compute key %zu\n", sizeof(GraphicsPipelineCacheKey),
                sizeof(ComputePipelineCacheKey));
    const std::streamoff table = in.tellg();
    // argv[3] (optional): dump the environment with this index to argv[4].
    if (argc > 4 && std::string{argv[2]} != "dumpcompute") {
        const u32 index = static_cast<u32>(std::strtoul(argv[3], nullptr, 10));
        u64 offset{}, size{};
        in.seekg(table + std::streamoff{index} * 16);
        in.read(reinterpret_cast<char*>(&offset), 8).read(reinterpret_cast<char*>(&size), 8);
        std::vector<char> blob(size);
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(blob.data(), static_cast<std::streamsize>(size));
        std::ofstream(argv[4], std::ios::binary).write(blob.data(), static_cast<std::streamsize>(size));
        std::printf("environment %u: %llu bytes at %llu written to %s\n", index,
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(offset),
                    argv[4]);
        return 0;
    }
    in.seekg(table + std::streamoff{env_count} * 16);

    size_t graphics = 0, compute = 0, vids = 0, eds3_blend = 0, eds3_enables = 0, eds2_logic = 0,
           tess = 0, mesh_like = 0;
    std::set<std::array<u64, 6>> program_sets;
    std::unordered_set<u64> stage_hashes[6];
    std::unordered_set<u64> pre_raster;  // stages 0-4 combined
    std::set<std::pair<u64, u64>> vs_fs;
    std::map<u32, size_t> raw1_flags;
    std::vector<u32> ids(256);
    for (u32 r = 0; r < record_count && in; ++r) {
        u8 kind{}, count{};
        u16 pad{};
        in.read(reinterpret_cast<char*>(&kind), 1)
            .read(reinterpret_cast<char*>(&count), 1)
            .read(reinterpret_cast<char*>(&pad), 2)
            .read(reinterpret_cast<char*>(ids.data()), count * sizeof(u32));
        if (kind == 1) {
            ComputePipelineCacheKey key{};
            in.read(reinterpret_cast<char*>(&key), sizeof(key));
            ++compute;
            // argv[2] == "dumpcompute", argv[3] = directory: every compute program's environment
            // once (env_<hash>.bin) and compute.txt with "<hash> <x> <y> <z> <shared> <env>".
            if (argc > 3 && std::string{argv[2]} == "dumpcompute") {
                static std::set<u64> seen;
                if (seen.insert(key.unique_hash).second) {
                    const auto here = in.tellg();
                    u64 offset{}, size{};
                    in.seekg(table + std::streamoff{ids[0]} * 16);
                    in.read(reinterpret_cast<char*>(&offset), 8).read(reinterpret_cast<char*>(&size), 8);
                    std::vector<char> blob(size);
                    in.seekg(static_cast<std::streamoff>(offset));
                    in.read(blob.data(), static_cast<std::streamsize>(size));
                    char name[64];
                    std::snprintf(name, sizeof(name), "/env_%016llx.bin",
                                  static_cast<unsigned long long>(key.unique_hash));
                    std::ofstream(std::string{argv[3]} + name, std::ios::binary)
                        .write(blob.data(), static_cast<std::streamsize>(size));
                    std::ofstream(std::string{argv[3]} + "/compute.txt", std::ios::app)
                        << std::hex << key.unique_hash << std::dec << ' ' << key.workgroup_size[0]
                        << ' ' << key.workgroup_size[1] << ' ' << key.workgroup_size[2] << ' '
                        << key.shared_memory_size << ' ' << ids[0] << '\n';
                    in.seekg(here);
                }
            }
            if (argc > 2 && key.unique_hash == std::strtoull(argv[2], nullptr, 16)) {
                std::printf("compute %016llx: workgroup %ux%ux%u (%u threads), shared memory %u "
                            "bytes, environment %u\n",
                            static_cast<unsigned long long>(key.unique_hash),
                            key.workgroup_size[0], key.workgroup_size[1], key.workgroup_size[2],
                            key.workgroup_size[0] * key.workgroup_size[1] * key.workgroup_size[2],
                            key.shared_memory_size, ids[0]);
            }
            continue;
        }
        GraphicsPipelineCacheKey key{};
        in.read(reinterpret_cast<char*>(&key), sizeof(key));
        ++graphics;
        const auto& s = key.state;
        vids += s.dynamic_vertex_input != 0;
        eds3_blend += s.extended_dynamic_state_3_blend != 0;
        eds3_enables += s.extended_dynamic_state_3_enables != 0;
        eds2_logic += s.extended_dynamic_state_2_logic_op != 0;
        tess += key.unique_hashes[2] != 0 || key.unique_hashes[3] != 0;
        ++raw1_flags[s.raw1 & 0x3F];
        program_sets.insert(key.unique_hashes);
        for (size_t i = 0; i < 6; ++i) {
            if (key.unique_hashes[i]) {
                stage_hashes[i].insert(key.unique_hashes[i]);
            }
        }
        u64 pre = 0;
        for (size_t i = 0; i < 5; ++i) {
            pre = pre * 0x9E3779B97F4A7C15ULL + key.unique_hashes[i];
        }
        pre_raster.insert(pre);
        vs_fs.insert({key.unique_hashes[1], key.unique_hashes[5]});
        (void)mesh_like;
    }
    std::printf("read: graphics %zu, compute %zu (stream ok: %d)\n", graphics, compute,
                static_cast<int>(static_cast<bool>(in)));
    std::printf("graphics flags: dynamic_vertex_input %zu, eds3_blend %zu, eds3_enables %zu, "
                "eds2_logic_op %zu, tessellation %zu\n",
                vids, eds3_blend, eds3_enables, eds2_logic, tess);
    std::printf("raw1 low 6 bits (eds, eds2, eds2_logic, eds3_blend, eds3_en, vids):\n");
    for (const auto& [bits, n] : raw1_flags) {
        std::printf("  0x%02x: %zu\n", bits, n);
    }
    std::printf("unique program sets %zu, unique pre-raster sets %zu, unique (VS,FS) %zu\n",
                program_sets.size(), pre_raster.size(), vs_fs.size());
    for (size_t i = 0; i < 6; ++i) {
        std::printf("  stage slot %zu: %zu unique programs\n", i, stage_hashes[i].size());
    }
    return 0;
}
