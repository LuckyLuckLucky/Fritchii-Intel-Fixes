// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/settings.h"

/// Requests from the debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md) to the thread
/// that owns the rasterizer. The server posts them from its own thread; RasterizerVulkan takes
/// them at the end of each presented frame, so every cache they touch is used from its owner.
/// Without the server nothing is posted and the per-frame cost is one relaxed atomic load.
namespace VideoCore::Nx2Debug {

/// Bumped by every store a live switch or experiment can take (the debug server's `switch`,
/// LoadExperiments, a switch's first read in SwitchValue). Hot paths keep what they derive from
/// the switches and look again only when this moved (Maxwell3D::FrontRunMacro's feature byte).
inline std::atomic<u32> g_switch_generation{0};
inline void BumpSwitchGeneration() {
    g_switch_generation.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] inline int EnvSwitch(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? 0 : std::atoi(value); // whole number: EDEN_NX2_DMA_SNAPSHOT=10
}
/// The switch's value, read from the environment on first use (-1: not read yet). Static
/// initialization ran before the frontend applied nx2_switches.txt, so those settings were lost.
[[nodiscard]] inline int SwitchValue(std::atomic<int>& value, const char* name,
                                    int fallback = 0) {
    int current = value.load(std::memory_order_relaxed);
    if (current < 0) [[unlikely]] {
        int unset = -1;
        const int initial = std::getenv(name) != nullptr ? EnvSwitch(name) : fallback;
        if (value.compare_exchange_strong(unset, initial, std::memory_order_relaxed)) {
            BumpSwitchGeneration();
        }
        current = value.load(std::memory_order_relaxed);
    }
    return current;
}
/// EDEN_NX2_DMA_WAIT_WRITERS: docs/fritchii/switches.md.
inline std::atomic<int> g_dma_wait_writers{-1};
/// EDEN_NX2_SEM_FAST: docs/fritchii/switches.md.
inline std::atomic<int> g_sem_fast{-1};
/// EDEN_NX2_LIST_VERIFY: docs/fritchii/switches.md.
inline std::atomic<int> g_list_verify{-1};
/// EDEN_NX2_LOOP_SKIP: docs/fritchii/switches.md.
inline std::atomic<int> g_loop_skip{-1};

/// EDEN_NX2_DMA_SNAPSHOT: docs/fritchii/switches.md.
inline std::atomic<int> g_dma_snapshot{-1};

/// EDEN_NX2_ACCURATE_PARTS: docs/fritchii/switches.md.
/// Whether GPU accuracy High is on, or the EDEN_NX2_ACCURATE_PARTS bit `part` asks for it.
[[nodiscard]] inline bool AccuratePart(int part) {
    static const int parts = EnvSwitch("EDEN_NX2_ACCURATE_PARTS");
    return Settings::IsGPULevelHigh() || (parts & part) != 0;
}

/// Whether fences run their callbacks (fence-timed releases among them) once the host GPU passed
/// the work before them, not when made (FenceManager::DelayFences).
[[nodiscard]] inline bool DelayedFences() {
    return Settings::IsGPUFenceBehaviorDefault()
               ? AccuratePart(16)
               : Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
}

/// EDEN_NX2_HOST_PACE: docs/fritchii/switches.md.
inline std::atomic<int> g_host_pace{-1};

/// EDEN_NX2_ARB_SPIN: docs/fritchii/switches.md.
inline std::atomic<int> g_arb_spin{-1};
/// EDEN_NX2_IDLE_PAUSE: docs/fritchii/switches.md.
inline std::atomic<int> g_idle_pause{-1};
/// EDEN_NX2_GPU_WAKE: docs/fritchii/switches.md.
inline std::atomic<int> g_gpu_wake{-1};
[[nodiscard]] inline bool GpuWake() {
    return SwitchValue(g_gpu_wake, "EDEN_NX2_GPU_WAKE", 0) > 0;
}
/// EDEN_NX2_BACK_FENCE_STATS: docs/fritchii/switches.md.
inline std::atomic<int> g_back_fence_stats{-1};
[[nodiscard]] inline bool BackFenceStats() {
    return SwitchValue(g_back_fence_stats, "EDEN_NX2_BACK_FENCE_STATS", 0) > 0;
}
/// EDEN_NX2_RUNAHEAD: docs/fritchii/switches.md.
inline std::atomic<int> g_runahead{-1};
/// EDEN_NX2_RUNAHEAD_MS: docs/fritchii/switches.md.
inline std::atomic<int> g_runahead_ms{-1};
/// EDEN_NX2_BUFFER_CHURN_STATS: docs/fritchii/switches.md.
inline std::atomic<int> g_buffer_churn_stats{-1};

/// EDEN_NX2_CB_DIRECT: docs/fritchii/switches.md.
inline std::atomic<int> g_cb_direct{-1};
/// EDEN_NX2_RT_DEPTH_NOUPLOAD: docs/fritchii/switches.md.
inline std::atomic<int> g_rt_depth_noupload{-1};
/// EDEN_NX2_FOLD_INSTANCE: docs/fritchii/switches.md.
inline std::atomic<int> g_fold_instance{-1};
/// EDEN_NX2_OOT_MACROS: docs/fritchii/switches.md.
inline std::atomic<int> g_oot_macros{-1};
/// EDEN_NX2_DRAW_PACKET: docs/fritchii/switches.md.
inline std::atomic<int> g_draw_packet{-1};
/// EDEN_NX2_BANK_SLOT_STATS: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_slot_stats{-1};
/// EDEN_NX2_BANK_PAGE_TABLE: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_page_table{-1};
/// EDEN_NX2_BANK_SLOTS: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_slots{-1};
/// EDEN_NX2_BANK_SKIP_VERIFY: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_skip_verify{-1};
/// EDEN_NX2_TEX_BANK_GEN: docs/fritchii/switches.md.
inline std::atomic<int> g_tex_bank_gen{-1};
/// EDEN_NX2_STORAGE_SYNC_SCAN: docs/fritchii/switches.md.
inline std::atomic<int> g_storage_sync_scan{-1};
/// EDEN_NX2_STORAGE_SYNC_MEMO: docs/fritchii/switches.md.
inline std::atomic<int> g_storage_sync_memo{-1};
/// FRITCHII_FILL_GEOM_KEEP (live, default 0): fable_b 25.2 cut e, the vertex and index binds'
/// synchronize skipped while the range's tracker managers stay clean (1 on, 2 verify, 3 dry count).
inline std::atomic<int> g_fill_geom_keep{-1};
/// FRITCHII_FILL_UPDATE_KEEP (live, default 0): fable_b 25.2 cut f, the index and vertex bindings
/// kept while their registers, the mappings and the buffers are unchanged (1 on, 2 verify, 3 dry).
inline std::atomic<int> g_fill_update_keep{-1};
/// FRITCHII_HANDLE_PROG_KEEP (live, default 0): fable_b 25.2 cut g, E's handle program results kept
/// while every word they read is unchanged (1 on, 2 verify, 3 dry count).
inline std::atomic<int> g_handle_prog_keep{-1};
/// FRITCHII_FILL_BRACKET_LEAN (live, default 0): fable_b 25.2 cut h, E's tracker record only for the
/// slots R can read, ring B's push without the window counters (1 on, 2 verify, 3 dry count).
inline std::atomic<int> g_fill_bracket_lean{-1};
/// FRITCHII_REPORT_WAIT (live, default 0 = EXP30 decides, as before): a macro read's wait for
/// pending query reports (FlushQueryResults). 1 = wait for those reports only (EXP30's wait),
/// 2 = verify: that wait, then the old one, the words read after each compared (must be 0).
inline std::atomic<int> g_report_wait{-1};
/// FRITCHII_OCCL_LEAN (live, default 0; fritchii_occl_lean.h): a draw that writes no colour takes
/// its pipeline without the pixel program when that program can change nothing else (1 on,
/// 2 verify: siblings made and compared, the full pipeline draws, 3 dry: census and timer only).
inline std::atomic<int> g_occl_lean{-1};
/// FRITCHII_OCCL_FLAG (fritchii_occl_flag.h; set at each boot from a switch or the Occlusion
/// speed-up setting): occlusion test draws store a flag instead of running a host query
/// (2 verify: compared with the host queries, which still decide).
inline std::atomic<int> g_occl_flag{-1};
/// FRITCHII_TEST_DEVICE_LOSS (live, TEST, default 0): 1 = the next queue submission reports a
/// device loss, so the report and the exit can be tested (docs/fritchii/switches.md).
inline std::atomic<int> g_test_device_loss{0};
/// EDEN_NX2_FEED_MEMORY_OVERLAY: docs/fritchii/switches.md.
inline std::atomic<int> g_feed_memory_overlay{-1};
/// FRITCHII_FEED_REARM: docs/fritchii/switches.md.
inline std::atomic<int> g_feed_rearm{-1};
/// FRITCHII_TEST_FEED_OFF (test only): docs/fritchii/switches.md.
inline std::atomic<int> g_test_feed_off{-1};
/// FRITCHII_TEST_SPLIT_RESTART (test only): docs/fritchii/switches.md.
inline std::atomic<int> g_test_split_restart{-1};
/// EDEN_NX2_QMD_3D_PATCH: docs/fritchii/switches.md.
inline std::atomic<int> g_qmd_3d_patch{-1};
/// FRITCHII_MEMSTAGE_THREAD: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_thread{-1};
/// FRITCHII_MEMSTAGE_FULL_BINDS: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_full_binds{-1};
/// FRITCHII_MEMSTAGE_AHEAD: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_ahead{-1};
/// FRITCHII_MEMSTAGE_LIST_DRAWS: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_list_draws{-1};
/// FRITCHII_MEMSTAGE_FAST_LOOP: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_fast_loop{-1};
/// FRITCHII_MEMSTAGE_ENTRY_FAST: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_entry_fast{-1};
/// FRITCHII_MEMSTAGE_CB_RUNS: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_cb_runs{-1};
/// FRITCHII_MEMSTAGE_GATHER: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_gather{-1};
/// FRITCHII_MEMSTAGE_RELEASE: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_release{-1};
/// FRITCHII_MEMSTAGE_PASS: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_pass{-1};
/// FRITCHII_MEMSTAGE_MARKS_AHEAD: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_marks_ahead{-1};
/// FRITCHII_DELAY_MACRO_DRAIN_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_macro_drain_us{-1};
/// FRITCHII_DELAY_POLL_DRAIN_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_poll_drain_us{-1};
/// FRITCHII_DELAY_BEFORE: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_before{-1};
/// FRITCHII_DELAY_E2C_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_e2c_us{-1};
/// FRITCHII_DELAY_DISPATCH_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_dispatch_us{-1};
/// FRITCHII_DELAY_DRAW_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_draw_us{-1};
/// FRITCHII_DELAY_RUN_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_run_us{-1};
/// FRITCHII_DELAY_RUN_KIND: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_run_kind{-1};
/// FRITCHII_DELAY_HANDOVER_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_handover_us{-1};
/// FRITCHII_DELAY_HANDOVER_KIND: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_handover_kind{-1};
/// FRITCHII_DELAY_RUNKIND_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_runkind_us{-1};
/// FRITCHII_DELAY_RUNKIND_CLASS: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_runkind_class{-1};
/// FRITCHII_NULL_RENDER: docs/fritchii/switches.md.
inline std::atomic<int> g_null_render{-1};
/// FRITCHII_DELAY_BACK_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_back_us{-1};
/// FRITCHII_DELAY_BACK_KIND: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_back_kind{-1};
/// FRITCHII_DELAY_FRONT_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_front_us{-1};
/// FRITCHII_DELAY_FRONT_KIND: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_front_kind{-1};
/// FRITCHII_DELAY_WORKER_US: docs/fritchii/switches.md.
inline std::atomic<int> g_delay_worker_us{-1};
/// FRITCHII_MEMSTAGE_RUN_SPIN_US: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_run_spin_us{-1};
/// FRITCHII_MEMSTAGE_SETUP_KEEP: docs/fritchii/switches.md.
inline std::atomic<int> g_memstage_setup_keep{-1};
/// FRITCHII_FRONT_RAN_READS: docs/fritchii/switches.md.
inline std::atomic<int> g_front_ran_reads{-1};
/// FRITCHII_DISPATCH_WALK_SUMMARY: docs/fritchii/switches.md.
inline std::atomic<int> g_dispatch_walk_summary{-1};
/// FRITCHII_DISPATCH_WAIT_CENSUS: docs/fritchii/switches.md.
inline std::atomic<int> g_dispatch_wait_census{-1};
/// FRITCHII_LAGGED_DEFER: docs/fritchii/switches.md.
inline std::atomic<int> g_lagged_defer{-1};
/// FRITCHII_ACQUIRE_TABLE_FIRST: docs/fritchii/switches.md.
inline std::atomic<int> g_acquire_table_first{-1};
/// FRITCHII_E2C_CENSUS: docs/fritchii/switches.md.
inline std::atomic<int> g_e2c_census{-1};
/// FRITCHII_KILL_RT_POOL: docs/fritchii/switches.md.
inline std::atomic<int> g_kill_rt_pool{-1};
/// FRITCHII_DEPTH_POOL_BIG: docs/fritchii/switches.md.
inline std::atomic<int> g_depth_pool_big{-1};
/// FRITCHII_HANG_REPORT_S: docs/fritchii/switches.md (default 5).
inline std::atomic<int> g_hang_report_s{-1};
/// FRITCHII_RAM_PREBUILT_KEEP: docs/fritchii/switches.md.
inline std::atomic<int> g_ram_prebuilt_keep{-1};
/// FRITCHII_DEPTH_POOL_RETRY: docs/fritchii/switches.md.
inline std::atomic<int> g_depth_pool_retry{-1};
/// FRITCHII_VRAM_POOL_FIT: docs/fritchii/switches.md.
inline std::atomic<int> g_vram_pool_fit{-1};
/// FRITCHII_VRAM_POOL_FLOOR: docs/fritchii/switches.md (0 off, 1 on).
inline std::atomic<int> g_vram_pool_floor{-1};
/// FRITCHII_VRAM_CRITICAL_FIT: docs/fritchii/switches.md.
inline std::atomic<int> g_vram_critical_fit{-1};
/// FRITCHII_VRAM_REPORT: docs/fritchii/switches.md.
inline std::atomic<int> g_vram_report{-1};
/// FRITCHII_VRAM_BUDGET_MODE: docs/fritchii/switches.md. 0 fixed, 1 vk, 2 os.
inline std::atomic<int> g_vram_budget_mode{-1};
/// FRITCHII_VRAM_BUDGET_MODE (live, default vk): where the texture cache's live budget cap comes
/// from. The environment takes a name (fixed, vk, os) or its number; the debug server's `switch`
/// takes the number.
[[nodiscard]] inline int VramBudgetMode() {
    int current = g_vram_budget_mode.load(std::memory_order_relaxed);
    if (current < 0) [[unlikely]] {
        int initial = 1;
        if (const char* value = std::getenv("FRITCHII_VRAM_BUDGET_MODE"); value != nullptr) {
            const std::string_view text{value};
            if (text == "fixed" || text == "off") {
                initial = 0;
            } else if (text == "vk") {
                initial = 1;
            } else if (text == "os") {
                initial = 2;
            } else {
                initial = std::atoi(value);
            }
            initial = initial < 0 ? 0 : (initial > 2 ? 2 : initial);
        }
        int unset = -1;
        if (g_vram_budget_mode.compare_exchange_strong(unset, initial,
                                                       std::memory_order_relaxed)) {
            BumpSwitchGeneration();
        }
        current = g_vram_budget_mode.load(std::memory_order_relaxed);
    }
    return current;
}
/// FRITCHII_VRAM_HEADROOM_MIB: docs/fritchii/switches.md (0 = auto).
inline std::atomic<int> g_vram_headroom_mib{-1};
/// FRITCHII_TC_CRITICAL_ENFORCE: docs/fritchii/switches.md.
inline std::atomic<int> g_tc_critical_enforce{-1};
/// FRITCHII_TC_THRASH_GUARD: docs/fritchii/switches.md (0 off, 1 on, 2 counts only).
inline std::atomic<int> g_tc_thrash_guard{-1};
/// FRITCHII_VRAM_OWNERS: docs/fritchii/switches.md (seconds between lines, 0 = off).
inline std::atomic<int> g_vram_owners{-1};
/// FRITCHII_VRAM_PACK: docs/fritchii/switches.md (0 off, 1 on, 2 auto = with the small card).
inline std::atomic<int> g_vram_pack{-1};
/// FRITCHII_VRAM_TRIM: docs/fritchii/switches.md (0 off, 1 auto, 2-100 percent of the budget).
inline std::atomic<int> g_vram_trim{-1};
/// FRITCHII_DEPTH_POOL_FIT: docs/fritchii/switches.md (bits: 1 size classes, 2 small depth pool).
inline std::atomic<int> g_depth_pool_fit{-1};
/// FRITCHII_VRAM_HOLES: docs/fritchii/switches.md (1 = hole and pinner histograms in the owners line).
inline std::atomic<int> g_vram_holes{-1};
/// FRITCHII_E2C_ON_E: docs/fritchii/switches.md.
inline std::atomic<int> g_e2c_on_e{-1};
/// FRITCHII_STAMP_READ_PAGES: docs/fritchii/switches.md.
inline std::atomic<int> g_stamp_read_pages{-1};
/// FRITCHII_MACRO_SAME_DRY: docs/fritchii/switches.md.
inline std::atomic<int> g_macro_same_dry{-1};
/// FRITCHII_MACRO_SEQ_DRY: docs/fritchii/switches.md.
inline std::atomic<int> g_macro_seq_dry{-1};
/// FRITCHII_ACQUIRE_MET_BY: docs/fritchii/switches.md.
inline std::atomic<int> g_acquire_met_by{-1};
/// FRITCHII_GATHER_ONE_LOCK: docs/fritchii/switches.md.
inline std::atomic<int> g_gather_one_lock{-1};
/// FRITCHII_GATHER_EMPTY_TEST: docs/fritchii/switches.md.
inline std::atomic<int> g_gather_empty_test{-1};
/// FRITCHII_OOT_BYPASS_SAMPLED: docs/fritchii/switches.md.
inline std::atomic<int> g_oot_bypass_sampled{-1};
/// FRITCHII_SCALED_EXTENT_CAP: docs/fritchii/switches.md.
inline std::atomic<int> g_scaled_extent_cap{-1};
/// FRITCHII_SCALE_DEPTH_MAX: docs/fritchii/switches.md.
inline std::atomic<int> g_scale_depth_max{-1};
/// FRITCHII_FENCE_COMMIT_FAST: docs/fritchii/switches.md.
inline std::atomic<int> g_fence_commit_fast{-1};
/// FRITCHII_PIPELINE_KEY_HASH: docs/fritchii/switches.md.
inline std::atomic<int> g_pipeline_key_hash{-1};
/// FRITCHII_CB_WORDS_BLOCK: docs/fritchii/switches.md.
inline std::atomic<int> g_cb_words_block{-1};
/// FRITCHII_CB_MERGE (live, default 0; 1 = dry count): docs/fritchii/switches.md.
inline std::atomic<int> g_cb_merge{-1};
/// FRITCHII_BANK_CENSUS: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_census{-1};
/// FRITCHII_BANK_FLAGGED: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_flagged{-1};
/// FRITCHII_BANK_FLAGGED_SLOTS: docs/fritchii/switches.md.
inline std::atomic<int> g_bank_flagged_slots{-1};
/// FRITCHII_VERTEX_INPUT_COMPACT: docs/fritchii/switches.md.
inline std::atomic<int> g_vertex_input_compact{-1};
/// FRITCHII_WORKER_SPIN: docs/fritchii/switches.md.
inline std::atomic<int> g_worker_spin{-1};
/// FRITCHII_SLOW_GPU: docs/fritchii/switches.md (vk_submit_times.h SlowGpu).
inline std::atomic<int> g_slow_gpu{-1};
/// FRITCHII_SLOW_GPU_MS: docs/fritchii/switches.md.
inline std::atomic<int> g_slow_gpu_ms{-1};
/// FRITCHII_BIND_ADDR_MEMO: docs/fritchii/switches.md.
inline std::atomic<int> g_bind_addr_memo{-1};
/// FRITCHII_STORE_MEMO: docs/fritchii/switches.md.
inline std::atomic<int> g_store_memo{-1};
/// FRITCHII_WRITE_REMARK (live, default 1): docs/fritchii/switches.md.
inline std::atomic<int> g_write_remark{-1};
/// FRITCHII_JIT_HOT_PACK: docs/fritchii/switches.md.
inline std::atomic<int> g_jit_hot_pack{-1};
/// FRITCHII_ASYNC_REDRAWN (live, default 0, 1 on AMD's Windows driver or once the driver compiles
/// prepared pipelines again; -1 = unset, see PipelineCache::AsyncRedrawnOn):
/// docs/fritchii/switches.md.
inline std::atomic<int> g_async_redrawn{-1};
/// FRITCHII_ASYNC_COMPUTE_SKIP (live, default 0; 2 = dry): docs/fritchii/switches.md
/// (renderer_vulkan/vk_async_compute_skip.h).
inline std::atomic<int> g_async_compute_skip{-1};
/// FRITCHII_COMPUTE_FAST_FIRST (live, default 0; 2 = test): docs/fritchii/switches.md
/// (renderer_vulkan/vk_compute_pipeline.cpp).
inline std::atomic<int> g_compute_fast_first{-1};
/// FRITCHII_OCCLUSION_VISIBLE (live, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_occlusion_visible{-1};
/// FRITCHII_UNSWIZZLE_IN_PLACE (live, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_unswizzle_in_place{-1};
/// FRITCHII_KEEP_BLOCKS (live, MiB, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_keep_blocks{-1};
/// FRITCHII_STAGING_RING (boot size, live use, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_staging_ring{-1};
/// FRITCHII_LINK_STATS (live, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_link_stats{-1};
/// FRITCHII_ZERO_UPLOAD (live, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_zero_upload{-1};
/// FRITCHII_HOSTTIMING_SPIN_BUDGET (live, percent, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_hosttiming_spin_budget{-1};
/// FRITCHII_HOSTTIMING_PRIORITY (live, default 0): docs/fritchii/switches.md (core_timing.cpp).
inline std::atomic<int> g_hosttiming_priority{-1};
/// FRITCHII_HOSTTIMING_OFF_GPU (live, default 0): docs/fritchii/switches.md (core_timing.cpp).
inline std::atomic<int> g_hosttiming_off_gpu{-1};
/// FRITCHII_QUERY_FLUSH_DRY (live, default 0): docs/fritchii/switches.md (vk_rasterizer.cpp).
inline std::atomic<int> g_query_flush_dry{-1};
/// FRITCHII_QUERY_LEND_DRY (live, default 0): docs/fritchii/switches.md (vk_rasterizer.cpp).
inline std::atomic<int> g_query_lend_dry{-1};
/// FRITCHII_EXTENT_CENSUS (live, default 0): docs/fritchii/switches.md (buffer_cache.h).
inline std::atomic<int> g_extent_census{-1};
/// FRITCHII_EXTENT_CBUF (live, default 0; =3's read counts need it at boot): docs/fritchii/
/// switches.md (buffer_cache.h, read_run_stats.h).
inline std::atomic<int> g_extent_cbuf{-1};
/// FRITCHII_EXTENT_SHARED (live, default 0; =3's read counts need it at boot): docs/fritchii/
/// switches.md (buffer_cache.h, read_run_stats.h).
inline std::atomic<int> g_extent_shared{-1};
/// FRITCHII_EXTENT_GUARD (live, default 0; =3's read counts need it at boot): docs/fritchii/
/// switches.md (buffer_cache.h, read_run_stats.h).
inline std::atomic<int> g_extent_guard{-1};
/// FRITCHII_STORE_FOOTPRINT (live 0/1/2/3 once armed: non-zero at boot; default 0): docs/fritchii/
/// switches.md (store_footprint_live.h, buffer_cache.h).
inline std::atomic<int> g_store_footprint_live{-1};
/// FRITCHII_GPU_PRIORITY (live, default 0): docs/fritchii/switches.md (fritchii_focus.h).
inline std::atomic<int> g_gpu_priority{-1};
/// FRITCHII_THREAD_QOS (live, default 0): docs/fritchii/switches.md (fritchii_focus.h).
inline std::atomic<int> g_thread_qos{-1};
/// FRITCHII_GPU_SIBLINGS_FREE (live, default 0): docs/fritchii/switches.md (common/thread.cpp,
/// KeepOffGpuCores).
inline std::atomic<int> g_gpu_siblings_free{-1};
/// FRITCHII_FOCUS_GRAB (live, default 0): docs/fritchii/switches.md (fritchii_focus.h).
inline std::atomic<int> g_focus_grab{-1};
/// FRITCHII_ONEF_RAISE_AFTER_LEND (live, default 0): docs/fritchii/switches.md.
inline std::atomic<int> g_onef_raise_after_lend{-1};
/// FRITCHII_PACE_FIX (live, default 0): docs/fritchii/switches.md (vk_host_pacer.cpp,
/// vk_present_manager.cpp).
inline std::atomic<int> g_pace_fix{-1};
/// FRITCHII_ONEF (boot): one follower step 1, the render thread R (one_follower.h).
inline std::atomic<int> g_onef{-1};
/// FRITCHII_ONEF_VIEW (live): step 2, needs FRITCHII_ONEF=1.
inline std::atomic<int> g_onef_view{-1};
/// FRITCHII_ONEF_BUILD (live): step 3, needs FRITCHII_ONEF_VIEW=1.
inline std::atomic<int> g_onef_build{-1};
/// FRITCHII_ONEF_UNLEND (live): step 4's draw kind mask, needs FRITCHII_ONEF_BUILD=1.
inline std::atomic<int> g_onef_unlend{-1};
/// FRITCHII_ONEF_VERIFY_EVERY (live): every Nth unlent draw forced lent and compared.
inline std::atomic<int> g_onef_verify_every{-1};
/// FRITCHII_ONEF_AHEAD (live): packets E may run ahead of R (0 = the ring).
inline std::atomic<int> g_onef_ahead{-1};
/// FRITCHII_ONEF_PIN (boot, default 1): R on PinnedRole::GpuMem's core.
inline std::atomic<int> g_onef_pin{-1};
/// FRITCHII_ONEF_E_PIPELINE (boot, default 1): fable_b 10.2 rule 3, E owns the pipeline lookup.
inline std::atomic<int> g_onef_e_pipeline{-1};
/// FRITCHII_ONEF_LEAN (boot, default 0): one follower at step 4 from boot with the verified
/// configuration and no compares (docs/fritchii/switches.md).
inline std::atomic<int> g_onef_lean{-1};
/// FRITCHII_ONEF_QUERY_OPS (live, default 0): fable_b 11.1 item 1, queries without parking E.
inline std::atomic<int> g_onef_query_ops{-1};
/// FRITCHII_ONEF_WAITS_E (live, default 0): fable_b 11.2, the front's waits on game memory and
/// lists read E's position.
inline std::atomic<int> g_onef_waits_e{-1};
/// FRITCHII_ONEF_INVAL_E (live, default 0): fable_b 11.1 item 2, the draw's invalidation on E.
inline std::atomic<int> g_onef_inval_e{-1};
/// FRITCHII_ONEF_COPY_E (live, default 0): fable_b 11.1 item 6's first piece, inline uploads on E.
inline std::atomic<int> g_onef_copy_e{-1};
/// FRITCHII_ONEF_POSTED (live, default 0): fable_b 12.2, posted is the default, lent the
/// exception (docs/fritchii/switches.md).
inline std::atomic<int> g_onef_posted{-1};
/// FRITCHII_ONEF_GATHER_E (live, default 0): fable_b 12.3 fix 5, the guest-write gathers on E.
inline std::atomic<int> g_onef_gather_e{-1};
/// FRITCHII_ONEF_OP_INLINE (live, default 0): fable_b 13.4 group 1, inline uploads' buffer
/// commands posted as ring B op packets.
inline std::atomic<int> g_onef_op_inline{-1};
/// FRITCHII_ONEF_OP_CLEAR (live, default 0): fable_b 13.4 group 3, clears as render ops.
inline std::atomic<int> g_onef_op_clear{-1};
/// FRITCHII_ONEF_TRACE (boot, default 0): fable_b 15.5's lend trace (one record per lend and
/// per posted op) to the log folder.
inline std::atomic<int> g_onef_trace{-1};
/// FRITCHII_ONEF_OP_INVAL (live, default 0): fable_b 13.4, the invalidation site's buffer commands posted as op packets.
inline std::atomic<int> g_onef_op_inval{-1};
/// FRITCHII_ONEF_OP_LISTKEEP (live, default 0): fable_b 15.4, kept list words posted (E's buffer side, R's copy as a render op).
inline std::atomic<int> g_onef_op_listkeep{-1};
/// FRITCHII_ONEF_OP_DMA (live, default 0): fable_b 13.4, buffer-to-buffer DMA copies and clears on E, posted.
inline std::atomic<int> g_onef_op_dma{-1};
/// FRITCHII_ONEF_OP_DISPATCH (live, default 0): fable_b 13.4 group 6, compute dispatches as posted packets.
inline std::atomic<int> g_onef_op_dispatch{-1};
/// FRITCHII_ONEF_DISPATCH_RANGES (live, default 0): fable_b 27.9, a posted image-writing dispatch records its images' guest ranges on E (EXP10) instead of "may write anything".
inline std::atomic<int> g_onef_dispatch_ranges{-1};
/// FRITCHII_ONEF_FRONT_OPS (live, default 0): fable_b 27/28 S2-S3, six hot OoT macros sent as one
/// op entry each (macro_ops_oot.h): 1 E expands the op, 2 + check, 3 E applies it directly, 4 + check.
inline std::atomic<int> g_onef_front_ops{-1};
/// FRITCHII_FRONT_CALL_LEAN (live, default 0): fable_b 27.2 B, the front's macro call set-up from
/// switch values kept in the front state and a whole call's words read in the list (2 = + compare).
inline std::atomic<int> g_front_call_lean{-1};
/// FRITCHII_FRONT_LIST_LANE (live, default 0): fable_b 27.3, FrontWrite's same-value exit where the
/// write starts: bit 1 command-list words, bit 2 handler writes (with FRITCHII_FRONT_CALL_LEAN),
/// bit 4 verify instead of drop (must-be-0 count).
inline std::atomic<int> g_front_list_lane{-1};
/// FRITCHII_FRONT_OP_LEAN (live, default 0): fable_b2 1.4, a lean decide for FRITCHII_ONEF_FRONT_OPS'
/// recorded writes (2 = + compared with the generic decide, must-be-0 count).
inline std::atomic<int> g_front_op_lean{-1};
/// FRITCHII_ONEF_DEAD_STORES (live, default 0): fable_b 27.7, 1 counts the 0xea0 trailing c[0][0x640]
/// stores the next 0xea0 overwrites with nothing between (count only; dropping them is not built).
inline std::atomic<int> g_onef_dead_stores{-1};
/// FRITCHII_ONEF_FILL_COUNT (live, default 0): fable_b 28.5 S4, 1 counts bound constant buffer
/// ranges whose bytes E reads differ from the front's noted upload words (count only).
inline std::atomic<int> g_onef_fill_count{-1};
/// FRITCHII_LIST_COPY_SKIP (live, default 0): fable_b 27.9 item 3, a CPU-written list entry skips the "list writers" wait when no queued writer's noted range touches its pages.
inline std::atomic<int> g_list_copy_skip{-1};
/// FRITCHII_ONEF_FLUSH_E (live, default 0): fable_b 17.3 (a), a flush E tests clean is not lent.
inline std::atomic<int> g_onef_flush_e{-1};
/// FRITCHII_ONEF_SYNC_MIRROR (live, default 0): fable_b 16.2's samples counter mirror, dry.
inline std::atomic<int> g_onef_sync_mirror{-1};
/// FRITCHII_ONEF_POST_FENCE (live, default 0): fable_b 16.3, clean sync points, references and
/// forced fence releases posted.
inline std::atomic<int> g_onef_post_fence{-1};
/// FRITCHII_ONEF_R_SPIN / FRITCHII_ONEF_E_SPIN (live, pauses, default 8192 = as before): how long
/// R (idle, waiting for packets) and E (parked, waiting) spin before they sleep (fable_b 23.3: R
/// spins through half its time on a game core's sibling with FRITCHII_ONEF_PIN=2).
inline std::atomic<int> g_onef_r_spin{-1};
inline std::atomic<int> g_onef_e_spin{-1};
/// FRITCHII_ONEF_LOOKUP_CHECK (live, default 0 = off; N = every Nth unlent draw): fable_b 18.4's
/// "lookup programs", "lookup blend", "lookup vertex input" must-be-0s.
inline std::atomic<int> g_onef_lookup_check{-1};
/// FRITCHII_ONEF_DRY_RELEASE (live, default 0): fable_b 19.5 step 3's dry price.
inline std::atomic<int> g_onef_dry_release{-1};
/// FRITCHII_ONEF_SEM_E (live, default 0; with POST_FENCE): fable_b 19.4, 19.6 and 20.3, the
/// sync point increments, the SignalFence callbacks and the payload writes on E.
inline std::atomic<int> g_onef_sem_e{-1};
/// FRITCHII_ONEF_SOFT_RELEASE_E (live, default 0): fable_b 22.3, a non-forced ReleaseFences
/// (the command list end) is answered on E: with the fencing thread it does nothing.
inline std::atomic<int> g_onef_soft_release_e{-1};
/// FRITCHII_ONEF_POST_IDLE (live, default 0; with POST_FENCE): fable_b 22.3, WaitForIdle posted
/// in 16.2's form: 1 only with R caught up, 2 also with R behind but no report landing due.
inline std::atomic<int> g_onef_post_idle{-1};
/// FRITCHII_ONEF_POST_FENCE_REPORT (live, default 0; with POST_FENCE): fence-flagged counter
/// reports posted as the rasterizer's fences are.
inline std::atomic<int> g_onef_post_fence_report{-1};
/// FRITCHII_ONEF_SIGNAL_ON_R (live, default 0; with SEM_E): the guest-visible signals SEM_E
/// would make at E's place happen at R's played place instead: a payload release is a posted op
/// R writes when it plays it (E goes on), a fence-flagged payload is written by R in its posted
/// fence, a sync point is signalled by R (posted fence kind 0), a SignalFence callback is lent.
/// The guest then never sees work done that R has not recorded yet (AMD tester, test 4 §5c).
inline std::atomic<int> g_onef_signal_on_r{-1};
/// FRITCHII_ONEF_WAITS_R (live, default 0): the front's waits that FRITCHII_ONEF_WAITS_E (and
/// SEM_E's lagged acquire check) end at E's position end at R's played position instead; the
/// same as WAITS_E=0 plus SEM_E's lagged check on R, as one override over a preset.
inline std::atomic<int> g_onef_waits_r{-1};
/// FRITCHII_ONEF_MIRROR_STOP (live, default 0): "a samples report: E's mirror started, R's
/// counter not" (R skipped a draw E's mirror counted). 1 = stop POST_FENCE for the session, as
/// the opposite case does; 2 = when R skipped a draw with the samples count on since the last
/// samples report, counted as explained (no must-be-0 note; E's mirror is a superset there).
inline std::atomic<int> g_onef_mirror_stop{-1};
/// FRITCHII_ONEF_CLEAR_NODEFER (live, default 0): diagnostic, a posted clear on R is never
/// deferred into the next render pass's load op (the lean darkness of 15 Nov).
inline std::atomic<int> g_onef_clear_nodefer{-1};
/// FRITCHII_ONEF_UNLENT_RESET (live, default 0): fable_b 19 step 2, E runs the indirect reset
/// test itself and lends only a draw that is one.
inline std::atomic<int> g_onef_unlent_reset{-1};
/// FRITCHII_ONEF_UNLENT_VALUES (live, default 0): fable_b 19 step 2, the engine hint, the macro
/// mode and the constant buffer bindings go to R's view by value when they change.
inline std::atomic<int> g_onef_unlent_values{-1};
/// FRITCHII_ONEF_BIND_SEED (live, default 0): fable_b 19 step 2, the lent channel bind re-seeds
/// R's view and E's tracking inside its own call (no lent draw after a bind).
inline std::atomic<int> g_onef_bind_seed{-1};
/// FRITCHII_ONEF_CARRY_OPS (live, default 0): fable_b 22.3 step 2, samples counter resets and
/// reports ride in E's next packet instead of an op packet each.
inline std::atomic<int> g_onef_carry_ops{-1};
/// FRITCHII_ONEF_UNION_FILTER (live, default 0): fable_b 14.4 cut 5, E's invalidations test the
/// union of R's page filters first.
inline std::atomic<int> g_onef_union_filter{-1};
/// FRITCHII_ONEF_CB_LEAN (live, default 0, bits): fable_b 23.1 cut 3, 1 = single constant buffer
/// words through the block write, 2 = the stream-write guard tests the union filter first.
inline std::atomic<int> g_onef_cb_lean{-1};
/// FRITCHII_ONEF_KEY_KEEP (live, default 0): fable_b 23.1 cut 2, 1 = count the clean lookups
/// with no watched word written since their snapshot (compare still run), 2 = skip the compare.
inline std::atomic<int> g_onef_key_keep{-1};
/// FRITCHII_ONEF_UBO_KEEP (live, default 0): fable_b 25.2 cut a, 1 = count the streamed uniform
/// binds a keep could skip and verify them, 2 = bind the slot's copy again without the read.
inline std::atomic<int> g_onef_ubo_keep{-1};
/// FRITCHII_BANK_FAST (live, default 0): fable_b 25.2 cut b, a settled native bank of a slot the
/// bank check skips whole is skipped before the check's switch reads and upkeep.
inline std::atomic<int> g_bank_fast{-1};
/// FRITCHII_FRONT_SPLIT_COUNT (live, default 0): the front split's step FS0, dry
/// (fritchii_front_split.h): 1 = stage timers 1 in 64 list entries, the 2a/2c counts, the 0xe2c
/// answers, front time per channel.
inline std::atomic<int> g_front_split_count{-1};
/// FRITCHII_ONEF_NULL_FILL (live, default 0; test only, the picture breaks): 1 = E skips the
/// flush, the fill and the push of every draw it would fill (FrontSplit::NullFill).
inline std::atomic<int> g_onef_null_fill{-1};
/// FRITCHII_NOTED_SUMMARY (live, default 0): F4, GpuSplit::NotedWriteSince answers from the
/// newest note position per page bucket before its walk: 1 = on, 2 = verify (both run, the walk
/// answers), 3 = dry (both run, counted and timed 1 in 64).
inline std::atomic<int> g_noted_summary{-1};
/// FRITCHII_NOTED_FAST (live, default 0): GpuSplit::NotedWriteSince answers from per-page note
/// chains instead of walking every note since: 1 = on, 2 = verify (both run, the walk answers,
/// differences counted), 3 = dry (both run, counted and timed 1 in 64).
inline std::atomic<int> g_noted_fast{-1};
/// FRITCHII_FRONT_TRIM_RANGE (live, default 0; helper Z): GpuSplit::StampRangedRead takes each
/// ranged write page's answer from a small per-page table instead of the one-entry caches that
/// miss when pages take turns, and RangeLastWrite reads its environment switches from a member:
/// 1 = on, 2 = verify (the old lookup answers, differences counted), 3 = dry (counted and timed
/// 1 in 64).
inline std::atomic<int> g_front_trim_range{-1};
/// FRITCHII_FRONT_TRIM_QMD (live, default 0; helper Z): the front's QMD scheduler copy finds its
/// overlays and upload shadows through compact address arrays instead of walking the large
/// entries: 1 = on, 2 = verify (both run, the old walk answers, differences counted), 3 = dry
/// (counted and timed 1 in 64).
inline std::atomic<int> g_front_trim_qmd{-1};
/// FRITCHII_FRONT_ASYNC_TABLE (live, default 0; helper AA): GpuSplit's async write table
/// (counter reports and timed releases, read by RanReadCheck and SemaphoreReadableNow) kept per
/// block of 8 neighbouring 16-byte slots, one cache line each, instead of one random line per
/// slot: 1 = on, 2 = verify (both kept, the old table answers, differences counted), 3 = dry
/// (both kept, counted and timed 1 in 64).
inline std::atomic<int> g_front_async_table{-1};
/// FRITCHII_TC_COLLECT_OVER (live, default 0; helper AG, port of zortos #1306): past the
/// critical memory the collector deletes one more image a frame per 2 MiB over it (up to 1,024
/// more), and from 1 GiB over also images unused for 3 frames instead of 10.
inline std::atomic<int> g_tc_collect_over{-1};
/// FRITCHII_TC_COLLECT_EVICTABLE (live, default 0; helper AG, port of zortos #1731): the collector
/// skips images it may not delete in this pass before it counts them against its budget, the
/// old costly pass sorts by memory block fill too, and deletes keep the block fill current.
inline std::atomic<int> g_tc_collect_evictable{-1};
/// FRITCHII_SWEEP_SUPERSEDED (live, frames, default 0 = off; helper AG, port of Skitchii
/// 16fb54111d): GPU-written images untouched for N frames whose memory a newer image of another
/// shape covers are deleted without a download, checked every min(N, 30) frames.
inline std::atomic<int> g_sweep_superseded{-1};
/// FRITCHII_COLD_SWEEP_MAX (live, default 0 = 16; helper AG, the part of zortos #1571 we lack):
/// how many images the cold sweep deletes a frame at most.
inline std::atomic<int> g_cold_sweep_max{-1};
/// FRITCHII_COVERED_RETIRE (live, frames, default 0 = off): a GPU-written render target whose
/// whole memory newer GPU-written images cover (OoT's pot room: per-object D32 targets bump-
/// allocated over last frames' ones, 20-300 dead ones under each new clear) is deleted without a
/// download once untouched for N frames, by the cold sweep and by the collector, instead of
/// living until EXP58's 60 frames or the collector's LRU reaching it.
inline std::atomic<int> g_covered_retire{-1};
/// FRITCHII_COLLECT_LAND_UNDER (live, default 0): EXP121's queued collector downloads still land
/// when an overlapping image the GPU wrote since is still GPU-only (its own download lands over
/// ours later, and landing it first drops ours); without it they were dropped and the image kept,
/// then queued again by the next pass (pot room: 5,000 downloads per 600 frames, 99% dropped).
inline std::atomic<int> g_collect_land_under{-1};
/// FRITCHII_NOTED_OVERFLOW (live, default 0; F7, with FRITCHII_NOTED_FAST 1 to 3): the table's
/// overflow answer of NotedWriteSinceFast asked again of the note chains, which keep the position
/// of each forgotten link: 1 = on (a chain-proved "no write" answers at FRITCHII_NOTED_FAST=1),
/// 2 = verify (proved answers checked against the table and a sampled long shadow of notes),
/// 3 = dry (counted and timed 1 in 64, with the front DMA read checks after the answer).
inline std::atomic<int> g_noted_overflow{-1};
/// FRITCHII_STEP_LEAN (live, default 0): F6, the list-wait line's clock read every 256th entry
/// and the command list storage recycled to the submitter (dma_pusher.h).
inline std::atomic<int> g_step_lean{-1};
/// FRITCHII_REFRESH_LEAN (live, default 0): a list refresh translates each command page once
/// (1), 2 = verify (dma_pusher.cpp, docs/fritchii/switches.md).
inline std::atomic<int> g_refresh_lean{-1};
/// FRITCHII_S4_COUNT (live, default 0): fable_b2 3.3's S4 counter, dry (fritchii_s4_count.h):
/// 1 = count the fill's uniform binds by page, list entry and cause of change, 2 = also compare
/// a sample of ranges at the front's list entry parse with E's bind in that entry.
inline std::atomic<int> g_s4_count{-1};
/// FRITCHII_BIND_SKIP (live, default 0): S4-lite (common/fritchii_bind_skip.h), a graphics uniform
/// bind skips the read, compare, copy and bank check of a range whose bytes provably did not
/// change since the same slot bound it: 1 = within a list entry, 2 = also across entries on
/// tracked pages, 3 = 2 + verify, 4 = 1 + verify, 5 = dry (decide, count and time only).
inline std::atomic<int> g_bind_skip{-1};
/// FRITCHII_ESPLIT_COUNT (live, default 0): the E split's step S0, dry (fritchii_e_split.h):
/// 1 = classify every ring entry E runs (E1, E2, join, lend), time 1 in 64, count the stalls,
/// 2 = also build and consume the E1 to E2 records on E and compare E2's view at each hand-over,
/// 3 = 2, and E's draw fill reads that view (SetInputsOverride) wherever it equals the engine.
inline std::atomic<int> g_esplit_count{-1};
/// FRITCHII_ESPLIT (boot, default 0): 1 = the E split's thread E2 (fritchii_e_split_thread.h).
inline std::atomic<int> g_esplit{-1};
/// FRITCHII_ESPLIT_DEPTH (live, default 0): E1's lead over E2 in draws; 0 = lockstep (S1).
inline std::atomic<int> g_esplit_depth{-1};
/// FRITCHII_ESPLIT_PIN (boot, default 0): where E1 and E2 run (fritchii_e_split_thread.h, S3).
inline std::atomic<int> g_esplit_pin{-1};
/// FRITCHII_ESPLIT_ASYNC_KINDS (live, default 7): the E2 entries the queued mode queues instead
/// of joining (fritchii_e_split_thread.h): bit 0 memory stage cache calls, bit 1 the 3D methods
/// that end in one rasterizer call, bit 2 compute engine entries.
inline std::atomic<int> g_esplit_async_kinds{-1};
/// FRITCHII_ESPLIT_IDLE (live, default 1): E1's batch end and idle without draining E2
/// (fritchii_e_split_thread.h); 0 = joins as before.
inline std::atomic<int> g_esplit_idle{-1};
/// FRITCHII_ESPLIT_OWED (live, default 1): a draw's flush is owed by shader page writes and E1's
/// own work, not by any cached write of an async job (fritchii_e_split_thread.h); 0 = S4's rule.
inline std::atomic<int> g_esplit_owed{-1};
/// FRITCHII_ESPLIT_CB_ASYNC (live, default 0; with DEPTH > 0): 1 = constant buffer words' memory
/// half queued for E2 without a wait (fritchii_e_split_thread.h, S4).
inline std::atomic<int> g_esplit_cb_async{-1};
/// FRITCHII_ESPLIT_FLUSH_ASYNC (live, default 0; with DEPTH > 0): 1 = a draw's flush that is not
/// owed goes after E1's lookup even with E2 idle (fritchii_e_split_thread.h, FlushPlace).
inline std::atomic<int> g_esplit_flush_async{-1};
/// FRITCHII_ESPLIT_BIND_BATCH (live, default 0; the queued mode): 1 = constant buffer binds packed
/// into E2's records, same-value binds in a record dropped; 2 = verify; 3 = dry count
/// (fritchii_e_split_thread.h).
inline std::atomic<int> g_esplit_bind_batch{-1};
/// FRITCHII_BARE_SSBO_MIB (live, default 16; EDEN_NX2_BARE_SSBO_MIB is read when it is unset):
/// how far a storage binding of a bare pointer (no size word) reaches, in MiB, 8 to 1024. 8 (or
/// 0) is the old cap. Written bindings stop at 16 at most. docs/fritchii/switches.md.
inline std::atomic<int> g_bare_ssbo_mib{-1};
/// FRITCHII_BARE_SSBO_CLAMP (live, default 0): a bare pointer's storage binding stops at the
/// first device page with no host backing (read as zeros today); one with none at its start is
/// the null binding. Not exact for written bindings (writes past the end are dropped, reads past
/// it are robust-access values, not the zeros the host buffer held), hence off by default.
inline std::atomic<int> g_bare_ssbo_clamp{-1};
/// The bare pointer reach in bytes (FRITCHII_BARE_SSBO_MIB).
[[nodiscard]] inline u32 BareStorageReach() {
    int mib = g_bare_ssbo_mib.load(std::memory_order_relaxed);
    if (mib < 0) [[unlikely]] {
        const char* const alias = std::getenv("EDEN_NX2_BARE_SSBO_MIB");
        mib = SwitchValue(g_bare_ssbo_mib, "FRITCHII_BARE_SSBO_MIB",
                          alias != nullptr ? std::atoi(alias) : 16);
    }
    return static_cast<u32>(mib < 8 ? 8 : (mib > 1024 ? 1024 : mib)) << 20;
}
/// EDEN_NX2_LIGHT_TABLE_ARRAYS: docs/fritchii/switches.md.
[[nodiscard]] inline bool LightTableArrays() {
    static const bool on = EnvSwitch("EDEN_NX2_LIGHT_TABLE_ARRAYS") > 0;
    return on;
}

/// Numbered experiment switches EDEN_NX2_EXP0 .. EDEN_NX2_EXP<ExperimentCount - 1>, live
/// through the debug server. What each one does: docs/fritchii/switches.md; the defaults:
/// src/common/fritchii_switch_defaults.cpp (LoadExperiments).
inline constexpr int ExperimentCount = 128;
inline std::atomic<int> g_experiments[ExperimentCount]{};
/// Set once LoadExperiments has run: Experiment() tests it inline instead of calling
/// LoadExperiments (a function-local static guard) on every check (0.5% of the back thread in
/// the 5.7 -> 5.8.4 sampler comparison, 2026-11-11).
inline std::atomic<bool> g_experiments_loaded{false};

/// Sets the experiments' defaults and reads EDEN_NX2_EXP<n> from the environment, once (a
/// function-local static: thread-safe, and safe from any static initializer). Defined in
/// src/common/fritchii_switch_defaults.cpp, compiled into common, since common (thread.cpp)
/// and tools that link only common read Experiment() too.
void LoadExperiments();

/// The value of experiment switch `n` (EDEN_NX2_EXP<n>), 0 unless set.
[[nodiscard]] inline int Experiment(int n) {
    if (!g_experiments_loaded.load(std::memory_order_acquire)) [[unlikely]] {
        LoadExperiments();
    }
    return g_experiments[n].load(std::memory_order_relaxed);
}

/// The live switch called `name`, or null.
[[nodiscard]] std::atomic<int>* FindSwitch(std::string_view name);

struct CaptureRequest {
    u64 id;
    std::filesystem::path directory;
    bool sampled;
    /// The uniform and storage buffers of every draw and dispatch, and dispatch group counts.
    bool buffers;
};

struct CaptureResult {
    std::string error;
};

struct GpuReadRequest {
    u64 id;
    u64 gpu_address;
    u64 size;
};

struct GpuReadResult {
    std::vector<u8> bytes;
    /// Tegra::MemoryManager::GetID of the address space that maps the whole range.
    u64 address_space;
    std::string error;
};

struct RejectChange {
    u64 generation;
    /// Whether a hash left the list, so programs rejected before must be lowered again.
    bool unrejected;
};

/// Opens a debug server session (clears requests and rejections) or ends it (wakes waiters).
void BeginSession();
void EndSession();

/// Rasterizer thread: whether a capture or a reject change is waiting.
[[nodiscard]] bool HasRequests() noexcept;

/// Server thread: queues a frame capture; the rasterizer takes it at the next frame end.
void RequestCapture(CaptureRequest request);
/// Server thread: withdraws a capture that was not taken yet (it timed out).
void CancelCapture(u64 id);
/// Server thread: waits for the capture's manifest to be complete. Empty on timeout or when the
/// session ends.
[[nodiscard]] std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<CaptureRequest> TakeCapture();
/// Frame dump writer thread, once the manifest summary is written (or the capture failed).
void FinishCapture(u64 id, CaptureResult result);

/// Server thread: queues a read of GPU virtual memory; the rasterizer takes it at the next frame
/// end, where the GPU's own reads of its address spaces run.
void RequestGpuRead(GpuReadRequest request);
/// Server thread: withdraws a read that was not taken yet (it timed out).
void CancelGpuRead(u64 id);
/// Server thread: empty on timeout or when the session ends.
[[nodiscard]] std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<GpuReadRequest> TakeGpuRead();
/// Rasterizer thread.
void FinishGpuRead(u64 id, GpuReadResult result);

/// Server thread: adds or removes native program keys or shader hashes (as EDEN_NX2_REJECT_HASH
/// takes them). Returns the generation to wait for.
u64 SetRejected(std::span<const u64> hashes, bool rejected);
[[nodiscard]] std::vector<u64> RejectedHashes();
/// Lowering: whether the debug server rejects this program. Cheap while the list is empty.
[[nodiscard]] bool IsRejected(u64 hash);
[[nodiscard]] bool AnyRejected() noexcept;
/// Rasterizer thread: the pending reject change, once.
[[nodiscard]] std::optional<RejectChange> TakeRejectChange();
/// Rasterizer thread: the cached programs of this generation were dropped.
void NoteRejectApplied(u64 generation);
/// Server thread: waits until the rasterizer applied the generation.
[[nodiscard]] bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout);

} // namespace VideoCore::Nx2Debug
