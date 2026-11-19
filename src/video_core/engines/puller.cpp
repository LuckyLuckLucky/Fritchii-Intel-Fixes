// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "common/nx2_hitch.h"
#include "common/settings.h"
#include "core/core.h"
#include <optional>

#include "video_core/nx2_profile.h"
#include "video_core/fritchii_front_split.h"
#include "video_core/fritchii_price.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_stage_link.h"
#include "video_core/memstage_shadow.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_fastdraw_count.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_qmd_prediction.h"
#include "video_core/nx2_runahead.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

/// EDEN_NX2_GPU_WAKE: the acquire's word in host memory, which the GPU
/// thread watches for the guest CPU's stores while it waits (Nx2SemaphoreWaiters::WordStored).
/// Null while the switch is off, or where the word is not 4-byte aligned in host memory.
u32* WatchedSemaphoreWord(MemoryManager& memory_manager, GPUVAddr address) {
    if (!VideoCore::Nx2Debug::GpuWake()) {
        return nullptr;
    }
    u32* const word = reinterpret_cast<u32*>(memory_manager.GetPointer(address));
    if (reinterpret_cast<uintptr_t>(word) % alignof(u32) != 0) {
        return nullptr;
    }
    return word;
}

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        VideoCommon::GpuSplit::Get().Run(
            [rasterizer = dma_pusher.rasterizer, device = *device] {
                rasterizer->FlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::DrainSite::SemaphoreFlush);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
// `flags`: GpuSplit::Defer's (Nx2FastDrawCount::Neutral() for calls that end no delta draw).
template <typename Func>
void OnBack(const Func& func, u64 flags = 0) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func, VideoCommon::GpuSplit::Effect::Unknown, flags);
        split.NoteMemoryWrite();
    } else {
        func();
    }
}

void DrainBack() {
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::Acquire);
}

/// The puller's semaphore release on the back (fable_stage_p3 3): a fixed layout, which the
/// memory stage reads (StageTag::Cache, StageCacheCall::SemaphoreRelease, memstage_shadow.h's
/// StageReleaseWord). One struct for the three releases: WriteLong, SemaphoreRelease and
/// SemaphoreExecute's release.
struct ReleaseCall {
    VideoCore::RasterizerInterface* rasterizer; ///< word 0
    GPUVAddr address;                           ///< word 1
    u32 payload;                                ///< word 2, low half
    u32 properties;                             ///< word 2, high half: QueryPropertiesFlags
    Tegra::MemoryManager* memory;               ///< word 3: the channel's address space
    void operator()() const {
        rasterizer->Query(address, VideoCommon::QueryType::Payload,
                          static_cast<VideoCommon::QueryPropertiesFlags>(properties), payload, 0);
    }
};
static_assert(sizeof(ReleaseCall) == 32 && std::is_trivially_copyable_v<ReleaseCall>);
static_assert(offsetof(ReleaseCall, address) ==
                  8 * static_cast<size_t>(VideoCommon::StageReleaseWord::Address) &&
              offsetof(ReleaseCall, payload) ==
                  8 * static_cast<size_t>(VideoCommon::StageReleaseWord::PayloadProperties) &&
              offsetof(ReleaseCall, properties) ==
                  8 * static_cast<size_t>(VideoCommon::StageReleaseWord::PayloadProperties) + 4 &&
              offsetof(ReleaseCall, memory) ==
                  8 * static_cast<size_t>(VideoCommon::StageReleaseWord::Memory));

/// A semaphore release, tagged for the memory stage, and the front's count of it (the passes
/// line: releases by fence flag and mapped).
void SendRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload,
                 VideoCommon::QueryPropertiesFlags properties) {
    OnBack(ReleaseCall{dma_pusher.rasterizer, address, payload, static_cast<u32>(properties),
                       &dma_pusher.memory_manager},
           VideoCommon::StageCacheCallBits(VideoCommon::StageCacheCall::SemaphoreRelease));
    if (VideoCommon::MemoryStageLink::RingActive()) {
        VideoCommon::MemoryStageLink::Get().NoteFrontRelease(
            properties == VideoCommon::QueryPropertiesFlags::IsAFence,
            dma_pusher.memory_manager.GpuToCpuAddress(address).has_value());
    }
}

// EDEN_NX2_RUNAHEAD (nx2_runahead.h): `acquire` of the u32 at `device` is answered met by the
// release sent at ring `position` that writes `payload`. Mode 1 snapshots the guest's CPU writes
// and writes the word now (the CPU goes on early), mode 2 takes a verify chunk. The word is not
// written on a GPU-written page (a cache may hold a newer copy) or the native syncpoint shim.
void RunaheadBoundary(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire, u64 device,
                      u32 payload, u64 position, bool predicted = false) {
    namespace Runahead = VideoCommon::Runahead;
    const Runahead::Mode mode = Runahead::CurrentMode();
    if (mode != Runahead::Mode::On && mode != Runahead::Mode::Verify) [[likely]] {
        return;
    }
    auto& memory = dma_pusher.memory_manager;
    u8* word = nullptr;
    if (!memory.OverlapsNativeSyncpointShim(acquire.address, sizeof(u32)) &&
        !Nx2IsGpuWritten(device)) {
        word = memory.GetPointer(acquire.address);
    }
    if (Runahead::AtBoundary(dma_pusher.system, device, word, payload, position,
                             VideoCommon::GpuSplit::Get().ConsumedNow(), predicted)) {
        Nx2SemaphoreWaiters::Get().OnWrite(device, payload);
    }
}

// EDEN_NX2_RUNAHEAD: an acquire answered met from the word as the queued writes leave it
// (EDEN_NX2_SEM_FAST=2 overlays them): when the newest release the front sent to the word is
// still queued and meets it, that release is the boundary.
void RunaheadOverlaidBoundary(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire) {
    namespace Runahead = VideoCommon::Runahead;
    const Runahead::Mode mode = Runahead::CurrentMode();
    if (mode != Runahead::Mode::On && mode != Runahead::Mode::Verify) [[likely]] {
        return;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    if (!device) {
        return;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    u64 position = 0;
    if (const std::optional<u32> release = split.NewestReleaseTo(*device, position);
        release && position > split.ConsumedNow() && acquire.Matches(*release)) {
        RunaheadBoundary(dma_pusher, acquire, *device, *release, position);
    }
}

// INTEL_FORK_DELAYED_SHORTCUTS=1 (boot, default 0): with delayed fences (Balanced or Accurate
// fence behaviour, EDEN_NX2_ACCURATE_PARTS=16) the front's acquire shortcuts stay on for words
// whose newest recorded release is a plain release with a payload the front knew when it sent it
// (puller releases, 3D payload releases; a 16-byte payload report only without accurate
// queries). A GPU-side acquire needs no guest-visible write: the release and the acquiring
// channel's later work run in order on the back and the host queue. Only the guest CPU must see
// the release late, and it still does: the write stays a delayed fence operation, made by a
// fence that is signalled without waiting for the host GPU. An unmet acquire suspends and
// resumes on its waiter's latch (the delayed write's OnWrite) instead of a forced drain that
// waits for the host GPU. Compute 0x6c3 releases and words without a recorded plain release
// (reports, QMD releases, the guest CPU) keep the drain. Off, or fences not delayed: no change.
namespace DelayedShortcuts {
[[nodiscard]] bool Enabled() {
    static const bool on = [] {
        const char* const value = std::getenv("INTEL_FORK_DELAYED_SHORTCUTS");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

/// Whether the newest release recorded for the u32 at `device` (GpuSplit::NoteRelease) writes a
/// payload known when it was sent, not a value the GPU makes.
[[nodiscard]] bool PlainRelease(u64 device) {
    using Release = VideoCommon::GpuSplit::ReleaseKind;
    switch (VideoCommon::GpuSplit::Get().RecordedReleaseKind(device)) {
    case Release::Puller:
    case Release::Payload3DShort:
        return true;
    case Release::Payload3DLong:
        // With accurate queries its operation runs at a later fence (AcquireTableFirst::TableKind).
        return !VideoCore::Nx2Debug::AccuratePart(1);
    default:
        return false;
    }
}

/// The switch is on, fences are delayed, and the word's newest release is plain.
[[nodiscard]] bool Allowed(DmaPusher& dma_pusher, GPUVAddr address) {
    if (!Enabled() || !VideoCore::Nx2Debug::DelayedFences()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    return device && PlainRelease(*device);
}

/// Queues a fence that commits the pending (delayed) operations without waiting for the host
/// GPU: they land on the fencing thread once the host GPU passed the work before them, and a
/// release among them latches the suspended acquire's waiter.
void SignalFenceNoWait(DmaPusher& dma_pusher) {
    // Memory stage tag Cache: the fence manager only; no 3D state (as the fence releases).
    VideoCommon::GpuSplit::Get().Defer(
        [rasterizer = dma_pusher.rasterizer] { rasterizer->SignalFence([] {}); },
        VideoCommon::GpuSplit::Effect::Unknown,
        VideoCommon::Nx2FastDrawCount::Neutral() |
            VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
    VideoCommon::GpuSplit::Get().NoteMemoryWrite();
}

/// The 10 s line's counters. Front thread only.
struct Stats {
    u64 met_queued{}; ///< Acquires met by a queued release with delayed fences
    u64 avoided{};    ///< Forced drains (host GPU waits) replaced by a fence without a wait
    u64 kept{};       ///< Forced drains kept: the word's newest release is not plain
    std::chrono::steady_clock::time_point since{};
};
Stats g_stats;

void Tick() {
    if (!Enabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_stats.since == std::chrono::steady_clock::time_point{}) {
        g_stats.since = now;
        return;
    }
    const double window = std::chrono::duration<double>(now - g_stats.since).count();
    if (window < 10.0) {
        return;
    }
    LOG_INFO(HW_GPU,
             "Intel fork delayed shortcuts (delayed fences {}): acquires met by queued release "
             "{:.0f}/s, drains avoided {:.0f}/s, drains kept for GPU-written payloads {:.0f}/s",
             VideoCore::Nx2Debug::DelayedFences() ? "on" : "off",
             static_cast<double>(g_stats.met_queued) / window,
             static_cast<double>(g_stats.avoided) / window,
             static_cast<double>(g_stats.kept) / window);
    g_stats = {};
    g_stats.since = now;
}
} // namespace DelayedShortcuts

// EDEN_NX2_ACQUIRE_PRECHECK=1: a GEQUAL acquire whose word already holds the value continues
// without draining the split, if nothing queued for the back may write the word (a release that
// lowers it, a fence action) and no GPU work ever wrote its page (then no cache holds a newer word
// than guest memory). It counts as a host sync for later GPU-written command lists, as the drain
// did. Equal and mask acquires always drain. Ported, with
// the queued-writer check added.
// A release still queued for the split's back meets the acquire: the back runs it before the
// acquire's channel goes on, so the acquire needs no drain (GpuSplit::QueuedReleasePayload).
bool MetByQueuedRelease(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire,
                        u32* miss_out = nullptr) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    if (!device) {
        return false;
    }
    u32 miss = 0;
    const auto payload = split.QueuedReleasePayload(*device, acquire.address, &miss);
    if (miss_out) {
        *miss_out = payload ? 4 : miss;
    }
    if (!payload || !acquire.Matches(*payload)) {
        if (VideoCommon::Nx2Profile::Enabled()) {
            using Counter = VideoCommon::Nx2Profile::Counter;
            VideoCommon::Nx2Profile::Add(miss == 1   ? Counter::AcquireNoRelease
                                         : miss == 2 ? Counter::AcquireReleaseRan
                                         : miss == 3 ? Counter::AcquireLaterWrite
                                                     : Counter::AcquirePayloadMiss,
                                         1);
        }
        return false;
    }
    split.NoteSync();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquiresQueued, 1);
    if (DelayedShortcuts::Enabled() && VideoCore::Nx2Debug::DelayedFences()) {
        ++DelayedShortcuts::g_stats.met_queued;
        DelayedShortcuts::Tick();
    }
    if (VideoCommon::Runahead::CurrentMode() != VideoCommon::Runahead::Mode::Off) [[unlikely]] {
        u64 position = 0;
        if (split.QueuedReleaseTo(*device, split.ConsumedNow(), position)) {
            RunaheadBoundary(dma_pusher, acquire, *device, *payload, position);
        }
    }
    return true;
}

// What a noted queued writer of a semaphore word is (GpuSplit::QueuedGuestWriters), for the log.
const char* NotedWriterName(const VideoCommon::GpuSplit::GuestWriters& writers) {
    using Cause = VideoCommon::GuestWriterCause;
    using Kind = VideoCommon::GpuSplit::WriterKind;
    switch (writers.noted_cause) {
    case Cause::Pending:
        return "noted write (puller semaphore, 3D report or upload)";
    case Cause::WriterRange:
        return writers.range_kind == Kind::Release ? "compute 0x6c3 release"
               : writers.range_kind == Kind::Copy  ? "copy or copy semaphore"
               : writers.range_kind == Kind::Upload ? "compute inline upload"
                                                    : "compute or copy range";
    case Cause::Ranged:
        return "3D ranged write";
    default:
        return "other";
    }
}

// fable_answer_6 6i (b): ACQUIRE_PRECHECK=2 answers "met" from guest memory alone. NVN2 resets
// its counters (a plain release of a low value) at scene and pass boundaries; a reset still
// queued for the back leaves the old, higher value in guest memory, and the channel then runs
// before the producer the reset preceded (a suspect for the washed-out scene). Asked only where
// level 2 would answer met, with what the front knows of that word:
// - over a queued lower release: the newest plain release recorded for the word
//   (GpuSplit::QueuedReleaseTo: puller WriteLong and SEM_EXECUTE releases, 3D and compute 0x6c3
//   payload releases) is still queued and writes less than the word read;
// - over a queued writer: anything queued the front noted to the word: that release whatever its
//   payload, a noted write over it (QueuedGuestWriters' noted writers: puller semaphores, 3D
//   reports, compute 0x6c3 releases and reductions, copies and copy semaphores, uploads), or,
//   with EXP13 on, a release the QMD prediction recorded for it (QMD completion releases; with
//   EXP13=0 the front can't tell which word a queued QMD releases). The lower ones are among
//   them;
// - readable below: SemaphoreReadableNow read the word current (guest memory with the queued
//   writes the front knows the words of overlaid, GpuSplit::OverlayQueuedWrites, or after
//   waiting for the noted ones, WaitForWritesTo) and below the value, yet guest memory meets it
//   now. Both lookups above ask what is queued NOW, from the same tables the readable read used
//   (pending_writes, queued_releases), so a write the overlay applied that is still queued shows
//   up there too. What they can't see: the readable read came earlier (at back position
//   ReadableRead::done), and work sent before the acquire may have run in between. The writes
//   SemaphoreReadableNow neither waits for nor overlays are the ones not in pending_writes:
//   compute 0x6c3 releases and reductions and copy semaphores (writer_ranges), and QMD
//   completion releases (dispatches, no per-word note with EXP13=0). One of those landing
//   between the two reads raises the word, and has run, so it is queued no more. Or the back
//   ran a known write and then one of those between the overlay's position load and its guest
//   memory read: the overlay then puts the known (older, lower) word back over the newer one.
//   Either way the write the overlay applied ran before level 2 looked. The first 8 log
//   the overlay's newest write since the read (kind, ring, value) and the writers sent after
//   the read, to tell such a raise from a reset.
// The first 8 of each are logged, and one line every 5 s (HW_GPU, Info). Front thread only, as
// the GpuSplit tables it reads.
// EDEN_NX2_EXP34 (live, default 1, nx2_debug_control.h): 1 = an acquire over a queued writer
// (lower releases included) or readable below the value takes the normal path instead (the
// drain or the readable fence release, as when guest memory is below the value), 2 = only one
// over a queued lower release does, 0 = count only. It only turns met answers into the normal
// path, never the other way.

// What SemaphoreReadableNow read for an acquire: the word, and the back's position loaded before
// guest memory was read for it.
struct ReadableRead {
    u32 word;
    u64 done;
};

class PrecheckAudit {
public:
    static PrecheckAudit& Get() {
        static PrecheckAudit audit;
        return audit;
    }

    /// Level 2 would answer met: guest memory holds `word` (>= `value`) at GPU `address`
    /// (device `device`); `readable`: what SemaphoreReadableNow read, when it read the word
    /// current. Returns whether the answer stands (EXP34).
    bool Allow(GPUVAddr address, u64 device, u32 word, u32 value,
               std::optional<ReadableRead> readable) {
        auto& split = VideoCommon::GpuSplit::Get();
        const u64 done = split.ConsumedNow();
        u64 release_position = 0;
        const std::optional<u32> release = split.QueuedReleaseTo(device, done, release_position);
        const bool lower = release && *release < word;
        const auto writers = split.QueuedGuestWriters(done, address, device, sizeof(u32));
        const bool noted = writers.noted > done;
        VideoCommon::QmdPrediction::Queued records{};
        if (VideoCommon::QmdPrediction::Enabled()) {
            records = VideoCommon::QmdPrediction::QueuedPayloads(device, done);
        }
        const bool writer = release || noted || records.count != 0;
        ++met;
        if (lower) {
            ++over_lower;
            if (logged_lower < 8) {
                ++logged_lower;
                LOG_INFO(HW_GPU,
                         "Acquire precheck (level 2) met over a queued lower release: {:#x} "
                         "(device {:#x}) word {:#x} >= {:#x}, queued release of {:#x} at ring {} "
                         "(back at {}, sent {})",
                         address, device, word, value, *release, release_position, done,
                         split.SentPosition());
            }
        }
        if (writer) {
            ++over_writer;
            if (logged_writer < 8) {
                ++logged_writer;
                LOG_INFO(HW_GPU,
                         "Acquire precheck (level 2) met over a queued writer: {:#x} (device "
                         "{:#x}) word {:#x} >= {:#x}; release {} (payload {:#x}, ring {}), noted "
                         "{} (ring {}), QMD prediction records {} (first {:#x}); back at {}, sent "
                         "{}",
                         address, device, word, value, release ? "queued" : "none",
                         release.value_or(0), release_position,
                         noted ? NotedWriterName(writers) : "none", noted ? writers.noted : 0,
                         records.count, records.count != 0 ? records.payloads[0] : 0, done,
                         split.SentPosition());
            }
        }
        const bool below = readable && readable->word < value;
        if (below) {
            ++readable_below;
            if (logged_below < 8) {
                ++logged_below;
                LogReadableBelow(split, address, device, word, value, *readable, done);
            }
        }
        // EXP34: 1 = over a queued writer (the lower releases among them) or readable below the
        // value the normal path answers, 2 = only over a queued lower release; anything else
        // counts only.
        const int guard = VideoCore::Nx2Debug::Experiment(34);
        const bool refuse = (guard == 1 && (writer || below)) || (guard == 2 && lower);
        refused += refuse ? 1 : 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds{5}) {
            const double window = std::chrono::duration<double>(now - last_report).count();
            last_report = now;
            LOG_INFO(HW_GPU,
                     "Acquire precheck (level 2): {} met, {} over a queued lower release, {} over "
                     "a queued writer, {} readable below the value; {} sent the normal way "
                     "(EXP34={}) ({:.1f} s)",
                     met, over_lower, over_writer, readable_below, refused, guard, window);
            met = over_lower = over_writer = readable_below = refused = 0;
        }
        return !refuse;
    }

private:
    // A readable-below case: what was written to the word, or sent that may write it, between
    // the readable read (back at `read.done`) and now (back at `done`).
    static void LogReadableBelow(VideoCommon::GpuSplit& split, GPUVAddr address, u64 device,
                                 u32 word, u32 value, const ReadableRead& read, u64 done) {
        // The overlay's source: the newest noted write over the word sent after the read.
        const auto overlay = split.NewestNotedWriteTo(read.done, device);
        // Releases and noted writers sent after the read, run since or still queued.
        u64 release_position = 0;
        const std::optional<u32> release =
            split.QueuedReleaseTo(device, read.done, release_position);
        const auto writers = split.QueuedGuestWriters(read.done, address, device, sizeof(u32));
        const bool noted = writers.noted > read.done;
        // Compute and copies (their 0x6c3, copy and QMD completion releases), unranged writers
        // and queued functions: what the readable read can't account for.
        const u64 untracked = split.UntrackedWriterPosition(device);
        LOG_INFO(HW_GPU,
                 "Acquire precheck (level 2) readable below the value: {:#x} (device {:#x}) "
                 "readable {:#x} < {:#x} <= word {:#x}; read at back {}, back now {}, sent {}; "
                 "overlay write {} at ring {} ({}, {} known words, writes {:#x}); release since "
                 "the read {} "
                 "(payload {:#x}, ring {}); noted since the read {} (ring {}); untracked writer "
                 "at ring {} ({})",
                 address, device, read.word, value, word, read.done, done, split.SentPosition(),
                 overlay.position != 0 ? (overlay.position > done ? "queued" : "ran")
                                       : "none",
                 overlay.position, overlay.what != nullptr ? overlay.what : "-", overlay.words,
                 overlay.words != 0 ? overlay.value : 0, release ? "sent" : "none",
                 release.value_or(0), release_position,
                 noted ? NotedWriterName(writers) : "none", noted ? writers.noted : 0, untracked,
                 untracked > done        ? "queued"
                 : untracked > read.done ? "ran since the read"
                                         : "before the read");
    }

    u64 met{};
    u64 over_lower{};
    u64 over_writer{};
    u64 readable_below{};
    u64 refused{};
    u32 logged_lower{};
    u32 logged_writer{};
    u32 logged_below{};
    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
};

// EDEN_NX2_ACQUIRE_PRECHECK=2: also while work is queued and on GPU-written pages. The word only
// grows (GEQUAL semaphores count up), so guest memory, if older than a queued release or a cache,
// holds a lower value: one that meets the acquire already, meets it then too. Only a write that
// lowers the word (a reset) could make this wrong (PrecheckAudit counts the met answers given over
// a queued write the front knows of). Ocarina drained ~2,200 acquires a second that were met at
// once. `readable`: what SemaphoreReadableNow read, when it read the word current.
bool GequalMetWithoutDrain(DmaPusher& dma_pusher, GPUVAddr address, u32 value,
                           std::optional<ReadableRead> readable = std::nullopt) {
    static const int level = [] {
        const char* setting = std::getenv("EDEN_NX2_ACQUIRE_PRECHECK");
        return setting == nullptr ? 0 : setting[0] == '2' ? 2 : setting[0] != '0' ? 1 : 0;
    }();
    auto& split = VideoCommon::GpuSplit::Get();
    if (level == 0 || !split.Active()) {
        return false;
    }
    auto& memory = dma_pusher.memory_manager;
    if (memory.OverlapsNativeSyncpointShim(address, sizeof(u32)) ||
        (level == 1 && split.HasQueuedMemoryWriters(address, address + sizeof(u32)))) {
        return false;
    }
    const auto device = memory.GpuToCpuAddress(address);
    if (!device || (level == 1 && Nx2IsGpuWritten(*device))) {
        return false;
    }
    const u32 word = memory.Read<u32>(address);
    if (word < value) {
        return false;
    }
    if (level == 2 && !PrecheckAudit::Get().Allow(address, *device, word, value, readable)) {
        return false;
    }
    split.NoteSync();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquiresFast, 1);
    return true;
}

void ReportLearning(double window);

// EDEN_NX2_ACQUIRE_TRACE=1: per semaphore address, the acquires that drained the split, the time
// the drains took, why the queued-release check missed and how each ended. Logged every 5 s.
class AcquireTrace {
public:
    enum Outcome : u32 { MetAfterDrain, MetAfterFences, Suspended, NumOutcomes };

    static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_NX2_ACQUIRE_TRACE") != nullptr;
        return enabled;
    }

    void Record(GPUVAddr address, u32 op, u32 channel, u32 miss, u32 word_before, u32 value,
                u64 drain_ns, Outcome outcome, u32 writer, u32 writer_method) {
        std::scoped_lock lock{mutex};
        Entry& entry = entries[address];
        ++entry.writers[(std::min)(writer, 4U)];
        if (writer == 3) {
            entry.writer_method = writer_method;
        }
        ++entry.count;
        entry.drain_ns += drain_ns;
        ++entry.miss[(std::min)(miss, 4U)];
        ++entry.outcomes[outcome];
        entry.op = op;
        entry.channel = channel;
        entry.behind += word_before < value ? 1 : 0;
        entry.last_value = value;
        entry.last_word = word_before;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::seconds{5}) {
            return;
        }
        const double window = std::chrono::duration<double>(now - last_report).count();
        last_report = now;
        std::vector<std::pair<GPUVAddr, Entry>> sorted(entries.begin(), entries.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) {
            return a.second.drain_ns > b.second.drain_ns;
        });
        u64 total_ns = 0;
        u32 total = 0;
        for (const auto& [_, e] : sorted) {
            total_ns += e.drain_ns;
            total += e.count;
        }
        LOG_INFO(HW_GPU, "Acquire trace: {:.0f} drained acquires/s, {:.1f} ms/s draining, {} addresses",
                 total / window, static_cast<double>(total_ns) / 1e6 / window, sorted.size());
        ReportLearning(window);
        for (size_t i = 0; i < (std::min)(sorted.size(), size_t{12}); ++i) {
            const auto& [entry_address, e] = sorted[i];
            LOG_INFO(HW_GPU,
                     "  {:#x} op {} ch {}: {:.0f}/s, {:.2f} ms/s, word behind {}x (last {:#x} < {:#x}); "
                     "miss none/ran/later/payload-miss {}/{}/{}/{}; met after drain {}, after "
                     "fences {}, suspended {}; queued writer none/compute/sync/unranged/ranged "
                     "{}/{}/{}/{}/{} (unranged method {:#x})",
                     entry_address, e.op, e.channel, e.count / window,
                     static_cast<double>(e.drain_ns) / 1e6 / window, e.behind, e.last_word,
                     e.last_value, e.miss[1], e.miss[2], e.miss[3], e.miss[4],
                     e.outcomes[MetAfterDrain], e.outcomes[MetAfterFences], e.outcomes[Suspended],
                     e.writers[0], e.writers[1], e.writers[2], e.writers[3], e.writers[4],
                     e.writer_method);
        }
        entries.clear();
    }

    static AcquireTrace& Get() {
        static AcquireTrace trace;
        return trace;
    }

private:
    struct Entry {
        u32 count{};
        u64 drain_ns{};
        std::array<u32, 5> miss{};
        std::array<u32, NumOutcomes> outcomes{};
        std::array<u32, 5> writers{};
        u32 writer_method{};
        u32 op{};
        u32 channel{};
        u32 behind{};
        u32 last_value{};
        u32 last_word{};
    };
    std::mutex mutex;
    std::unordered_map<GPUVAddr, Entry> entries;
    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
};

// EDEN_NX2_ACQUIRE_EQUAL=1: an equal acquire whose word holds the value continues without a
// drain, unless a release of another payload to the word is still queued. NVN2's queue-sync slots
// get one release per frame number, so the word changes again only after the acquiring queue
// moved on. Ocarina drained ~30 ms/s on such acquires that were met before the drain.
bool EqualMetWithoutDrain(DmaPusher& dma_pusher, GPUVAddr address, u32 value) {
    static const bool enabled = [] {
        const char* setting = std::getenv("EDEN_NX2_ACQUIRE_EQUAL");
        return setting != nullptr && setting[0] == '1';
    }();
    auto& split = VideoCommon::GpuSplit::Get();
    if (!enabled || !split.Active()) {
        return false;
    }
    auto& memory = dma_pusher.memory_manager;
    if (memory.OverlapsNativeSyncpointShim(address, sizeof(u32)) || memory.Read<u32>(address) != value) {
        return false;
    }
    const auto device = memory.GpuToCpuAddress(address);
    if (!device) {
        return false;
    }
    u32 miss = 0;
    if (const auto payload = split.QueuedReleasePayload(*device, address, &miss);
        (payload && *payload != value) || miss == 3) {
        return false;
    }
    if (u32 method = 0; split.QueuedWriterKind(address, address + sizeof(u32), method) == 4) {
        return false; // An upload to the word is queued
    }
    split.NoteSync();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquiresFast, 1);
    return true;
}

// EDEN_NX2_ACQUIRE_LEARN=1 (experiment): acquires drained per address. An address whose last 32
// drained acquires were all met at once after the drain (releases of queued work: QMD
// completions) is taken as met without one; the back checks the word when it gets there and
// counts misses (a miss turns this off). One whose last 32 all suspended suspends at once:
// that is always safe, the drain told it nothing.
class AcquireLearning {
public:
    static constexpr u32 Streak = 32;

    static bool Enabled() {
        static const bool enabled = [] {
            const char* setting = std::getenv("EDEN_NX2_ACQUIRE_LEARN");
            return setting != nullptr && setting[0] == '1';
        }();
        return enabled;
    }

    struct Stats {
        u32 met{};
        u32 suspended{};
    };

    Stats& Get(GPUVAddr address) {
        return stats[address];
    }

    void NoteMet(GPUVAddr address) {
        Stats& entry = stats[address];
        entry.met = (std::min)(entry.met + 1, Streak);
        entry.suspended = 0;
    }

    void NoteSuspended(GPUVAddr address) {
        Stats& entry = stats[address];
        entry.suspended = (std::min)(entry.suspended + 1, Streak);
        entry.met = 0;
    }

    static inline std::atomic<u32> misses{};
    static inline std::atomic<u64> speculated{};
    static inline std::atomic<u64> early_suspends{};

    static AcquireLearning& Instance() {
        static AcquireLearning learning;
        return learning;
    }

private:
    std::unordered_map<GPUVAddr, Stats> stats;
};

void ReportLearning(double window) {
    static u64 last_speculated = 0, last_early = 0;
    const u64 speculated = AcquireLearning::speculated.load(std::memory_order_relaxed);
    const u64 early = AcquireLearning::early_suspends.load(std::memory_order_relaxed);
    LOG_INFO(HW_GPU, "Acquire learning: {:.0f} taken as met/s, {:.0f} suspended without a drain/s, "
             "{} misses", (speculated - last_speculated) / window, (early - last_early) / window,
             AcquireLearning::misses.load(std::memory_order_relaxed));
    last_speculated = speculated;
    last_early = early;
}

// EDEN_NX2_EXP3: FenceRelease drains skipped and taken, logged every 5 s.
struct FenceReleaseSkips {
    using FenceQuiet = VideoCommon::GpuSplit::FenceQuiet;
    std::array<u64, 4> taken{}; ///< By GpuSplit::FenceQuiet; Quiet = fence work added since
    u64 skipped{};
    std::chrono::steady_clock::time_point since{};

    void Count(FenceQuiet state, bool skip) {
        if (skip) {
            ++skipped;
        } else {
            ++taken[static_cast<size_t>(state)];
        }
        const auto now = std::chrono::steady_clock::now();
        if (since == std::chrono::steady_clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        const auto rate = [window](u64 count) { return static_cast<double>(count) / window; };
        LOG_INFO(HW_GPU,
                 "Fence release skip (EXP3={}): {:.0f} drains skipped/s, {:.0f} taken/s: {:.0f} "
                 "back behind, {:.0f} fence work pending, {:.0f} fence work added since, {:.0f} "
                 "unchecked",
                 VideoCore::Nx2Debug::Experiment(3), rate(skipped),
                 rate(taken[0] + taken[1] + taken[2] + taken[3]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Behind)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Busy)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Quiet)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Unchecked)]));
        *this = {};
        since = now;
    }
};

// The fence manager records into the scheduler and downloads the CPU's writes over GPU data:
// the back's state, which a drained back still uses from its idle hook (it submits).
// EDEN_NX2_EXP3 (1, or 2 to allow pending fences without delayed fences): no drain when the
// back, having run everything sent, found the release would only invalidate the GPU cache
// (FenceManager::FencesQuiet) and no fence work was added since; the invalidation still runs
// on the back, in order. Fences already made complete without a release.
void ReleaseFences(DmaPusher& dma_pusher) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (VideoCore::Nx2Debug::Experiment(3) != 0 && split.Active()) {
        static FenceReleaseSkips skips;
        u64 epoch{};
        const auto state = split.FencesQuietNow(epoch);
        const bool skip = state == VideoCommon::GpuSplit::FenceQuiet::Quiet &&
                          dma_pusher.rasterizer->FenceWorkEpoch() == epoch;
        skips.Count(state, skip);
        if (skip) {
            // Memory stage tag Cache (fable_stage_levelpoints 14.5): a gather (the buffer,
            // texture and pipeline caches); no 3D state, no Query.
            OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); },
                   VideoCommon::Nx2FastDrawCount::Neutral() |
                       VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
            return;
        }
    }
    // Delta draw count: fence releases are neutral, the fence manager breaks per fence released.
    // Memory stage tag Cache: the fence manager only; no 3D state.
    split.Run([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
              VideoCommon::DrainSite::FenceRelease,
              VideoCommon::Nx2FastDrawCount::Neutral() |
                  VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
}

// How a suspended acquire was found met (Puller::PendingAcquire::resume_route, set by
// CanResumeAcquire), for the resume histogram of FenceWriterStats.
enum class FenceResumeRoute : u8 {
    Latch,  ///< The back ran a release that met it and latched its waiter
    Word,   ///< The word read met (or acquire learning's read), no drain
    Answer, ///< A queued release, the QMD prediction or the GEQUAL bound met it, no drain
    Drain,  ///< Met after a poll drained the back (the 16th unmet poll, or 1 ms since a drain)
    Lagged, ///< EXP12=5/6: met at a lagged check (the latch, word or answer after its wait)
    Count,
};

// EDEN_NX2_EXP12: the fence releases of acquires whose word the front read current but unmet
// (ReleaseFencesForReadable), logged every 5 s. Front thread only.
struct FenceWriterStats {
    using Clock = std::chrono::steady_clock;
    static constexpr size_t Routes = static_cast<size_t>(FenceResumeRoute::Count);
    /// Acquire to resume: under 100 us, 500 us, 1 ms, 5 ms, then longer.
    static constexpr std::array<u64, 4> BucketLimitsNs{100'000, 500'000, 1'000'000, 5'000'000};
    static constexpr size_t Buckets = BucketLimitsNs.size() + 1;

    /// Fence path acquires of one channel, and what became of them.
    struct Channel {
        s32 id{-1};
        u64 acquires{};  ///< Readable acquires on the fence path
        u64 met{};       ///< Met there (no suspension)
        u64 suspended{};
        u64 resumed{};
        u64 resume_ns{};
        std::array<u64, Routes> routes{};
    };

    u64 acquires{};   ///< Readable acquires unmet before their fence release
    u64 delayed{};    ///< With EXP12 on, not applied: fences are delayed (GPU accuracy High)
    u64 waits{};      ///< Waits for the back to run the writers the read can't account for
    u64 wait_ns{};
    u64 met{};        ///< Met once the back ran those writers (EXP12=4: read without a wait)
    u64 full{};       ///< Full drains (EXP12 0 or 3, 1 when still unmet)
    u64 full_ns{};    ///< Their time (from the target on, when there was one)
    u64 full_met{};   ///< Met after the full drain
    u64 suspended{};  ///< EXP12=2 or 4: unmet, suspended without the full drain
    u64 saved_ns{};   ///< EXP12=3: the full drains' time past the target, of acquires met there
    u64 resumed{};    ///< Readable acquires suspended after their fence release, resumed since
    u64 resume_ns{};  ///< Their time from the acquire to the resume
    u64 skipped{};        ///< EXP12=4/5/6: writer waits not made
    /// Of them, the back's stored position was short of the target. Overstates: the back stores
    /// its position only every few thousand words of a batch and at its end.
    u64 skipped_behind{};
    /// EXP12=4, deferred "met by" with a reduction bound: checked at the resume (a word read
    /// within 1 ms), or left unchecked.
    u64 resume_bound_checks{};
    u64 resume_bound_dropped{};
    /// The resumed ones by route and time from the acquire (Buckets), and their time by route.
    std::array<std::array<u64, Buckets>, Routes> resume_hist{};
    std::array<u64, Routes> route_ns{};
    /// Poll drains (CanResumeAcquire) of fence path acquires: [0] pending under 2 ms, [1] longer.
    std::array<u64, 2> poll_drains{};
    std::array<u64, 2> poll_drain_ns{};
    u64 other_poll_drains{}; ///< Poll drains of the other suspended acquires
    u64 other_poll_drain_ns{};
    /// EXP12=5/6: lagged checks (CanResumeAcquire), those whose wait found the back short of
    /// the target, their time, and those that found the acquire met.
    u64 lagged_checks{};
    u64 lagged_waits{};
    u64 lagged_wait_ns{};
    u64 lagged_met{};
    /// EXP12=6: acquires whose word had a queued writer (waited for it) or none (suspended at
    /// once), which term set the wait's target (an unlanded prediction record, the block or page
    /// stamp over the word, a queued puller release), and those with the write stamps off (mode
    /// 2's target). `stamp_terms`: the term the full WriteStamp would have waited for, by
    /// StampTerm (counting only).
    u64 word_waits{};
    u64 word_none{};
    std::array<u64, 3> word_terms{};
    u64 word_stamps_off{};
    std::array<u64, static_cast<size_t>(VideoCommon::StampTerm::Count)> stamp_terms{};
    /// Suspended acquires still pending 100 ms after the acquire, each counted once: [0] fence
    /// path, [1] the others. Acquires the guest CPU releases can legitimately wait that long
    /// (loads, pauses): compare with EXP12=2 in the same scene.
    std::array<u64, 2> long_pending{};
    std::array<Channel, 8> channels{};
    size_t channel_count{};
    Channel other_channels{}; ///< Channels past the first 8 seen in the window
    Clock::time_point since{};

    static FenceWriterStats& Get() {
        static FenceWriterStats stats;
        return stats;
    }

    static u64 Nanoseconds(Clock::time_point begin, Clock::time_point end) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    }

    Channel& ChannelRow(s32 id) {
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i].id == id) {
                return channels[i];
            }
        }
        if (channel_count < channels.size()) {
            channels[channel_count].id = id;
            return channels[channel_count++];
        }
        return other_channels;
    }

    /// ProcessSemaphoreTriggerMethod: a readable acquire's outcome on the fence path.
    void NoteOutcome(s32 channel, bool suspended_there) {
        Channel& row = ChannelRow(channel);
        ++row.acquires;
        row.met += suspended_there ? 0 : 1;
        row.suspended += suspended_there ? 1 : 0;
    }

    /// ResumeAcquire: a fence path acquire resumed `ns` after it, by `route`.
    void NoteResume(s32 channel, u8 route, u64 ns) {
        const size_t r = (std::min)(static_cast<size_t>(route), Routes - 1);
        size_t bucket = 0;
        while (bucket < BucketLimitsNs.size() && ns >= BucketLimitsNs[bucket]) {
            ++bucket;
        }
        ++resumed;
        resume_ns += ns;
        ++resume_hist[r][bucket];
        route_ns[r] += ns;
        Channel& row = ChannelRow(channel);
        ++row.resumed;
        row.resume_ns += ns;
        ++row.routes[r];
    }

    /// CanResumeAcquire drained the back for a suspended acquire.
    void NotePollDrain(bool fence_path, bool young, u64 ns) {
        if (!fence_path) {
            ++other_poll_drains;
            other_poll_drain_ns += ns;
            return;
        }
        ++poll_drains[young ? 0 : 1];
        poll_drain_ns[young ? 0 : 1] += ns;
    }

    void Tick() {
        const auto now = Clock::now();
        if (since == Clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        const auto rate = [window](u64 count) { return static_cast<double>(count) / window; };
        const auto ms = [window](u64 ns) { return static_cast<double>(ns) / 1e6 / window; };
        LOG_INFO(HW_GPU,
                 "Fence writer waits (EXP12={}): {:.0f} readable acquires/s unmet before their "
                 "fence release ({:.0f}/s not applied, delayed fences); writer waits {:.0f}/s "
                 "{:.2f} ms/s, met there {:.0f}/s; full drains {:.0f}/s {:.2f} ms/s, met after "
                 "them {:.0f}/s; suspended without one {:.0f}/s; full drain past the writers of "
                 "acquires met there (EXP12=3) {:.2f} ms/s; resumed {:.0f}/s, {:.0f} us from "
                 "acquire to resume",
                 VideoCore::Nx2Debug::Experiment(12), rate(acquires), rate(delayed), rate(waits),
                 ms(wait_ns), rate(met), rate(full), ms(full_ns), rate(full_met), rate(suspended),
                 ms(saved_ns), rate(resumed),
                 resumed != 0 ? static_cast<double>(resume_ns) / 1e3 / resumed : 0.0);
        // Per route: per second under 100 us / 500 us / 1 ms / 5 ms / longer, then the mean.
        constexpr std::array<const char*, Routes> route_names{
            "latch", "word read", "front answer", "poll drain", "lagged check"};
        std::string routes;
        for (size_t r = 0; r < Routes; ++r) {
            u64 count = 0;
            for (const u64 n : resume_hist[r]) {
                count += n;
            }
            routes += fmt::format("{}{} {:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f} ({:.0f} us)",
                                  r != 0 ? ", " : "", route_names[r], rate(resume_hist[r][0]),
                                  rate(resume_hist[r][1]), rate(resume_hist[r][2]),
                                  rate(resume_hist[r][3]), rate(resume_hist[r][4]),
                                  count != 0 ? static_cast<double>(route_ns[r]) / 1e3 / count
                                             : 0.0);
        }
        std::string rows;
        const auto add_row = [&](const Channel& row, bool other) {
            if (row.acquires == 0 && row.resumed == 0) {
                return;
            }
            rows += fmt::format(
                " [{}: {:.0f} acquires/s, met there {:.0f}, suspended {:.0f}, resumed {:.0f} "
                "(latch {:.0f}, word {:.0f}, answer {:.0f}, drain {:.0f}, lagged {:.0f}), "
                "{:.0f} us to resume]",
                other ? std::string{"other channels"} : fmt::format("channel {}", row.id),
                rate(row.acquires), rate(row.met), rate(row.suspended), rate(row.resumed),
                rate(row.routes[0]), rate(row.routes[1]), rate(row.routes[2]),
                rate(row.routes[3]), rate(row.routes[4]),
                row.resumed != 0 ? static_cast<double>(row.resume_ns) / 1e3 / row.resumed
                                 : 0.0);
        };
        for (size_t i = 0; i < channel_count; ++i) {
            add_row(channels[i], false);
        }
        add_row(other_channels, true);
        // StampTerm's order (gpu_split.h).
        constexpr std::array<const char*, static_cast<size_t>(VideoCommon::StampTerm::Count)>
            stamp_names{"none",          "block stamp",      "page stamp",
                        "stamps on since/over 1 MiB", "unknown write", "unbounded writer",
                        "unranged writer", "queued function", "3D method write",
                        "ranged overflow"};
        std::string stamp_text;
        for (size_t i = 0; i < stamp_terms.size(); ++i) {
            if (stamp_terms[i] != 0) {
                stamp_text += fmt::format(" {} {:.0f}/s", stamp_names[i], rate(stamp_terms[i]));
            }
        }
        if (stamp_text.empty()) {
            stamp_text = " nothing";
        }
        LOG_INFO(HW_GPU,
                 "Fence writer resumes (EXP12={}): writer waits skipped {:.0f}/s (back short of "
                 "the target {:.0f}/s); acquire to resume per route, /s under 100 us/500 us/1 "
                 "ms/5 ms/longer: {}; poll drains of these acquires pending under 2 ms {:.0f}/s "
                 "{:.2f} ms/s, 2 ms or more {:.0f}/s {:.2f} ms/s, of other acquires {:.0f}/s "
                 "{:.2f} ms/s; lagged checks (EXP12=5/6) {:.0f}/s, waited {:.0f}/s {:.2f} ms/s, "
                 "met there {:.0f}/s; word writer waits (EXP12=6) {:.0f}/s (target by an "
                 "unlanded prediction record {:.0f}, the block/page stamp {:.0f}, a queued "
                 "release {:.0f}; stamps off {:.0f}), none queued {:.0f}/s; the full write "
                 "stamp would have waited by:{}; "
                 "pending over 100 ms (compare with EXP12=2 in the same scene): {} fence path, "
                 "{} other; "
                 "reduction bounds of the resumed (EXP12=4) {:.0f}/s checked, "
                 "{:.0f}/s not (latch, front answer, drain or after 1 ms); by channel:{}",
                 VideoCore::Nx2Debug::Experiment(12), rate(skipped), rate(skipped_behind), routes,
                 rate(poll_drains[0]), ms(poll_drain_ns[0]), rate(poll_drains[1]),
                 ms(poll_drain_ns[1]), rate(other_poll_drains), ms(other_poll_drain_ns),
                 rate(lagged_checks), rate(lagged_waits), ms(lagged_wait_ns), rate(lagged_met),
                 rate(word_waits), rate(word_terms[0]), rate(word_terms[1]),
                 rate(word_terms[2]), rate(word_stamps_off), rate(word_none), stamp_text,
                 long_pending[0],
                 long_pending[1], rate(resume_bound_checks), rate(resume_bound_dropped), rows);
        *this = {};
        since = now;
    }
};

// EXP13 (nx2_qmd_prediction.h): whether a QMD or copy release the front predicted, still queued,
// would meet an unmet readable acquire. Returns its device and the back's release count, for
// QmdPredictionMetBy once the fences ran.
struct QmdPredictionCheck {
    u64 device;
    u64 sequence;
    /// EXP13=2: the reduction lower bound computed for it (fable_answer_6 6c).
    std::optional<VideoCommon::QmdPrediction::Bound> bound;
};
std::optional<QmdPredictionCheck> CheckQmdPrediction(DmaPusher& dma_pusher,
                                                     const Puller::PendingAcquire& acquire) {
    namespace Prediction = VideoCommon::QmdPrediction;
    auto& split = VideoCommon::GpuSplit::Get();
    if (!Prediction::Enabled() || !split.Active()) {
        return std::nullopt;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    if (!device) {
        return std::nullopt;
    }
    if (Prediction::MirrorMode()) {
        // The word first, then the back's position: a record past that position had not run
        // when the word was read, so it is not in it (6c section 3, steps 2-3).
        const u32 word = dma_pusher.memory_manager.Read<u32>(acquire.address);
        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 done = split.ConsumedNow();
        const auto bound = Prediction::CheckAcquire({
            .device = *device,
            .address = acquire.address,
            .operation = acquire.operation,
            .value = acquire.value,
            .word = word,
            .done = done,
            .gpu_dirty = Nx2IsGpuWritten(*device),
            .pointer = dma_pusher.memory_manager.GetPointer(acquire.address),
        });
        return QmdPredictionCheck{*device, Prediction::BackSequence(), bound};
    }
    const auto queued = Prediction::QueuedPayloads(*device, split.ConsumedNow());
    const bool meets = std::any_of(queued.payloads.begin(),
                                   queued.payloads.begin() + queued.count,
                                   [&](u32 payload) { return acquire.Matches(payload); });
    Prediction::NoteAcquire(meets              ? Prediction::Coverage::WouldMeet
                            : queued.count != 0 ? Prediction::Coverage::WrongValue
                            : queued.any        ? Prediction::Coverage::AlreadyRan
                                                : Prediction::Coverage::None);
    return QmdPredictionCheck{*device, Prediction::BackSequence(), std::nullopt};
}

void QmdPredictionMetBy(DmaPusher& dma_pusher, const QmdPredictionCheck& check,
                        const Puller::PendingAcquire& acquire, bool met) {
    namespace Prediction = VideoCommon::QmdPrediction;
    using MetBy = Prediction::MetBy;
    if (!met) {
        Prediction::NoteMetBy(MetBy::Unmet);
        return;
    }
    if (check.bound) {
        // Safety: once met, the word must be at least the bound (counted, nothing else).
        Prediction::CheckBound(*check.bound,
                               dma_pusher.memory_manager.Read<u32>(acquire.address));
    }
    auto release = Prediction::BackReleaseSince(check.device, check.sequence);
    // A release the back made before the check that landed after it (a reduction, a fence
    // release) met it too: the newest one the back made for the word.
    bool before_check = false;
    if (!release) {
        release = Prediction::BackReleaseLatest(check.device);
        before_check = release.has_value();
    }
    MetBy met_by = MetBy::Other;
    if (release && release->reduction) {
        met_by = MetBy::QmdReduction;
    } else if (release && acquire.Matches(release->payload)) {
        met_by = release->writer == Prediction::Writer::Qmd ? MetBy::QmdRelease : MetBy::DmaRelease;
    }
    Prediction::NoteMetBy(met_by);
    if (met_by == MetBy::Other) {
        // fable_b 19.4: of these, the releases recorded for the word that meet it, by kind.
        auto& split = VideoCommon::GpuSplit::Get();
        u64 position = 0;
        if (const auto payload = split.NewestReleaseTo(check.device, position);
            payload && acquire.Matches(*payload)) {
            Prediction::NoteMetBy(split.RecordedReleaseKind(check.device) ==
                                          VideoCommon::GpuSplit::ReleaseKind::Puller
                                      ? MetBy::OtherPullerRelease
                                      : MetBy::OtherPayloadRelease);
        }
    }
    if (before_check && met_by != MetBy::Other) {
        Prediction::NoteMetBy(MetBy::BeforeCheck);
    }
}

// "Acquire resumes met by" (fable_front_drains 2.3 items 2-3; FRITCHII_ACQUIRE_MET_BY, live,
// default 0 since 2026-11-14: a page table lookup, two table looks, a noted-write scan and a clock
// read per resume when on):
// for every suspended acquire that resumes, the release that met it as the front's tables tell,
// by kind, and whether its value was known to the front when it sent it; split into resumes by
// the back's latch and by the other routes (word read, front answer, poll drain, lagged check).
// Logged every 5 s. Front thread only.
// Which release: with EXP13 on, the newest the back made for the word since the suspension (the
// QMD copy's table, as QmdPredictionMetBy asks), if it meets the acquire (or is a reduction);
// else the release recorded for the word (GpuSplit::NoteRelease) if its payload meets it; else
// the newest write noted over the word in the last 2^18 ring words; else none the front saw
// (the guest CPU, or a writer the front does not note). Known at send: the payload releases the
// front records (puller, 3D, compute 0x6c3) and the noted writes with known words; QMD and copy
// releases when the QMD copy predicts them (EXP13 2-4, reductions never).
class AcquireMetByStats {
public:
    enum class Kind : u8 {
        PullerRelease,
        Payload3DShort,
        Payload3DLong,
        ComputeReport,
        QmdRelease,
        QmdReduction,
        CopyRelease,
        CounterReport,
        OtherNoted,
        Unseen,
        Count,
    };

    static AcquireMetByStats& Get() {
        static AcquireMetByStats stats;
        return stats;
    }

    void Note(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire) {
        const auto now = std::chrono::steady_clock::now();
        // A resume 5 s after the previous one counted: the switch was off (or nothing resumed),
        // so the window starts over here rather than reporting old counts over the whole time.
        if (since != std::chrono::steady_clock::time_point{} &&
            now - last > std::chrono::seconds{5}) {
            counts = {};
            since = {};
        }
        last = now;
        const bool latch = acquire.resume_route == static_cast<u8>(FenceResumeRoute::Latch);
        bool known = false;
        const Kind kind = Classify(dma_pusher, acquire, known);
        ++counts[latch ? 0 : 1][static_cast<size_t>(kind)][known ? 1 : 0];
        Tick(now);
    }

private:
    static Kind FromRelease(VideoCommon::GpuSplit::ReleaseKind kind) {
        using Release = VideoCommon::GpuSplit::ReleaseKind;
        switch (kind) {
        case Release::Puller:
            return Kind::PullerRelease;
        case Release::Payload3DShort:
            return Kind::Payload3DShort;
        case Release::Payload3DLong:
            return Kind::Payload3DLong;
        case Release::ComputeReport:
            return Kind::ComputeReport;
        default:
            return Kind::OtherNoted;
        }
    }

    static Kind Classify(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire,
                         bool& known) {
        namespace Prediction = VideoCommon::QmdPrediction;
        auto& split = VideoCommon::GpuSplit::Get();
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
        if (!device || !split.Active()) {
            return Kind::Unseen;
        }
        if (Prediction::Enabled() && acquire.back_sequence != ~u64{0}) {
            if (const auto release = Prediction::BackReleaseSince(*device, acquire.back_sequence);
                release && (release->reduction || acquire.Matches(release->payload))) {
                switch (release->writer) {
                case Prediction::Writer::Qmd:
                    known = !release->reduction && Prediction::MirrorMode();
                    return release->reduction ? Kind::QmdReduction : Kind::QmdRelease;
                case Prediction::Writer::Dma:
                    known = Prediction::MirrorMode();
                    return Kind::CopyRelease;
                default:
                    // Puller and report releases: the front had the payload when it sent them.
                    known = true;
                    if (release->writer == Prediction::Writer::Puller) {
                        return Kind::PullerRelease;
                    }
                    return FromRelease(split.RecordedReleaseKind(*device));
                }
            }
        }
        if (u64 position = 0; const auto payload = split.NewestReleaseTo(*device, position)) {
            if (acquire.Matches(*payload)) {
                known = true;
                return FromRelease(split.RecordedReleaseKind(*device));
            }
        }
        constexpr u64 NotedWindow = u64{1} << 18;
        const u64 sent = split.SentPosition();
        const auto noted = split.NewestNotedWriteTo(sent > NotedWindow ? sent - NotedWindow : 0,
                                                    *device);
        if (noted.position == 0) {
            return Kind::Unseen;
        }
        known = noted.words != 0;
        if (noted.what == VideoCommon::NotedCounterReport) {
            return Kind::CounterReport;
        }
        if (noted.what == VideoCommon::NotedPayloadReport) {
            known = true; // The payload was in the front's registers
            return Kind::Payload3DLong;
        }
        if (noted.what == VideoCommon::NotedPayloadRelease) {
            return Kind::Payload3DShort;
        }
        return Kind::OtherNoted;
    }

    void Tick(std::chrono::steady_clock::time_point now) {
        if (since == std::chrono::steady_clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> names{
            "puller release",       "3D payload release (4 bytes)", "3D payload report (16 bytes)",
            "compute 0x6c3 release", "QMD release",                  "QMD reduction",
            "copy semaphore",       "counter or sample report",     "other noted write",
            "not seen by the front"};
        const auto group = [&](size_t g) {
            std::string text;
            for (size_t k = 0; k < names.size(); ++k) {
                const auto& n = counts[g][k];
                if (n[0] + n[1] != 0) {
                    text += fmt::format("{} {} {:.0f}/{:.0f}", text.empty() ? "" : ",", names[k],
                                        static_cast<double>(n[1]) / window,
                                        static_cast<double>(n[0]) / window);
                }
            }
            return text.empty() ? std::string{" none"} : text;
        };
        LOG_INFO(HW_GPU,
                 "Acquire resumes met by (suspended acquires; the release that met each, /s with "
                 "its value known at send/unknown): by the latch:{}; by the other routes:{}",
                 group(0), group(1));
        counts = {};
        since = now;
    }

    /// [0 latch, 1 other routes][Kind][known at send]
    std::array<std::array<std::array<u64, 2>, static_cast<size_t>(Kind::Count)>, 2> counts{};
    std::chrono::steady_clock::time_point since{};
    std::chrono::steady_clock::time_point last{}; ///< The previous resume counted
};

// At a suspension: the QMD copy's back release count, for AcquireMetByStats at the resume.
void NoteBackSequence(Puller::PendingAcquire& acquire) {
    if (VideoCommon::QmdPrediction::Enabled()) {
        acquire.back_sequence = VideoCommon::QmdPrediction::BackSequence();
    }
    if (VideoCommon::OneFollower::g_dry_release.load(std::memory_order_relaxed)) [[unlikely]] {
        acquire.dry_stall = VideoCommon::OneFollower::DryStallNow();
    }
}

// At a suspension, GPU profile on (fable_stage_levelpoints 18.4 row 7): what was sent by now and
// when, for the resume's split (until the back reached that position, then after).
void NotePriceSuspension(Puller::PendingAcquire& acquire) {
    if (VideoCommon::FritchiiPrice::On()) [[unlikely]] {
        acquire.price_target = VideoCommon::GpuSplit::Get().SentPosition();
        acquire.price_start = VideoCommon::FritchiiPrice::Ticks();
    }
}

// EXP12=4: the QMD prediction's "met by" of an acquire the fence path suspended without waiting
// for the writers is noted at its resume (Puller::ResumeAcquire) instead of at once, where
// nearly all would count as unmet (the writers had not run). The reduction bound is checked
// there only when a read of the word met it within 1 ms of the acquire (ResumeAcquire); later,
// or by a latch, the word may already be a later write's. Front thread only.
class DeferredMetBy {
public:
    static DeferredMetBy& Get() {
        static DeferredMetBy table;
        return table;
    }

    void Add(const void* owner, const QmdPredictionCheck& check) {
        std::erase_if(entries, [owner](const auto& entry) { return entry.first == owner; });
        if (entries.size() >= 64) {
            entries.erase(entries.begin()); // A channel that went away while suspended
        }
        entries.emplace_back(owner, check);
    }

    std::optional<QmdPredictionCheck> Take(const void* owner) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->first == owner) {
                const QmdPredictionCheck check = it->second;
                entries.erase(it);
                return check;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<std::pair<const void*, QmdPredictionCheck>> entries;
};

// EXP13=2: a plain release the puller sent, recorded so the reduction bound sees it queued or
// run but not landed (it sets the word).
void RecordPullerRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload) {
    namespace Prediction = VideoCommon::QmdPrediction;
    auto& split = VideoCommon::GpuSplit::Get();
    if (!Prediction::MirrorMode() || !split.Active()) {
        return;
    }
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        Prediction::Note(*device,
                         {.writer = Prediction::Writer::Puller,
                          .kind = Prediction::Kind::Plain,
                          .payload = payload,
                          .op = 0,
                          .is_signed = false},
                         dma_pusher.memory_manager.Read<u32>(address), split.SentPosition(), 0);
    }
}

// EXP13=3 (nx2_qmd_prediction.h): an acquire a queued release or the reduction bound meets goes
// on as one a queued release meets does (MetByQueuedRelease): the back runs the channel's later
// work behind the releases, and it counts as a host-level wait (NoteSync).
// `fed_out` (optional): what met it (FRITCHII_ACQUIRE_TABLE_FIRST's count).
bool MetByQmdPrediction(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire,
                        bool resume,
                        VideoCommon::QmdPrediction::FedRelease* fed_out = nullptr) {
    namespace Prediction = VideoCommon::QmdPrediction;
    auto& split = VideoCommon::GpuSplit::Get();
    if (!Prediction::FeedRequested() || !split.Active()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    if (!device) {
        return false;
    }
    // The word first, then the back's position, then the marks (Feed drains them).
    const u32 word = dma_pusher.memory_manager.Read<u32>(acquire.address);
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 done = split.ConsumedNow();
    Prediction::FedRelease fed{};
    if (!Prediction::Feed(
            {
                .device = *device,
                .address = acquire.address,
                .operation = acquire.operation,
                .value = acquire.value,
                .word = word,
                .done = done,
                .gpu_dirty = Nx2IsGpuWritten(*device),
                .pointer = dma_pusher.memory_manager.GetPointer(acquire.address),
            },
            resume, &fed)) {
        return false;
    }
    if (fed_out != nullptr) {
        *fed_out = fed;
    }
    split.NoteSync();
    // EDEN_NX2_RUNAHEAD: a queued plain release with an exact payload is a boundary too (the
    // untracked writers the drain path can't answer are mostly these QMD releases).
    namespace Runahead = VideoCommon::Runahead;
    if (Runahead::CurrentMode() != Runahead::Mode::Off) [[unlikely]] {
        Runahead::NotePredictedAnswer();
        if (fed.exact && fed.position > done && acquire.Matches(fed.payload)) {
            RunaheadBoundary(dma_pusher, acquire, *device, fed.payload, fed.position, true);
        }
    }
    return true;
}

// FRITCHII_ACQUIRE_TABLE_FIRST (live, default 0; fable_front_drains 2.3 item 2 and 6.3, the
// market's "Acquire resumes met by" line): a suspended acquire's poll asks the front's release
// table first, right after the latch: before the word read (SemaphoreReadableNow, which waits for
// the noted writers of the word when it can't overlay them) and above the unmet-poll throttle
// (16 polls or 1 ms; EXP12=5/6's 1 ms lagged check), which held MetByQueuedRelease back. Every
// poll point the front has (each command list's end, the idle loop) asks, so the first poll after
// another channel sent the release asks too.
// 1: met when the newest release recorded for the word (GpuSplit::NoteRelease) is a puller
//    release or a 3D payload release or report (values known at send), still queued, its payload
//    meets the acquire and nothing sent after it may write the word: MetByQueuedRelease's own
//    rule, which the parse of the acquire and the throttled poll use today. The resumed
//    channel's entries go into the ring after the release, so the back runs the release first.
//    Not with delayed fences (the release lands at a later fence, not when the back runs it),
//    nor a 16-byte report with accurate queries (its operation waits for a fence too).
// 2: also the QMD feed (EXP13=3/4, MetByQmdPrediction) at the same place, before the word read
//    (today: after it, still above the throttle). Its own rules and checks, unchanged: the back's
//    check of every release against its record (the feed turns itself off at the first
//    mismatch) and its audit of met acquires ("acquire wrongly met"). A test mode: a wrong
//    prediction is found only after the resumed channel ran.
// Must be 0, for 1: each resume watches its word with a waiter of its own (Nx2SemaphoreWaiters,
// added before the last check), and once the back ran the release (consumed past it, its async
// report settled) the waiter must have latched: the back wrote a value that meets the acquire.
// If the word met already when the waiter was added (the release landed first) the watch is not
// judged. Front thread only.
class AcquireTableFirst {
public:
    enum class Kind : u8 { Puller, Payload3DShort, Payload3DLong, FeedExact, FeedOther, Count };

    static AcquireTableFirst& Get() {
        static AcquireTableFirst table;
        return table;
    }

    [[nodiscard]] static int Mode() {
        return VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_acquire_table_first,
                                                "FRITCHII_ACQUIRE_TABLE_FIRST");
    }

    /// A poll of `acquire` (after the latch): whether the tables meet it. `first`: no earlier
    /// answer of this suspension (ResumeAcquire asks again; counted and watched once). With 2 the
    /// poll asks the feed again after the word read when this does not meet (the read may have
    /// waited for writers since), so the feed's own gate counters count both asks.
    bool Met(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire, int mode,
             bool first) {
        Settle();
        Tick(mode);
        auto& split = VideoCommon::GpuSplit::Get();
        // INTEL_FORK_DELAYED_SHORTCUTS: with delayed fences too, for the plain releases TableKind
        // takes (puller, 3D payload; the 16-byte one without accurate queries), all known at send.
        const bool delayed = VideoCore::Nx2Debug::DelayedFences();
        if (!split.Active() || (delayed && !DelayedShortcuts::Enabled())) {
            return false;
        }
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
        if (!device) {
            return false;
        }
        u64 position = 0;
        const std::optional<u32> newest = split.NewestReleaseTo(*device, position);
        const Kind kind = TableKind(split.RecordedReleaseKind(*device));
        if (newest && kind != Kind::Count && position > split.ConsumedNow() &&
            acquire.Matches(*newest)) {
            // The watch first: a write the back makes after this sees its waiter. Not with
            // delayed fences: the write lands at a later fence, so the watch's judgement (latched
            // once the back ran the release) does not hold; counted as unwatched.
            const s32 slot =
                first && !delayed ? Watch(dma_pusher, acquire, *device, *newest, position) : -1;
            if (MetByQueuedRelease(dma_pusher, acquire)) {
                if (first) {
                    ++resumed[static_cast<size_t>(kind)];
                    if (slot < 0) {
                        ++unwatched;
                    }
                }
                return true;
            }
            Unwatch(slot);
        }
        // Not the QMD feed with delayed fences (INTEL_FORK_DELAYED_SHORTCUTS covers plain
        // releases only): asked as before at the parse and the polls.
        if (mode >= 2 && !delayed) {
            VideoCommon::QmdPrediction::FedRelease fed{};
            if (MetByQmdPrediction(dma_pusher, acquire, true, &fed)) {
                if (first) {
                    ++resumed[static_cast<size_t>(fed.exact ? Kind::FeedExact : Kind::FeedOther)];
                }
                return true;
            }
        }
        return false;
    }

    /// The switch went to 0: the watches' waiters go.
    void Clear() {
        if (active == 0) {
            return;
        }
        for (s32 i = 0; i < static_cast<s32>(watches.size()); ++i) {
            if (watches[i].active) {
                Unwatch(i);
            }
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    struct WatchEntry {
        bool active;
        bool landed_before; ///< The word met when the waiter was added: not judged
        u32 operation;
        u32 value;
        u32 payload;
        u64 device;
        u64 position;
        const u8* pointer;
        Clock::time_point since;
    };

    static Kind TableKind(VideoCommon::GpuSplit::ReleaseKind kind) {
        using Release = VideoCommon::GpuSplit::ReleaseKind;
        switch (kind) {
        case Release::Puller:
            return Kind::Puller;
        case Release::Payload3DShort:
            return Kind::Payload3DShort;
        case Release::Payload3DLong:
            // Not a fence: with accurate queries its operation runs at a later fence.
            return VideoCore::Nx2Debug::AccuratePart(1) ? Kind::Count : Kind::Payload3DLong;
        default:
            return Kind::Count;
        }
    }

    static u32 WordAt(const u8* pointer) {
        return std::atomic_ref<u32>{*reinterpret_cast<u32*>(const_cast<u8*>(pointer))}.load(
            std::memory_order_acquire);
    }

    /// A free watch for the release at `position` (-1: none free, or the word has no host
    /// memory).
    s32 Watch(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire, u64 device,
              u32 payload, u64 position) {
        using Waiters = Tegra::Nx2SemaphoreWaiters;
        const u8* const pointer = dma_pusher.memory_manager.GetPointer(acquire.address);
        if (pointer == nullptr || (reinterpret_cast<uintptr_t>(pointer) & 3) != 0 ||
            (acquire.operation != Waiters::AcquireEqual &&
             acquire.operation != Waiters::AcquireGequal &&
             acquire.operation != Waiters::AcquireMask)) {
            return -1;
        }
        for (s32 i = 0; i < static_cast<s32>(watches.size()); ++i) {
            WatchEntry& entry = watches[i];
            if (entry.active) {
                continue;
            }
            Tegra::Nx2SemaphoreWaiters::Get().Add(&tokens[i], device, acquire.value,
                                                  acquire.operation);
            // After the waiter: a release that landed before it shows in the word.
            entry = {.active = true,
                     .landed_before = acquire.Matches(WordAt(pointer)),
                     .operation = acquire.operation,
                     .value = acquire.value,
                     .payload = payload,
                     .device = device,
                     .position = position,
                     .pointer = pointer,
                     .since = Clock::now()};
            ++active;
            return i;
        }
        return -1;
    }

    void Unwatch(s32 slot) {
        if (slot < 0 || !watches[slot].active) {
            return;
        }
        Tegra::Nx2SemaphoreWaiters::Get().Remove(&tokens[slot]);
        watches[slot].active = false;
        --active;
    }

    /// Judges the watches whose release the back ran.
    void Settle() {
        if (active == 0) {
            return;
        }
        auto& split = VideoCommon::GpuSplit::Get();
        const u64 done = split.ConsumedNow();
        const auto now = Clock::now();
        for (s32 i = 0; i < static_cast<s32>(watches.size()); ++i) {
            WatchEntry& entry = watches[i];
            if (!entry.active) {
                continue;
            }
            if (entry.position > done || !Tegra::Nx2AsyncReportsSettled(entry.device, 4)) {
                if (now - entry.since > std::chrono::seconds{2}) {
                    ++unresolved;
                    Unwatch(i);
                }
                continue;
            }
            const bool latched = Tegra::Nx2SemaphoreWaiters::Get().Latched(&tokens[i]);
            if (latched) {
                ++met_at_write;
            } else if (entry.landed_before) {
                ++landed_before;
            } else {
                ++not_met;
                ++session_not_met;
                if (session_not_met <= 8) {
                    LOG_WARNING(HW_GPU,
                                "Acquire table first (must be 0): device {:#x} op {} value {:#x}: "
                                "the release it was resumed on (payload {:#x}, ring {}) ran, and "
                                "no write that met it was seen; the word is {:#x} now",
                                entry.device, entry.operation, entry.value, entry.payload,
                                entry.position, WordAt(entry.pointer));
                }
            }
            Unwatch(i);
        }
    }

    void Tick(int mode) {
        const auto now = Clock::now();
        if (since == Clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        const auto rate = [window](u64 count) { return static_cast<double>(count) / window; };
        const auto kind = [&](Kind k) { return rate(resumed[static_cast<size_t>(k)]); };
        LOG_INFO(HW_GPU,
                 "Acquire table first (FRITCHII_ACQUIRE_TABLE_FIRST={}) {:.0f} s: resumed, /s: "
                 "puller release {:.0f}, 3D payload release (4 bytes) {:.0f}, 3D payload report "
                 "(16 bytes) {:.0f}, QMD feed exact {:.0f}, QMD feed other {:.0f}; watches: met "
                 "at the write {}, landed before the watch {}, unwatched {}, unresolved after 2 s "
                 "{}, open {}; not met at the write (must be 0) {} ({} this session)",
                 mode, window, kind(Kind::Puller), kind(Kind::Payload3DShort),
                 kind(Kind::Payload3DLong), kind(Kind::FeedExact), kind(Kind::FeedOther),
                 met_at_write, landed_before, unwatched, unresolved, active, not_met,
                 session_not_met);
        resumed = {};
        met_at_write = 0;
        landed_before = 0;
        unwatched = 0;
        unresolved = 0;
        not_met = 0;
        since = now;
    }

    std::array<WatchEntry, 16> watches{};
    /// The watches' waiter owners (one address each).
    std::array<u8, 16> tokens{};
    u32 active{};
    std::array<u64, static_cast<size_t>(Kind::Count)> resumed{};
    u64 met_at_write{};
    u64 landed_before{};
    u64 unwatched{};
    u64 unresolved{};
    u64 not_met{};
    u64 session_not_met{};
    Clock::time_point since{};
};

} // Anonymous namespace

// `read_done` (optional): the back's position loaded before guest memory was read for `word`.
static bool SemaphoreReadableNow(DmaPusher& dma_pusher, GPUVAddr address, u32& word,
                                 u64* read_done = nullptr);

// EDEN_NX2_RUNAHEAD (nx2_runahead.h), for an acquire ReleaseFencesForReadable would drain for:
// counts what it waits for, and answers it met without the drain when the newest release the
// front sent to the word is still queued, meets it, and nothing sent after it may write the word
// (GpuSplit::QueuedGuestWriters with that release left out, no compute or copy range over it,
// and no native dispatch sent since that the back has not seen miss it). MetByQueuedRelease
// refused it only because some work was sent after the release. Within EDEN_NX2_RUNAHEAD_MS.
// Mode 1 then also writes the word (RunaheadBoundary), 3 only answers, 2 takes a verify chunk
// and lets the drain run. The fences are released in order on the back, as EXP12 does.
static bool RunaheadAnswer(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire,
                           u64 device, VideoCommon::Runahead::Mode mode) {
    namespace Runahead = VideoCommon::Runahead;
    auto& split = VideoCommon::GpuSplit::Get();
    const u64 done = split.ConsumedNow();
    u64 position = 0;
    const std::optional<u32> release = split.NewestReleaseTo(device, position);
    if (!release) {
        // A native compute launch queued: its QMD completion release or reduction (the front
        // knows those only with EXP13's QMD copy). Otherwise copies, unranged writers, maps.
        Runahead::CountUnmet(split.UntrackedWriterPosition(device) <= done
                                 ? Runahead::Unmet::Nothing
                             : split.LastDispatchPosition() > done
                                 ? Runahead::Unmet::UntrackedDispatch
                                 : Runahead::Unmet::UntrackedWriter);
        return false;
    }
    if (!acquire.Matches(*release)) {
        Runahead::CountUnmet(Runahead::Unmet::WrongPayload);
        return false;
    }
    if (position <= done) {
        Runahead::CountUnmet(Runahead::Unmet::ReleaseRan);
        return false;
    }
    Runahead::CountUnmet(Runahead::Unmet::QueuedLaterWriter);
    const std::array<u64, 1> skip{position};
    const auto writers = split.QueuedGuestWriters(done, acquire.address, device, sizeof(u32), skip);
    if (writers.noted > position || writers.unbounded > position ||
        split.WritersMayWriteSince(position, device, sizeof(u32))) {
        return false;
    }
    // QueuedGuestWriters leaves native compute launches out (their QMD releases are EXP13's).
    if (split.LastDispatchPosition() > position) {
        u64 checked = 0;
        bool waited = false;
        if (split.DispatchesMayWriteSince(position, device, sizeof(u32), false, checked, waited) !=
            VideoCommon::GpuSplit::DispatchCheck::Miss) {
            return false;
        }
    }
    if (!Runahead::WithinBound(done)) {
        return false;
    }
    if (mode == Runahead::Mode::Verify) {
        RunaheadBoundary(dma_pusher, acquire, device, *release, position);
        return false;
    }
    if (mode == Runahead::Mode::On) {
        RunaheadBoundary(dma_pusher, acquire, device, *release, position);
    }
    // Memory stage tag Cache: the fence manager only; no 3D state.
    split.Defer([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
                VideoCommon::GpuSplit::Effect::Unknown,
                VideoCommon::Nx2FastDrawCount::Neutral() |
                    VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
    split.NoteMemoryWrite();
    // A host-level wait for the commands after it, as the drain was.
    split.NoteSync();
    split.Publish();
    Runahead::NoteEarlyAnswer(split.SentPosition(), true);
    return true;
}

// The fence release of an acquire whose word SemaphoreReadableNow read current but unmet: the
// drain only matters for writes that read can't account for. Returns whether the acquire is met.
// EDEN_NX2_EXP12 (live, 0 = off): the back first runs only the queued work that may write the
// word without the front knowing its words (GpuSplit::UntrackedWriterPosition: compute and
// copies, with their QMD and report releases, unranged writers, a release recorded for the word,
// queued functions), the fence release queued behind everything as before. 1 = met there: go
// on, the fences released on the back in order; still unmet: the full drain as before.
// 2 = unmet there too suspends without the full drain (the channel polls as usual). 3 = the full
// drain always, timed past that point (measures what 1 saves). 4 = as 2 without the wait for
// those writers: the fence release is queued, the word read once more, and unmet suspends at
// once. Exact for the reason 2 is: the waiter was registered before the fence release was
// queued, the back latches it at any release that meets the value (OnWrite), and a suspended
// channel resumes only on that latch or on a met read of the word (CanResumeAcquire), the same
// polls 2's unmet acquires go through after their wait. The wait only made the front look
// later. 5 = 4 here; its polls check lagged instead of draining (CanResumeAcquire). 6 = 5, but
// waits here when something queued can write the word itself (fable_fence_writers_2 section 4):
// to the newest of the QMD prediction's unlanded record for the word, the write stamp over it
// and a queued puller release to it (mode 2's target while the write stamps are off); with none
// past the back's position it suspends at once as 4. Exact as 4: it only chooses when to look.
// `skipped_wait` (optional): set when 4, 5 or 6 skipped the wait. Not with delayed fences, whose
// releases land when the host GPU passed them, not when the back runs them.
static bool ReleaseFencesForReadable(DmaPusher& dma_pusher, const void* owner,
                                     const Puller::PendingAcquire& acquire,
                                     bool* skipped_wait = nullptr) {
    using Clock = FenceWriterStats::Clock;
    auto& stats = FenceWriterStats::Get();
    const auto met = [&] {
        u32 word{};
        return Nx2SemaphoreWaiters::Get().Latched(owner) ||
               (SemaphoreReadableNow(dma_pusher, acquire.address, word) && acquire.Matches(word));
    };
    ++stats.acquires;
    const int mode = VideoCore::Nx2Debug::Experiment(12);
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    auto& split = VideoCommon::GpuSplit::Get();
    if (const auto runahead = VideoCommon::Runahead::CurrentMode();
        runahead != VideoCommon::Runahead::Mode::Off && device && split.Active()) [[unlikely]] {
        if (RunaheadAnswer(dma_pusher, acquire, *device, runahead)) {
            stats.Tick();
            return true;
        }
    }
    // INTEL_FORK_DELAYED_SHORTCUTS: with delayed fences, a word whose newest release is plain
    // takes the EXP12 path below; its fence release is a fence without a host GPU wait.
    const bool delayed = VideoCore::Nx2Debug::DelayedFences();
    const bool delayed_shortcut = delayed && device && DelayedShortcuts::Enabled() &&
                                  DelayedShortcuts::PlainRelease(*device);
    if (mode <= 0 || mode > 6 || !device || !split.Active() || (delayed && !delayed_shortcut)) {
        if (delayed && DelayedShortcuts::Enabled() && mode > 0 && mode <= 6 && device &&
            split.Active()) {
            ++DelayedShortcuts::g_stats.kept;
            DelayedShortcuts::Tick();
        }
        stats.delayed += mode != 0 ? 1 : 0;
        const auto begin = Clock::now();
        ReleaseFences(dma_pusher);
        const bool result = met();
        ++stats.full;
        const u64 full_ns = FenceWriterStats::Nanoseconds(begin, Clock::now());
        stats.full_ns += full_ns;
        Common::Nx2Hitch::Add(Common::Nx2Hitch::Cause::FullDrain, full_ns);
        stats.full_met += result ? 1 : 0;
        stats.Tick();
        return result;
    }
    // Before the fence release is queued, which would count as a writer itself.
    u64 target = 0;
    bool word_writer = true;
    if (mode == 6) {
        if (split.WriteStampsOn()) {
            const u64 done = split.ConsumedNow();
            const u64 predicted = VideoCommon::QmdPrediction::Enabled()
                                      ? VideoCommon::QmdPrediction::NewestUnlandedPosition(*device)
                                      : 0;
            // The block and page stamps alone: WriteStamp's floors (writers of anything, 3D
            // method writes, ...) would make this wait as often as 2. A wait skipped is never
            // a wrong answer.
            const u64 stamp = split.BytesStamp(*device, sizeof(u32), acquire.address);
            u64 release = 0;
            (void)split.QueuedReleaseTo(*device, done, release);
            target = (std::max)({predicted, stamp, release});
            word_writer = target > done;
            if (word_writer) {
                ++stats.word_terms[target == predicted ? 0 : target == stamp ? 1 : 2];
            }
            // Counting only: the term the full WriteStamp would have waited for, real writers
            // (block, page) against floors.
            if (const auto why = split.WriteStampWhy(*device, sizeof(u32), acquire.address);
                why.stamp > done) {
                ++stats.stamp_terms[(std::min)(static_cast<size_t>(why.term),
                                               stats.stamp_terms.size() - 1)];
            }
        } else {
            ++stats.word_stamps_off;
            target = split.UntrackedWriterPosition(*device);
        }
        stats.word_waits += word_writer ? 1 : 0;
        stats.word_none += word_writer ? 0 : 1;
    } else {
        target = split.UntrackedWriterPosition(*device);
    }
    if (delayed_shortcut) {
        // A forced release would wait for the host GPU (WaitPendingFences(true) waits for its
        // fence's callback, which delayed fences run once the host GPU passed it).
        DelayedShortcuts::SignalFenceNoWait(dma_pusher);
        ++DelayedShortcuts::g_stats.avoided;
        DelayedShortcuts::Tick();
    } else {
        // Memory stage tag Cache: the fence manager only; no 3D state.
        split.Defer([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
                    VideoCommon::GpuSplit::Effect::Unknown,
                    VideoCommon::Nx2FastDrawCount::Neutral() |
                        VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        split.NoteMemoryWrite();
    }
    const auto begin = Clock::now();
    const bool no_wait = mode == 4 || mode == 5 || (mode == 6 && !word_writer);
    if (no_wait) {
        // No wait: the back runs the writers and the fence release on its own (Publish below).
        ++stats.skipped;
        stats.skipped_behind += split.Ran(target) ? 0 : 1;
        if (skipped_wait) {
            *skipped_wait = true;
        }
    } else if (split.WaitUntil(target, mode == 6 ? VideoCommon::DrainSite::FenceWordWriters
                                                 : VideoCommon::DrainSite::FenceWriters)) {
        ++stats.waits;
    }
    const auto reached = Clock::now();
    stats.wait_ns += no_wait ? 0 : FenceWriterStats::Nanoseconds(begin, reached);
    const bool met_there = met();
    stats.met += met_there ? 1 : 0;
    if (mode == 3 || (mode == 1 && !met_there)) {
        split.Drain(VideoCommon::DrainSite::FenceRelease);
        const u64 ns = FenceWriterStats::Nanoseconds(reached, Clock::now());
        const bool result = met();
        ++stats.full;
        stats.full_ns += ns;
        Common::Nx2Hitch::Add(Common::Nx2Hitch::Cause::FullDrain, ns);
        stats.full_met += result ? 1 : 0;
        stats.saved_ns += mode == 3 && met_there ? ns : 0;
        stats.Tick();
        return result;
    }
    // A host-level wait for the commands after it, as the drain was; the fences go out at once.
    split.NoteSync();
    split.Publish();
    stats.suspended += met_there ? 0 : 1;
    stats.Tick();
    return met_there;
}

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

namespace LaggedDefer {
namespace {
// FRITCHII_LAGGED_DEFER (front only): the lagged checks put off since the last Clear, and the
// counters of the 10 s line.
struct State {
    bool pending{};
    u64 target{~u64{0}};
    /// One of them waits for R's position (DrainSite::LaggedCheck), the stricter of the two.
    bool render{};
    u64 polls{};       ///< Polls that put their check off (the back short of its target)
    u64 acquires{};    ///< Acquires that put a check off at least once
    u64 ran_after{};   ///< Checks that ran after being put off, the back there by then
    u64 sleep_waits{}; ///< Waits made where the front would sleep
    u64 sleep_wait_ns{};
    u64 logged_ns{};
    std::array<u64, 5> before{};
};
State g_defer;

void MaybeLog() {
    auto& d = g_defer;
    const u64 now = Common::Nx2Hitch::NowNs();
    if (d.logged_ns == 0) {
        d.logged_ns = now;
        return;
    }
    if (now - d.logged_ns < 10'000'000'000ULL) {
        return;
    }
    const double seconds = static_cast<double>(now - d.logged_ns) / 1e9;
    const std::array<u64, 5> current{d.polls, d.acquires, d.ran_after, d.sleep_waits,
                                     d.sleep_wait_ns};
    const auto rate = [&](size_t i) {
        return static_cast<double>(current[i] - d.before[i]) / seconds;
    };
    LOG_INFO(HW_GPU,
             "Lagged check defer (FRITCHII_LAGGED_DEFER={}) 10 s, per second: polls that put "
             "their check off {:.0f} ({:.0f} acquires), checks run later with the back there "
             "(no wait) {:.0f}, waits where the front would sleep {:.0f} ({:.1f} ms/s)",
             VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_lagged_defer,
                                              "FRITCHII_LAGGED_DEFER"),
             rate(0), rate(1), rate(2), rate(3), rate(4) / 1e6);
    d.before = current;
    d.logged_ns = now;
}

void Note(u64 target, bool render, bool first) {
    auto& d = g_defer;
    d.pending = true;
    d.target = (std::min)(d.target, target);
    d.render |= render;
    ++d.polls;
    d.acquires += first ? 1 : 0;
    MaybeLog();
}
} // Anonymous namespace

void Clear() {
    g_defer.pending = false;
    g_defer.target = ~u64{0};
    g_defer.render = false;
}

bool Pending() {
    return g_defer.pending;
}

void WaitBeforeSleep() {
    auto& d = g_defer;
    const u64 target = d.target;
    const bool render = d.render;
    Clear();
    if (target == ~u64{0}) {
        return;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    const VideoCommon::OneFollower::DryStallScope dry_stall;
    const u64 begin = Common::Nx2Hitch::NowNs();
    // The site of the checks put off: R's position when any of them reads it (the later one).
    if (split.WaitUntil(target, render ? VideoCommon::DrainSite::LaggedCheck
                                       : VideoCommon::DrainSite::LaggedCheckEngine)) {
        ++d.sleep_waits;
        d.sleep_wait_ns += Common::Nx2Hitch::NowNs() - begin;
    }
    MaybeLog();
}
} // namespace LaggedDefer

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    VideoCommon::FrontSplit::AcquireCheck(); // FRITCHII_FRONT_SPLIT_COUNT (as the first check)
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePolls, 1);
    // Fast path (EDEN_NX2_SEM_FAST), as for the first check in ProcessSemaphoreTriggerMethod:
    // when the word is current without draining the back, answer from it. Ocarina of Time polls
    // each waiting acquire about 12 times before it is met, and every poll drained the back
    // (about 380 ms/s of the front's time). The drained path below runs now and then for an
    // unmet word, and reports an acquire not met for 2 s.
    // `met_by` notes the route for EXP12's resume histogram (FenceWriterStats::NoteResume). Only
    // the first: ResumePending asks first, then ResumeAcquire asks again, often answered by the
    // latch or the word the first answer's drain left behind.
    const auto met_by = [this](FenceResumeRoute route) {
        if (pending_acquire->resume_route == PendingAcquire::RouteUnset) {
            pending_acquire->resume_route = static_cast<u8>(route);
        }
        return true;
    };
    // The 2 s report of an acquire not met (`current`: the word as last read).
    const auto report_if_stuck = [&](u32 current) {
        if (pending_acquire->reported ||
            std::chrono::steady_clock::now() - pending_acquire->since <= std::chrono::seconds{2}) {
            return;
        }
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for 2 s; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current, device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
    };
    // Acquires pending 100 ms, counted once each (FenceWriterStats).
    if (!pending_acquire->long_pending &&
        std::chrono::steady_clock::now() - pending_acquire->since >
            std::chrono::milliseconds{100}) {
        pending_acquire->long_pending = true;
        ++FenceWriterStats::Get().long_pending[pending_acquire->readable_release ? 0 : 1];
    }
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        // The back ran a release that met the value and latched this waiter: resume at once.
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePollsFast, 1);
        return met_by(FenceResumeRoute::Latch);
    }
    // FRITCHII_ACQUIRE_TABLE_FIRST (live, 0 = off; AcquireTableFirst above): the release table,
    // with 2 the QMD feed too, before the word read and above the unmet-poll throttle.
    if (const int table_first = AcquireTableFirst::Mode(); table_first != 0) {
        if (AcquireTableFirst::Get().Met(
                dma_pusher, *pending_acquire, table_first,
                pending_acquire->resume_route == PendingAcquire::RouteUnset)) {
            return met_by(FenceResumeRoute::Answer);
        }
    } else {
        AcquireTableFirst::Get().Clear();
    }
    // EXP13=3: asked once per poll, before the unmet-poll throttle most polls end at.
    bool predicted = false;
    std::optional<ReadableRead> readable;
    u64 read_done = 0;
    if (u32 word{}; SemaphoreReadableNow(dma_pusher, pending_acquire->address, word, &read_done)) {
        if (pending_acquire->Matches(word)) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePollsFast, 1);
            return met_by(FenceResumeRoute::Word);
        }
        readable = ReadableRead{.word = word, .done = read_done};
        // An unmet word is answered without a drain for at most 16 polls or 1 ms since the back
        // was last drained for this acquire. A release the back holds as deferred work lands only
        // when the back runs, so a channel polled without ever draining again waited forever
        // (Mario Kart World hung after 63 s once the 2 s report had been made).
        predicted = true;
        if (MetByQmdPrediction(dma_pusher, *pending_acquire, true)) {
            return met_by(FenceResumeRoute::Answer);
        }
        const auto now = std::chrono::steady_clock::now();
        // EXP12=5/6 (live), with EDEN_NX2_GPU_WAKE on and default fences: no full drain here.
        // Every 1 ms (alone: with 16 polls the first check comes before the back reached the
        // acquire) a lagged check publishes what was sent, waits only for what was sent by the
        // previous check (by the suspension for the first), and asks again without
        // ReadSemaphoreWord (the back is running). Exact: the drain added two tests, the latch
        // (asked again) and ReadSemaphoreWord, which differs from this read only when the buffer
        // cache holds GPU-written bytes of the word; those mark its page (never cleared) and a
        // marked page makes SemaphoreReadableNow false, so that poll takes the drain below. No
        // hang: everything sent is published, and what was sent by one check has run by the
        // next, where it latched, shows in the word, or marked the page. Without the wake an
        // idle front sleeps 1 ms at a time, which the drain hid: then the drain as for 4.
        // INTEL_FORK_DELAYED_SHORTCUTS: with delayed fences too for a word whose newest release
        // is plain; an unmet check then signals a fence without a wait, so the releases sent
        // land (and latch) without a later fence the guest may never send.
        const bool lagged_delayed = VideoCore::Nx2Debug::DelayedFences();
        if (const int fence_mode = VideoCore::Nx2Debug::Experiment(12);
            (fence_mode == 5 || fence_mode == 6) && VideoCore::Nx2Debug::GpuWake() &&
            (!lagged_delayed ||
             DelayedShortcuts::Allowed(dma_pusher, pending_acquire->address)) &&
            VideoCommon::GpuSplit::Get().Active()) {
            if (now - pending_acquire->last_drain < std::chrono::milliseconds{1}) {
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePollsFast,
                                             1);
                return false;
            }
            auto& split = VideoCommon::GpuSplit::Get();
            auto& stats = FenceWriterStats::Get();
            // ResumeAcquire asks again right after a met answer: not a check of its own.
            const bool repeat = pending_acquire->resume_route != PendingAcquire::RouteUnset;
            // FRITCHII_ONEF_SEM_E (fable_b 19.4): payload releases are written on E (or lent,
            // E parked until R ran them): a word whose newest release is one waits for E's
            // position.
            VideoCommon::DrainSite lagged_site = VideoCommon::DrainSite::LaggedCheck;
            if (VideoCommon::OneFollower::g_sem_e.load(std::memory_order_relaxed)) [[unlikely]] {
                if (const auto device =
                        dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
                    device && split.RecordedReleaseKind(*device) !=
                                  VideoCommon::GpuSplit::ReleaseKind::Other) {
                    lagged_site = VideoCommon::DrainSite::LaggedCheckEngine;
                }
            }
            // FRITCHII_LAGGED_DEFER (live, default 0): while the back is short of the target,
            // the check (and its wait) is put off instead of blocking the front; the front
            // goes on with other channels and new command lists, and waits only where it would
            // otherwise sleep (LaggedDefer::WaitBeforeSleep, gpu_thread.cpp). The check itself
            // is unchanged and runs at the first poll that finds the back there, so the same
            // checks decide the resume; only how other channels' work interleaves with it
            // changes (it already moved with the 1 ms timer). No hang: everything sent is
            // published here, and the front waits for the target before any sleep.
            if (!repeat && VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_lagged_defer,
                                                            "FRITCHII_LAGGED_DEFER") != 0) {
                if (!split.ReachedSite(pending_acquire->lagged_target, lagged_site)) {
                    split.Publish();
                    LaggedDefer::Note(pending_acquire->lagged_target,
                                      lagged_site == VideoCommon::DrainSite::LaggedCheck,
                                      !pending_acquire->lagged_deferred);
                    pending_acquire->lagged_deferred = true;
                    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePollsFast,
                                                 1);
                    return false;
                }
                if (pending_acquire->lagged_deferred) {
                    pending_acquire->lagged_deferred = false;
                    ++LaggedDefer::g_defer.ran_after;
                }
            }
            stats.lagged_checks += repeat ? 0 : 1;
            split.Publish();
            const auto wait_begin = std::chrono::steady_clock::now();
            const VideoCommon::OneFollower::DryStallScope dry_stall;
            if (split.WaitUntil(pending_acquire->lagged_target, lagged_site)) {
                ++stats.lagged_waits;
                stats.lagged_wait_ns += FenceWriterStats::Nanoseconds(
                    wait_begin, std::chrono::steady_clock::now());
            }
            const auto lagged_met = [&] {
                stats.lagged_met += repeat ? 0 : 1;
                return met_by(FenceResumeRoute::Lagged);
            };
            if (Nx2SemaphoreWaiters::Get().Latched(this)) {
                return lagged_met();
            }
            std::optional<ReadableRead> reread;
            u32 again = word;
            if (u64 again_done = 0; SemaphoreReadableNow(dma_pusher, pending_acquire->address,
                                                         again, &again_done)) {
                if (pending_acquire->Matches(again)) {
                    return lagged_met();
                }
                reread = ReadableRead{.word = again, .done = again_done};
            }
            if (MetByQueuedRelease(dma_pusher, *pending_acquire) ||
                MetByQmdPrediction(dma_pusher, *pending_acquire, true) ||
                (static_cast<GpuSemaphoreOperation>(pending_acquire->operation) ==
                     GpuSemaphoreOperation::AcquireGequal &&
                 GequalMetWithoutDrain(dma_pusher, pending_acquire->address,
                                       pending_acquire->value, reread))) {
                return lagged_met();
            }
            if (lagged_delayed) {
                // INTEL_FORK_DELAYED_SHORTCUTS: what was sent lands at a fence, not when the back
                // runs it; this one (no host GPU wait) is among what the next check waits for.
                DelayedShortcuts::SignalFenceNoWait(dma_pusher);
                split.Publish();
            }
            // Unmet: the next check waits for what was sent up to now. Only here, so that
            // ResumeAcquire's ask after a met answer is not held off by the 1 ms.
            pending_acquire->lagged_target = split.SentPosition();
            pending_acquire->last_drain = std::chrono::steady_clock::now();
            report_if_stuck(again);
            return false;
        }
        if (++pending_acquire->unmet_polls % 16 != 0 &&
            now - pending_acquire->last_drain < std::chrono::milliseconds{1}) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquirePollsFast, 1);
            return false;
        }
    }
    if (MetByQueuedRelease(dma_pusher, *pending_acquire)) {
        return met_by(FenceResumeRoute::Answer);
    }
    if (!predicted && MetByQmdPrediction(dma_pusher, *pending_acquire, true)) {
        return met_by(FenceResumeRoute::Answer);
    }
    if (static_cast<GpuSemaphoreOperation>(pending_acquire->operation) ==
            GpuSemaphoreOperation::AcquireGequal &&
        GequalMetWithoutDrain(dma_pusher, pending_acquire->address, pending_acquire->value,
                              readable)) {
        return met_by(FenceResumeRoute::Answer);
    }
    // EDEN_NX2_ACQUIRE_LEARN: resume when a release met the waiter (GPU releases notify
    // Nx2SemaphoreWaiters) or the word in memory does (CPU releases); drain only once the channel
    // has waited 2 ms. Every command list pushed retried this with a drain (~5 ms a frame).
    if (AcquireLearning::Enabled() &&
        std::chrono::steady_clock::now() - pending_acquire->since < std::chrono::milliseconds{2}) {
        if (Nx2SemaphoreWaiters::Get().Latched(this)) {
            VideoCommon::GpuSplit::Get().NoteSync();
            return met_by(FenceResumeRoute::Latch);
        }
        if (pending_acquire->Matches(dma_pusher.memory_manager.Read<u32>(pending_acquire->address))) {
            VideoCommon::GpuSplit::Get().NoteSync();
            return met_by(FenceResumeRoute::Word);
        }
        return false;
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    // FRITCHII_DELAY_POLL_DRAIN_US (live, 0 = off): the delay test of this drain, outside the
    // poll drain's own time below.
    using DelayTest = VideoCommon::GpuSplit::DelayTest;
    const u32 delay_us = VideoCommon::GpuSplit::Get().DelayBeforeWait(DelayTest::PollDrain);
    const auto drain_begin = std::chrono::steady_clock::now();
    DrainBack();
    pending_acquire->last_drain = std::chrono::steady_clock::now();
    VideoCommon::GpuSplit::DelayAfterWait(DelayTest::PollDrain, delay_us);
    FenceWriterStats::Get().NotePollDrain(
        pending_acquire->readable_release,
        drain_begin - pending_acquire->since < std::chrono::milliseconds{2},
        FenceWriterStats::Nanoseconds(drain_begin, pending_acquire->last_drain));
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (static std::atomic<u32> rescued{};
            !pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, current);
        }
        return met_by(FenceResumeRoute::Drain);
    }
    if (pending_acquire->Matches(current)) {
        return met_by(FenceResumeRoute::Drain);
    }
    // With delayed fence operations (Balanced or Accurate fences, accurate GPU), a release
    // queued after the acquire released the fences waits in the caches for the next fence. With
    // every channel waiting none comes: release them again now and then while waiting.
    // The same predicate as the fence manager's (FenceManager::DelayFences): the settings alone
    // missed EDEN_NX2_ACCURATE_PARTS=16 (delayed fences, never released again here: a hang).
    const bool delayed = VideoCore::Nx2Debug::DelayedFences();
    if (const auto now = std::chrono::steady_clock::now();
        delayed && now - pending_acquire->last_fences >= std::chrono::milliseconds{1}) {
        pending_acquire->last_fences = now;
        if (DelayedShortcuts::Allowed(dma_pusher, pending_acquire->address)) {
            // INTEL_FORK_DELAYED_SHORTCUTS: a fence without the host GPU wait; the release lands
            // on the fencing thread and latches the waiter, which a later poll finds.
            DelayedShortcuts::SignalFenceNoWait(dma_pusher);
            VideoCommon::GpuSplit::Get().Publish();
            ++DelayedShortcuts::g_stats.avoided;
            DelayedShortcuts::Tick();
            report_if_stuck(current);
            return false;
        }
        ReleaseFences(dma_pusher);
        if (Nx2SemaphoreWaiters::Get().Latched(this) ||
            pending_acquire->Matches(ReadSemaphoreWord(dma_pusher, pending_acquire->address))) {
            return met_by(FenceResumeRoute::Drain);
        }
    }
    report_if_stuck(current);
    return false;
}

namespace {
// EDEN_NX2_ACQUIRE_LOG=1 (read once; Fable heavypath 3): every acquire resumed from a suspension,
// grouped by semaphore address: resumes and total wait per second, waits of 1 ms and more, the
// operation and value waited for, whether its fence release was readable. Logged every 5 s, the
// 8 addresses with the most wait first ("Acquires resumed (5 s").
struct AcquireLogEntry {
    u64 count{};
    u64 wait_ns{};
    u64 long_count{};
    u64 long_ns{};
    u64 readable{};
    u32 operation{};
    u32 value{};
    u64 device{};
};
struct AcquireLog {
    std::mutex mutex;
    std::unordered_map<GPUVAddr, AcquireLogEntry> entries;
    std::chrono::steady_clock::time_point since{};
};
bool AcquireLogEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_LOG");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}
void NoteAcquireResumed(GPUVAddr address, u32 operation, u32 value, u64 device, u64 ns,
                        bool readable) {
    static AcquireLog log;
    std::scoped_lock lock{log.mutex};
    AcquireLogEntry& entry = log.entries[address];
    ++entry.count;
    entry.wait_ns += ns;
    if (ns >= 1'000'000) {
        ++entry.long_count;
        entry.long_ns += ns;
    }
    entry.readable += readable ? 1 : 0;
    entry.operation = operation;
    entry.value = value;
    entry.device = device;
    const auto now = std::chrono::steady_clock::now();
    if (log.since == std::chrono::steady_clock::time_point{}) {
        log.since = now;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - log.since).count();
    if (seconds < 5.0) {
        return;
    }
    std::vector<std::pair<GPUVAddr, AcquireLogEntry>> sorted(log.entries.begin(), log.entries.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.wait_ns > b.second.wait_ns; });
    u64 count = 0;
    u64 wait_ns = 0;
    for (const auto& [addr, e] : sorted) {
        count += e.count;
        wait_ns += e.wait_ns;
    }
    std::string text;
    for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
        const AcquireLogEntry& e = sorted[i].second;
        text += fmt::format(" [{:#x} dev {:#x} op {} value {:#x}: {:.0f}/s, {:.1f} ms/s, {:.0f} us each, "
                            ">=1ms {:.0f}/s {:.1f} ms/s, readable {:.0f}/s]",
                            sorted[i].first, e.device, e.operation, e.value, e.count / seconds,
                            e.wait_ns / 1e6 / seconds, e.count ? e.wait_ns / 1e3 / e.count : 0.0,
                            e.long_count / seconds, e.long_ns / 1e6 / seconds, e.readable / seconds);
    }
    LOG_INFO(HW_GPU, "Acquires resumed (5 s, per second): {:.0f} resumes, {:.1f} ms/s waited, {} addresses; top:{}",
             count / seconds, wait_ns / 1e6 / seconds, sorted.size(), text);
    log.entries.clear();
    log.since = now;
}
} // namespace

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        if (pending_acquire) {
            // ResumePending's answer did not hold: not resumed by it after all.
            pending_acquire->resume_route = PendingAcquire::RouteUnset;
        }
        return false;
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those made the count odd ("suspended") at random, so submit-time snapshots were
    // thrown away for about half of all entries (CommandList::snapshot_suspensions).
    if (pending_acquire) {
        // FRITCHII_DELAY_FRONT_KIND=1 ("A"): a suspended acquire found met, before the channel
        // resumes (FRITCHII_DELAY_BEFORE=1 spins at the suspension instead). Off: one relaxed
        // load and a branch.
        VideoCommon::GpuSplit::DelayFrontIf(VideoCommon::GpuSplit::FrontDelayKind::AcquireMet,
                                            false);
        // fable_stage_levelpoints 18.4 row 7: the suspension split at the back's arrival at
        // what was sent by then (GPU profile on).
        if (pending_acquire->price_start != 0) [[unlikely]] {
            VideoCommon::FritchiiPrice::NoteFrontWait(VideoCommon::FritchiiPrice::AcquireSite,
                                                      pending_acquire->price_target,
                                                      pending_acquire->price_start);
        }
        // FRITCHII_ONEF_DRY_RELEASE (fable_b 19.5 step 3): the release that met it, E's pass
        // against its write, and the front's stall between.
        if (VideoCommon::OneFollower::g_dry_release.load(std::memory_order_relaxed))
            [[unlikely]] {
            if (const auto device =
                    dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address)) {
                VideoCommon::OneFollower::DryNoteResume(
                    *device,
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         pending_acquire->since.time_since_epoch())
                                         .count()),
                    pending_acquire->dry_stall);
            }
        }
        // FRITCHII_ACQUIRE_MET_BY (live, default 0: one load here): "Acquire resumes met by".
        if (VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_acquire_met_by,
                                             "FRITCHII_ACQUIRE_MET_BY") != 0) [[unlikely]] {
            AcquireMetByStats::Get().Note(dma_pusher, *pending_acquire);
        }
        if (pending_acquire->readable_release) {
            auto& stats = FenceWriterStats::Get();
            const u64 ns = FenceWriterStats::Nanoseconds(pending_acquire->since,
                                                         FenceWriterStats::Clock::now());
            stats.NoteResume(dma_pusher.channel_state.bind_id, pending_acquire->resume_route, ns);
            // EXP12=4: the QMD prediction's "met by", deferred to here (DeferredMetBy).
            if (auto check = DeferredMetBy::Get().Take(this)) {
                if (check->bound) {
                    // The reduction bound only where the word read met it within 1 ms: as 2's
                    // read after its wait. Later the word may already be a later write's.
                    if (pending_acquire->resume_route ==
                            static_cast<u8>(FenceResumeRoute::Word) &&
                        ns < 1'000'000) {
                        ++stats.resume_bound_checks;
                    } else {
                        ++stats.resume_bound_dropped;
                        check->bound.reset();
                    }
                }
                QmdPredictionMetBy(dma_pusher, *check, *pending_acquire, true);
            }
        }
        if (AcquireLogEnabled()) [[unlikely]] {
            const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
            NoteAcquireResumed(pending_acquire->address, pending_acquire->operation,
                               pending_acquire->value, device.value_or(0),
                               static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() -
                                                    pending_acquire->since)
                                                    .count()),
                               pending_acquire->readable_release);
        }
        pending_acquire.reset();
        dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
        Nx2SemaphoreWaiters::Get().Remove(this);
    }
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        // Memory stage tag Cache (both closures here): the fence manager (its releases commit
        // the buffer cache's async flushes); no 3D state.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
               VideoCommon::Nx2FastDrawCount::Neutral() |
                   VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        VideoCommon::GpuSplit::Get().NoteSync();
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        }, VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

static bool SemaphoreReadableNow(DmaPusher& dma_pusher, GPUVAddr address, u32& word,
                                 u64* read_done);

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        // Memory stage tag Cache: the query cache's report (a fence commits the buffer cache's
        // async flushes); no 3D register, draw state, binding or channel is written. Its kind
        // lets GPUMem have the back run it at its entry (FRITCHII_MEMSTAGE_RELEASE).
        SendRelease(dma_pusher, sequence_address, payload, properties);
        if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(sequence_address)) {
                split.NoteRelease(*device, sequence_address, payload,
                                  VideoCommon::GpuSplit::ReleaseKind::Puller);
            }
        }
        RecordPullerRelease(dma_pusher, sequence_address, payload);
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .last_fences = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireChecks, 1);
        // EDEN_NX2_ACQUIRE_TRIVIAL (boot, default on, 0 = off;
        // ported): an unsigned "at least 0" acquire is met whatever the word holds
        // (Matches). OoT issues ~1,000 a second on the syncpoint shim (no guest memory behind
        // it), and the ones the cheap checks couldn't read drained the back.
        static const bool trivial = [] {
            const char* value = std::getenv("EDEN_NX2_ACQUIRE_TRIVIAL");
            return value == nullptr || value[0] != '0';
        }();
        if (trivial && op == GpuSemaphoreOperation::AcquireGequal && acquire.value == 0) {
            return;
        }
        // FRITCHII_FRONT_SPLIT_COUNT: this check reads the front's records of what was sent
        // (2a's join when engine methods are still in its queue). Off: one plain load.
        VideoCommon::FrontSplit::AcquireCheck();
        u32 word{};
        u64 read_done = 0;
        const bool readable = SemaphoreReadableNow(dma_pusher, acquire.address, word, &read_done);
        if (readable && acquire.Matches(word)) {
            RunaheadOverlaidBoundary(dma_pusher, acquire);
            return;
        }
        u32 miss = 0;
        if (MetByQueuedRelease(dma_pusher, acquire, &miss)) {
            return;
        }
        if (MetByQmdPrediction(dma_pusher, acquire, false)) {
            return;
        }
        if (op == GpuSemaphoreOperation::AcquireGequal &&
            GequalMetWithoutDrain(dma_pusher, acquire.address, acquire.value,
                                  readable ? std::optional<ReadableRead>{ReadableRead{
                                                 .word = word, .done = read_done}}
                                           : std::nullopt)) {
            return;
        }
        if (op == GpuSemaphoreOperation::AcquireEqual &&
            EqualMetWithoutDrain(dma_pusher, acquire.address, acquire.value)) {
            return;
        }
        const bool trace = AcquireTrace::Enabled();
        const u32 word_before = trace ? dma_pusher.memory_manager.Read<u32>(acquire.address) : 0;
        u32 writer_method = 0;
        const u32 writer =
            trace ? VideoCommon::GpuSplit::Get().QueuedWriterKind(
                        acquire.address, acquire.address + sizeof(u32), writer_method)
                  : 0;
        const auto drain_begin = trace ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        const auto record = [&](AcquireTrace::Outcome outcome) {
            if (trace) {
                AcquireTrace::Get().Record(
                    acquire.address, acquire.operation, dma_pusher.channel_state.bind_id, miss,
                    word_before, acquire.value,
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - drain_begin)
                                         .count()),
                    outcome, writer, writer_method);
            }
        };
        const bool learn = AcquireLearning::Enabled();
        if (learn) {
            auto& learning = AcquireLearning::Instance();
            const auto stats = learning.Get(acquire.address);
            if (stats.met >= AcquireLearning::Streak &&
                AcquireLearning::misses.load(std::memory_order_relaxed) == 0) {
                AcquireLearning::speculated.fetch_add(1, std::memory_order_relaxed);
                VideoCommon::GpuSplit::Get().NoteSync();
                OnBack([memory = &dma_pusher.memory_manager, acquire] {
                    if (!acquire.Matches(memory->Read<u32>(acquire.address)) &&
                        AcquireLearning::misses.fetch_add(1) < 8) {
                        LOG_WARNING(HW_GPU, "Learned acquire {:#x} (op {}, {:#x}) not met on the "
                                    "back (word {:#x}): learning off",
                                    acquire.address, acquire.operation, acquire.value,
                                    memory->Read<u32>(acquire.address));
                    }
                });
                return;
            }
            if (stats.suspended >= AcquireLearning::Streak &&
                !acquire.Matches(dma_pusher.memory_manager.Read<u32>(acquire.address))) {
                AcquireLearning::early_suspends.fetch_add(1, std::memory_order_relaxed);
                if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
                    Nx2SemaphoreWaiters::Get().Add(
                        this, *device, acquire.value, acquire.operation,
                        WatchedSemaphoreWord(dma_pusher.memory_manager, acquire.address));
                }
                pending_acquire = acquire;
                // EXP12=5/6, as the other suspension: the first lagged check comes 1 ms from
                // here, not at the first poll (other modes keep their first poll's drain).
                if (const int fence_mode = VideoCore::Nx2Debug::Experiment(12);
                    fence_mode == 5 || fence_mode == 6) {
                    pending_acquire->last_drain = std::chrono::steady_clock::now();
                }
                pending_acquire->lagged_target = VideoCommon::GpuSplit::Get().SentPosition();
                NoteBackSequence(*pending_acquire);
                dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
                NotePriceSuspension(*pending_acquire);
                // FRITCHII_DELAY_FRONT_KIND=1 with FRITCHII_DELAY_BEFORE=1: the control spin.
                VideoCommon::GpuSplit::DelayFrontIf(
                    VideoCommon::GpuSplit::FrontDelayKind::AcquireMet, true);
                return;
            }
        }
        if (readable) {
            // The word is current (no queued or GPU write the front knows of can change it):
            // the value comes from the CPU or another channel later, and the polls in
            // CanResumeAcquire pick it up without a drain (ported).
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireNoDrain, 1);
        } else {
            DrainBack();
            if (acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireMetAfterDrain, 1);
                record(AcquireTrace::MetAfterDrain);
                if (learn) {
                    AcquireLearning::Instance().NoteMet(acquire.address);
                }
                return;
            }
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it. With EDEN_NX2_GPU_WAKE the word in memory is
        // watched for the guest CPU's stores (Nx2SemaphoreWaiters::WordStored).
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation,
                        WatchedSemaphoreWord(dma_pusher.memory_manager, acquire.address));
        }
        bool met_now;
        // EXP12=4: the QMD check of an acquire suspended without the writer wait, for its resume.
        std::optional<QmdPredictionCheck> deferred_check;
        if (readable) {
            const auto qmd_check = CheckQmdPrediction(dma_pusher, acquire);
            bool skipped_wait = false;
            met_now = ReleaseFencesForReadable(dma_pusher, this, acquire, &skipped_wait);
            if (qmd_check) {
                const bool met = met_now || waiters.Latched(this);
                if (skipped_wait && !met) {
                    deferred_check = qmd_check;
                } else {
                    QmdPredictionMetBy(dma_pusher, *qmd_check, acquire, met);
                }
            }
        } else {
            ReleaseFences(dma_pusher);
            met_now = acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address));
        }
        const bool suspend = !waiters.Latched(this) && !met_now;
        if (readable) {
            FenceWriterStats::Get().NoteOutcome(dma_pusher.channel_state.bind_id, suspend);
        }
        if (deferred_check) {
            if (suspend) {
                DeferredMetBy::Get().Add(this, *deferred_check);
            } else {
                // Latched in between: met after all.
                QmdPredictionMetBy(dma_pusher, *deferred_check, acquire, true);
            }
        }
        if (suspend) {
            pending_acquire = acquire;
            pending_acquire->last_drain = std::chrono::steady_clock::now();
            pending_acquire->readable_release = readable;
            // EXP12=5/6: the first lagged check waits for the acquire's own fence release.
            pending_acquire->lagged_target = VideoCommon::GpuSplit::Get().SentPosition();
            NoteBackSequence(*pending_acquire);
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
            record(AcquireTrace::Suspended);
            if (learn) {
                AcquireLearning::Instance().NoteSuspended(acquire.address);
            }
            NotePriceSuspension(*pending_acquire);
            // FRITCHII_DELAY_FRONT_KIND=1 with FRITCHII_DELAY_BEFORE=1: the control spin.
            VideoCommon::GpuSplit::DelayFrontIf(VideoCommon::GpuSplit::FrontDelayKind::AcquireMet,
                                                true);
        } else {
            waiters.Remove(this);
            record(AcquireTrace::MetAfterFences);
            if (learn) {
                AcquireLearning::Instance().NoteMet(acquire.address);
            }
        }
    } else {
        DrainBack();
        do {
            const u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    // Tagged for the memory stage (fable_stage_p3 3): untagged, each one made GPUMem's copies
    // stale (a full copy at the next 3D entry).
    SendRelease(dma_pusher, sequence_address, payload, VideoCommon::QueryPropertiesFlags::IsAFence);
    RecordPullerRelease(dma_pusher, sequence_address, payload);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack();
    // EDEN_NX2_FASTDRAW_COUNT: the CPU may rewrite what the next draws read once it sees
    // the release this waits for.
    VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::Fence);
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        ReleaseFences(dma_pusher);
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    if (operation == 1) { // RELEASE
        // Tagged for the memory stage (fable_stage_p3 3), as ProcessSemaphoreRelease.
        SendRelease(dma_pusher, address, payload, VideoCommon::QueryPropertiesFlags::IsAFence);
        // An acquire it meets while queued needs no drain (ported).
        if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
                split.NoteRelease(*device, address, payload,
                                  VideoCommon::GpuSplit::ReleaseKind::Puller);
            }
        }
        RecordPullerRelease(dma_pusher, address, payload);
        return;
    }
    // Acquires: the result only feeds the warning below, so with the split the pending fences are
    // released in order on the back instead of after a drain (ported). Later
    // GPU-written list fetches and macro reads still wait for the work before it (NoteSync).
    // Memory stage tag Cache (both closures here, fable_stage_levelpoints 14.5): the fence manager
    // only; no 3D state.
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
               VideoCommon::Nx2FastDrawCount::Neutral() |
                   VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        split.NoteSync();
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    if (VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_sem_fast, "EDEN_NX2_SEM_FAST") != 0 &&
        VideoCommon::GpuSplit::Get().Active()) {
        // Only the warning below reads the result: release the fences in order instead.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
               VideoCommon::Nx2FastDrawCount::Neutral() |
                   VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        return;
    }
    DrainBack();
    VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::Fence);
    ReleaseFences(dma_pusher);
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    const bool ready = operation == 0   ? word == payload
                       : operation == 2 ? word >= payload
                       : operation == 3 ? static_cast<s32>(word - payload) >= 0
                       : operation == 4 ? (word & payload) != 0
                       : operation == 5 ? (word & payload) == 0
                                        : true;
    if (!ready) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
}

// A semaphore release queued for the split's back: the front's macro DMA reads of those bytes
// wait for it (EDEN_NX2_DMA_WAIT_WRITERS).
// A fence release (`fence_payload`) is written when the back runs it, so reads overlay it.
static void NoteSemaphoreWrite(DmaPusher& dma_pusher, GPUVAddr address,
                               std::optional<u32> fence_payload) {
    auto& split = VideoCommon::GpuSplit::Get();
    split.NoteMemoryWriter();
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return; // Queries write nothing at an unmapped address.
    } else if (fence_payload) {
        split.NoteMemoryWriteData(*device, &*fence_payload, 1);
    } else {
        // Written when its fence is signalled: reads wait until the back ran it and it was.
        split.NoteAsyncWrite(*device, 16);
        split.NoteMemoryWrite(*device, 16, "semaphore (timed)");
    }
}

// EDEN_NX2_SEM_FAST: whether the word at `address` can be read without draining the back: not
// GPU-written, after waiting only for the queued writes noted to it.
// =2 also overlays the queued fence releases the front knows, instead of waiting for them.
static bool SemaphoreReadableNow(DmaPusher& dma_pusher, GPUVAddr address, u32& word,
                                 u64* read_done) {
    auto& split = VideoCommon::GpuSplit::Get();
    const int mode = VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_sem_fast, "EDEN_NX2_SEM_FAST");
    using VideoCommon::Nx2Profile::Counter;
    if (!split.Active() || mode == 0) {
        VideoCommon::Nx2Profile::Add(Counter::SemNoFastPath, 1);
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device || Nx2IsGpuWritten(*device)) {
        VideoCommon::Nx2Profile::Add(Counter::SemGpuWrittenPage, 1);
        return false;
    }
    if (const u64 async_position = split.HasAsyncWrite(*device, 4);
        async_position != 0 &&
        (async_position > split.ConsumedNow() || !Nx2AsyncReportsSettled(*device, 4))) {
        VideoCommon::Nx2Profile::Add(Counter::SemAsyncPending, 1);
        return false;
    }
    if (mode >= 2) {
        const u64 since = split.ConsumedNow();
        word = dma_pusher.memory_manager.Read<u32>(address);
        if (split.OverlayQueuedWrites(since, *device, &word, 1)) {
            if (read_done) {
                *read_done = since;
            }
            return true;
        }
    }
    if (!split.WaitForWritesTo(*device, 16)) {
        VideoCommon::Nx2Profile::Add(Counter::SemWritersWait, 1);
        return false;
    }
    if (read_done) {
        *read_done = split.ConsumedNow();
    }
    word = dma_pusher.memory_manager.Read<u32>(address);
    return true;
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        // Memory stage tag Cache: the fence manager only; no 3D state.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); },
               VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        VideoCommon::GpuSplit::Get().NoteMemoryWriter();
        break;
    case BufferMethods::WaitForIdle:
        // Memory stage tag Cache: a barrier, the query cache's NotifyWFI (which can reach the
        // buffer cache) and the fence manager; no 3D state.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); },
               VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        VideoCommon::GpuSplit::Get().NoteSync();
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        if (static_cast<GpuSemaphoreOperation>(regs.semaphore_trigger & 0xF) ==
            GpuSemaphoreOperation::WriteLong) {
            const bool fence = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0;
            NoteSemaphoreWrite(dma_pusher, regs.semaphore_address.SemaphoreAddress(),
                               fence ? std::optional<u32>{regs.semaphore_sequence} : std::nullopt);
        }
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        // Memory stage tag Cache: a gather (the buffer, texture and pipeline caches); no 3D
        // state, no Query.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); },
               VideoCommon::Nx2FastDrawCount::Neutral() |
                   VideoCommon::StageTagBits(VideoCommon::StageTag::Cache));
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation C,D");
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        if (g_nx2_ampere && (method_call.argument & 7) == 1) {
            NoteSemaphoreWrite(dma_pusher,
                               (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                                   (regs.reg_array[0x17] & ~3U),
                               regs.reg_array[0x19]);
        } else if (!g_nx2_ampere) {
            NoteSemaphoreWrite(dma_pusher, regs.semaphore_address.SemaphoreAddress(),
                               std::nullopt);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

// EDEN_NX2_FASTDRAW_COUNT: any other engine's method ends delta draw reuse. With the
// split, GpuSplit::Execute counts them where the back runs them; here only without it.
static void NoteFastDrawCountEngine(EngineID engine) {
    if (VideoCommon::Nx2FastDrawCount::g_on && engine != EngineID::MAXWELL_B) [[unlikely]] {
        if (!VideoCommon::GpuSplit::Get().Active() || VideoCommon::GpuSplit::OnBackThread()) {
            VideoCommon::Nx2FastDrawCount::Break(VideoCommon::Nx2FastDrawCount::Reason::OtherEngine);
        }
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    NoteFastDrawCountEngine(engine);
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    NoteFastDrawCountEngine(engine);
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
