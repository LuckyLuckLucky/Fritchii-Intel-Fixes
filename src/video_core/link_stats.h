// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/lowend_sim.h"
#include "video_core/buffer_churn_stats.h"
#include "video_core/nx2_debug_control.h"

/// FRITCHII_LINK_STATS (live, default 0; docs/fritchii/switches.md): what crosses the link between
/// the CPU and the GPU, by direction and by who asked, and how often a thread waits for the GPU.
/// One line every 10 s. FRITCHII_SIM_LINK turns the counting on too and is charged from here, so
/// what is simulated is exactly what the line shows. Off: one relaxed load per staging request.
namespace VideoCommon::LinkStats {

using Caller = BufferChurnStats::StagingCaller;

/// How the bytes cross. The same upload crosses differently depending on where its staging lives:
/// written into device memory (the resizable BAR window) the CPU pays for it while it writes; in
/// host memory the GPU pays when it reads it.
enum class Path : u32 {
    CpuWritesDevice,
    GpuReadsHost,
    GpuWritesHost,
    Count,
};

inline constexpr size_t NumPaths = static_cast<size_t>(Path::Count);
inline constexpr size_t NumCallers = static_cast<size_t>(Caller::Count);

inline constexpr std::array<const char*, NumCallers> CallerNames{
    "buffer uploads", "buffer downloads", "uniforms", "texture uploads", "texture downloads",
    "queries",        "compute passes",   "rasterizer", "other",
};

struct Counters {
    std::array<std::array<std::atomic<u64>, NumCallers>, NumPaths> bytes{};
    std::array<std::array<std::atomic<u64>, NumCallers>, NumPaths> calls{};
    std::atomic<u64> waits{};
    std::atomic<u64> wait_ns{};
    std::atomic<u64> query_results{};
    std::atomic<u64> list_draws{};
    std::atomic<u64> list_draws_host{};
    std::atomic<u64> stream_full{};
};
inline Counters g_counters;

[[nodiscard]] inline bool On() {
    return VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_link_stats,
                                            "FRITCHII_LINK_STATS") > 0 ||
           Common::LowEndSim::LinkOn();
}

/// `bytes` of a staging request that cross the link by `path`.
inline void Note(Path path, Caller caller, u64 bytes) {
    const size_t p = static_cast<size_t>(path);
    const size_t c = static_cast<size_t>(caller);
    g_counters.bytes[p][c].fetch_add(bytes, std::memory_order_relaxed);
    g_counters.calls[p][c].fetch_add(1, std::memory_order_relaxed);
    if (path == Path::CpuWritesDevice) {
        Common::LowEndSim::LinkChargeCpu(bytes);
    } else {
        Common::LowEndSim::LinkChargeGpu(bytes);
    }
}

/// A stream buffer had no free region: the request went to a staging buffer in host memory, which
/// the GPU then reads across the link (counted under "GPU reads of host memory" as well).
inline void NoteStreamFull() {
    g_counters.stream_full.fetch_add(1, std::memory_order_relaxed);
}

/// A thread really waited for the GPU (the tick had not been reached).
inline void NoteWait(u64 ns) {
    g_counters.waits.fetch_add(1, std::memory_order_relaxed);
    g_counters.wait_ns.fetch_add(ns, std::memory_order_relaxed);
    Common::LowEndSim::LinkRoundTrip();
}

/// Occlusion query results read by the CPU. Where the driver keeps them is its own choice, so
/// they are counted and not charged.
inline void NoteQueryResults(u64 count) {
    g_counters.query_results.fetch_add(count, std::memory_order_relaxed);
}

/// A list draw's arguments: with `in_host_memory` the GPU writes them across the link and its
/// command processor fetches them back, one round trip it waits for.
inline void NoteListDraw(bool in_host_memory) {
    g_counters.list_draws.fetch_add(1, std::memory_order_relaxed);
    if (in_host_memory) {
        g_counters.list_draws_host.fetch_add(1, std::memory_order_relaxed);
        Common::LowEndSim::LinkChargeGpuFetch();
    }
}

/// FRITCHII_LINK_STATS=2: buffer uploads hashed in 256-byte chunks against the last upload of each
/// chunk. A dry count, costly, for the question "how much of what we upload is the same bytes
/// again". An upper bound on what a chunk skip could save: the GPU may have changed its copy.
[[nodiscard]] inline bool ChunkCensusOn() {
    return VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_link_stats,
                                            "FRITCHII_LINK_STATS") >= 2;
}

struct ChunkCensus {
    static constexpr u64 ChunkBytes = 256;
    static constexpr size_t MaxChunks = size_t{1} << 21; // 512 MiB of guest memory
    std::mutex mutex;
    std::unordered_map<u64, u64> hashes;
    std::atomic<u64> bytes{};
    std::atomic<u64> same_bytes{};
};
inline ChunkCensus g_chunks;

/// `data` must be ordinary cached memory: never a staging mapping.
inline void NoteUploadChunks(u64 address, const u8* data, u64 size) {
    ChunkCensus& census = g_chunks;
    census.bytes.fetch_add(size, std::memory_order_relaxed);
    const u64 first = (address + ChunkCensus::ChunkBytes - 1) / ChunkCensus::ChunkBytes;
    const u64 last = (address + size) / ChunkCensus::ChunkBytes;
    u64 same = 0;
    std::scoped_lock lock{census.mutex};
    if (census.hashes.size() > ChunkCensus::MaxChunks) {
        census.hashes.clear();
    }
    for (u64 chunk = first; chunk < last; ++chunk) {
        const u8* const bytes = data + (chunk * ChunkCensus::ChunkBytes - address);
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(bytes),
                                            ChunkCensus::ChunkBytes);
        const auto [it, inserted] = census.hashes.try_emplace(chunk, hash);
        if (!inserted) {
            same += it->second == hash ? ChunkCensus::ChunkBytes : 0;
            it->second = hash;
        }
    }
    census.same_bytes.fetch_add(same, std::memory_order_relaxed);
}

namespace Detail {
struct Window {
    std::chrono::steady_clock::time_point start{};
    u64 sim_cpu_ns{};
    u64 sim_gpu_ns{};
};
inline Window g_window;

/// "12.3 MB in 456 (uniforms 10.0, buffer uploads 2.3)", per second; the counters restart.
struct PathLine {
    std::string text;
    u64 bytes{};
};

[[nodiscard]] inline PathLine TakePath(Path path, double seconds) {
    const size_t p = static_cast<size_t>(path);
    PathLine line;
    u64 calls = 0;
    std::string parts;
    for (size_t c = 0; c < NumCallers; ++c) {
        const u64 bytes = g_counters.bytes[p][c].exchange(0, std::memory_order_relaxed);
        calls += g_counters.calls[p][c].exchange(0, std::memory_order_relaxed);
        line.bytes += bytes;
        if (bytes != 0) {
            parts += fmt::format("{}{} {:.1f}", parts.empty() ? "" : ", ", CallerNames[c],
                                 static_cast<double>(bytes) / 1e6 / seconds);
        }
    }
    line.text = fmt::format("{:.1f} MB in {:.0f} requests{}{}{}",
                            static_cast<double>(line.bytes) / 1e6 / seconds,
                            static_cast<double>(calls) / seconds, parts.empty() ? "" : " (", parts,
                            parts.empty() ? "" : ")");
    return line;
}
} // namespace Detail

/// Once a frame, from the rasterizer. A line every 10 s while on.
inline void MaybeLog() {
    if (!On()) [[likely]] {
        Detail::g_window.start = {};
        return;
    }
    using Clock = std::chrono::steady_clock;
    Detail::Window& window = Detail::g_window;
    const auto now = Clock::now();
    if (window.start == Clock::time_point{}) {
        // A fresh window: what was counted while off or before this frame is not in it.
        for (size_t p = 0; p < NumPaths; ++p) {
            (void)Detail::TakePath(static_cast<Path>(p), 1.0);
        }
        g_counters.waits.store(0, std::memory_order_relaxed);
        g_counters.wait_ns.store(0, std::memory_order_relaxed);
        g_counters.query_results.store(0, std::memory_order_relaxed);
        g_counters.list_draws.store(0, std::memory_order_relaxed);
        g_counters.list_draws_host.store(0, std::memory_order_relaxed);
        g_counters.stream_full.store(0, std::memory_order_relaxed);
        g_chunks.bytes.store(0, std::memory_order_relaxed);
        g_chunks.same_bytes.store(0, std::memory_order_relaxed);
        window.start = now;
        window.sim_cpu_ns = Common::LowEndSim::LinkCpuNsTotal();
        window.sim_gpu_ns = Common::LowEndSim::LinkGpuNsTotal();
        return;
    }
    const double seconds = std::chrono::duration<double>(now - window.start).count();
    if (seconds < 10.0) {
        return;
    }
    window.start = now;
    const Detail::PathLine cpu_writes = Detail::TakePath(Path::CpuWritesDevice, seconds);
    const Detail::PathLine gpu_reads = Detail::TakePath(Path::GpuReadsHost, seconds);
    const Detail::PathLine gpu_writes = Detail::TakePath(Path::GpuWritesHost, seconds);
    const auto per_second = [seconds](std::atomic<u64>& counter) {
        return static_cast<double>(counter.exchange(0, std::memory_order_relaxed)) / seconds;
    };
    const double waits = per_second(g_counters.waits);
    const double wait_ms = per_second(g_counters.wait_ns) / 1e6;
    const double query_results = per_second(g_counters.query_results);
    const double list_draws = per_second(g_counters.list_draws);
    const double list_draws_host = per_second(g_counters.list_draws_host);
    const double stream_full = per_second(g_counters.stream_full);
    // What the same traffic would take on two common slow links (ms of each second).
    const auto at = [&](double mb_per_s) {
        const double to_ms = 1e3 / (mb_per_s * 1e6) / seconds;
        return fmt::format("CPU {:.1f} ms/s, GPU {:.1f} ms/s",
                           static_cast<double>(cpu_writes.bytes) * to_ms,
                           static_cast<double>(gpu_reads.bytes + gpu_writes.bytes) * to_ms);
    };
    std::string sim;
    if (const double chunk_bytes = per_second(g_chunks.bytes); chunk_bytes > 0.0) {
        const double same = per_second(g_chunks.same_bytes);
        sim = fmt::format("; buffer uploads {:.1f} MB/s, {:.1f} MB/s in 256-byte chunks equal to "
                          "their last upload ({:.0f}%, at most what a chunk skip could save)",
                          chunk_bytes / 1e6, same / 1e6, 100.0 * same / chunk_bytes);
    }
    if (Common::LowEndSim::LinkOn()) {
        const u64 cpu_ns = Common::LowEndSim::LinkCpuNsTotal();
        const u64 gpu_ns = Common::LowEndSim::LinkGpuNsTotal();
        sim += fmt::format("; simulated link {} MB/s ({} free): CPU threads spun {:.1f} ms/s, GPU "
                           "owed {:.1f} ms/s",
                           Common::LowEndSim::LinkMbps(), Common::LowEndSim::LinkFreeMbps(),
                          static_cast<double>(cpu_ns - window.sim_cpu_ns) / 1e6 / seconds,
                          static_cast<double>(gpu_ns - window.sim_gpu_ns) / 1e6 / seconds);
        window.sim_cpu_ns = cpu_ns;
        window.sim_gpu_ns = gpu_ns;
    }
    LOG_INFO(HW_GPU,
             "Link traffic per second (FRITCHII_LINK_STATS): CPU writes into device memory {}; GPU "
             "reads of host memory {}; GPU writes to host memory {}; stream buffer full {:.0f}/s; "
             "waits for the GPU {:.0f}/s, {:.1f} ms/s; occlusion results read {:.0f}/s; list draws "
             "{:.0f}/s ({:.0f}/s with arguments in host memory); this traffic alone at 3200 MB/s "
             "(PCIe 3.0 x4): {}; at 2200 MB/s (Thunderbolt): {}{}. Not counted: the driver's own "
             "traffic (command buffers, push descriptors, query pools, present)",
             cpu_writes.text, gpu_reads.text, gpu_writes.text, stream_full, waits, wait_ms,
             query_results, list_draws, list_draws_host, at(3200.0), at(2200.0), sim);
}

} // namespace VideoCommon::LinkStats
