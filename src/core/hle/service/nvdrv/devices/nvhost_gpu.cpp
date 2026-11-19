#pragma GCC diagnostic ignored "-Wunused-variable"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <vector>
#include "common/assert.h"
#include "common/logging.h"
#include "common/scratch_buffer.h"
#include "common/nx2_timeline.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"

namespace Service::Nvidia::Devices {
namespace {
Tegra::CommandHeader BuildFenceAction(Tegra::Engines::Puller::FenceOperation op, u32 syncpoint_id) {
    Tegra::Engines::Puller::FenceAction result{};
    result.op.Assign(op);
    result.syncpoint_id.Assign(syncpoint_id);
    return {result.raw};
}
} // namespace

nvhost_gpu::nvhost_gpu(Core::System& system_, EventInterface& events_interface_,
                       NvCore::Container& core_)
    : nvdevice{system_}, events_interface{events_interface_}, core{core_},
      syncpoint_manager{core_.GetSyncpointManager()}, nvmap{core.GetNvMapFile()},
      channel_state{system.GPU().AllocateChannel()} {
    channel_syncpoint = syncpoint_manager.AllocateSyncpoint(false);
    sm_exception_breakpoint_int_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointInt");
    sm_exception_breakpoint_pause_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointPause");
    error_notifier_event = events_interface.CreateEvent("GpuChannelErrorNotifier");
}

nvhost_gpu::~nvhost_gpu() {
    events_interface.FreeEvent(sm_exception_breakpoint_int_report_event);
    events_interface.FreeEvent(sm_exception_breakpoint_pause_report_event);
    events_interface.FreeEvent(error_notifier_event);
    for (auto& [id, event] : nx2_events) {
        events_interface.FreeEvent(event);
    }
    syncpoint_manager.FreeSyncpoint(channel_syncpoint);
}

NvResult nvhost_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output) {
    switch (command.group) {
    case 0x0:
        switch (command.cmd) {
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::GetWaitbase, input, output);
        default:
            break;
        }
        break;
    case 'H':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_gpu::SetNVMAPfd, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeout, input, output);
        case 0x8:
            return SubmitGPFIFOIoctl1(input, output, fd, false);
        case 0x9:
            return WrapFixed(this, &nvhost_gpu::AllocateObjectContext, input, output);
        case 0xb:
            return WrapFixed(this, &nvhost_gpu::ZCullBind, input, output);
        case 0xc:
            return WrapFixed(this, &nvhost_gpu::SetErrorNotifier, input, output);
        case 0xd:
            return WrapFixed(this, &nvhost_gpu::SetChannelPriority, input, output);
        case 0x18:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx, input, output, fd);
        case 0x1a:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx2, input, output, fd);
        case 0x1b:
            return SubmitGPFIFOIoctl1(input, output, fd, true);
        case 0x1d:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeslice, input, output);
        default:
            break;
        }
        break;
    case 'G':
        switch (command.cmd) {
        case 0x14:
            return WrapFixed(this, &nvhost_gpu::SetClientData, input, output);
        case 0x15:
            return WrapFixed(this, &nvhost_gpu::GetClientData, input, output);
        default:
            break;
        }
        break;
    }
    if (g_nx2_driver) {
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
};

NvResult nvhost_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<const u8> inline_input, std::span<u8> output) {
    switch (command.group) {
    case 'H':
        switch (command.cmd) {
        case 0x1b:
            return SubmitGPFIFOIoctl2(input, inline_input, output);
        }
        break;
    }
    if (g_nx2_driver) {
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output, std::span<u8> inline_output) {
    if (g_nx2_driver) {
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    sessions[fd] = session_id;
}

void nvhost_gpu::OnClose(DeviceFD fd) {
    sessions.erase(fd);
}

NvResult nvhost_gpu::SetNVMAPfd(IoctlSetNvmapFD& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return NvResult::Success;
}

NvResult nvhost_gpu::SetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    user_data = params.data;
    return NvResult::Success;
}

NvResult nvhost_gpu::GetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.data = user_data;
    return NvResult::Success;
}

NvResult nvhost_gpu::ZCullBind(IoctlZCullBind& params) {
    zcull_params = params;
    LOG_DEBUG(Service_NVDRV, "called, gpu_va={:X}, mode={:X}", zcull_params.gpu_va,
              zcull_params.mode);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetErrorNotifier(IoctlSetErrorNotifier& params) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called, offset={:X}, size={:X}, mem={:X}", params.offset,
                params.size, params.mem);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetChannelPriority(IoctlChannelSetPriority& params) {
    channel_priority = params.priority;
    LOG_INFO(Service_NVDRV, "called, priority={:X}", channel_priority);

    switch (static_cast<ChannelPriority>(channel_priority)) {
    case ChannelPriority::Low: channel_timeslice = 1300; break;
    case ChannelPriority::Medium: channel_timeslice = 2600; break;
    case ChannelPriority::High: channel_timeslice = 5200; break;
    default : return NvResult::BadParameter;
    }

    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV, "called, num_entries={:X}, flags={:X}, reserved1={:X}, "
              "reserved2={:X}, reserved3={:X}",
              params.num_entries, params.flags, params.reserved[0], params.reserved[1],
              params.reserved[2]);

    if (channel_state->initialized) {
        LOG_DEBUG(Service_NVDRV, "Channel already initialized; AllocGPFIFOEx returning AlreadyAllocated");
        return NvResult::AlreadyAllocated;
    }

    u64 program_id{};
    if (auto* const session = core.GetSession(sessions[fd]); session != nullptr) {
        program_id = session->process->GetProgramId();
    }

    // Store program id for later lazy initialization
    channel_state->program_id = program_id;

    // If address space is not yet bound, defer channel initialization.
    if (!channel_state->memory_manager) {
        params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);
        return NvResult::Success;
    }

    system.GPU().InitChannel(*channel_state, program_id);

    params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);

    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx2(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV,
              "called, num_entries={:X}, flags={:X}, reserved1={:X}, "
              "reserved2={:X}, reserved3={:X}",
              params.num_entries, params.flags, params.reserved[0], params.reserved[1],
              params.reserved[2]);

    if (channel_state->initialized) {
        LOG_DEBUG(Service_NVDRV, "Channel already initialized; AllocGPFIFOEx2 returning AlreadyAllocated");
        return NvResult::AlreadyAllocated;
    }

    u64 program_id{};
    if (auto* const session = core.GetSession(sessions[fd]); session != nullptr) {
        program_id = session->process->GetProgramId();
    }

    // Store program id for later lazy initialization
    channel_state->program_id = program_id;

    // If address space is not yet bound, defer channel initialization.
    if (!channel_state->memory_manager) {
        params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);
        return NvResult::Success;
    }

    system.GPU().InitChannel(*channel_state, program_id);

    params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);

    return NvResult::Success;
}

s32_le nvhost_gpu::GetObjectContextClassNumberIndex(CtxClasses class_number) {
    constexpr s32_le invalid_class_number_index = -1;
    switch (class_number) {
    case CtxClasses::Ctx2D: return 0;
    case CtxClasses::Ctx3D: return 1;
    case CtxClasses::CtxCompute: return 2;
    case CtxClasses::CtxKepler: return 3;
    case CtxClasses::CtxDMA: return 4;
    case CtxClasses::CtxChannelGPFIFO: return 5;
    default: return invalid_class_number_index;
    }
}

NvResult nvhost_gpu::AllocateObjectContext(IoctlAllocObjCtx& params) {
    LOG_DEBUG(Service_NVDRV, "called, class_num={:#x}, flags={:#x}, obj_id={:#x}", params.class_num,
              params.flags, params.obj_id);

    // Do not require channel initialization here: some clients allocate contexts before binding.
    if (!channel_state) {
        LOG_ERROR(Service_NVDRV, "No channel state available!");
        return NvResult::InvalidState;
    }

    std::scoped_lock lk(channel_mutex);

    if (params.flags) {
        LOG_WARNING(Service_NVDRV, "non-zero flags={:#x} for class={:#x}", params.flags,
                    params.class_num);

        constexpr u32 allowed_mask{};
        params.flags = allowed_mask;
    }

    if (g_nx2_driver) {
        // Switch 2 NvRm asks for Ampere classes, or class 0 (it reads the class numbers from a
        // characteristics layout we don't know yet). Map them onto the Maxwell engine slots.
        u32 mapped = params.class_num;
        switch (params.class_num) {
        case 0xC797: case 0xC697: case 0xC597: mapped = static_cast<u32>(CtxClasses::Ctx3D); break;
        case 0xC7C0: case 0xC6C0: case 0xC5C0: mapped = static_cast<u32>(CtxClasses::CtxCompute); break;
        case 0xC7B5: case 0xC6B5: case 0xC5B5: mapped = static_cast<u32>(CtxClasses::CtxDMA); break;
        case 0xC56F: case 0xC76F: mapped = static_cast<u32>(CtxClasses::CtxChannelGPFIFO); break;
        case 0: {
            for (const auto candidate : {CtxClasses::Ctx3D, CtxClasses::CtxCompute, CtxClasses::CtxDMA,
                                         CtxClasses::Ctx2D, CtxClasses::CtxKepler}) {
                const auto index = GetObjectContextClassNumberIndex(candidate);
                if (index >= 0 && !ctxObjs[index].has_value()) {
                    mapped = static_cast<u32>(candidate);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
        if (mapped != params.class_num) {
            LOG_WARNING(Service_NVDRV, "Switch 2 object class {:#x} -> {:#x}", params.class_num, mapped);
            params.class_num = mapped;
        }
    }

    s32_le ctx_class_number_index =
        GetObjectContextClassNumberIndex(static_cast<CtxClasses>(params.class_num));
    if (ctx_class_number_index < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid class number for object context: {:#x}",
                  params.class_num);
        return NvResult::BadParameter;
    }

    if (ctxObjs[ctx_class_number_index].has_value()) {
        LOG_WARNING(Service_NVDRV, "Object context for class {:#x} already allocated on this channel",
                    params.class_num);
        return NvResult::AlreadyAllocated;
    }

    // Defer actual hardware context binding until channel is initialized.
    ctxObjs[ctx_class_number_index] = params;

    return NvResult::Success;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildWaitCommandList(
    NvFence fence) {
    return {
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {fence.value},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                  Tegra::SubmissionMode::Increasing),
        BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Acquire, fence.id),
    };
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementCommandList(
    NvFence fence) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};

    for (u32 count = 0; count < 2; ++count) {
        result.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                                   Tegra::SubmissionMode::Increasing));
        result.push_back(
            BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Increment, fence.id));
    }

    return result;
}

// Switch 2 pushbuffers signal completion with semaphore releases, never host1x syncpoint methods,
// but the driver still has the kernel raise the syncpoint max by the submission's increment value
// and then waits for that value. Append the increments after its commands.
static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementCountCommandList(
    NvFence fence, u32 count) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1,
                                  Tegra::SubmissionMode::Increasing),
        {},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};
    for (u32 i = 0; i < count; ++i) {
        result.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                                   Tegra::SubmissionMode::Increasing));
        result.push_back(
            BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Increment, fence.id));
    }
    return result;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementWithWfiCommandList(
    NvFence fence) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};
    auto increment_list{BuildIncrementCommandList(fence)};
    result.insert(result.end(), increment_list.begin(), increment_list.end());
    return result;
}

namespace {
// EXP101 counters, every 5 s while it is on: submits, entries, reads made, loop time.
void NotePbSnapshot(int mode, size_t entries, u64 reads, std::chrono::steady_clock::duration took) {
    static std::atomic<u64> submits{}, entry_count{}, read_count{}, ns{};
    static std::atomic<s64> next_report{};
    submits.fetch_add(1, std::memory_order_relaxed);
    entry_count.fetch_add(entries, std::memory_order_relaxed);
    read_count.fetch_add(reads, std::memory_order_relaxed);
    ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count()),
                 std::memory_order_relaxed);
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 next = next_report.load(std::memory_order_relaxed);
    if (next == 0) {
        next_report.compare_exchange_strong(next, now + 5'000'000'000);
        return;
    }
    if (now < next || !next_report.compare_exchange_strong(next, now + 5'000'000'000)) {
        return;
    }
    const u64 s = submits.exchange(0), e = entry_count.exchange(0), r = read_count.exchange(0),
              t = ns.exchange(0);
    LOG_INFO(Service_NVDRV,
             "Pushbuffer snapshot (EXP101={}, 5 s, per second): {:.0f} submits, {:.0f} entries, "
             "{:.0f} reads ({:.1f} entries each), loop {:.2f} ms/s",
             mode, s / 5.0, e / 5.0, r / 5.0, r != 0 ? static_cast<double>(e) / r : 0.0, t / 5e6);
}
} // namespace

NvResult nvhost_gpu::SubmitGPFIFOImpl(IoctlSubmitGpfifo& params, Tegra::CommandList&& entries) {
    LOG_TRACE(Service_NVDRV, "called, gpfifo={:X}, num_entries={:X}, flags={:X}", params.address,
              params.num_entries, params.flags.raw);

    auto& gpu = system.GPU();

    std::unique_lock lock(channel_mutex);

    // Lazily initialize channel when address space is available
    if (!channel_state->initialized && channel_state->memory_manager) {
        system.GPU().InitChannel(*channel_state, channel_state->program_id);
    }

    const auto bind_id = channel_state->bind_id;

    if (g_nx2_driver) {
        for ([[maybe_unused]] const auto& entry : entries.command_lists) {
            LOG_TRACE(Service_NVDRV, "nx2 GPFIFO entry raw={:#018x} addr={:#x} size={:#x}", entry.raw,
                     entry.addr.Value(), entry.size.Value());
        }
        static const bool check = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;
        if (check) {
            std::vector<u32> words;
            for (const auto& entry : entries.command_lists) {
                words.resize(entry.size);
                system.ApplicationMemory().ReadBlock(entry.addr.Value(), words.data(),
                                                     words.size() * sizeof(u32));
                entries.submit_hashes.push_back(Tegra::HashPushbufferWords(words));
            }
        }
        // The present returns before the GPU thread reaches this frame's work (nx2_gsv.h,
        // frames ahead), and Mario Kart World then refills command memory it already
        // submitted: the end of a frame ran with the next frame's present image as its render
        // target, leaving an image three frames old to be shown. Keep the words as submitted.
        // EDEN_NX2_PB_SNAPSHOT=0: execute from memory.
        static const bool snapshot = [] {
            const char* value = std::getenv("EDEN_NX2_PB_SNAPSHOT");
            return value == nullptr || value[0] != '0';
        }();
        if (snapshot && channel_state->memory_manager && channel_state->payload) {
            // Before the copy: a suspension during it must count as one after it.
            entries.snapshot_suspensions =
                channel_state->payload->dma_pusher.suspensions.load(std::memory_order_acquire);
            size_t words = 0;
            for (const auto& entry : entries.command_lists) {
                words += entry.size;
            }
            entries.submitted_words.resize(words);
            entries.submitted_offsets.reserve(entries.command_lists.size());
            size_t offset = 0;
            // EDEN_NX2_EXP101 (live; fable52_answer_presenter): entries whose GPU addresses follow
            // each other are read with one ReadBlockUnsafe per run instead of one per entry (OoT:
            // ~6,500 entries a frame, a few words each, on the thread that presents). The words
            // land where the per-entry reads put them; any entry a queued map touches takes the
            // per-entry path as before.
            const int run_mode = VideoCore::Nx2Debug::Experiment(101);
            const auto loop_begin = run_mode != 0 ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
            u64 runs_read = 0;
            if (run_mode == 2) {
                // EXP101=2 (fable52_answer_presenter 2.1): an entry inside one 4 KiB GPU page is
                // copied from MemoryManager's per-thread page pointer cache (it drops stale
                // entries on any map serial or translation change), and the first and last
                // lines of the entry 8 ahead are prefetched. Same bytes as ReadBlockUnsafe.
                const auto& lists = entries.command_lists;
                auto& gmmu = *channel_state->memory_manager;
                const auto host_of = [&](u32 index) -> const u8* {
                    const u64 addr = static_cast<u64>(lists[index].addr);
                    const u64 bytes = static_cast<u64>(lists[index].size) * sizeof(u32);
                    if (bytes == 0 || bytes > 0x1000 || (addr & 0xFFF) + bytes > 0x1000) {
                        return nullptr;
                    }
                    return gmmu.GetPointerForDescriptor(addr);
                };
                for (u32 i = 0; i < lists.size(); ++i) {
                    if (i + 8 < lists.size()) {
                        if (const u8* ahead = host_of(i + 8)) {
                            const u64 ahead_bytes =
                                static_cast<u64>(lists[i + 8].size) * sizeof(u32);
                            _mm_prefetch(reinterpret_cast<const char*>(ahead), _MM_HINT_T0);
                            _mm_prefetch(reinterpret_cast<const char*>(ahead + ahead_bytes - 1),
                                         _MM_HINT_T0);
                        }
                    }
                    const auto& entry = lists[i];
                    entries.submitted_offsets.push_back(static_cast<u32>(offset));
                    const u64 bytes = static_cast<u64>(entry.size) * sizeof(u32);
                    if (system.GPU().IsMapPending(entry.addr, bytes)) {
                        entries.deferred_entries.push_back(i);
                    } else if (const u8* host = host_of(i)) {
                        std::memcpy(entries.submitted_words.data() + offset, host, bytes);
                        ++runs_read;
                    } else {
                        gmmu.ReadBlockUnsafe(entry.addr, entries.submitted_words.data() + offset,
                                             bytes);
                        ++runs_read;
                    }
                    offset += entry.size;
                }
                NotePbSnapshot(run_mode, lists.size(), runs_read,
                               std::chrono::steady_clock::now() - loop_begin);
            } else if (run_mode != 0) {
                const auto& lists = entries.command_lists;
                for (u32 i = 0; i < lists.size();) {
                    u32 end = i + 1;
                    u64 run_bytes = static_cast<u64>(lists[i].size) * sizeof(u32);
                    while (end < lists.size() &&
                           static_cast<u64>(lists[end].addr) ==
                               static_cast<u64>(lists[i].addr) + run_bytes) {
                        run_bytes += static_cast<u64>(lists[end].size) * sizeof(u32);
                        ++end;
                    }
                    if (end - i > 1 && !system.GPU().IsMapPending(lists[i].addr, run_bytes)) {
                        for (u32 j = i; j < end; ++j) {
                            entries.submitted_offsets.push_back(static_cast<u32>(offset));
                            offset += lists[j].size;
                        }
                        channel_state->memory_manager->ReadBlockUnsafe(
                            lists[i].addr,
                            entries.submitted_words.data() + entries.submitted_offsets[i],
                            run_bytes);
                        ++runs_read;
                        i = end;
                        continue;
                    }
                    // One entry (or a run a queued map touches): as before.
                    const auto& entry = lists[i];
                    entries.submitted_offsets.push_back(static_cast<u32>(offset));
                    const u64 bytes = static_cast<u64>(entry.size) * sizeof(u32);
                    if (system.GPU().IsMapPending(entry.addr, bytes)) {
                        entries.deferred_entries.push_back(i);
                    } else {
                        channel_state->memory_manager->ReadBlockUnsafe(
                            entry.addr, entries.submitted_words.data() + offset,
                            entry.size * sizeof(u32));
                        ++runs_read;
                    }
                    offset += entry.size;
                    ++i;
                }
                NotePbSnapshot(run_mode, lists.size(), runs_read,
                               std::chrono::steady_clock::now() - loop_begin);
            }
            for (u32 i = 0; run_mode == 0 && i < entries.command_lists.size(); ++i) {
                const auto& entry = entries.command_lists[i];
                entries.submitted_offsets.push_back(static_cast<u32>(offset));
                // GMMU maps run on the GPU thread (GPU::MapInOrder): an entry on addresses a
                // queued map changes would copy as zeros (Pokopia's first lists). DmaPusher::Push
                // copies it after the map instead.
                const u64 bytes = static_cast<u64>(entry.size) * sizeof(u32);
                if (system.GPU().IsMapPending(entry.addr, bytes)) {
                    entries.deferred_entries.push_back(i);
                } else {
                    channel_state->memory_manager->ReadBlockUnsafe(
                        entry.addr, entries.submitted_words.data() + offset,
                        entry.size * sizeof(u32));
                }
                offset += entry.size;
            }
            // submitted_words is not zero-filled when sized: the entries copied above wrote
            // theirs; a deferred entry's words stay zero, as before, until DmaPusher::Push copies
            // them after the map.
            for (const u32 index : entries.deferred_entries) {
                std::memset(entries.submitted_words.data() + entries.submitted_offsets[index], 0,
                            static_cast<size_t>(entries.command_lists[index].size) * sizeof(u32));
            }
        }
    }

    auto& flags = params.flags;

    if (flags.fence_wait.Value()) {
        if (flags.increment_value.Value()) {
            return NvResult::BadParameter;
        }

        if (!syncpoint_manager.IsFenceSignalled(params.fence)) {
            gpu.PushGPUEntries(bind_id, Tegra::CommandList{BuildWaitCommandList(params.fence)});
        }
    }

    params.fence.id = channel_syncpoint;

    u32 increment{(flags.fence_increment.Value() != 0 ? 2 : 0) +
                  (flags.increment_value.Value() != 0 ? params.fence.value : 0)};
    params.fence.value = syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint, increment);
    // The syncpoint value this submission's fence reaches, which the guest's event waits name;
    // the lists it sends are PushCommand's "submit list" markers right after.
    if (Common::Nx2Timeline::Active()) {
        Common::Nx2Timeline::Instant("guest: submit gpfifo", {"syncpoint", channel_syncpoint},
                                     {"value", params.fence.value},
                                     {"entries", entries.command_lists.size()});
    }
    gpu.PushGPUEntries(bind_id, std::move(entries));

    if (g_nx2_driver && !flags.fence_increment.Value() && increment > 0) {
        gpu.PushGPUEntries(bind_id,
                           Tegra::CommandList{BuildIncrementCountCommandList(params.fence, increment)});
    }

    if (flags.fence_increment.Value()) {
        if (flags.suppress_wfi.Value()) {
            gpu.PushGPUEntries(bind_id,
                               Tegra::CommandList{BuildIncrementCommandList(params.fence)});
        } else {
            gpu.PushGPUEntries(bind_id,
                               Tegra::CommandList{BuildIncrementWithWfiCommandList(params.fence)});
        }
    }

    if (g_nx2_driver) {
        LOG_TRACE(Service_NVDRV,
                 "nx2 SubmitGPFIFO entries={} flags={:#x} fence_out=({}, {}) guest_value={}",
                 params.num_entries, flags.raw, params.fence.id, params.fence.value,
                 system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(params.fence.id));
    }
    flags.raw = 0;

    static const bool sync_submit = [] {
        const char* value = std::getenv("EDEN_NX2_SYNC_SUBMIT");
        return value != nullptr && value[0] != '0';
    }();
    if (g_nx2_driver && sync_submit && increment > 0) {
        lock.unlock();
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (!syncpoints.IsReadyCpuGuest(params.fence.id, params.fence.value)) {
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            syncpoints.RegisterCpuGuestAction(params.fence.id, params.fence.value, [wake] {
                {
                    std::scoped_lock lk{wake->mutex};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
            std::unique_lock lk{wake->mutex};
            while (!wake->done && system.IsPoweredOn()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    break;
                }
                wake->cv.wait_for(
                    lk, std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                 std::chrono::milliseconds{50}));
            }
        }
        static std::atomic<u32> timeouts{};
        if (!syncpoints.IsReadyCpuGuest(params.fence.id, params.fence.value) &&
            (timeouts.fetch_add(1, std::memory_order_relaxed) % 64) == 0) {
            LOG_WARNING(Service_NVDRV,
                        "nx2 sync submit timed out waiting for fence ({}, {}), current value={}",
                        params.fence.id, params.fence.value,
                        syncpoints.GetGuestSyncpointValue(params.fence.id));
        }
    }

    return NvResult::Success;
}

Core::Memory::Memory& nvhost_gpu::GetSessionMemory(DeviceFD fd) {
    if (const auto it = sessions.find(fd); it != sessions.end())
        if (auto* const session = core.GetSession(it->second);
            session != nullptr && session->process != nullptr)
            return session->process->GetMemory();

    LOG_ERROR(Service_NVDRV, "No session for fd={}, falling back to application memory", fd);
    return system.ApplicationMemory();
}

NvResult nvhost_gpu::SubmitGPFIFOBase1(IoctlSubmitGpfifo& params,
                                       std::span<Tegra::CommandListHeader> commands, DeviceFD fd,
                                       bool kickoff) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    // Every entry is written below (copied, or read from guest memory, which zero-fills
    // unmapped bytes): no zero fill first.
    Tegra::CommandList entries(params.num_entries, boost::container::default_init);
    if (kickoff) {
        this->GetSessionMemory(fd).ReadBlock(params.address, entries.command_lists.data(),
                                             params.num_entries * sizeof(Tegra::CommandListHeader));
    } else {
        std::memcpy(entries.command_lists.data(), commands.data(),
                    params.num_entries * sizeof(Tegra::CommandListHeader));
    }

    return SubmitGPFIFOImpl(params, std::move(entries));
}

namespace {
/// The submit's entries, reused per thread (WrapGeneric made a zero-filled vector each time).
Common::ScratchBuffer<Tegra::CommandListHeader>& SubmitEntriesBuffer(std::span<const u8> bytes) {
    thread_local Common::ScratchBuffer<Tegra::CommandListHeader> buffer;
    const size_t count = bytes.size() / sizeof(Tegra::CommandListHeader);
    buffer.resize_destructive(count);
    if (count > 0) {
        std::memcpy(buffer.data(), bytes.data(), count * sizeof(Tegra::CommandListHeader));
    }
    return buffer;
}
} // namespace

NvResult nvhost_gpu::SubmitGPFIFOIoctl1(std::span<const u8> input, std::span<u8> output,
                                        DeviceFD fd, bool kickoff) {
    // As WrapGeneric<IoctlSubmitGpfifo, CommandListHeader, Null, Null>.
    IoctlSubmitGpfifo fixed{};
    const size_t var_offset = (std::min)(sizeof(IoctlSubmitGpfifo), input.size());
    if (var_offset > 0) {
        std::memcpy(&fixed, input.data(), var_offset);
    }
    auto& entries = SubmitEntriesBuffer(input.subspan(var_offset));
    const size_t count = (input.size() - var_offset) / sizeof(Tegra::CommandListHeader);
    const NvResult result = SubmitGPFIFOBase1(
        fixed, std::span<Tegra::CommandListHeader>(entries.data(), count), fd, kickoff);
    if (output.size() > 0) {
        std::memcpy(output.data(), &fixed, (std::min)(output.size(), sizeof(IoctlSubmitGpfifo)));
    }
    if (count > 0 && output.size() > var_offset) {
        std::memcpy(output.data() + var_offset, entries.data(),
                    (std::min)(output.size() - var_offset,
                               count * sizeof(Tegra::CommandListHeader)));
    }
    return result;
}

NvResult nvhost_gpu::SubmitGPFIFOIoctl2(std::span<const u8> input,
                                        std::span<const u8> inline_input, std::span<u8> output) {
    // As WrapGeneric<IoctlSubmitGpfifo, Null, CommandListHeader, Null>.
    IoctlSubmitGpfifo fixed{};
    const size_t fixed_bytes = (std::min)(sizeof(IoctlSubmitGpfifo), input.size());
    if (fixed_bytes > 0) {
        std::memcpy(&fixed, input.data(), fixed_bytes);
    }
    auto& entries = SubmitEntriesBuffer(inline_input);
    const size_t count = inline_input.size() / sizeof(Tegra::CommandListHeader);
    const NvResult result = SubmitGPFIFOBase2(
        fixed, std::span<const Tegra::CommandListHeader>(entries.data(), count));
    if (output.size() > 0) {
        std::memcpy(output.data(), &fixed, (std::min)(output.size(), sizeof(IoctlSubmitGpfifo)));
    }
    return result;
}

NvResult nvhost_gpu::SubmitGPFIFOBase2(IoctlSubmitGpfifo& params,
                                       std::span<const Tegra::CommandListHeader> commands) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    // Every entry is written below (copied, or read from guest memory, which zero-fills
    // unmapped bytes): no zero fill first.
    Tegra::CommandList entries(params.num_entries, boost::container::default_init);
    std::memcpy(entries.command_lists.data(), commands.data(),
                params.num_entries * sizeof(Tegra::CommandListHeader));
    return SubmitGPFIFOImpl(params, std::move(entries));
}

NvResult nvhost_gpu::GetWaitbase(IoctlGetWaitbase& params) {
    LOG_INFO(Service_NVDRV, "called, unknown={:#x}", params.unknown);

    params.value = 0; // Seems to be hard coded at 0
    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeout(IoctlChannelSetTimeout& params) {
    LOG_INFO(Service_NVDRV, "called, timeout={:#x}", params.timeout);

    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeslice(IoctlSetTimeslice& params) {
    LOG_INFO(Service_NVDRV, "called, timeslice={:#x}", params.timeslice);

    if (params.timeslice < 1000 || params.timeslice > 5000) {
        return NvResult::BadParameter;
    }

    channel_timeslice = params.timeslice;

    return NvResult::Success;
}

Kernel::KEvent* nvhost_gpu::QueryEvent(u32 event_id) {
    switch (event_id) {
    case 1:
        return sm_exception_breakpoint_int_report_event;
    case 2:
        return sm_exception_breakpoint_pause_report_event;
    case 3:
        return error_notifier_event;
    default:
        if (g_nx2_driver) {
            // Switch 2 NVN also asks for event 7 and multi-waits on it with its syncpoint events.
            // Nothing signals it yet.
            auto*& event = nx2_events[event_id];
            if (event == nullptr) {
                event = events_interface.CreateEvent(fmt::format("GpuChannelNx2Event{}", event_id));
            }
            LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 channel event {}", event_id);
            return event;
        }
        LOG_CRITICAL(Service_NVDRV, "Unknown Ctrl GPU Event {}", event_id);
        return nullptr;
    }
}

} // namespace Service::Nvidia::Devices
