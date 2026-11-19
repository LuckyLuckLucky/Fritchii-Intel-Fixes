// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/early_compute.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_submit_times.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

bool SubmitTimes::Enabled() {
    return static_cast<bool>(VideoCommon::EarlyCompute::g_submit_times);
}

void SubmitTimes::Begin(const Device& device_, vk::CommandBuffer upload_cmdbuf) {
    recording = false;
    if (failed) {
        return;
    }
    if (!pool) {
        device = &device_;
        const auto physical = device_.GetPhysical();
        const u32 bits =
            physical.GetQueueFamilyProperties()[device_.GetGraphicsFamily()].timestampValidBits;
        period_ns = physical.GetProperties().limits.timestampPeriod;
        if (bits == 0 || bits > 64 || period_ns <= 0) {
            LOG_INFO(Render_Vulkan, "FRITCHII_SUBMIT_TIMES: the graphics queue has no timestamps");
            failed = true;
            return;
        }
        mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        try {
            pool = device_.GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = Slots * 2,
                .pipelineStatistics = 0,
            });
        } catch (const vk::Exception& error) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SUBMIT_TIMES unavailable: {}", error.what());
            failed = true;
            return;
        }
        LOG_INFO(Render_Vulkan, "FRITCHII_SUBMIT_TIMES: a GPU timestamp at the start and end of "
                                "every submission ({} slots)",
                 Slots);
    }
    slot = static_cast<u32>(sequence % Slots);
    // Not a tick of this slot any more: its queries are about to be reset.
    slot_tick[slot].store(0, std::memory_order_release);
    upload_cmdbuf.ResetQueryPool(*pool, slot * 2, 2);
    upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, slot * 2);
    recording = true;
}

void SubmitTimes::End(vk::CommandBuffer cmdbuf, u64 tick) {
    if (!recording) {
        return;
    }
    recording = false;
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, slot * 2 + 1);
    tick_slot[tick % Slots].store(slot, std::memory_order_relaxed);
    slot_tick[slot].store(tick, std::memory_order_release);
    ++sequence;
}

bool SubmitTimes::ReadSlot(u32 index, u64& start_ns, u64& end_ns) const {
    std::array<u64, 2> stamps{};
    const VkResult result = device->GetLogical().GetQueryResults(
        *pool, index * 2, 2, sizeof(stamps), stamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false;
    }
    start_ns = static_cast<u64>(static_cast<double>(stamps[0] & mask) * period_ns);
    end_ns = static_cast<u64>(static_cast<double>(stamps[1] & mask) * period_ns);
    return end_ns >= start_ns;
}

SubmitTimes::Span SubmitTimes::Measure(u64 first, u64 last, MasterSemaphore& master) const {
    Span span{};
    if (!pool || failed || first == 0 || last < first) {
        return span;
    }
    // The newest ones count: a long run's oldest submissions had mostly run at the read.
    if (last - first + 1 > MaxSpan) {
        first = last - MaxSpan + 1;
    }
    master.Refresh();
    u64 previous_end = 0;
    u64 first_start = 0;
    for (u64 tick = first; tick <= last; ++tick) {
        ++span.submissions;
        if (!master.IsFree(tick)) {
            previous_end = 0;
            continue;
        }
        const u32 index = tick_slot[tick % Slots].load(std::memory_order_relaxed);
        if (slot_tick[index].load(std::memory_order_acquire) != tick) {
            previous_end = 0;
            continue;
        }
        u64 start = 0;
        u64 end = 0;
        // Read, then make sure the slot still held this tick (not reset meanwhile).
        if (!ReadSlot(index, start, end) ||
            slot_tick[index].load(std::memory_order_acquire) != tick) {
            previous_end = 0;
            continue;
        }
        ++span.resolved;
        span.busy_ns += end - start;
        if (previous_end != 0) {
            if (start >= previous_end) {
                span.gap_ns += start - previous_end;
            } else {
                span.overlap_ns += previous_end - start;
            }
        }
        if (first_start == 0) {
            first_start = start;
        }
        span.span_ns = end > first_start ? end - first_start : 0;
        previous_end = end;
    }
    return span;
}

namespace {

constexpr u64 Ms = 1'000'000;
constexpr u64 Second = 1'000'000'000;

std::string_view BaseName(const char* file) {
    if (file == nullptr) {
        return "?";
    }
    std::string_view name{file};
    if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    return name;
}

std::string_view ThreadName(const SlowGpu::Origin& origin) {
    const auto length = static_cast<size_t>(
        std::find(origin.thread.begin(), origin.thread.end(), '\0') - origin.thread.begin());
    const std::string_view name{origin.thread.data(), length};
    return name.empty() ? std::string_view{"unnamed thread"} : name;
}

/// The top 3 pipelines of one bind point by draws or dispatches.
std::string TopPrograms(const std::array<vk::CmdStats::Program, vk::CmdStats::MaxPrograms>& table,
                        u8 count, bool is_compute) {
    std::array<u8, vk::CmdStats::MaxPrograms> order{};
    for (u8 i = 0; i < count; ++i) {
        order[i] = i;
    }
    const size_t top = (std::min)(size_t{3}, size_t{count});
    std::partial_sort(order.begin(), order.begin() + top, order.begin() + count,
                      [&](u8 a, u8 b) { return table[a].uses > table[b].uses; });
    std::string text;
    for (size_t i = 0; i < top; ++i) {
        const vk::CmdStats::Program& program = table[order[i]];
        if (program.uses == 0) {
            break;
        }
        const char* const sep = text.empty() ? "" : ", ";
        if (program.label != nullptr) {
            text += fmt::format("{}{} x{}", sep, program.label, program.uses);
        } else if (program.hash_a == 0 && program.hash_b == 0) {
            text += fmt::format("{}unnamed x{}", sep, program.uses);
        } else if (is_compute) {
            text += fmt::format("{}cs {:016x} x{}", sep, program.hash_a, program.uses);
        } else {
            text += fmt::format("{}vs {:016x} fs {:016x} x{}", sep, program.hash_a,
                                program.hash_b, program.uses);
        }
    }
    return text.empty() ? std::string{"none"} : text;
}

/// One submission's contents, for a slow line and the breadcrumbs.
std::string Describe(const vk::CmdStats& s) {
    return fmt::format(
        "draws {} ({} indirect), dispatches {} ({} indirect), render passes {}, clears {}, "
        "fills {} ({} KiB), buffer copies {} ({} KiB), image uploads {}, image downloads {}, "
        "image copies/blits {}, query ops {}, barriers {} ({} items), pipeline binds {} ({} past "
        "the table), texture decode passes {}, present passes {} ({} draws, {} dispatches); top "
        "graphics: {}; top compute: {}",
        s.draws, s.indirect_draws, s.dispatches, s.indirect_dispatches, s.render_passes,
        s.clears, s.fills, s.fill_bytes / 1024, s.buffer_copies, s.buffer_copy_bytes / 1024,
        s.image_uploads, s.image_downloads, s.image_copies, s.query_ops, s.barriers,
        s.barrier_items, s.pipeline_binds, s.untracked_binds, s.texture_passes, s.presents,
        s.present_draws, s.present_dispatches, TopPrograms(s.graphics, s.graphics_count, false),
        TopPrograms(s.compute, s.compute_count, true));
}

std::string OriginText(const SlowGpu::Origin& origin) {
    return fmt::format("{} at {}:{}{}{}{}", ThreadName(origin), BaseName(origin.file), origin.line,
                       origin.finish ? ", a Finish (the thread waits for it)" : "",
                       origin.signals ? ", signals a semaphore" : "",
                       origin.waits ? ", waits on a semaphore" : "");
}

int SlowGpuMode() {
    return VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_slow_gpu, "FRITCHII_SLOW_GPU",
                                            1);
}

} // Anonymous namespace

SlowGpu::~SlowGpu() {
    if (master_semaphore == nullptr || last_stamped_tick == 0) {
        return;
    }
    try {
        // Only what reached the queue: a tick never submitted would never signal.
        const u64 tick = (std::min)(last_stamped_tick, master_semaphore->QueueSubmittedTick());
        if (tick != 0) {
            master_semaphore->Wait(tick);
        }
    } catch (...) {
        // A lost device writes nothing more.
    }
}

u64 SlowGpu::NowNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

bool SlowGpu::CreatePool(const Device& device_) {
    device = &device_;
    const auto physical = device_.GetPhysical();
    const u32 bits =
        physical.GetQueueFamilyProperties()[device_.GetGraphicsFamily()].timestampValidBits;
    period_ns = physical.GetProperties().limits.timestampPeriod;
    if (bits == 0 || bits > 64 || period_ns <= 0) {
        LOG_INFO(Render_Vulkan, "FRITCHII_SLOW_GPU: the graphics queue has no timestamps; "
                                "breadcrumbs only");
        failed = true;
        return false;
    }
    mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    try {
        pool = device_.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = Slots * 2,
            .pipelineStatistics = 0,
        });
    } catch (const vk::Exception& error) {
        LOG_WARNING(Render_Vulkan, "FRITCHII_SLOW_GPU: no timestamps ({}); breadcrumbs only",
                    error.what());
        failed = true;
        return false;
    }
    // A host reset after each read: no reset command in the submissions.
    host_reset = device_.IsHostQueryResetSupported() &&
                 device_.GetDispatchLoader().vkResetQueryPool != nullptr;
    if (host_reset) {
        device_.GetLogical().ResetQueryPool(*pool, 0, Slots * 2);
    }
    LOG_INFO(Render_Vulkan,
             "FRITCHII_SLOW_GPU: a GPU timestamp at the start and end of every main-queue "
             "submission ({} slots, {} reset), a line for each over FRITCHII_SLOW_GPU_MS={} ms",
             Slots, host_reset ? "host" : "command buffer",
             VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_slow_gpu_ms,
                                              "FRITCHII_SLOW_GPU_MS", 50));
    return true;
}

void SlowGpu::Begin(const Device& device_, vk::CommandBuffer upload_cmdbuf,
                    MasterSemaphore& master) {
    counting = false;
    stamping = false;
    const int mode = SlowGpuMode();
    if (mode <= 0 && stamp_read == stamp_next) {
        return; // off: one load
    }
    master_semaphore = &master;
    const u64 now = NowNs();
    if (stamp_read != stamp_next) {
        Poll(master, now);
    }
    if (window_start_ns == 0) {
        window_start_ns = now;
        minute_start_ns = now;
    } else if (now - window_start_ns >= 10 * Second) {
        Summary(now, mode >= 2);
    }
    if (mode <= 0) {
        return;
    }
    counting = true;
    current = vk::CmdStats{};
    if (failed || (!pool && !CreatePool(device_))) {
        return;
    }
    if (stamp_next - stamp_read >= Slots) {
        // The GPU is 256 stamped submissions behind: this one goes untimed.
        ++unstamped;
        return;
    }
    const u32 slot = static_cast<u32>(stamp_next % Slots);
    slot_tick[slot].store(0, std::memory_order_release);
    if (!host_reset) {
        upload_cmdbuf.ResetQueryPool(*pool, slot * 2, 2);
    }
    upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, slot * 2);
    stamping = true;
}

void SlowGpu::End(vk::CommandBuffer cmdbuf, u64 tick, const Origin& origin) {
    vk::t_cmd_stats = nullptr;
    if (!counting) {
        return; // off
    }
    counting = false;
    last_end_tick = tick;
    u32 slot = ~0u;
    if (stamping) {
        stamping = false;
        slot = static_cast<u32>(stamp_next % Slots);
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, slot * 2 + 1);
        Entry& entry = ring[slot];
        entry.tick = tick;
        entry.submit_ns = 0;
        entry.submit_call_us = 0;
        entry.origin = origin;
        entry.stats = current;
        slot_tick[slot].store(tick, std::memory_order_release);
        last_stamped_tick = tick;
        ++stamp_next;
    }
    std::scoped_lock lock{crumb_mutex};
    Crumb& crumb = crumbs[crumb_next % History];
    crumb.entry.tick = tick;
    crumb.entry.submit_ns = 0;
    crumb.entry.submit_call_us = 0;
    crumb.entry.origin = origin;
    crumb.entry.stats = current;
    crumb.slot = slot;
    ++crumb_next;
}

void SlowGpu::Submitted(u64 tick, u64 call_ns) {
    if (tick != last_end_tick) {
        return; // off
    }
    const u64 return_ns = NowNs();
    const u32 call_us = static_cast<u32>(
        (std::min)((return_ns > call_ns ? return_ns - call_ns : 0) / 1000, u64{0xffffffff}));
    if (stamp_next != stamp_read) {
        Entry& entry = ring[(stamp_next - 1) % Slots];
        if (entry.tick == tick) {
            entry.submit_ns = call_ns;
            entry.submit_call_us = call_us;
        }
    }
    std::scoped_lock lock{crumb_mutex};
    if (crumb_next == 0) {
        return;
    }
    Crumb& crumb = crumbs[(crumb_next - 1) % History];
    if (crumb.entry.tick == tick) {
        crumb.entry.submit_ns = call_ns;
        crumb.entry.submit_call_us = call_us;
    }
}

u64 SlowGpu::Unwrap(u64 raw) {
    if (mask == ~u64{0}) {
        return raw;
    }
    if (!have_raw) {
        have_raw = true;
        last_raw = raw;
        last_unwrapped = raw;
        return raw;
    }
    const u64 forward = (raw - last_raw) & mask;
    if (forward <= mask / 2) {
        last_unwrapped += forward;
        last_raw = raw;
        return last_unwrapped;
    }
    // Before the newest stamp read (a start overlapping the previous end): no new high mark.
    return last_unwrapped - ((last_raw - raw) & mask);
}

void SlowGpu::Poll(MasterSemaphore& master, u64 now) {
    const u64 pending = stamp_next - stamp_read;
    // Batched: one query read for several submissions, unless they wait long.
    if (pending < 8 && now - last_poll_ns < 50 * Ms) {
        return;
    }
    last_poll_ns = now;
    if (!master.IsFree(ring[stamp_read % Slots].tick)) {
        master.Refresh();
    }
    struct Result {
        u64 value;
        u64 available;
    };
    std::array<Result, 64> results{};
    while (stamp_read != stamp_next) {
        const u32 first = static_cast<u32>(stamp_read % Slots);
        const u32 limit = static_cast<u32>((std::min)(
            {stamp_next - stamp_read, u64{Slots - first}, u64{results.size() / 2}}));
        u32 ready = 0;
        while (ready < limit && master.IsFree(ring[first + ready].tick)) {
            ++ready;
        }
        if (ready == 0) {
            return;
        }
        const VkResult result = device->GetLogical().GetQueryResults(
            *pool, first * 2, ready * 2, ready * 2 * sizeof(Result), results.data(),
            sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SLOW_GPU: timestamps off (query result {})",
                        static_cast<int>(result));
            failed = true;
            stamp_read = stamp_next;
            return;
        }
        u32 done = 0;
        for (; done < ready; ++done) {
            const Result& start = results[done * 2];
            const Result& end = results[done * 2 + 1];
            if (start.available == 0 || end.available == 0) {
                break; // done on the timeline, not visible yet: the next poll reads it
            }
            Process(ring[first + done], start.value & mask, end.value & mask, stamp_read + done,
                    now);
            slot_tick[first + done].store(0, std::memory_order_release);
        }
        if (done != 0 && host_reset) {
            device->GetLogical().ResetQueryPool(*pool, first * 2, done * 2);
        }
        stamp_read += done;
        if (done < ready) {
            return;
        }
    }
}

void SlowGpu::Process(const Entry& entry, u64 start_raw, u64 end_raw, u64 sequence, u64 now) {
    const double start_ns = static_cast<double>(Unwrap(start_raw)) * period_ns;
    const double end_ns = static_cast<double>(Unwrap(end_raw)) * period_ns;
    const double gpu_ms = end_ns > start_ns ? (end_ns - start_ns) / 1e6 : 0.0;
    // Queue wait: GPU start minus CPU submit, less the smallest such difference seen lately
    // (a submission the GPU started at once: the two clocks' offset plus the launch latency).
    double queue_ms = -1.0;
    if (entry.submit_ns != 0) {
        const double offset = start_ns - static_cast<double>(entry.submit_ns);
        offset_min = (std::min)(offset_min, offset);
        queue_ms = (offset - (std::min)(offset_min, offset_min_previous)) / 1e6;
    }
    const bool follows = previous_sequence != ~u64{0} && previous_sequence + 1 == sequence;
    const double gap_ms = follows ? (start_ns - previous_end_ns) / 1e6 : 0.0;
    previous_end_ns = end_ns;
    previous_sequence = sequence;

    ++window_timed;
    window_busy_ms += gpu_ms;
    if (gpu_ms > window_max_ms) {
        window_max_ms = gpu_ms;
        window_max_tick = entry.tick;
    }
    const int threshold = VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_slow_gpu_ms,
                                                           "FRITCHII_SLOW_GPU_MS", 50);
    if (threshold <= 0 || gpu_ms < static_cast<double>(threshold)) {
        return;
    }
    ++window_over;
    if (now - minute_start_ns >= 60 * Second) {
        minute_start_ns = now;
        minute_lines = 0;
    }
    if (minute_lines >= 20) {
        ++window_held;
        return;
    }
    ++minute_lines;
    const std::string queue_text =
        queue_ms < 0.0 ? std::string{"queue wait unknown"}
                       : fmt::format("queue wait ~{:.1f} ms", (std::max)(queue_ms, 0.0));
    const std::string gap_text =
        !follows        ? std::string{"previous submission not timed"}
        : gap_ms >= 0.0 ? fmt::format("started {:.1f} ms after the previous one ended", gap_ms)
                        : fmt::format("started {:.1f} ms before the previous one ended", -gap_ms);
    LOG_INFO(Render_Vulkan,
             "Slow GPU submission: tick {} ran {:.1f} ms on the GPU (over {} ms; {}, {}, "
             "vkQueueSubmit {} us); from {}; {}",
             entry.tick, gpu_ms, threshold, queue_text, gap_text, entry.submit_call_us,
             OriginText(entry.origin), Describe(entry.stats));
}

void SlowGpu::Summary(u64 now, bool verbose) {
    const double seconds = static_cast<double>(now - window_start_ns) / 1e9;
    if (verbose || window_over != 0 || window_held != 0 || unstamped != 0) {
        LOG_INFO(Render_Vulkan,
                 "Slow GPU {:.0f} s: {} submissions timed, {} not (the GPU {} or more behind), {} "
                 "over FRITCHII_SLOW_GPU_MS={} ms ({} lines held back, 20 a minute), longest "
                 "{:.1f} ms (tick {}), submission times sum {:.0f} ms/s",
                 seconds, window_timed, unstamped, Slots, window_over,
                 VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_slow_gpu_ms,
                                                  "FRITCHII_SLOW_GPU_MS", 50),
                 window_held, window_max_ms, window_max_tick,
                 seconds > 0.0 ? window_busy_ms / seconds : 0.0);
    }
    window_start_ns = now;
    window_timed = 0;
    window_over = 0;
    window_held = 0;
    window_busy_ms = 0.0;
    window_max_ms = 0.0;
    window_max_tick = 0;
    unstamped = 0;
    // The clocks' offset drifts slowly: the estimate uses this window's and the last one's.
    offset_min_previous = offset_min;
    offset_min = 1e300;
}

int SlowGpu::StampWritten(u32 slot, u64 tick, u32 which) const {
    if (slot >= Slots || slot_tick[slot].load(std::memory_order_acquire) != tick || !pool) {
        return -1;
    }
    struct Result {
        u64 value;
        u64 available;
    };
    Result result{};
    const VkResult status = device->GetLogical().GetQueryResults(
        *pool, slot * 2 + which, 1, sizeof(result), &result, sizeof(Result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if ((status != VK_SUCCESS && status != VK_NOT_READY) ||
        slot_tick[slot].load(std::memory_order_acquire) != tick) {
        return -1;
    }
    return result.available != 0 ? 1 : 0;
}

void SlowGpu::LogBreadcrumbs(const char* prefix, MasterSemaphore& master) const {
    // Read directly: this runs inside the device loss report, and MasterSemaphore::Refresh would
    // call the report again on a lost device, which never returns to the thread already in it.
    u64 reported = master.KnownGpuTick();
    u64 read = 0;
    const bool readable =
        VideoCore::Nx2Debug::g_test_device_loss.load(std::memory_order_relaxed) != 2 &&
        master.TryReadGpuTick(read);
    if (readable) {
        reported = (std::max)(reported, read);
    }

    std::array<Crumb, Crumbs> copy{};
    std::array<Crumb, OldestChecked> oldest{};
    u32 oldest_count = 0;
    bool older_lost = false;
    u64 next = 0;
    {
        // The worker holds it for a copy; a report never waits on a stuck thread for long.
        std::unique_lock lock{crumb_mutex, std::defer_lock};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (!lock.try_lock()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                LOG_CRITICAL(Render_Vulkan, "{}: GPU breadcrumbs: their lock is held, skipped",
                             prefix);
                return;
            }
            std::this_thread::yield();
        }
        next = crumb_next;
        const u64 newest = (std::min)(next, u64{Crumbs});
        for (u64 i = next - newest; i < next; ++i) {
            copy[i % Crumbs] = crumbs[i % History];
        }
        // Ticks rise with the index: the first kept one past what the GPU reported.
        for (u64 i = next - (std::min)(next, u64{History}); i < next; ++i) {
            const Crumb& crumb = crumbs[i % History];
            if (crumb.entry.tick <= reported) {
                continue;
            }
            older_lost = crumb.entry.tick != reported + 1;
            for (; i < next && oldest_count < OldestChecked; ++i) {
                oldest[oldest_count++] = crumbs[i % History];
            }
            break;
        }
    }
    if (next == 0) {
        LOG_CRITICAL(Render_Vulkan,
                     "{}: GPU breadcrumbs: none (FRITCHII_SLOW_GPU=0, or nothing submitted)",
                     prefix);
        return;
    }
    const std::string read_note =
        readable ? std::string{}
                 : std::string{" (its semaphore can't be read now: the last value read)"};
    const u64 submitted_tick = master.QueueSubmittedTick();
    const u64 now = NowNs();
    const auto submitted_text = [now](const Entry& entry) {
        return entry.submit_ns == 0
                   ? std::string{"not through vkQueueSubmit yet"}
                   : fmt::format("submitted {:.1f} ms ago (vkQueueSubmit {} us)",
                                 now > entry.submit_ns
                                     ? static_cast<double>(now - entry.submit_ns) / 1e6
                                     : 0.0,
                                 entry.submit_call_us);
    };
    const auto started_text = [this](const Crumb& crumb) {
        switch (Started(crumb.slot, crumb.entry.tick)) {
        case 1:
            return "NOT done, started on the GPU";
        case 0:
            return "NOT done, not started on the GPU";
        default:
            return "NOT done";
        }
    };

    // The submission holding the queue up: the first not reported done whose end stamp isn't
    // written either (the semaphore's last read can lag behind the stamps).
    u32 stuck = 0;
    while (stuck < oldest_count &&
           Ended(oldest[stuck].slot, oldest[stuck].entry.tick) == 1) {
        ++stuck;
    }
    if (stuck < oldest_count) {
        const Crumb& crumb = oldest[stuck];
        LOG_CRITICAL(Render_Vulkan,
                     "{}: GPU oldest unfinished submission: tick {}: {}; {} more submitted behind "
                     "it; the GPU reported tick {} done{}{}; {}; from {}; {}",
                     prefix, crumb.entry.tick, started_text(crumb),
                     submitted_tick > crumb.entry.tick ? submitted_tick - crumb.entry.tick : 0,
                     reported, read_note,
                     older_lost && stuck == 0
                         ? "; older unfinished ones are not kept (over 256 behind, or FRITCHII_"
                           "SLOW_GPU was off)"
                         : "",
                     submitted_text(crumb.entry), OriginText(crumb.entry.origin),
                     Describe(crumb.entry.stats));
    } else if (oldest_count == 0) {
        LOG_CRITICAL(Render_Vulkan,
                     "{}: GPU oldest unfinished submission: none kept; the GPU reported tick {} "
                     "done{}, newest submitted {}",
                     prefix, reported, read_note, submitted_tick);
    } else {
        LOG_CRITICAL(Render_Vulkan,
                     "{}: GPU oldest unfinished submission: past tick {}: the {} after the GPU's "
                     "report (tick {}{}) all have their end stamps",
                     prefix, oldest[oldest_count - 1].entry.tick, oldest_count, reported,
                     read_note);
    }

    const u64 count = (std::min)(next, u64{Crumbs});
    LOG_CRITICAL(Render_Vulkan,
                 "{}: GPU breadcrumbs: the main queue's last {} submissions, oldest first; the GPU "
                 "reported tick {} done{}; newest submitted {}",
                 prefix, count, reported, read_note, submitted_tick);
    for (u64 i = next - count; i < next; ++i) {
        const Crumb& crumb = copy[i % Crumbs];
        const Entry& entry = crumb.entry;
        LOG_CRITICAL(Render_Vulkan, "{}: GPU breadcrumb tick {}: {}; {}; from {}; {}", prefix,
                     entry.tick, entry.tick <= reported ? "done" : started_text(crumb),
                     submitted_text(entry), OriginText(entry.origin), Describe(entry.stats));
    }
}

} // namespace Vulkan
