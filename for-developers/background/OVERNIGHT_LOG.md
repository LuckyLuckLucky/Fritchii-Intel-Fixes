# Overnight log (autonomous work, 2026-11-11)

Working rules: Backups of every edited
file go to `_backups/` (latest working state). Postponed items: `POSTPONED_FOR_USER.md`.
Base state at start: IntelFix4 (fixes 1-8, see `INTEL_CHANGES.md`).

## Owner feedback on IntelFix4
- FPS improved again; small areas run a constant 40 fps.
- Large areas (more visuals at once) drop to 0 fps, in every area. Long-term goal: stable 60.

## Progress

### Log analysis (IntelFix4)
- Owner B580 (Ryzen 5 5600G): 17-58 fps; large areas 12-28 fps **without** held draws.
  Profiling zones in the slow window (t=242): GPU **back thread busy 859 ms/s** (52,476 draws/s,
  ~16 us per draw), front waits for it 520 ms/s (`split drain`). In the 60 fps area back = 230 ms/s.
  => large areas are **CPU-bound in the emulator's back thread**, not GPU-bound.
  Biggest back zones: method 137, bind stage buffers 134 (108k/s), split wait 187, update buffers 67,
  pipeline lookup 54, compute sync 39, fence 36, read handles 35, draw 33, flush work 33 (ms/s).
  Intel driver calls are cheap (draw record 3.9, push descriptors 13.9 ms/s).
- 2.8 s freeze at t=222: one pipeline compile (VkPipelineBuilder 2734 ms).
- 53 s gap at t=268-320: log silent, ~3 cores busy -> matches the preparation running while
  paused (fix 6), not a gameplay freeze.
- 140V: 9-30 fps (was ~9). New tester **A750** (Alchemist): `VK_KHR_cooperative_matrix` removed
  (no 8x16x16 shape) -> still on the 15 ms software HMMA path; log too short (title only).
- Native bank check (EXP9=3): ~24k "hot" page compares/s that never change: ~1-2 % of back thread.

### Tooling found
- The debug server (`EDEN_NX2_DEBUG_SERVER`) has `input`/`input_sequence` (inject controller input),
  `sample` (symbolized thread hot spots), `switch` (live EXP toggles), `screenshot`, `status` (fps).
  Controls are SDL-gamepad only, so input injection is the only way to drive the game here.
- `_tools/dbg.ps1` (PowerShell client, no Python), `_tools/launch_debug.cmd`.
- `_tools/gplbench/ctime.exe` + `_tools/ctime_batch.ps1`: compile time / SIMD / spill of a sample of
  1,014 compute shaders on the B580 (running).

### Compile-time survey (1,014 of 4,058 compute shaders, B580)
- Median 25 ms; **13 % take 9-12.7 s** (SIMD16, ~55k instructions) in the offline profile. With the
  B580's real profile (descriptor aliasing on, 64-byte SSBO alignment; shader-sweep SWEEP_INTEL
  updated) the same shaders take **~4.7 s** each (24-33k instructions, some 5.7 KB spill at SIMD32).
- Cause: the global-memory fallback (`DefineGlobalMemoryFunctions`): each guest pointer load
  searches up to 10 candidate storage buffers; 61 x 128-bit loads, each a 264-line search, all
  inlined. `EDEN_NX2_SPIRV_INLINE_GLOBALS=0` (DontInline) changes nothing on Intel (the driver
  inlines anyway: identical instruction counts).
- Possible fix (not done, too invasive without in-game validation): buffer-device-address loads
  (select the base once, one load) instead of 10 branches with loads. -> POSTPONED_FOR_USER.md.

### Fix 9: title/menu red lines (verified in game, IntelFix5)
- Launched the game myself via the debug server (`_tools/launch_debug.cmd`, `_tools/dbg.ps1`,
  `_tools/shots.ps1`). IntelFix4 title movie: pink horizontal line + speckled columns (top left),
  reproducible at fixed frame counts; with `EDEN_NX2_NO_COOP_MATRIX=1` clean.
- Cause: the matrix extension also sets `g_native_tensor_ops`, which makes "Automatic" movie
  upscaling use the game's own network (32-lane HMMA compute programs f9ea14a22394f5b1,
  eff3ccbe1438679d, e440c9f7923c2973, 4c1ae2fe58f30a97 ...). On the 8x16x16 path that network
  draws garbage (likely partial subgroups at image edges; offline full-tile tests matched).
- Fix 9 (`vulkan_device.cpp`): not on Intel's 8x16x16 path -> movies use the bilinear copy (as
  before fix 7). Gameplay HMMA keeps the matrices. Screenshot at the same frame: clean.

### Hyrule Field measurement (IntelFix5, this PC, debug server, 2026-11-11 ~11:30-12:00)
- Hyrule Field at night (save "11/11/2026 9:15"): **~38-39 fps** standing and running, with and
  without `EDEN_NX2_GPU_PROFILE` (profiling overhead negligible). Worst frames 43-49 ms, 0
  pipelines built in them (everything cached). Link died to a Stalfos while running (no save).
- GPU: ~25 ms of GPU work per frame (`gpu_submission_sum_ms`), span 31-34 ms, ~115 submissions per
  frame (~3,200/s from fences). Timed top: HMMA 6bd913e1f34e83eb 2.46 ms, 1e1964184a291f00
  2.0 ms (4x4x6 groups), f84d5909908cc5ea 1.1 ms (16 single-group dispatches), 75d4369f3b1f5868
  0.42 ms.
- CPU: front 23 ms/frame, back 25 ms/frame (thread CPU incl. spinning). Front thread samples:
  ~21 % waiting for the host GPU in `ReadGpuWrittenCommandList` (GPU-written command lists),
  ~27 % draining in `FrontDmaRead` (macro DMA reads), ~15 % draining at semaphore triggers.
  `EDEN_NX2_LIST_WAIT_TRACE=1`: 150 list waits/s, 162 ms/s; writer a107e12b6771be48 alone 130 ms/s
  (~4.2 ms per frame, 3.7 submissions behind).
- Back thread: ~20 % in buffer synchronization (`SynchronizeBuffer`/`ForEachModifiedRange`).
  Upstream's `clean_words` option targets this but is off by default since zortos293/Lichii#161
  (wrong geometry) -> not touched.
- So this scene is limited by GPU time *and* the CPU<->GPU round trips; both ~25 ms per frame.

### Driver statistics of the hot compute shaders (B580, real profile)
- f84d5909908cc5ea (no warp ops): driver picks SIMD32 -> 31,383 instr, **5.7 KB spill**, 8.4 s
  build; SIMD16 -> 24,634 instr, **no spill**, 3.4 s build.
- 75d4369f3b1f5868 (has warp ops, must stay 32): SIMD32 10 KB spill, 8.3 s build.
- 1e1964184a291f00 (warp ops): small (835 instr), no spill; its 2 ms is not register pressure.
- Of a third of OoT's compute shaders (1,366), 1,275 use no warp operations; 171 are >= 100 KB of
  SPIR-V (the global-memory-fallback group). Measuring compile time and spills, default vs 16
  lanes, on a 56-shader sample (`_tools/hot/compute/ctime_default.csv`, `ctime_16.csv`).

### Compile test: driver's choice vs 16 lanes (56 compute shaders without warp ops, B580)
- Large global-memory shaders (24-31k instructions): driver's choice **4.3-5.3 s** each (it builds
  SIMD16 *and* SIMD32, then often keeps SIMD32 with 5.4-5.7 KB scratch spill); forced 16 lanes:
  **1.75-2.0 s**, no spill. Small shaders: 25-40 % faster builds, same instruction counts.
- Spilling shaders in the sample: 16 -> 0. (`_tools/hot/compute/ctime_default.csv`, `ctime_16.csv`;
  `ctime.exe` now takes `CTIME_SUBGROUP=16`.)

### 1e1964184a291f00 = contended float atomic add
- Its SPIR-V: one image sample, a warp reduction (64 shuffles) and a **compare-exchange loop for a
  float32 atomic add** (the emulator never emits native float atomics). 96 groups x 256 threads
  hammering it: 2.0 ms per frame. The B580 has `shaderBufferFloat32AtomicAdd`.

### Fix 10 + 11 (built as IntelFix6 candidate, files backed up in `_backups/` first)
- 10: `Device::NarrowComputeSubgroupSize` + `vk_compute_pipeline.cpp`: on Intel, compute shaders
  without warp operations that use global memory are built with requiredSubgroupSize 16.
  `EDEN_NX2_INTEL_COMPUTE_SIMD16=0/1/2` (off / global-memory shaders, default / all without warp ops).
- 11: VK_EXT_shader_atomic_float enabled on Intel; storage float32 atomic adds emit
  OpAtomicFAddEXT on an aliased float view (placeholder OpAtomicFMaxEXT rewritten after assembly,
  like the warp placeholders). `EDEN_NX2_NO_FLOAT_ATOMICS=1` keeps the loop.

### In-game A/B, Hyrule Field spawn at night (same save, Link standing; fps every 5 s, settled)
| Build / switches | fps (settled) |
|---|---|
| IntelFix5 | ~37.5 and ~40.0 (two runs) |
| IntelFix6, fix 10 off (`EDEN_NX2_INTEL_COMPUTE_SIMD16=0`), fix 11 on | ~37.5 and ~39.4 |
| IntelFix6, fix 10 on, fix 11 off | ~35.4 |
| IntelFix6, both on | ~34 (and ~30 in a run with a stray `find` taking a core) |
- Run-to-run noise is about +-2 fps; each run has a dip to ~29-33 about 15-25 s after loading in
  every build (a scene event).
- **Fix 10 (16 lanes) is a runtime regression**: f84d5909908cc5ea took 0.14 instead of 0.07 ms a
  dispatch. Faster compiles (2.6x) do not pay for it. Attempt 1 failed -> default changed to
  **off** (mode 0, identical pipelines to before); the option stays for an opt-in A/B.
  Postponed (see POSTPONED_FOR_USER.md).
- **Fix 11 (native float atomics)**: 1e1964184a291f00 disappears from the GPU top list (was
  ~1.8-2.0 ms a frame); offline difftest vs the loop: 8 of 8.4 M values differ in the last bit
  (summation order), max relative difference 0; `spirv-val` valid; screenshot unchanged. FPS on
  the B580 equal within noise (the scene is bound by CPU<->GPU round trips here); expected to
  help more on GPU-bound Intel GPUs (A750, 140V). Kept on.
- Note: Intel's GPU clock follows load; per-shader GPU times are only comparable at equal load.

### IntelFix6 packaged (12:35)
- Final IntelFix6 = fixes 1-9 + 11 (fix 10 present but off). Field check: settled ~37.8 fps
  (37.0-38.7), same band as IntelFix5 (37.5 / 40.0); screenshot normal. `intel-fix6.patch`
  (cumulative, 20 files), backups refreshed, INTEL_CHANGES.md + AGENT.md updated.
- Not done: GPU-written command list wait (~4 ms/frame, writer a107e12b6771be48) is inherent to
  the design (the front needs the list's contents); only less GPU work per frame shortens it.

### VulkanWorker thread (IntelFix6, field) and remaining ideas
- VulkanWorker: 56 % idle; Intel driver ~43 % (submits ~10 %). Not on the critical path.
- HMMA 8x16x16 (fix 8): each multiply re-stores 6 constant-zero words per lane (B columns 8-15
  and the zeroed K halves); OoT's hot shader has ~384 HMMAs. Writing them once at shader entry
  could save maybe 0.1-0.3 ms a frame, but needs an entry-block hook in the SPIR-V backend
  (local-variable placement). Not done tonight (small gain, moderate risk) -> POSTPONED item 3.
- Rejected without trying (known visual risks upstream): fence flush batching
  (`EDEN_NX2_FENCE_FLUSH_US`, white flashes in OoT), `clean_words` (zortos293/Lichii#161).
- Field spawn at night: a Stalfos kills Link after ~2-6 min standing still (Game Over, no save
  written); measurement runs must stay short.

## Day session 2026-11-11 (owner supervising)
- Owner feedback on IntelFix6: no noticeable major change, maybe slightly better framerate.
- Owner's IntelFix6 log (eden_log.txt.old.txt, 17:59): preparation restarted (stamp changed) and
  was stopped after 603 s: 4,511 prepared, **9 failed**, 115,604 not reached. Failures are logged
  only at Debug level (`Precompiled pipeline N failed: ...`), so not identifiable from that log.
  Gameplay: long stretches at a steady 60 fps (worst frame 18-22 ms); one stretch t=479-499 at
  ~25 fps with no compiles.
- Postponed item 1 (global-memory redesign): **not attempted**. It only shortens compiles, which
  the completed preparation already removes; the owner's slow stretch had no compiles. Owner rule:
  any attempt must not break older builds.
- Owner rule: run the full preparation only once no more shader changes are planned (players
  should prepare once).
- Tried to reproduce the 9 failures (IntelFix6, EDEN_NX2_PRECOMPILE=1, Render.Vulkan:Debug): the
  emulator waited before boot (a dialog behind other windows), stopped it. To redo together with
  the final preparation run.

### Distance-view drop (owner: Hyrule Field save, looking into the distance -> ~20 fps)
- Reproduced (IntelFix6, debug server; Link turned with the left stick, camera recentred with ZL):
  facing the cliff **40 fps**, looking across the field **25-26 fps**.
- Per frame: cliff GPU 18 ms (span 25), distance GPU 28-30 ms (span 37-41); front/back CPU 19 ->
  31 ms (they wait on the GPU). Draws 1,330 -> 2,230 timed items. Timed draw/dispatch sum only
  9.5 -> 13.9 ms: **~6.5 ms of the increase is GPU work outside draws/dispatches** (clears,
  copies, barriers, render pass loads/stores, queries). Even fixed passes get slower (HMMA
  2.18 -> 2.89 ms), i.e. the GPU itself is saturated/slower per item.
- **Not resolution:** 0.75x (`resolution_setup=2`, also in frizti.ini, which otherwise restores
  1x at boot) gave **20 fps** (worse: rescale passes). Configs restored from `_backups/`.
- **fragment_barrier** (EDEN_NX2_PERF_ON=fragment_barrier): render pass ends ~3,900/s -> ~3,300/s
  (moved to other sites), GPU 26-30 -> 23-25 ms, fps unchanged (25.5-26.5). Not worth upstream's
  #161 risk. Dropped.
- Next: needs a real GPU frame breakdown (what the ~15 ms untimed GPU work is). No GPU profiler
  installed (no RenderDoc / Intel GPA). Asked owner about installing one.

### RenderDoc breakdown of the distance view (owner approved RenderDoc, 2026-11-11 evening)
- Portable RenderDoc 1.47 in `_tools/renderdoc/` (signature valid). Per-user Vulkan layer
  registration was blocked by the permission system, so `_tools/rdoc/launch_rdoc.cmd` loads the
  layer for that process only (VK_ADD_IMPLICIT_LAYER_PATH / VK_ADD_LAYER_PATH /
  ENABLE_VULKAN_RENDERDOC_CAPTURE). `trigger.py` (target control), `analyze.py` (EventGPUDuration
  per action), `detail.py` (call parameters) run inside `qrenderdoc --py`.
- Capture `_tools/rdoc/captures/oot_frame3419.rdc` (1.7 GB), distance view, 3,228 actions. Replay
  GPU time: dispatches 16.8 ms (14.1 ms is the HMMA pass on RenderDoc's software path; ~2.7 ms
  live), copies 11.6 ms (8.7 ms one D32 921x921 image->buffer reinterpret at eid 8, likely a
  RenderDoc-only path/first-event artifact: the B580 has VK_EXT_shader_stencil_export, so the
  live game converts depth by shader), draws 5.1 ms (2,166), mesh tasks 3.9 ms (82; top 0.78 ms).
  Realistic work ~17 ms vs 28-30 ms GPU-busy live: the rest is synchronization (barriers, ~100
  render pass breaks, ~90 submits, ~780 copies) and clock behaviour.
- EXP62 (upstream batched upload barriers, live switch) A/B in the distance view: 24.3 -> 22-24
  -> 24.2 fps. No gain. Left off.
- Day/night advances quickly in this save (day, sunset, night within ~10 min), which changes the
  scene cost; A/B runs must be short and back to back.

### Non-mesh improvements and IntelFix7 (owner: mesh shaders later, improve the rest, then package)
- RenderDoc eid 8 (8.7 ms D32 921x921 image->buffer) is Image::DownloadMemory (async download
  barrier shape). Diagnostic build (`EDEN_NX2_FLUSH_AREA_LOG=1`, texture_cache.h GetFlushArea and
  DownloadMemory): **no** image marked by CPU reads and **no** image flush downloads in the
  distance view, so eid 8 is not per frame (one-off in the capture). Lead closed.
- `EDEN_NX2_NO_REACTIVE=1` (read traps off): distance 25.2-26.5 -> 27.8-28.6 fps (+2), but CPU reads
  of GPU-written memory may go stale. Not enabled.
- Front waits for the back (distance view, per second): macro DMA read (occlusion reports)
  ~1,340 / 270 ms, fence release ~1,400 / 190 ms, macro DMA dispatch ~30 / 25 ms. Slow clears
  (render target prep 0.7-2 ms on the back) ~5-14/s: minor.
- Live A/B in the distance view (one instance, 2 rounds x 2 samples, ~25-27 fps each): EXP40=3,
  EXP30=1, EXP31=1, EXP10=1, EXP10=5, EXP12=1, EXP24=1, EXP37=1: **all within noise**. EXP32 isn't
  a live switch. Nothing enabled.
- Process slip: one A/B run left the emulator running (-Keep) and the next started a second
  instance (owner noticed). The run that overlapped was stopped and redone with one instance; the earlier
  A/B ran alone. `distance_run.ps1` now closes any running instance first.
- New tools: `_tools/distance_run.ps1` (field save -> distance view -> fps, -Keep), `_tools/ab_live.ps1`
  (live EXP phases).
- **IntelFix7 packaged** (`Fritchii-v5-IntelFix7/`): IntelFix6 + D1 diagnostic log. Shader stamp
  unchanged (b2fd2d3b00a4bf2b): no new preparation for players. `CHANGELOG-Intel.md` at the root
  only (owner: separate file, build folders untouched). Shortened on request: all positives,
  asks for logs, asks people to try every version (long draft in `_backups/CHANGELOG-Intel.long.md`). `intel-fix7.patch`
  (cumulative, 21 files). Backups: `_backups/fix12/`, `*.pre7` docs.
- Still open: full preparation run + the 9 failed pipelines (do once shaders are final; IntelFix7
  doesn't change them), mesh shaders (owner: later).
