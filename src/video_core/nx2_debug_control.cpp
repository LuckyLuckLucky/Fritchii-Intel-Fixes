// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "common/fritchii_lock_census.h"
#include "common/host_memory.h"
#include "common/lowend_sim.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/small_card.h"
#include "video_core/texture_cache/lost_contents.h"

namespace VideoCore::Nx2Debug {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<CaptureRequest> captures;
    std::unordered_map<u64, CaptureResult> finished;
    std::deque<GpuReadRequest> gpu_reads;
    std::unordered_map<u64, GpuReadResult> gpu_read_results;
    std::vector<u64> rejected;
    u64 reject_generation{};
    u64 applied_generation{};
    bool reject_pending{};
    bool unrejected{};
    bool open{};
};

State state;
std::atomic<bool> pending{};
std::atomic<bool> any_rejected{};

void UpdatePending() {
    pending.store(!state.captures.empty() || !state.gpu_reads.empty() || state.reject_pending, std::memory_order_relaxed);
}

} // Anonymous namespace

void BeginSession() {
    std::scoped_lock lock{state.mutex};
    state.captures.clear();
    state.finished.clear();
    state.gpu_reads.clear();
    state.gpu_read_results.clear();
    state.rejected.clear();
    state.reject_pending = false;
    state.unrejected = false;
    state.applied_generation = state.reject_generation;
    state.open = true;
    any_rejected.store(false, std::memory_order_relaxed);
    UpdatePending();
}

void EndSession() {
    {
        std::scoped_lock lock{state.mutex};
        state.open = false;
        state.captures.clear();
        state.gpu_reads.clear();
        UpdatePending();
    }
    state.changed.notify_all();
}

bool HasRequests() noexcept {
    return pending.load(std::memory_order_relaxed);
}

void RequestCapture(CaptureRequest request) {
    std::scoped_lock lock{state.mutex};
    state.captures.push_back(std::move(request));
    UpdatePending();
}

void CancelCapture(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.captures, [id](const CaptureRequest& request) { return request.id == id; });
    state.finished.erase(id);
    UpdatePending();
}

std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout, [id] { return !state.open || state.finished.contains(id); });
    const auto it = state.finished.find(id);
    if (it == state.finished.end()) {
        return std::nullopt;
    }
    CaptureResult result = std::move(it->second);
    state.finished.erase(it);
    return result;
}

std::optional<CaptureRequest> TakeCapture() {
    std::scoped_lock lock{state.mutex};
    if (state.captures.empty()) {
        return std::nullopt;
    }
    CaptureRequest request = std::move(state.captures.front());
    state.captures.pop_front();
    UpdatePending();
    return request;
}

void FinishCapture(u64 id, CaptureResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.finished.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

void RequestGpuRead(GpuReadRequest request) {
    std::scoped_lock lock{state.mutex};
    state.gpu_reads.push_back(request);
    UpdatePending();
}

void CancelGpuRead(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.gpu_reads, [id](const GpuReadRequest& request) { return request.id == id; });
    state.gpu_read_results.erase(id);
    UpdatePending();
}

std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout,
                           [id] { return !state.open || state.gpu_read_results.contains(id); });
    const auto it = state.gpu_read_results.find(id);
    if (it == state.gpu_read_results.end()) {
        return std::nullopt;
    }
    GpuReadResult result = std::move(it->second);
    state.gpu_read_results.erase(it);
    return result;
}

std::optional<GpuReadRequest> TakeGpuRead() {
    std::scoped_lock lock{state.mutex};
    if (state.gpu_reads.empty()) {
        return std::nullopt;
    }
    const GpuReadRequest request = state.gpu_reads.front();
    state.gpu_reads.pop_front();
    UpdatePending();
    return request;
}

void FinishGpuRead(u64 id, GpuReadResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.gpu_read_results.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

u64 SetRejected(std::span<const u64> hashes, bool rejected) {
    std::scoped_lock lock{state.mutex};
    bool changed = false;
    for (const u64 hash : hashes) {
        const auto it = std::ranges::find(state.rejected, hash);
        if (rejected && it == state.rejected.end()) {
            state.rejected.push_back(hash);
            changed = true;
        } else if (!rejected && it != state.rejected.end()) {
            state.rejected.erase(it);
            state.unrejected = true;
            changed = true;
        }
    }
    any_rejected.store(!state.rejected.empty(), std::memory_order_relaxed);
    if (changed) {
        ++state.reject_generation;
        state.reject_pending = true;
        UpdatePending();
    }
    return state.reject_generation;
}

std::vector<u64> RejectedHashes() {
    std::scoped_lock lock{state.mutex};
    return state.rejected;
}

bool IsRejected(u64 hash) {
    if (!any_rejected.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{state.mutex};
    return std::ranges::find(state.rejected, hash) != state.rejected.end();
}

bool AnyRejected() noexcept {
    return any_rejected.load(std::memory_order_relaxed);
}

std::optional<RejectChange> TakeRejectChange() {
    std::scoped_lock lock{state.mutex};
    if (!state.reject_pending) {
        return std::nullopt;
    }
    const RejectChange change{.generation = state.reject_generation,
                              .unrejected = state.unrejected};
    state.reject_pending = false;
    state.unrejected = false;
    UpdatePending();
    return change;
}

void NoteRejectApplied(u64 generation) {
    {
        std::scoped_lock lock{state.mutex};
        state.applied_generation = (std::max)(state.applied_generation, generation);
    }
    state.changed.notify_all();
}

bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    return state.changed.wait_for(lock, timeout, [generation] {
        return !state.open || state.applied_generation >= generation;
    }) && state.applied_generation >= generation;
}

std::atomic<int>* FindSwitch(std::string_view name) {
    if (constexpr std::string_view prefix = "EDEN_NX2_EXP";
        name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix) {
        int n = 0;
        for (const char c : name.substr(prefix.size())) {
            if (c < '0' || c > '9') {
                return nullptr;
            }
            n = n * 10 + (c - '0');
        }
        if (n >= ExperimentCount) {
            return nullptr;
        }
        LoadExperiments();
        return &g_experiments[n];
    }
    if (name == "EDEN_NX2_LIST_VERIFY") {
        return &g_list_verify;
    }
    if (name == "EDEN_NX2_DMA_WAIT_WRITERS") {
        return &g_dma_wait_writers;
    }
    if (name == "EDEN_NX2_SEM_FAST") {
        return &g_sem_fast;
    }
    if (name == "EDEN_NX2_DMA_SNAPSHOT") {
        return &g_dma_snapshot;
    }
    if (name == "EDEN_NX2_LOOP_SKIP") {
        return &g_loop_skip;
    }
    if (name == "EDEN_NX2_HOST_PACE") {
        return &g_host_pace;
    }
    if (name == "EDEN_NX2_ARB_SPIN") {
        return &g_arb_spin;
    }
    if (name == "EDEN_NX2_IDLE_PAUSE") {
        return &g_idle_pause;
    }
    if (name == "EDEN_NX2_GPU_WAKE") {
        return &g_gpu_wake;
    }
    if (name == "EDEN_NX2_BACK_FENCE_STATS") {
        return &g_back_fence_stats;
    }
    if (name == "EDEN_NX2_BUFFER_CHURN_STATS") {
        return &g_buffer_churn_stats;
    }
    if (name == "EDEN_NX2_RUNAHEAD") {
        return &g_runahead;
    }
    if (name == "EDEN_NX2_RUNAHEAD_MS") {
        return &g_runahead_ms;
    }
    if (name == "EDEN_NX2_CB_DIRECT") {
        return &g_cb_direct;
    }
    if (name == "EDEN_NX2_RT_DEPTH_NOUPLOAD") {
        return &g_rt_depth_noupload;
    }
    if (name == "EDEN_NX2_FOLD_INSTANCE") {
        return &g_fold_instance;
    }
    if (name == "EDEN_NX2_OOT_MACROS") {
        return &g_oot_macros;
    }
    if (name == "EDEN_NX2_DRAW_PACKET") {
        return &g_draw_packet;
    }
    if (name == "EDEN_NX2_BANK_SLOT_STATS") {
        return &g_bank_slot_stats;
    }
    if (name == "EDEN_NX2_BANK_PAGE_TABLE") {
        return &g_bank_page_table;
    }
    if (name == "EDEN_NX2_BANK_SLOTS") {
        return &g_bank_slots;
    }
    if (name == "EDEN_NX2_BANK_SKIP_VERIFY") {
        return &g_bank_skip_verify;
    }
    if (name == "EDEN_NX2_TEX_BANK_GEN") {
        return &g_tex_bank_gen;
    }
    if (name == "EDEN_NX2_STORAGE_SYNC_SCAN") {
        return &g_storage_sync_scan;
    }
    if (name == "EDEN_NX2_STORAGE_SYNC_MEMO") {
        return &g_storage_sync_memo;
    }
    if (name == "FRITCHII_FILL_GEOM_KEEP") {
        return &g_fill_geom_keep;
    }
    if (name == "FRITCHII_FILL_UPDATE_KEEP") {
        return &g_fill_update_keep;
    }
    if (name == "FRITCHII_HANDLE_PROG_KEEP") {
        return &g_handle_prog_keep;
    }
    if (name == "FRITCHII_FILL_BRACKET_LEAN") {
        return &g_fill_bracket_lean;
    }
    if (name == "FRITCHII_REPORT_WAIT") {
        return &g_report_wait;
    }
    if (name == "FRITCHII_OCCL_LEAN") {
        return &g_occl_lean;
    }
    if (name == "FRITCHII_OCCL_FLAG") {
        return &g_occl_flag;
    }
    if (name == "FRITCHII_LOCK_CENSUS") {
        // Lives in common (the cache locks' contended path); -1 until its first read.
        return &Common::LockCensus::g_mode;
    }
    if (name == "FRITCHII_FENCE_YIELD_E") {
        return &Common::LockCensus::g_fence_yield_e;
    }
    if (name == "FRITCHII_FENCE_COPY_UNLOCKED") {
        // Lives in common (the census line's E waits); -1 until its first read.
        return &Common::LockCensus::g_fence_copy_unlocked;
    }
    if (name == "EDEN_NX2_FEED_MEMORY_OVERLAY") {
        return &g_feed_memory_overlay;
    }
    if (name == "FRITCHII_FEED_REARM") {
        return &g_feed_rearm;
    }
    if (name == "FRITCHII_TEST_FEED_OFF") {
        return &g_test_feed_off;
    }
    if (name == "FRITCHII_TEST_SPLIT_RESTART") {
        return &g_test_split_restart;
    }
    if (name == "EDEN_NX2_QMD_3D_PATCH") {
        return &g_qmd_3d_patch;
    }
    if (name == "FRITCHII_MEMSTAGE_THREAD") {
        return &g_memstage_thread;
    }
    if (name == "FRITCHII_MEMSTAGE_FULL_BINDS") {
        return &g_memstage_full_binds;
    }
    if (name == "FRITCHII_MEMSTAGE_AHEAD") {
        return &g_memstage_ahead;
    }
    if (name == "FRITCHII_MEMSTAGE_LIST_DRAWS") {
        return &g_memstage_list_draws;
    }
    if (name == "FRITCHII_MEMSTAGE_FAST_LOOP") {
        return &g_memstage_fast_loop;
    }
    if (name == "FRITCHII_MEMSTAGE_ENTRY_FAST") {
        return &g_memstage_entry_fast;
    }
    if (name == "FRITCHII_MEMSTAGE_CB_RUNS") {
        return &g_memstage_cb_runs;
    }
    if (name == "FRITCHII_MEMSTAGE_GATHER") {
        return &g_memstage_gather;
    }
    if (name == "FRITCHII_MEMSTAGE_RELEASE") {
        return &g_memstage_release;
    }
    if (name == "FRITCHII_MEMSTAGE_PASS") {
        return &g_memstage_pass;
    }
    if (name == "FRITCHII_MEMSTAGE_MARKS_AHEAD") {
        return &g_memstage_marks_ahead;
    }
    if (name == "FRITCHII_DELAY_MACRO_DRAIN_US") {
        return &g_delay_macro_drain_us;
    }
    if (name == "FRITCHII_DELAY_POLL_DRAIN_US") {
        return &g_delay_poll_drain_us;
    }
    if (name == "FRITCHII_DELAY_BEFORE") {
        return &g_delay_before;
    }
    if (name == "FRITCHII_DELAY_E2C_US") {
        return &g_delay_e2c_us;
    }
    if (name == "FRITCHII_DELAY_DISPATCH_US") {
        return &g_delay_dispatch_us;
    }
    if (name == "FRITCHII_DELAY_DRAW_US") {
        return &g_delay_draw_us;
    }
    if (name == "FRITCHII_DELAY_RUN_US") {
        return &g_delay_run_us;
    }
    if (name == "FRITCHII_DELAY_RUN_KIND") {
        return &g_delay_run_kind;
    }
    if (name == "FRITCHII_DELAY_HANDOVER_US") {
        return &g_delay_handover_us;
    }
    if (name == "FRITCHII_DELAY_HANDOVER_KIND") {
        return &g_delay_handover_kind;
    }
    if (name == "FRITCHII_DELAY_RUNKIND_US") {
        return &g_delay_runkind_us;
    }
    if (name == "FRITCHII_DELAY_RUNKIND_CLASS") {
        return &g_delay_runkind_class;
    }
    if (name == "FRITCHII_NULL_RENDER") {
        return &g_null_render;
    }
    if (name == "FRITCHII_DELAY_BACK_US") {
        return &g_delay_back_us;
    }
    if (name == "FRITCHII_DELAY_BACK_KIND") {
        return &g_delay_back_kind;
    }
    if (name == "FRITCHII_DELAY_FRONT_US") {
        return &g_delay_front_us;
    }
    if (name == "FRITCHII_DELAY_FRONT_KIND") {
        return &g_delay_front_kind;
    }
    if (name == "FRITCHII_DELAY_WORKER_US") {
        return &g_delay_worker_us;
    }
    if (name == "FRITCHII_MEMSTAGE_RUN_SPIN_US") {
        return &g_memstage_run_spin_us;
    }
    if (name == "FRITCHII_MEMSTAGE_SETUP_KEEP") {
        return &g_memstage_setup_keep;
    }
    if (name == "FRITCHII_FRONT_RAN_READS") {
        return &g_front_ran_reads;
    }
    if (name == "FRITCHII_DISPATCH_WALK_SUMMARY") {
        return &g_dispatch_walk_summary;
    }
    if (name == "FRITCHII_DISPATCH_WAIT_CENSUS") {
        return &g_dispatch_wait_census;
    }
    if (name == "FRITCHII_LAGGED_DEFER") {
        return &g_lagged_defer;
    }
    if (name == "FRITCHII_ACQUIRE_TABLE_FIRST") {
        return &g_acquire_table_first;
    }
    if (name == "FRITCHII_E2C_CENSUS") {
        return &g_e2c_census;
    }
    if (name == "FRITCHII_KILL_RT_POOL") {
        return &g_kill_rt_pool;
    }
    if (name == "FRITCHII_DEPTH_POOL_BIG") {
        return &g_depth_pool_big;
    }
    if (name == "FRITCHII_HANG_REPORT_S") {
        return &g_hang_report_s;
    }
    if (name == "FRITCHII_RAM_PREBUILT_KEEP") {
        return &g_ram_prebuilt_keep;
    }
    if (name == "FRITCHII_DEPTH_POOL_RETRY") {
        return &g_depth_pool_retry;
    }
    if (name == "FRITCHII_VRAM_POOL_FIT") {
        return &g_vram_pool_fit;
    }
    if (name == "FRITCHII_VRAM_POOL_FLOOR") {
        return &g_vram_pool_floor;
    }
    if (name == "FRITCHII_VRAM_CRITICAL_FIT") {
        return &g_vram_critical_fit;
    }
    if (name == "FRITCHII_VRAM_REPORT") {
        return &g_vram_report;
    }
    if (name == "FRITCHII_VRAM_BUDGET_MODE") {
        return &g_vram_budget_mode;
    }
    if (name == "FRITCHII_VRAM_HEADROOM_MIB") {
        return &g_vram_headroom_mib;
    }
    if (name == "FRITCHII_TC_CRITICAL_ENFORCE") {
        return &g_tc_critical_enforce;
    }
    if (name == "FRITCHII_TC_THRASH_GUARD") {
        return &g_tc_thrash_guard;
    }
    if (name == "FRITCHII_VRAM_OWNERS") {
        return &g_vram_owners;
    }
    if (name == "FRITCHII_VRAM_PACK") {
        return &g_vram_pack;
    }
    if (name == "FRITCHII_VRAM_TRIM") {
        return &g_vram_trim;
    }
    if (name == "FRITCHII_DEPTH_POOL_FIT") {
        return &g_depth_pool_fit;
    }
    if (name == "FRITCHII_VRAM_HOLES") {
        return &g_vram_holes;
    }
    if (name == "FRITCHII_SMALL_CARD") {
        return &SmallCard::g_small_card;
    }
    if (name == "FRITCHII_TEST_DEVICE_LOSS") {
        return &g_test_device_loss;
    }
    if (name == "FRITCHII_SIM_GPU_SLOW") {
        return &Common::LowEndSim::g_gpu_slow;
    }
    if (name == "FRITCHII_SIM_LINK") {
        return &Common::LowEndSim::g_link_mbps;
    }
    if (name == "FRITCHII_SIM_LINK_LOAD") {
        return &Common::LowEndSim::g_link_load_mbps;
    }
    if (name == "FRITCHII_SIM_LINK_US") {
        return &Common::LowEndSim::g_link_us;
    }
    if (name == "FRITCHII_SIM_LINK_HOG") {
        return &Common::LowEndSim::g_link_hog_mibs;
    }
    if (name == "FRITCHII_SIM_CPU_SLOW") {
        return &Common::LowEndSim::g_cpu_slow;
    }
    if (name == "FRITCHII_E2C_ON_E") {
        return &g_e2c_on_e;
    }
    if (name == "FRITCHII_STAMP_READ_PAGES") {
        return &g_stamp_read_pages;
    }
    if (name == "FRITCHII_MACRO_SAME_DRY") {
        return &g_macro_same_dry;
    }
    if (name == "FRITCHII_MACRO_SEQ_DRY") {
        return &g_macro_seq_dry;
    }
    if (name == "FRITCHII_ACQUIRE_MET_BY") {
        return &g_acquire_met_by;
    }
    if (name == "FRITCHII_GATHER_ONE_LOCK") {
        return &g_gather_one_lock;
    }
    if (name == "FRITCHII_GATHER_EMPTY_TEST") {
        return &g_gather_empty_test;
    }
    if (name == "FRITCHII_OOT_BYPASS_SAMPLED") {
        return &g_oot_bypass_sampled;
    }
    if (name == "FRITCHII_SCALED_EXTENT_CAP") {
        return &g_scaled_extent_cap;
    }
    if (name == "FRITCHII_SCALE_DEPTH_MAX") {
        return &g_scale_depth_max;
    }
    if (name == "FRITCHII_FENCE_COMMIT_FAST") {
        return &g_fence_commit_fast;
    }
    if (name == "FRITCHII_PIPELINE_KEY_HASH") {
        return &g_pipeline_key_hash;
    }
    if (name == "FRITCHII_CB_WORDS_BLOCK") {
        return &g_cb_words_block;
    }
    if (name == "FRITCHII_CB_MERGE") {
        return &g_cb_merge;
    }
    if (name == "FRITCHII_BANK_CENSUS") {
        return &g_bank_census;
    }
    if (name == "FRITCHII_BANK_FLAGGED") {
        return &g_bank_flagged;
    }
    if (name == "FRITCHII_BANK_FLAGGED_SLOTS") {
        return &g_bank_flagged_slots;
    }
    if (name == "FRITCHII_VERTEX_INPUT_COMPACT") {
        return &g_vertex_input_compact;
    }
    if (name == "FRITCHII_WORKER_SPIN") {
        return &g_worker_spin;
    }
    if (name == "FRITCHII_SLOW_GPU") {
        return &g_slow_gpu;
    }
    if (name == "FRITCHII_SLOW_GPU_MS") {
        return &g_slow_gpu_ms;
    }
    if (name == "FRITCHII_BIND_ADDR_MEMO") {
        return &g_bind_addr_memo;
    }
    if (name == "FRITCHII_STORE_MEMO") {
        return &g_store_memo;
    }
    if (name == "FRITCHII_WRITE_REMARK") {
        return &g_write_remark;
    }
    if (name == "FRITCHII_JIT_HOT_PACK") {
        return &g_jit_hot_pack;
    }
    if (name == "FRITCHII_ASYNC_REDRAWN") {
        return &g_async_redrawn;
    }
    if (name == "FRITCHII_ASYNC_COMPUTE_SKIP") {
        return &g_async_compute_skip;
    }
    if (name == "FRITCHII_COMPUTE_FAST_FIRST") {
        return &g_compute_fast_first;
    }
    if (name == "FRITCHII_OCCLUSION_VISIBLE") {
        return &g_occlusion_visible;
    }
    if (name == "FRITCHII_UNSWIZZLE_IN_PLACE") {
        return &g_unswizzle_in_place;
    }
    if (name == "FRITCHII_KEEP_BLOCKS") {
        return &g_keep_blocks;
    }
    if (name == "FRITCHII_STAGING_RING") {
        return &g_staging_ring;
    }
    if (name == "FRITCHII_LINK_STATS") {
        return &g_link_stats;
    }
    if (name == "FRITCHII_ZERO_UPLOAD") {
        return &g_zero_upload;
    }
    if (name == "FRITCHII_STALE_ZERO") {
        return &g_stale_zero;
    }
    if (name == "FRITCHII_LOST_WRITEBACK") {
        return &g_lost_writeback;
    }
    if (name == "FRITCHII_LOST_KEEP") {
        return &g_lost_keep;
    }
    if (name == "FRITCHII_HOSTTIMING_SPIN_BUDGET") {
        return &g_hosttiming_spin_budget;
    }
    if (name == "FRITCHII_HOSTTIMING_PRIORITY") {
        return &g_hosttiming_priority;
    }
    if (name == "FRITCHII_HOSTTIMING_OFF_GPU") {
        return &g_hosttiming_off_gpu;
    }
    if (name == "FRITCHII_QUERY_FLUSH_DRY") {
        return &g_query_flush_dry;
    }
    if (name == "FRITCHII_QUERY_LEND_DRY") {
        return &g_query_lend_dry;
    }
    if (name == "FRITCHII_EXTENT_CENSUS") {
        return &g_extent_census;
    }
    if (name == "FRITCHII_EXTENT_CBUF") {
        return &g_extent_cbuf;
    }
    if (name == "FRITCHII_EXTENT_SHARED") {
        return &g_extent_shared;
    }
    if (name == "FRITCHII_EXTENT_GUARD") {
        return &g_extent_guard;
    }
    if (name == "FRITCHII_STORE_FOOTPRINT") {
        return &g_store_footprint_live;
    }
    if (name == "FRITCHII_GPU_PRIORITY") {
        return &g_gpu_priority;
    }
    if (name == "FRITCHII_THREAD_QOS") {
        return &g_thread_qos;
    }
    if (name == "FRITCHII_GPU_SIBLINGS_FREE") {
        return &g_gpu_siblings_free;
    }
    if (name == "FRITCHII_FOCUS_GRAB") {
        return &g_focus_grab;
    }
    if (name == "FRITCHII_ONEF_RAISE_AFTER_LEND") {
        return &g_onef_raise_after_lend;
    }
    if (name == "FRITCHII_PACE_FIX") {
        return &g_pace_fix;
    }
    if (name == "FRITCHII_PROTECT_UNLOCKED") {
        return &Common::g_protect_unlocked;
    }
    if (name == "FRITCHII_ZERO_TRIM_CHUNK") {
        // Lives in common (the zero-page pass); -1 until the first pass reads it.
        return &Common::g_zero_trim_chunk;
    }
    // FRITCHII_ONEF and FRITCHII_ONEF_PIN are boot switches: not live.
    if (name == "FRITCHII_ONEF_VIEW") {
        return &g_onef_view;
    }
    if (name == "FRITCHII_ONEF_BUILD") {
        return &g_onef_build;
    }
    if (name == "FRITCHII_ONEF_UNLEND") {
        return &g_onef_unlend;
    }
    if (name == "FRITCHII_ONEF_VERIFY_EVERY") {
        return &g_onef_verify_every;
    }
    if (name == "FRITCHII_ONEF_AHEAD") {
        return &g_onef_ahead;
    }
    if (name == "FRITCHII_ONEF_QUERY_OPS") {
        return &g_onef_query_ops;
    }
    if (name == "FRITCHII_ONEF_WAITS_E") {
        return &g_onef_waits_e;
    }
    if (name == "FRITCHII_ONEF_INVAL_E") {
        return &g_onef_inval_e;
    }
    if (name == "FRITCHII_ONEF_LEAN") {
        return &g_onef_lean;
    }
    if (name == "FRITCHII_ONEF_COPY_E") {
        return &g_onef_copy_e;
    }
    if (name == "FRITCHII_ONEF_POSTED") {
        return &g_onef_posted;
    }
    if (name == "FRITCHII_ONEF_GATHER_E") {
        return &g_onef_gather_e;
    }
    if (name == "FRITCHII_ONEF_OP_INLINE") {
        return &g_onef_op_inline;
    }
    if (name == "FRITCHII_ONEF_OP_CLEAR") {
        return &g_onef_op_clear;
    }
    if (name == "FRITCHII_ONEF_TRACE") {
        return &g_onef_trace;
    }
    if (name == "FRITCHII_ONEF_OP_INVAL") {
        return &g_onef_op_inval;
    }
    if (name == "FRITCHII_ONEF_OP_LISTKEEP") {
        return &g_onef_op_listkeep;
    }
    if (name == "FRITCHII_ONEF_OP_DMA") {
        return &g_onef_op_dma;
    }
    if (name == "FRITCHII_ONEF_OP_DISPATCH") {
        return &g_onef_op_dispatch;
    }
    if (name == "FRITCHII_ONEF_DISPATCH_RANGES") {
        return &g_onef_dispatch_ranges;
    }
    if (name == "FRITCHII_ONEF_FRONT_OPS") {
        return &g_onef_front_ops;
    }
    if (name == "FRITCHII_FRONT_CALL_LEAN") {
        return &g_front_call_lean;
    }
    if (name == "FRITCHII_FRONT_LIST_LANE") {
        return &g_front_list_lane;
    }
    if (name == "FRITCHII_FRONT_OP_LEAN") {
        return &g_front_op_lean;
    }
    if (name == "FRITCHII_ONEF_DEAD_STORES") {
        return &g_onef_dead_stores;
    }
    if (name == "FRITCHII_ONEF_FILL_COUNT") {
        return &g_onef_fill_count;
    }
    if (name == "FRITCHII_LIST_COPY_SKIP") {
        return &g_list_copy_skip;
    }
    if (name == "FRITCHII_ONEF_FLUSH_E") {
        return &g_onef_flush_e;
    }
    if (name == "FRITCHII_ONEF_SYNC_MIRROR") {
        return &g_onef_sync_mirror;
    }
    if (name == "FRITCHII_ONEF_R_SPIN") {
        return &g_onef_r_spin;
    }
    if (name == "FRITCHII_ONEF_E_SPIN") {
        return &g_onef_e_spin;
    }
    if (name == "FRITCHII_ONEF_POST_FENCE") {
        return &g_onef_post_fence;
    }
    if (name == "FRITCHII_ONEF_LOOKUP_CHECK") {
        return &g_onef_lookup_check;
    }
    if (name == "FRITCHII_ONEF_DRY_RELEASE") {
        return &g_onef_dry_release;
    }
    if (name == "FRITCHII_ONEF_SEM_E") {
        return &g_onef_sem_e;
    }
    if (name == "FRITCHII_ONEF_SOFT_RELEASE_E") {
        return &g_onef_soft_release_e;
    }
    if (name == "FRITCHII_ONEF_POST_IDLE") {
        return &g_onef_post_idle;
    }
    if (name == "FRITCHII_ONEF_POST_FENCE_REPORT") {
        return &g_onef_post_fence_report;
    }
    if (name == "FRITCHII_ONEF_SIGNAL_ON_R") {
        return &g_onef_signal_on_r;
    }
    if (name == "FRITCHII_ONEF_WAITS_R") {
        return &g_onef_waits_r;
    }
    if (name == "FRITCHII_ONEF_MIRROR_STOP") {
        return &g_onef_mirror_stop;
    }
    if (name == "FRITCHII_ONEF_CLEAR_NODEFER") {
        return &g_onef_clear_nodefer;
    }
    if (name == "FRITCHII_ONEF_UNLENT_RESET") {
        return &g_onef_unlent_reset;
    }
    if (name == "FRITCHII_ONEF_UNLENT_VALUES") {
        return &g_onef_unlent_values;
    }
    if (name == "FRITCHII_ONEF_BIND_SEED") {
        return &g_onef_bind_seed;
    }
    if (name == "FRITCHII_ONEF_CARRY_OPS") {
        return &g_onef_carry_ops;
    }
    if (name == "FRITCHII_ONEF_UNION_FILTER") {
        return &g_onef_union_filter;
    }
    if (name == "FRITCHII_ONEF_CB_LEAN") {
        return &g_onef_cb_lean;
    }
    if (name == "FRITCHII_ONEF_KEY_KEEP") {
        return &g_onef_key_keep;
    }
    if (name == "FRITCHII_ONEF_UBO_KEEP") {
        return &g_onef_ubo_keep;
    }
    if (name == "FRITCHII_BANK_FAST") {
        return &g_bank_fast;
    }
    if (name == "FRITCHII_FRONT_SPLIT_COUNT") {
        return &g_front_split_count;
    }
    if (name == "FRITCHII_ONEF_NULL_FILL") {
        return &g_onef_null_fill;
    }
    if (name == "FRITCHII_NOTED_SUMMARY") {
        return &g_noted_summary;
    }
    if (name == "FRITCHII_NOTED_FAST") {
        return &g_noted_fast;
    }
    if (name == "FRITCHII_FRONT_TRIM_RANGE") {
        return &g_front_trim_range;
    }
    if (name == "FRITCHII_FRONT_TRIM_QMD") {
        return &g_front_trim_qmd;
    }
    if (name == "FRITCHII_FRONT_ASYNC_TABLE") {
        return &g_front_async_table;
    }
    if (name == "FRITCHII_BARE_SSBO_MIB") {
        return &g_bare_ssbo_mib;
    }
    if (name == "FRITCHII_BARE_SSBO_CLAMP") {
        return &g_bare_ssbo_clamp;
    }
    if (name == "FRITCHII_TC_COLLECT_OVER") {
        return &g_tc_collect_over;
    }
    if (name == "FRITCHII_TC_COLLECT_EVICTABLE") {
        return &g_tc_collect_evictable;
    }
    if (name == "FRITCHII_SWEEP_SUPERSEDED") {
        return &g_sweep_superseded;
    }
    if (name == "FRITCHII_COLD_SWEEP_MAX") {
        return &g_cold_sweep_max;
    }
    if (name == "FRITCHII_COVERED_RETIRE") {
        return &g_covered_retire;
    }
    if (name == "FRITCHII_COLLECT_LAND_UNDER") {
        return &g_collect_land_under;
    }
    if (name == "FRITCHII_NOTED_OVERFLOW") {
        return &g_noted_overflow;
    }
    if (name == "FRITCHII_STEP_LEAN") {
        return &g_step_lean;
    }
    if (name == "FRITCHII_REFRESH_LEAN") {
        return &g_refresh_lean;
    }
    if (name == "FRITCHII_S4_COUNT") {
        return &g_s4_count;
    }
    if (name == "FRITCHII_BIND_SKIP") {
        return &g_bind_skip;
    }
    if (name == "FRITCHII_ESPLIT_COUNT") {
        return &g_esplit_count;
    }
    if (name == "FRITCHII_ESPLIT_DEPTH") {
        return &g_esplit_depth;
    }
    if (name == "FRITCHII_ESPLIT_ASYNC_KINDS") {
        return &g_esplit_async_kinds;
    }
    if (name == "FRITCHII_ESPLIT_IDLE") {
        return &g_esplit_idle;
    }
    if (name == "FRITCHII_ESPLIT_OWED") {
        return &g_esplit_owed;
    }
    if (name == "FRITCHII_ESPLIT_CB_ASYNC") {
        return &g_esplit_cb_async;
    }
    if (name == "FRITCHII_ESPLIT_FLUSH_ASYNC") {
        return &g_esplit_flush_async;
    }
    if (name == "FRITCHII_ESPLIT_BIND_BATCH") {
        return &g_esplit_bind_batch;
    }
    return nullptr;
}

} // namespace VideoCore::Nx2Debug
