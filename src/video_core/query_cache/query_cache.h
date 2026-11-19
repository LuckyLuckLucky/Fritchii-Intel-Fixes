// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/fritchii_occlusion.h"
#include "video_core/fritchii_report_runs.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_report_census.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/intel_report_guard.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_runahead.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/query_cache/bank_base.h"
#include "video_core/query_cache/frame_trace.h"
#include "video_core/query_cache/query_base.h"
#include "video_core/query_cache/query_cache_base.h"
#include "video_core/one_follower_hooks.h"
#include "video_core/query_cache/query_stream.h"
#include "video_core/query_cache/types.h"

namespace VideoCommon {

using Maxwell = Tegra::Engines::Maxwell3D;

struct SyncValuesStruct {
    VAddr address;
    u64 value;
    u64 size;

    static constexpr bool GeneratesBaseBuffer = true;
};

template <typename Traits>
class GuestStreamer : public SimpleStreamer<GuestQuery> {
public:
    using RuntimeType = typename Traits::RuntimeType;

    GuestStreamer(size_t id_, RuntimeType& runtime_)
        : SimpleStreamer<GuestQuery>(id_), runtime{runtime_} {}

    virtual ~GuestStreamer() = default;

    size_t WriteCounter(VAddr address, bool has_timestamp, u32 value,
                        std::optional<u32> subreport = std::nullopt) override {
        auto new_id = BuildQuery(has_timestamp, address, static_cast<u64>(value));
        // The value is kept here too: with EXP28 & 1 the sync can come after fences that freed
        // the slot, which may hold another release by then.
        pending_sync.push_back(PendingSync{
            .id = new_id,
            .address = address,
            .value = static_cast<u64>(value),
            .has_timestamp = has_timestamp,
        });
        return new_id;
    }

    bool HasPendingSync() const override {
        return !pending_sync.empty();
    }

    void NoteSyncDeferred() override {
        if (!pending_sync.empty()) {
            sync_deferred = true;
        }
    }

    void SyncWrites() override {
        if (pending_sync.empty()) {
            sync_deferred = false;
            return;
        }
        std::vector<SyncValuesStruct> sync_values;
        sync_values.reserve(pending_sync.size());
        if (std::exchange(sync_deferred, false)) {
            SyncDeferredWrites(sync_values);
        } else {
            for (const PendingSync& pending : pending_sync) {
                auto& query = slot_queries[pending.id];
                if (True(query.flags & QueryFlagBits::IsRewritten) ||
                    True(query.flags & QueryFlagBits::IsInvalidated)) {
                    continue;
                }
                query.flags |= QueryFlagBits::IsHostSynced;
                sync_values.emplace_back(SyncValuesStruct{
                    .address = query.guest_address,
                    .value = query.value,
                    .size = static_cast<u64>(
                        True(query.flags & QueryFlagBits::HasTimestamp) ? 8 : 4)});
            }
        }
        pending_sync.clear();
        if (sync_values.size() > 0) {
            runtime.template SyncValues<SyncValuesStruct>(sync_values);
        }
    }

private:
    struct PendingSync {
        size_t id;
        VAddr address;
        u64 value;
        bool has_timestamp;
    };

    /// Syncs that waited past a fence (EXP28 & 1, the original's SyncWrites): the values
    /// as reported, newest first, one per address (one copy command must not write a range
    /// twice); a slot's flags count only while it still holds that release.
    void SyncDeferredWrites(std::vector<SyncValuesStruct>& sync_values) {
        std::vector<VAddr> seen;
        seen.reserve(pending_sync.size());
        for (auto it = pending_sync.rbegin(); it != pending_sync.rend(); ++it) {
            if (std::find(seen.begin(), seen.end(), it->address) != seen.end()) {
                continue;
            }
            seen.push_back(it->address);
            auto& query = slot_queries[it->id];
            const bool same = query.guest_address == it->address && query.value == it->value;
            if (same && (True(query.flags & QueryFlagBits::IsRewritten) ||
                         True(query.flags & QueryFlagBits::IsInvalidated))) {
                continue;
            }
            if (same) {
                query.flags |= QueryFlagBits::IsHostSynced;
            }
            sync_values.emplace_back(SyncValuesStruct{
                .address = it->address,
                .value = it->value,
                .size = static_cast<u64>(it->has_timestamp ? 8 : 4)});
        }
    }

    RuntimeType& runtime;
    std::deque<PendingSync> pending_sync;
    /// EXP28 & 1: pending_sync waited past a fence (NoteSyncDeferred).
    bool sync_deferred{};
};

template <typename Traits>
class StubStreamer : public GuestStreamer<Traits> {
public:
    using RuntimeType = typename Traits::RuntimeType;

    StubStreamer(size_t id_, RuntimeType& runtime_, u32 stub_value_)
        : GuestStreamer<Traits>(id_, runtime_), stub_value{stub_value_} {}

    ~StubStreamer() override = default;

    size_t WriteCounter(VAddr address, bool has_timestamp, [[maybe_unused]] u32 value,
                        std::optional<u32> subreport = std::nullopt) override {
        size_t new_id =
            GuestStreamer<Traits>::WriteCounter(address, has_timestamp, stub_value, subreport);
        return new_id;
    }

private:
    u32 stub_value;
};

template <typename Traits>
struct QueryCacheBase<Traits>::QueryCacheBaseImpl {
    using RuntimeType = typename Traits::RuntimeType;

    QueryCacheBaseImpl(QueryCacheBase<Traits>* owner_, VideoCore::RasterizerInterface& rasterizer_,
                       Tegra::MaxwellDeviceMemoryManager& device_memory_, RuntimeType& runtime_,
                       Tegra::GPU& gpu_)
        : owner{owner_}, rasterizer{rasterizer_}, device_memory{device_memory_}, runtime{runtime_},
          gpu{gpu_} {
        streamer_mask = 0;
        for (size_t i = 0; i < static_cast<size_t>(QueryType::MaxQueryTypes); i++) {
            streamers[i] = runtime.GetStreamerInterface(static_cast<QueryType>(i));
            if (streamers[i]) {
                streamer_mask |= 1ULL << streamers[i]->GetId();
            }
        }
    }

    template <typename Func>
    void ForEachStreamerIn(u64 mask, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result<Func, StreamerInterface*>, bool>;
        while (mask != 0) {
            size_t position = std::countr_zero(mask);
            mask &= ~(1ULL << position);
            if constexpr (RETURNS_BOOL) {
                if (func(streamers[position])) {
                    return;
                }
            } else {
                func(streamers[position]);
            }
        }
    }

    template <typename Func>
    void ForEachStreamer(Func&& func) {
        ForEachStreamerIn(streamer_mask, func);
    }

    QueryBase* ObtainQuery(QueryCacheBase<Traits>::QueryLocation location) {
        size_t which_stream = location.stream_id.Value();
        auto* streamer = streamers[which_stream];
        if (!streamer) {
            return nullptr;
        }
        return streamer->GetQuery(location.query_id.Value());
    }

    using Location = typename QueryCacheBase<Traits>::QueryLocation;

    /// What a report's operation did (EXP119=2 compares it with what its run computed).
    enum class ReportOutcome : u8 { Written, Invalidated, Unsynchronized, Unmapped };
    struct ReportResult {
        ReportOutcome outcome;
        u64 value; ///< Written: the value written (amended)
    };

    /// A report's operation: its value, amended, to guest memory (CounterReport's
    /// ReportOperation, and EXP119's runs). Fencing thread, once the fence's results are in.
    ReportResult WriteReport(QueryBase* query_base, DAddr cpu_addr, GPUVAddr addr,
                             Location query_location, QueryType report_type, bool synced,
                             size_t asid) {
        auto* const stream = streamers[query_location.stream_id.Value()];
        if (True(query_base->flags & QueryFlagBits::IsInvalidated)) {
            if (VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
                VideoCommon::Nx2ReportCensus::Add(
                    VideoCommon::Nx2ReportCensus::g_counters.invalidated);
            }
            if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Discard)) {
                LOG_INFO(HW_GPU,
                         "DKB_QUERY discard n={} as={} gpu={:#x} device={:#x} type={} "
                         "reason=invalidated flags={:#x}",
                         trace, asid, addr, cpu_addr, static_cast<u32>(report_type),
                         static_cast<u32>(query_base->flags));
            }
            if (!synced) [[likely]] {
                pending_unregister.push_back(query_location);
            }
            return {ReportOutcome::Invalidated, 0};
        }
        if (False(query_base->flags & QueryFlagBits::IsFinalValueSynced)) [[unlikely]] {
            if (VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
                VideoCommon::Nx2ReportCensus::Add(
                    VideoCommon::Nx2ReportCensus::g_counters.unsynchronized);
            }
            if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Discard)) {
                LOG_INFO(HW_GPU,
                         "DKB_QUERY discard n={} as={} gpu={:#x} device={:#x} type={} "
                         "reason=unsynchronized flags={:#x}",
                         trace, asid, addr, cpu_addr, static_cast<u32>(report_type),
                         static_cast<u32>(query_base->flags));
            }
            LOG_ERROR(HW_GPU,
                      "Query report value not synchronized. Consider increasing GPU accuracy.");
            if (!synced) [[likely]] {
                pending_unregister.push_back(query_location);
            }
            return {ReportOutcome::Unsynchronized, 0};
        }
        query_base->value += stream->GetAmendValue();
        stream->SetAccumulationValue(query_base->value);
        if (report_type == QueryType::ZPassPixelCount64 &&
            VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
            auto& census = VideoCommon::Nx2ReportCensus::g_counters;
            VideoCommon::Nx2ReportCensus::Add(query_base->value == 0 ? census.sample_zero
                                                                     : census.sample_nonzero);
        }
        const bool wide = True(query_base->flags & QueryFlagBits::HasTimestamp);
        owner->FinishReads(cpu_addr, wide ? 16 : 4);
        // Looked up here, not when the report was made (two pointers less to carry).
        u8* const pointer = device_memory.template GetPointer<u8>(cpu_addr);
        if (pointer == nullptr) [[unlikely]] {
            // Unmapped since the report: nothing to write.
            if (!synced) [[likely]] {
                pending_unregister.push_back(query_location);
            }
            return {ReportOutcome::Unmapped, 0};
        }
        if (report_type == QueryType::Payload) {
            // EXP13=2: marked before the word is written (nx2.h).
            Tegra::Nx2NoteLanded(cpu_addr, static_cast<u32>(query_base->value),
                                 Tegra::Nx2Landing::QueryCompletion);
        }
        // EDEN_NX2_RUNAHEAD: as for the immediate write in CounterReport (with the default
        // fences a fence report's operation runs while the back runs the release).
        const bool superseded = report_type == QueryType::Payload &&
                                VideoCommon::Runahead::LandingSuperseded(cpu_addr);
        if (wide) {
            const u64 ticks = gpu.GetTicks();
            u8* const ticks_pointer = device_memory.template GetPointer<u8>(cpu_addr + 8);
            if (ticks_pointer != nullptr) {
                std::memcpy(ticks_pointer, &ticks, sizeof(ticks));
            }
            if (!superseded) {
                std::memcpy(pointer, &query_base->value, sizeof(query_base->value));
            }
        } else if (!superseded) {
            const u32 word = static_cast<u32>(query_base->value);
            std::memcpy(pointer, &word, sizeof(word));
        }
        Tegra::Nx2SemaphoreWaiters::Get().OnWrite(cpu_addr, static_cast<u32>(query_base->value));
        Common::Nx2GpuWrite::Notify(Common::Nx2GpuWrite::Path::QueryCompletion, cpu_addr, addr,
                                    pointer, static_cast<u32>(query_base->value));
        if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Write)) {
            LOG_INFO(HW_GPU,
                     "DKB_QUERY write n={} path=completion as={} gpu={:#x} device={:#x} "
                     "type={} bytes={} value={:#x} flags={:#x}",
                     trace, asid, addr, cpu_addr, static_cast<u32>(report_type), wide ? 8 : 4,
                     wide ? query_base->value : static_cast<u32>(query_base->value),
                     static_cast<u32>(query_base->flags));
        }
        if (!synced) [[likely]] {
            pending_unregister.push_back(query_location);
        }
        return {ReportOutcome::Written, query_base->value};
    }

    // EXP119, report runs (video_core/fritchii_report_runs.h).

    enum class EntryKind : u8 {
        Report,      ///< A sample count report: written (1)
        Reset,       ///< A counter reset's amend reset (1)
        ReportCheck, ///< EXP119=2: computed into its verify record only
        ResetCheck,  ///< EXP119=2: resets the computation's amend only
    };

    struct RunEntry {
        QueryBase* query;  ///< Reports: the slot
        DAddr cpu_addr;
        GPUVAddr addr;
        Location location; ///< Reports: the slot's; resets: the streamer (stream_id)
        u32 verify_seq;    ///< ReportCheck: its record
        u16 asid;          ///< For traces only
        EntryKind kind;
    };

    struct Run {
        std::vector<RunEntry> entries;
        int mode{}; ///< 1 or 2, one per run
    };

    /// A run's operation in the fence manager's list. Owns the run: a copy or move takes it from
    /// its source (std::function needs a copyable callable; exactly one runs or drops it). Dropped
    /// unrun (fences discarded when emulation stops): the written reports' completions are
    /// settled as ReportOperation settles them, without touching the cache (it may be gone).
    struct RunOperation {
        QueryCacheBaseImpl* state;
        mutable Run* run;

        RunOperation(QueryCacheBaseImpl* state_, Run* run_) noexcept : state{state_}, run{run_} {}
        RunOperation(const RunOperation& other) noexcept : state{other.state}, run{other.run} {
            other.run = nullptr;
        }
        RunOperation(RunOperation&& other) noexcept
            : RunOperation(static_cast<const RunOperation&>(other)) {}
        RunOperation& operator=(const RunOperation&) = delete;
        RunOperation& operator=(RunOperation&&) = delete;
        ~RunOperation() {
            if (run != nullptr) {
                DropRun(run);
            }
        }

        void operator()() {
            Run* const taken = std::exchange(run, nullptr);
            if (taken == nullptr) {
                return;
            }
            state->ExecuteRun(*taken);
            state->RecycleRun(taken);
        }
    };
    static_assert(std::is_nothrow_move_constructible_v<RunOperation>);

    static void DropRun(Run* run) noexcept {
        for (const RunEntry& entry : run->entries) {
            if (entry.kind == EntryKind::Report) {
                Tegra::Nx2CompleteAsyncReport(entry.cpu_addr);
                // Intel fork (2026-11-19): report read guard (intel_report_guard.h): dropped.
                VideoCore::IntelReportGuard::NoteLanded(entry.cpu_addr);
            }
        }
        FritchiiReportRuns::g_run_counters.dropped_runs.fetch_add(1, std::memory_order_relaxed);
        delete run;
    }

    /// Fencing thread: the run's entries in order, each as its own operation would have run.
    void ExecuteRun(Run& run) {
        auto* const sample_stream = streamers[static_cast<size_t>(QueryType::ZPassPixelCount64)];
        // EXP119=2: the amend the reports' own operations will see (they all follow this run in
        // the list; only the resets between them change it).
        u64 check_amend = sample_stream != nullptr ? sample_stream->GetAmendValue() : 0;
        for (const RunEntry& entry : run.entries) {
            switch (entry.kind) {
            case EntryKind::Report:
                WriteReport(entry.query, entry.cpu_addr, entry.addr, entry.location,
                            QueryType::ZPassPixelCount64, false, entry.asid);
                Tegra::Nx2CompleteAsyncReport(entry.cpu_addr);
                // Intel fork (2026-11-19): report read guard (intel_report_guard.h): written.
                VideoCore::IntelReportGuard::NoteLanded(entry.cpu_addr);
                break;
            case EntryKind::Reset:
                streamers[entry.location.stream_id.Value()]->ResetAmend();
                break;
            case EntryKind::ReportCheck: {
                const ReportResult result = PeekReport(entry.query, entry.cpu_addr, check_amend);
                verify_records[entry.verify_seq & (VerifyRecordCount - 1)] = VerifyRecord{
                    .seq = entry.verify_seq,
                    .outcome = result.outcome,
                    .cpu_addr = entry.cpu_addr,
                    .value = result.value,
                };
                break;
            }
            case EntryKind::ResetCheck:
                check_amend = 0;
                break;
            }
        }
        auto& counters = FritchiiReportRuns::g_run_counters;
        counters.run_ops.fetch_add(1, std::memory_order_relaxed);
        counters.run_entries.fetch_add(run.entries.size(), std::memory_order_relaxed);
    }

    /// EXP119=2: WriteReport's outcome and value, nothing changed.
    ReportResult PeekReport(const QueryBase* query_base, DAddr cpu_addr, u64 amend) {
        if (True(query_base->flags & QueryFlagBits::IsInvalidated)) {
            return {ReportOutcome::Invalidated, 0};
        }
        if (False(query_base->flags & QueryFlagBits::IsFinalValueSynced)) {
            return {ReportOutcome::Unsynchronized, 0};
        }
        if (device_memory.template GetPointer<u8>(cpu_addr) == nullptr) {
            return {ReportOutcome::Unmapped, 0};
        }
        return {ReportOutcome::Written, query_base->value + amend};
    }

    /// EXP119=2, fencing thread, in a report's own operation after it wrote: its run must have
    /// computed the same, before it (the record is there).
    void VerifyReport(u32 seq, DAddr cpu_addr, GPUVAddr addr, const ReportResult& actual) {
        const VerifyRecord& record = verify_records[seq & (VerifyRecordCount - 1)];
        const bool ran_before = record.seq == seq;
        const bool same = ran_before && record.outcome == actual.outcome &&
                          record.cpu_addr == cpu_addr &&
                          (actual.outcome != ReportOutcome::Written || record.value == actual.value);
        auto& counters = FritchiiReportRuns::g_run_counters;
        counters.compared.fetch_add(1, std::memory_order_relaxed);
        if (same) {
            return;
        }
        counters.mismatches.fetch_add(1, std::memory_order_relaxed);
        if (FritchiiReportRuns::g_mismatches_logged.fetch_add(1, std::memory_order_relaxed) <
            FritchiiReportRuns::MismatchLogCap) {
            LOG_WARNING(HW_GPU,
                        "Report runs (EXP119=2) mismatch: report {} gpu={:#x} device={:#x}: run "
                        "{} outcome {} device={:#x} value={:#x}, its operation outcome {} "
                        "value={:#x}",
                        seq, addr, cpu_addr, ran_before ? "computed" : "had not run:",
                        static_cast<u32>(record.outcome), record.cpu_addr, record.value,
                        static_cast<u32>(actual.outcome), actual.value);
        }
    }

    /// Back thread: `entry` joins the open run when that is still the last operation queued (and
    /// of this mode), else starts a run of its own.
    void AddToRun(const RunEntry& entry, int mode) {
        if (open_run != nullptr && open_mode == mode) {
            struct Context {
                Run* run;
                const RunEntry* entry;
            } context{open_run, &entry};
            // The run is only touched when the stamp proves it is still queued, unrun.
            const auto append = [](void* raw) {
                auto* const append_context = static_cast<Context*>(raw);
                append_context->run->entries.push_back(*append_context->entry);
            };
            if (rasterizer.AppendToLastOperation(open_stamp, append, &context)) {
                return;
            }
        }
        Run* const run = TakeRun();
        run->mode = mode;
        run->entries.push_back(entry);
        open_stamp = rasterizer.SyncOperationStamped(std::function<void()>(RunOperation{this, run}));
        open_run = open_stamp != 0 ? run : nullptr;
        open_mode = mode;
        ++run_stats.runs;
    }

    Run* TakeRun() {
        {
            std::scoped_lock lock{run_pool_guard};
            if (!run_pool.empty()) {
                Run* const run = run_pool.back().release();
                run_pool.pop_back();
                return run;
            }
        }
        Run* const run = new Run{};
        run->entries.reserve(64);
        return run;
    }

    void RecycleRun(Run* run) {
        run->entries.clear();
        std::unique_ptr<Run> owned{run};
        std::scoped_lock lock{run_pool_guard};
        if (run_pool.size() < 64) {
            run_pool.push_back(std::move(owned));
        }
    }

    struct VerifyRecord {
        u32 seq;
        ReportOutcome outcome;
        DAddr cpu_addr;
        u64 value;
    };
    static constexpr size_t VerifyRecordCount = 16384;

    QueryCacheBase<Traits>* owner;
    VideoCore::RasterizerInterface& rasterizer;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    RuntimeType& runtime;
    Tegra::GPU& gpu;
    std::array<StreamerInterface*, static_cast<size_t>(QueryType::MaxQueryTypes)> streamers;
    u64 streamer_mask;
    std::mutex flush_guard;
    std::deque<u64> flushes_pending;
    std::vector<QueryCacheBase<Traits>::QueryLocation> pending_unregister;

    // EXP119 (fritchii_report_runs.h). Back thread: the open run, the stamp that names its
    // operation, the stats.
    Run* open_run{};
    u64 open_stamp{};
    int open_mode{};
    u32 next_verify_seq{};
    FritchiiReportRuns::Stats run_stats;
    std::mutex run_pool_guard;
    std::vector<std::unique_ptr<Run>> run_pool; ///< Guarded by run_pool_guard
    /// EXP119=2: by seq, written by a run and read by the report's operation after it (both on
    /// the thread running the list). Sized once by the back before its first check entry.
    std::vector<VerifyRecord> verify_records;
};

template <typename Traits>
QueryCacheBase<Traits>::QueryCacheBase(Tegra::GPU& gpu_,
                                       VideoCore::RasterizerInterface& rasterizer_,
                                       Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                       RuntimeType& runtime_)
    : cached_queries{} {
    impl = std::make_unique<QueryCacheBase<Traits>::QueryCacheBaseImpl>(
        this, rasterizer_, device_memory_, runtime_, gpu_);
}

template <typename Traits>
QueryCacheBase<Traits>::~QueryCacheBase() = default;

template <typename Traits>
void QueryCacheBase<Traits>::CounterEnable(QueryType counter_type, bool is_enabled) {
    size_t index = static_cast<size_t>(counter_type);
    StreamerInterface* streamer = impl->streamers[index];
    if (!streamer) [[unlikely]] {
        UNREACHABLE();
        return;
    }
    // FRITCHII_OCCLUSION_VISIBLE (live): sample count reports are made up on the back
    // (Maxwell3D::ProcessQueryGet), so the host counter stays stopped: no occlusion query around
    // the draws. Asked at every draw, so the counter starts again at the first draw after the
    // switch goes off; a report pair across that point reads as different, which is "visible".
    if (is_enabled && counter_type == QueryType::ZPassPixelCount64 &&
        VideoCommon::FritchiiOcclusion::Mode() != 0) [[unlikely]] {
        is_enabled = false;
    }
    if (is_enabled) {
        streamer->StartCounter();
    } else {
        streamer->PauseCounter();
    }
}

template <typename Traits>
void QueryCacheBase<Traits>::CounterClose(QueryType counter_type) {
    size_t index = static_cast<size_t>(counter_type);
    StreamerInterface* streamer = impl->streamers[index];
    if (!streamer) [[unlikely]] {
        UNREACHABLE();
        return;
    }
    streamer->CloseCounter();
}

template <typename Traits>
void QueryCacheBase<Traits>::CounterReset(QueryType counter_type) {
    size_t index = static_cast<size_t>(counter_type);
    StreamerInterface* streamer = impl->streamers[index];
    if (!streamer) [[unlikely]] {
        UNIMPLEMENTED();
        return;
    }
    // EXP119 (fritchii_report_runs.h): the amend reset goes into the report run, where its own
    // operation would have gone; with 2 its own operation still runs (keeping the run's stamp)
    // and the run only resets its computed amend.
    if (const int runs_switch = VideoCore::Nx2Debug::Experiment(FritchiiReportRuns::Switch);
        runs_switch != 0 && counter_type == QueryType::ZPassPixelCount64) [[unlikely]] {
        const int mode = runs_switch == 2 ? 2 : 1;
        auto& stats = impl->run_stats;
        stats.MaybeLog(mode);
        ++stats.resets;
        if (streamer->ResetCounterWithoutOperation()) {
            ++stats.resets_in_runs;
            QueryLocation location{};
            location.stream_id.Assign(static_cast<u32>(index));
            using Impl = QueryCacheBaseImpl;
            impl->AddToRun(
                typename Impl::RunEntry{
                    .query = nullptr,
                    .cpu_addr = 0,
                    .addr = 0,
                    .location = location,
                    .verify_seq = 0,
                    .asid = 0,
                    .kind = mode == 2 ? Impl::EntryKind::ResetCheck : Impl::EntryKind::Reset,
                },
                mode);
            if (mode == 2) {
                impl->rasterizer.SyncOperationKeepStamp(
                    std::function<void()>([streamer] { streamer->ResetAmend(); }));
            }
            return;
        }
    }
    streamer->ResetCounter();
}

template <typename Traits>
void QueryCacheBase<Traits>::BindToChannel(s32 id) {
    VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo>::BindToChannel(id);
    impl->runtime.Bind3DEngine(maxwell3d);
}

template <typename Traits>
bool QueryCacheBase<Traits>::ReportMakesPendingSync(QueryType type) const {
    const auto index = static_cast<size_t>(type);
    return index < impl->streamers.size() && impl->streamers[index] != nullptr &&
           impl->streamers[index]->ReportMakesPendingSync();
}

template <typename Traits>
bool QueryCacheBase<Traits>::ReportLandsLater(QueryType type) const {
    const auto index = static_cast<size_t>(type);
    return type != QueryType::Payload && index < impl->streamers.size() &&
           impl->streamers[index] != nullptr;
}

template <typename Traits>
void QueryCacheBase<Traits>::WritePayloadNow(DAddr cpu_addr, GPUVAddr addr, u32 payload,
                                             bool has_timestamp, bool completion) {
    FinishReads(cpu_addr, has_timestamp ? 16 : 4);
    u8* const pointer = impl->device_memory.template GetPointer<u8>(cpu_addr);
    if (pointer == nullptr) [[unlikely]] {
        return; // unmapped: nothing to write
    }
    // EXP13=2: marked before the word is written (nx2.h).
    Tegra::Nx2NoteLanded(cpu_addr, payload,
                         completion ? Tegra::Nx2Landing::QueryCompletion
                                    : Tegra::Nx2Landing::QueryImmediate);
    const bool superseded = VideoCommon::Runahead::LandingSuperseded(cpu_addr);
    if (has_timestamp) {
        const u64 ticks = impl->gpu.GetTicks();
        if (u8* const ticks_pointer = impl->device_memory.template GetPointer<u8>(cpu_addr + 8)) {
            std::memcpy(ticks_pointer, &ticks, sizeof(ticks));
        }
        if (!superseded) {
            const u64 value = static_cast<u64>(payload);
            std::memcpy(pointer, &value, sizeof(value));
        }
    } else if (!superseded) {
        std::memcpy(pointer, &payload, sizeof(payload));
    }
    Tegra::Nx2SemaphoreWaiters::Get().OnWrite(cpu_addr, payload);
    Common::Nx2GpuWrite::Notify(completion ? Common::Nx2GpuWrite::Path::QueryCompletion
                                           : Common::Nx2GpuWrite::Path::QueryImmediate,
                                cpu_addr, addr, pointer, payload);
    if (!completion && VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
        VideoCommon::Nx2ReportCensus::Add(VideoCommon::Nx2ReportCensus::g_counters.immediate);
    }
}

template <typename Traits>
void QueryCacheBase<Traits>::NotePayloadWritten(DAddr cpu_addr, bool has_timestamp, u32 payload,
                                                u32 subreport) {
    VideoCommon::OneFollower::RenderOwned("QueryCache::NotePayloadWritten");
    auto* const streamer = impl->streamers[static_cast<size_t>(QueryType::Payload)];
    streamer->Free(streamer->WriteCounter(cpu_addr, has_timestamp, payload, subreport));
}

template <typename Traits>
void QueryCacheBase<Traits>::CounterReport(GPUVAddr addr, QueryType counter_type,
                                           QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    CounterReportAt(addr, gpu_memory->GpuToCpuAddress(addr), counter_type, flags, payload,
                    subreport);
}

template <typename Traits>
void QueryCacheBase<Traits>::CounterReportAt(GPUVAddr addr, std::optional<DAddr> device,
                                             QueryType counter_type, QueryPropertiesFlags flags,
                                             u32 payload, u32 subreport) {
    VideoCommon::OneFollower::RenderOwned("QueryCache::CounterReport");
    const bool has_timestamp = True(flags & QueryPropertiesFlags::HasTimeout);
    const bool is_fence = True(flags & QueryPropertiesFlags::IsAFence);
    const auto requested_type = counter_type;
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    static const bool trace_reports = std::getenv("EDEN_NX2_QUERY_TRACE") != nullptr;
    const bool traced = trace_reports || (watch != 0 && (addr & ~GPUVAddr{0xf}) == watch);
    if (traced) {
        LOG_INFO(HW_GPU, "nx2 report addr={:#x} type={} flags={:#x} payload={:#x} as={} device={:#x}", addr,
                 static_cast<u32>(counter_type), static_cast<u32>(flags), payload, gpu_memory->GetID(),
                 gpu_memory->GpuToCpuAddress(addr).value_or(0));
    }
    size_t streamer_id = static_cast<size_t>(counter_type);
    auto* streamer = impl->streamers[streamer_id];
    if (VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
        VideoCommon::Nx2ReportCensus::NoteReport(static_cast<u32>(requested_type), has_timestamp,
                                                 is_fence, streamer == nullptr);
    }
    if (streamer == nullptr) [[unlikely]] {
        counter_type = QueryType::Payload;
        payload = 1U;
        streamer_id = static_cast<size_t>(counter_type);
        streamer = impl->streamers[streamer_id];
    }
    auto cpu_addr_opt = device;
    if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Request)) {
        LOG_INFO(HW_GPU,
                 "DKB_QUERY request n={} as={} gpu={:#x} device={:#x} mapped={} type={} "
                 "effective_type={} flags={:#x} payload={:#x} subreport={}",
                 trace, gpu_memory->GetID(), addr, cpu_addr_opt.value_or(0),
                 cpu_addr_opt.has_value(), static_cast<u32>(requested_type),
                 static_cast<u32>(counter_type), static_cast<u32>(flags), payload, subreport);
    }
    // EXP119 (fritchii_report_runs.h): a sample count report that is not a fence joins a report
    // run instead of queuing its own operation (1), or with 2 also joins one that only computes,
    // while its own operation writes and compares. Anything else: today's path.
    const int runs_switch = VideoCore::Nx2Debug::Experiment(FritchiiReportRuns::Switch);
    const int runs_mode = runs_switch == 0 ? 0 : runs_switch == 2 ? 2 : 1;
    bool in_run = false;
    if (runs_mode != 0) [[unlikely]] {
        auto& stats = impl->run_stats;
        stats.MaybeLog(runs_mode);
        ++stats.reports;
        if (requested_type != QueryType::ZPassPixelCount64 ||
            counter_type != QueryType::ZPassPixelCount64) {
            stats.Fall(FritchiiReportRuns::Fallback::OtherType);
        } else if (is_fence) {
            stats.Fall(FritchiiReportRuns::Fallback::Fence);
        } else if (traced) {
            stats.Fall(FritchiiReportRuns::Fallback::Traced);
        } else if (!cpu_addr_opt) {
            ++stats.unmapped;
        } else {
            in_run = true;
            ++stats.in_runs;
        }
    }
    if (!cpu_addr_opt) [[unlikely]] {
        return;
    }
    DAddr cpu_addr = *cpu_addr_opt;
    // Intel fork (2026-11-19, Longshot's "a count not known yet reads visible"):
    // FRITCHII_OCCL_PROVISIONAL=1 writes 4096 into a sample-count report's slot that still reads
    // 0 when the report is requested. Without delayed fences the guest passes its syncpoint 1-3
    // frames before the host GPU ran the queries, and a CPU read of the slot then saw the 0 the
    // game cleared it with: the object was dropped for a frame (sky through rocks while turning).
    // The real count overwrites it when it lands; the GPU-side 0xe2c read waits for the landing
    // as before.
    static const bool occl_provisional = [] {
        const char* const v = std::getenv("FRITCHII_OCCL_PROVISIONAL");
        return v != nullptr && v[0] == '1';
    }();
    if (occl_provisional && !is_fence && requested_type == QueryType::ZPassPixelCount64 &&
        counter_type == QueryType::ZPassPixelCount64) [[unlikely]] {
        if (u8* const pointer = impl->device_memory.template GetPointer<u8>(cpu_addr)) {
            static std::atomic<u64> written{}, kept{};
            static std::atomic<s64> window{};
            // A GROWING value (+4096 a report, never 0), written over whatever the slot holds:
            // OoT draws an object when its before and after reports differ, so a constant read
            // 4096/4096 and still hid it, and a stale count from an earlier frame could be equal
            // too (2026-11-19 user run: rarer flashes only).
            static std::atomic<u32> next{};
            u32 visible = next.fetch_add(4096, std::memory_order_relaxed) + 4096;
            if (visible == 0) [[unlikely]] {
                visible = next.fetch_add(4096, std::memory_order_relaxed) + 4096;
            }
            u32 low = 0;
            std::memcpy(&low, pointer, sizeof(low));
            (low == 0 ? written : kept).fetch_add(1, std::memory_order_relaxed);
            if (has_timestamp) {
                const u64 wide = visible;
                std::memcpy(pointer, &wide, sizeof(wide));
            } else {
                std::memcpy(pointer, &visible, sizeof(visible));
            }
            const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            s64 begun = window.load(std::memory_order_relaxed);
            if (begun == 0) {
                window.compare_exchange_strong(begun, now);
            } else if (now - begun >= 10 && window.compare_exchange_strong(begun, now)) {
                LOG_INFO(HW_GPU,
                         "Intel fork provisional occlusion counts (FRITCHII_OCCL_PROVISIONAL=1) "
                         "in {} s: {} slots read 0, {} held a count; all got a growing provisional count",
                         now - begun, written.exchange(0), kept.exchange(0));
            }
        }
    }
    const size_t new_query_id = streamer->WriteCounter(cpu_addr, has_timestamp, payload, subreport);
    auto* query = streamer->GetQuery(new_query_id);
    if (is_fence) {
        query->flags |= QueryFlagBits::IsFence;
    }
    QueryLocation query_location{};
    query_location.stream_id.Assign(static_cast<u32>(streamer_id));
    query_location.query_id.Assign(static_cast<u32>(new_query_id));
    const auto gen_caching_indexing = [](VAddr cur_addr) {
        return std::make_pair<u64, u32>(cur_addr >> Core::DEVICE_PAGEBITS,
                                        static_cast<u32>(cur_addr & Core::DEVICE_PAGEMASK));
    };
    u32 verify_seq = 0;
    if (in_run) [[unlikely]] {
        using Impl = QueryCacheBaseImpl;
        typename Impl::RunEntry entry{
            .query = query,
            .cpu_addr = cpu_addr,
            .addr = addr,
            .location = query_location,
            .verify_seq = 0,
            .asid = static_cast<u16>(gpu_memory->GetID()),
            .kind = Impl::EntryKind::Report,
        };
        if (runs_mode == 1) {
            // The completion it owes is settled by the run (or when the run is dropped).
            Tegra::Nx2IssueAsyncReport(cpu_addr);
            impl->AddToRun(entry, 1);
            // Listed as today (the split's front, guest reads and render enable look here),
            // with one hash fewer: one try_emplace where a find and an insert were.
            auto [cont_addr, base] = gen_caching_indexing(cpu_addr);
            std::scoped_lock lock(cache_mutex);
            auto& sub_container = cached_queries.try_emplace(cont_addr).first->second;
            page_filter.Mark(cpu_addr, 16);
            const auto [it_current, inserted] = sub_container.try_emplace(base, query_location);
            if (!inserted) {
                auto* old_query = impl->ObtainQuery(it_current->second);
                old_query->flags |= QueryFlagBits::IsRewritten;
                it_current->second = query_location;
            }
            return;
        }
        // 2: the run computes; the operation below (queued keeping the run's stamp, so the
        // runs take the same shape as with 1) writes, then compares.
        if (impl->verify_records.empty()) {
            impl->verify_records.resize(Impl::VerifyRecordCount);
        }
        verify_seq = ++impl->next_verify_seq;
        if (verify_seq == 0) {
            verify_seq = ++impl->next_verify_seq;
        }
        entry.verify_seq = verify_seq;
        entry.kind = Impl::EntryKind::ReportCheck;
        impl->AddToRun(entry, 2);
    }
    // EXP33: a fence-flagged report stays registered until its fence runs
    // (as Balanced already does), and its fence is made and waited for even when empty
    // (g_signaling_query_report_fence). Both halves: the fence alone, with the query freed at
    // once, left a stale operation behind (three channels waiting forever on a semaphore).
    const bool keep_fence_report = is_fence && VideoCore::Nx2Debug::Experiment(33) != 0;
    const bool is_synced = !keep_fence_report &&
                           (Settings::IsGPUFenceBehaviorDefault()
                                ? !VideoCore::Nx2Debug::AccuratePart(1)
                                : !Settings::IsGPUFenceBehaviorBalanced() &&
                                      !Settings::IsGPUFenceBehaviorAccurate()) &&
                           is_fence;
    // The report's operation, small enough for std::function to hold without allocating (a report
    // per draw allocated one each, as a ported change found): at most 48 bytes and a nothrow move,
    // or MSVC's std::function allocates whatever the size. Built only once the report is issued.
    // It owes the report's completion, counted for Nx2AsyncReportsSettled (SEM_FAST) and
    // FlushQueryResults' own-report wait (EXP30): completed on every way
    // out of the operation, written or discarded, and when the operation is dropped without
    // running (fences discarded when emulation stops), or the slot stays unequal and a later wait
    // on it never ends. A copy or move takes the debt from its source: exactly one completes it.
    struct ReportOperation {
        /// Guard: a sample-count report the Intel fork's report read guard counted
        /// (intel_report_guard.h); settling it counts it landed.
        enum : u8 { Synced = 1, Owed = 2, Guard = 4 };

        QueryCacheBase* cache;
        QueryBase* query_base;
        DAddr cpu_addr;
        GPUVAddr addr;
        QueryLocation query_location;
        u8 type;         ///< QueryType
        mutable u8 bits; ///< Synced, Owed, Guard
        u16 asid;        ///< For traces only
        u32 verify_seq;  ///< EXP119=2: its run's record, 0 = none

        ReportOperation(QueryCacheBase* cache_, QueryBase* query_base_, DAddr cpu_addr_,
                        GPUVAddr addr_, QueryLocation query_location_, QueryType type_,
                        bool synced_, size_t asid_, u32 verify_seq_, bool guard_) noexcept
            : cache{cache_}, query_base{query_base_}, cpu_addr{cpu_addr_}, addr{addr_},
              query_location{query_location_}, type{static_cast<u8>(type_)},
              bits{static_cast<u8>((synced_ ? Synced : 0) | Owed | (guard_ ? Guard : 0))},
              asid{static_cast<u16>(asid_)}, verify_seq{verify_seq_} {}
        ReportOperation(const ReportOperation& other) noexcept
            : cache{other.cache}, query_base{other.query_base}, cpu_addr{other.cpu_addr},
              addr{other.addr}, query_location{other.query_location}, type{other.type},
              bits{other.bits}, asid{other.asid}, verify_seq{other.verify_seq} {
            other.bits &= static_cast<u8>(~Owed);
        }
        ReportOperation(ReportOperation&& other) noexcept
            : ReportOperation(static_cast<const ReportOperation&>(other)) {}
        ReportOperation& operator=(const ReportOperation&) = delete;
        ReportOperation& operator=(ReportOperation&&) = delete;
        ~ReportOperation() {
            Settle();
        }

        void Settle() noexcept {
            if ((bits & Owed) != 0) {
                bits &= static_cast<u8>(~Owed);
                Tegra::Nx2CompleteAsyncReport(cpu_addr);
                if ((bits & Guard) != 0) [[unlikely]] {
                    // Intel fork (2026-11-19): written or discarded, after the write.
                    VideoCore::IntelReportGuard::NoteLanded(cpu_addr);
                }
            }
        }

        void operator()() {
            SCOPE_EXIT {
                Settle();
            };
            // The write itself is shared with EXP119's report runs (QueryCacheBaseImpl).
            auto& state = *cache->impl;
            const auto result =
                state.WriteReport(query_base, cpu_addr, addr, query_location,
                                  static_cast<QueryType>(type), (bits & Synced) != 0, asid);
            if (verify_seq != 0) [[unlikely]] {
                state.VerifyReport(verify_seq, cpu_addr, addr, result);
            }
        }
    };
    static_assert(sizeof(ReportOperation) <= 48,
                  "the report operation must fit std::function's own storage");
    static_assert(std::is_nothrow_move_constructible_v<ReportOperation>,
                  "MSVC's std::function allocates a callable without a nothrow move");
    // Issues the report, then builds its operation (which completes it).
    // Intel fork (2026-11-19): a sample-count report RasterizerVulkan::Query counted for the
    // report read guard (intel_report_guard.h) owes it a landing on every way out below.
    const bool guard_report =
        requested_type == QueryType::ZPassPixelCount64 && VideoCore::IntelReportGuard::Enabled();
    const auto issue_operation = [&] {
        Tegra::Nx2IssueAsyncReport(cpu_addr);
        return std::function<void()>(ReportOperation{this, query, cpu_addr, addr, query_location,
                                                     counter_type, is_synced,
                                                     gpu_memory->GetID(), verify_seq,
                                                     guard_report});
    };
    if (is_fence) {
        std::function<void()> operation = issue_operation();
        VideoCore::g_signaling_query_report_fence = keep_fence_report;
        impl->rasterizer.SignalFence(std::move(operation));
        VideoCore::g_signaling_query_report_fence = false;
    } else {
        if (!VideoCore::Nx2Debug::AccuratePart(1) && counter_type == QueryType::Payload) {
            u8* pointer = impl->device_memory.template GetPointer<u8>(cpu_addr);
            u8* pointer_timestamp = impl->device_memory.template GetPointer<u8>(cpu_addr + 8);
            FinishReads(cpu_addr, has_timestamp ? 16 : 4);
            // EXP13=2: marked before the word is written (nx2.h).
            Tegra::Nx2NoteLanded(cpu_addr, payload, Tegra::Nx2Landing::QueryImmediate);
            // EDEN_NX2_RUNAHEAD: the split's front wrote this word already, for this release or a
            // later one; writing it again would undo what the guest CPU did since.
            const bool superseded = VideoCommon::Runahead::LandingSuperseded(cpu_addr);
            if (has_timestamp) {
                u64 timestamp = impl->gpu.GetTicks();
                u64 value = static_cast<u64>(payload);
                std::memcpy(pointer_timestamp, &timestamp, sizeof(timestamp));
                if (!superseded) {
                    std::memcpy(pointer, &value, sizeof(value));
                }
            } else if (!superseded) {
                std::memcpy(pointer, &payload, sizeof(payload));
            }
            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(cpu_addr, payload);
            Common::Nx2GpuWrite::Notify(Common::Nx2GpuWrite::Path::QueryImmediate, cpu_addr, addr,
                                        pointer, payload);
            if (VideoCommon::Nx2ReportCensus::Enabled()) [[unlikely]] {
                VideoCommon::Nx2ReportCensus::Add(VideoCommon::Nx2ReportCensus::g_counters.immediate);
            }
            if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Write)) {
                LOG_INFO(HW_GPU,
                         "DKB_QUERY write n={} path=immediate as={} gpu={:#x} device={:#x} type={} "
                         "bytes={} value={:#x} flags={:#x}",
                         trace, gpu_memory->GetID(), addr, cpu_addr,
                         static_cast<u32>(counter_type), has_timestamp ? 8 : 4, payload,
                         static_cast<u32>(query->flags));
            }
            streamer->Free(new_query_id);
            if (guard_report) [[unlikely]] {
                VideoCore::IntelReportGuard::NoteLanded(cpu_addr); // written at once
            }
            return;
        }
        if (verify_seq != 0) [[unlikely]] {
            // EXP119=2: at its own place, keeping the run's stamp.
            impl->rasterizer.SyncOperationKeepStamp(issue_operation());
        } else {
            impl->rasterizer.SyncOperation(issue_operation());
        }
    }
    if (is_synced) {
        streamer->Free(new_query_id);
        return;
    }
    auto [cont_addr, base] = gen_caching_indexing(cpu_addr);
    {
        std::scoped_lock lock(cache_mutex);
        auto it1 = cached_queries.try_emplace(cont_addr);
        page_filter.Mark(cpu_addr, 16);
        auto& sub_container = it1.first->second;
        auto it_current = sub_container.find(base);
        if (it_current == sub_container.end()) {
            sub_container.insert_or_assign(base, query_location);
            return;
        }
        auto* old_query = impl->ObtainQuery(it_current->second);
        old_query->flags |= QueryFlagBits::IsRewritten;
        sub_container.insert_or_assign(base, query_location);
    }
}

template <typename Traits>
void QueryCacheBase<Traits>::UnregisterPending() {
    const auto gen_caching_indexing = [](VAddr cur_addr) {
        return std::make_pair<u64, u32>(cur_addr >> Core::DEVICE_PAGEBITS,
                                        static_cast<u32>(cur_addr & Core::DEVICE_PAGEMASK));
    };
    std::scoped_lock lock(cache_mutex);
    for (QueryLocation loc : impl->pending_unregister) {
        const auto [streamer_id, query_id] = loc.unpack();
        auto* streamer = impl->streamers[streamer_id];
        if (!streamer) [[unlikely]] {
            continue;
        }
        auto* query = streamer->GetQuery(query_id);
        auto [cont_addr, base] = gen_caching_indexing(query->guest_address);
        auto it1 = cached_queries.find(cont_addr);
        if (it1 != cached_queries.end()) {
            auto it2 = it1->second.find(base);
            if (it2 != it1->second.end()) {
                if (it2->second.raw == loc.raw) {
                    it1->second.erase(it2);
                }
            }
        }
        streamer->Free(query_id);
    }
    impl->pending_unregister.clear();
}

template <typename Traits>
void QueryCacheBase<Traits>::NotifyWFI() {
    bool should_sync = false;
    impl->ForEachStreamer(
        [&should_sync](StreamerInterface* streamer) { should_sync |= streamer->HasPendingSync(); });
    if (!should_sync) {
        return;
    }

    impl->ForEachStreamer([](StreamerInterface* streamer) { streamer->PresyncWrites(); });
    impl->runtime.Barriers(true);
    impl->ForEachStreamer([](StreamerInterface* streamer) { streamer->SyncWrites(); });
    impl->runtime.Barriers(false);
}

template <typename Traits>
void QueryCacheBase<Traits>::NotifySegment(bool resume) {
    if (resume) {
        impl->runtime.ResumeHostConditionalRendering();
    } else {
        impl->runtime.PauseHostConditionalRendering();
    }
}

template <typename Traits>
bool QueryCacheBase<Traits>::AccelerateHostConditionalRendering() {
    bool qc_dirty = false;
    const auto gen_lookup = [this, &qc_dirty](GPUVAddr address) -> VideoCommon::LookupData {
        auto cpu_addr_opt = gpu_memory->GpuToCpuAddress(address);
        if (!cpu_addr_opt) [[unlikely]] {
            return VideoCommon::LookupData{
                .address = 0,
                .found_query = nullptr,
            };
        }
        VAddr cpu_addr = *cpu_addr_opt;
        std::scoped_lock lock(cache_mutex);
        auto it1 = cached_queries.find(cpu_addr >> Core::DEVICE_PAGEBITS);
        if (it1 == cached_queries.end()) {
            return VideoCommon::LookupData{
                .address = cpu_addr,
                .found_query = nullptr,
            };
        }
        auto& sub_container = it1->second;
        auto it_current = sub_container.find(cpu_addr & Core::DEVICE_PAGEMASK);

        if (it_current == sub_container.end()) {
            auto it_current_2 = sub_container.find((cpu_addr & Core::DEVICE_PAGEMASK) + 4);
            if (it_current_2 == sub_container.end()) {
                return VideoCommon::LookupData{
                    .address = cpu_addr,
                    .found_query = nullptr,
                };
            }
            it_current = it_current_2;
        }
        auto* query = impl->ObtainQuery(it_current->second);
        qc_dirty |= True(query->flags & QueryFlagBits::IsHostManaged) &&
                    False(query->flags & QueryFlagBits::IsGuestSynced);
        return VideoCommon::LookupData{
            .address = cpu_addr,
            .found_query = query,
        };
    };

    // EXP119 (fritchii_report_runs.h): 1 = the Conditional mode without its lookup, the
    // IfEqual / IfNotEqual modes with both lookups under one lock; 2 = both ways, today's in
    // charge, the lookups compared.
    const int runs_switch = VideoCore::Nx2Debug::Experiment(FritchiiReportRuns::Switch);
    const int runs_mode = runs_switch == 0 ? 0 : runs_switch == 2 ? 2 : 1;
    if (runs_mode != 0) [[unlikely]] {
        impl->run_stats.MaybeLog(runs_mode);
        ++impl->run_stats.enable_calls;
    }
    // Both lookups under one lock, as gen_lookup does each.
    const auto gen_lookup_pair = [this](GPUVAddr address_1, GPUVAddr address_2, bool& dirty) {
        const auto cpu_1 = gpu_memory->GpuToCpuAddress(address_1);
        const auto cpu_2 = gpu_memory->GpuToCpuAddress(address_2);
        std::array<VideoCommon::LookupData, 2> found{
            VideoCommon::LookupData{.address = cpu_1.value_or(0), .found_query = nullptr},
            VideoCommon::LookupData{.address = cpu_2.value_or(0), .found_query = nullptr},
        };
        const std::array<bool, 2> mapped{cpu_1.has_value(), cpu_2.has_value()};
        std::scoped_lock lock(cache_mutex);
        for (size_t i = 0; i < found.size(); ++i) {
            if (!mapped[i]) {
                continue;
            }
            const VAddr cpu_addr = found[i].address;
            auto it1 = cached_queries.find(cpu_addr >> Core::DEVICE_PAGEBITS);
            if (it1 == cached_queries.end()) {
                continue;
            }
            auto& sub_container = it1->second;
            auto it_current = sub_container.find(cpu_addr & Core::DEVICE_PAGEMASK);
            if (it_current == sub_container.end()) {
                it_current = sub_container.find((cpu_addr & Core::DEVICE_PAGEMASK) + 4);
                if (it_current == sub_container.end()) {
                    continue;
                }
            }
            auto* query = impl->ObtainQuery(it_current->second);
            dirty |= True(query->flags & QueryFlagBits::IsHostManaged) &&
                     False(query->flags & QueryFlagBits::IsGuestSynced);
            found[i].found_query = query;
        }
        return found;
    };
    // IfEqual / IfNotEqual: the two lookups, one way or both (2: today's in charge).
    const auto lookup_two = [&](GPUVAddr address) {
        if (runs_mode == 1) {
            ++impl->run_stats.enable_one_lock;
            return gen_lookup_pair(address, address + 16, qc_dirty);
        }
        std::array<VideoCommon::LookupData, 2> today{gen_lookup(address), gen_lookup(address + 16)};
        if (runs_mode == 2) {
            ++impl->run_stats.enable_one_lock;
            bool pair_dirty = false;
            const auto pair = gen_lookup_pair(address, address + 16, pair_dirty);
            ++impl->run_stats.enable_compared;
            if (pair_dirty != qc_dirty || pair[0].address != today[0].address ||
                pair[1].address != today[1].address ||
                pair[0].found_query != today[0].found_query ||
                pair[1].found_query != today[1].found_query) {
                ++impl->run_stats.enable_mismatches;
            }
        }
        return today;
    };

    auto& regs = maxwell3d->regs;
    if (regs.render_enable_override != Maxwell::Regs::RenderEnable::Override::UseRenderEnable) {
        impl->runtime.EndHostConditionalRendering();
        return false;
    }
    const ComparisonMode mode = static_cast<ComparisonMode>(regs.render_enable.mode);
    const GPUVAddr address = regs.render_enable.Address();
    switch (mode) {
    case ComparisonMode::True:
        impl->runtime.EndHostConditionalRendering();
        return false;
    case ComparisonMode::False:
        impl->runtime.EndHostConditionalRendering();
        return false;
    case ComparisonMode::Conditional: {
        if (runs_mode != 0) [[unlikely]] {
            // The runtime reads only the address here (HostConditionalRenderingCompareValue):
            // the found query and qc_dirty were never used, so no lookup and no lock.
            ++impl->run_stats.enable_no_lookup;
            const auto cpu_addr_opt = gpu_memory->GpuToCpuAddress(address);
            VideoCommon::LookupData object_1{.address = cpu_addr_opt.value_or(0),
                                             .found_query = nullptr};
            if (runs_mode == 2) {
                const VideoCommon::LookupData today{gen_lookup(address)};
                ++impl->run_stats.enable_compared;
                if (today.address != object_1.address) {
                    ++impl->run_stats.enable_mismatches;
                }
                return impl->runtime.HostConditionalRenderingCompareValue(today, qc_dirty);
            }
            return impl->runtime.HostConditionalRenderingCompareValue(object_1, false);
        }
        VideoCommon::LookupData object_1{gen_lookup(address)};
        return impl->runtime.HostConditionalRenderingCompareValue(object_1, qc_dirty);
    }
    case ComparisonMode::IfEqual: {
        if (runs_mode != 0) [[unlikely]] {
            const auto objects = lookup_two(address);
            return impl->runtime.HostConditionalRenderingCompareValues(objects[0], objects[1],
                                                                       qc_dirty, true);
        }
        VideoCommon::LookupData object_1{gen_lookup(address)};
        VideoCommon::LookupData object_2{gen_lookup(address + 16)};
        return impl->runtime.HostConditionalRenderingCompareValues(object_1, object_2, qc_dirty,
                                                                   true);
    }
    case ComparisonMode::IfNotEqual: {
        if (runs_mode != 0) [[unlikely]] {
            const auto objects = lookup_two(address);
            return impl->runtime.HostConditionalRenderingCompareValues(objects[0], objects[1],
                                                                       qc_dirty, false);
        }
        VideoCommon::LookupData object_1{gen_lookup(address)};
        VideoCommon::LookupData object_2{gen_lookup(address + 16)};
        return impl->runtime.HostConditionalRenderingCompareValues(object_1, object_2, qc_dirty,
                                                                   false);
    }
    default:
        return false;
    }
}

// Async downloads
template <typename Traits>
void QueryCacheBase<Traits>::CommitAsyncFlushes() {
    // Make sure to have the results synced in Host.
    NotifyWFI();

    u64 mask{};
    {
        std::scoped_lock lk(impl->flush_guard);
        impl->ForEachStreamer([&mask](StreamerInterface* streamer) {
            bool local_result = streamer->HasUnsyncedQueries();
            if (local_result) {
                mask |= 1ULL << streamer->GetId();
            }
        });
        impl->flushes_pending.push_back(mask);
    }
    std::function<void()> func([this] { UnregisterPending(); });
    impl->rasterizer.SyncOperation(std::move(func));
    if (mask == 0) {
        return;
    }
    u64 ran_mask = ~mask;
    while (mask) {
        impl->ForEachStreamerIn(mask, [&mask, &ran_mask](StreamerInterface* streamer) {
            u64 dep_mask = streamer->GetDependentMask();
            if ((dep_mask & ~ran_mask) != 0) {
                return;
            }
            u64 index = streamer->GetId();
            ran_mask |= (1ULL << index);
            mask &= ~(1ULL << index);
            streamer->PushUnsyncedQueries();
        });
    }
}

template <typename Traits>
bool QueryCacheBase<Traits>::HasUncommittedFlushes() const {
    bool result = false;
    impl->ForEachStreamer([&result](StreamerInterface* streamer) {
        result |= streamer->HasUnsyncedQueries();
        return result;
    });
    return result;
}

template <typename Traits>
bool QueryCacheBase<Traits>::HasPendingFenceWork(bool ignore_payload_sync) const {
    bool result = false;
    StreamerInterface* const payload =
        ignore_payload_sync ? impl->streamers[static_cast<size_t>(QueryType::Payload)] : nullptr;
    impl->ForEachStreamer([&result, payload](StreamerInterface* streamer) {
        result |= streamer->HasUnsyncedQueries() ||
                  (streamer != payload && streamer->HasPendingSync());
        return result;
    });
    return result;
}

template <typename Traits>
bool QueryCacheBase<Traits>::HasPendingSync() const {
    // NotifyWFI's own test.
    bool result = false;
    impl->ForEachStreamer([&result](StreamerInterface* streamer) {
        result |= streamer->HasPendingSync();
        return result;
    });
    return result;
}

template <typename Traits>
void QueryCacheBase<Traits>::NotePayloadSyncDeferred() {
    if (StreamerInterface* const payload =
            impl->streamers[static_cast<size_t>(QueryType::Payload)];
        payload != nullptr) {
        payload->NoteSyncDeferred();
    }
}

template <typename Traits>
bool QueryCacheBase<Traits>::ShouldWaitAsyncFlushes() {
    std::scoped_lock lk(impl->flush_guard);
    return !impl->flushes_pending.empty() && impl->flushes_pending.front() != 0ULL;
}

template <typename Traits>
void QueryCacheBase<Traits>::PopAsyncFlushes() {
    u64 mask;
    {
        std::scoped_lock lk(impl->flush_guard);
        mask = impl->flushes_pending.front();
        impl->flushes_pending.pop_front();
    }
    if (mask == 0) {
        return;
    }
    u64 ran_mask = ~mask;
    while (mask) {
        impl->ForEachStreamerIn(mask, [&mask, &ran_mask](StreamerInterface* streamer) {
            u64 dep_mask = streamer->GetDependenceMask();
            if ((dep_mask & ~ran_mask) != 0) {
                return;
            }
            u64 index = streamer->GetId();
            ran_mask |= (1ULL << index);
            mask &= ~(1ULL << index);
            streamer->PopUnsyncedQueries();
        });
    }
}

// Invalidation

template <typename Traits>
void QueryCacheBase<Traits>::InvalidateQuery(QueryCacheBase<Traits>::QueryLocation location) {
    auto* query_base = impl->ObtainQuery(location);
    if (!query_base) {
        return;
    }
    query_base->flags |= QueryFlagBits::IsInvalidated;
}

template <typename Traits>
bool QueryCacheBase<Traits>::IsQueryDirty(QueryCacheBase<Traits>::QueryLocation location) {
    auto* query_base = impl->ObtainQuery(location);
    if (!query_base) {
        return false;
    }
    return True(query_base->flags & QueryFlagBits::IsHostManaged) &&
           False(query_base->flags & QueryFlagBits::IsGuestSynced);
}

template <typename Traits>
bool QueryCacheBase<Traits>::SemiFlushQueryDirty(QueryCacheBase<Traits>::QueryLocation location) {
    auto* query_base = impl->ObtainQuery(location);
    if (!query_base) {
        return false;
    }
    if (True(query_base->flags & QueryFlagBits::IsFinalValueSynced) &&
        False(query_base->flags & QueryFlagBits::IsGuestSynced)) {
        FinishReads(query_base->guest_address,
                    True(query_base->flags & QueryFlagBits::HasTimestamp) ? 8 : 4);
        auto* ptr = impl->device_memory.template GetPointer<u8>(query_base->guest_address);
        if (True(query_base->flags & QueryFlagBits::HasTimestamp)) {
            std::memcpy(ptr, &query_base->value, sizeof(query_base->value));
        } else {
            u32 value_l = static_cast<u32>(query_base->value);
            std::memcpy(ptr, &value_l, sizeof(value_l));
        }
        Common::Nx2GpuWrite::Notify(Common::Nx2GpuWrite::Path::QuerySemiFlush,
                                    query_base->guest_address, 0, ptr,
                                    static_cast<u32>(query_base->value));
        if (const u64 trace = QueryTrace::Sample(QueryTrace::Event::Write)) {
            const bool wide = True(query_base->flags & QueryFlagBits::HasTimestamp);
            LOG_INFO(HW_GPU,
                     "DKB_QUERY write n={} path=semi-flush device={:#x} stream={} slot={} "
                     "bytes={} value={:#x} flags={:#x}",
                     trace, query_base->guest_address, location.stream_id.Value(),
                     location.query_id.Value(), wide ? 8 : 4,
                     wide ? query_base->value : static_cast<u32>(query_base->value),
                     static_cast<u32>(query_base->flags));
        }
        return false;
    }
    return True(query_base->flags & QueryFlagBits::IsHostManaged) &&
           False(query_base->flags & QueryFlagBits::IsGuestSynced);
}

template <typename Traits>
void QueryCacheBase<Traits>::FinishReads(DAddr address, size_t size) {
    // EDEN_NX2_EXP49: the query cache writes results to guest memory
    // through device memory pointers, not the memory manager: land a guest read's download still
    // on its way there first, or it writes the GPU's older copy over the result later.
    // SemiFlushQueryDirty asks with cache_mutex held, which no holder of the buffer cache's lock
    // takes.
    if (VideoCore::ReadDownloadsMayCover(address, size)) [[unlikely]] {
        impl->rasterizer.FinishRead(address, size);
    }
}

template <typename Traits>
void QueryCacheBase<Traits>::RequestGuestHostSync() {
    impl->rasterizer.ReleaseFences();
}

} // namespace VideoCommon
