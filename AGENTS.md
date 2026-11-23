# AGENTS.md: Intel Arc fork of Fritchii for Zelda: Ocarina of Time (Switch 2)

Notes for a human or an AI agent who wants to understand, rebuild or port what this fork changed.
Target: Ocarina of Time (title 0400486024DEC000, update 2.0.0) on Intel Arc (tested on a B580, Windows 11,
driver 32.0.101.9175, Vulkan). Other games may behave differently: game-specific choices are allowed here.

Lineage: Lichii (zortos293/Lichii, branch nx2) -> Fritchii (Frizti/Fritchii, v6.2) -> this fork.
Fritchii is a fork of Lichii, so `git merge-base` / `git log A..B` show exactly what each side changed.
All code is GPL (yuzu lineage); ported code keeps its author credit in the commit message.

## 1. Where the Intel changes live

* Almost every change is behind a switch (environment variable, also accepted in `nx2_switches.txt` next to the
  exe). The fork's defaults are baked in for Intel's Windows driver (`VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS`);
  other GPUs keep upstream behaviour unless noted.
* The boot log prints one line with all Intel defaults: search the log for `Intel fork defaults:`.
* Intel settings revision (`src/yuzu/main_window.cpp`, `IntelForkRevision`): the first start of a new build
  writes the fork's standard settings once (fence behaviour, vsync ...), then the player's changes are kept.
  `INTEL_FORK_SETTINGS_REV=0` skips it (the test harness does this, so test runs keep the old settings unless
  told otherwise).
* Experiments (`EDEN_NX2_EXP<n>`) have built-in defaults in `src/common/fritchii_switch_defaults.cpp`.

## 2. Rock flicker / sky shining through rocks and objects (FIXED)

Symptom: small and distant rocks (and other objects) flicker or vanish for a frame, and the skybox shows through
them. Worse while turning the camera, in new areas and at day/night changes. Seen on Intel Arc; Lichii does not
show it.

Key fact: Ocarina of Time decides per object whether to draw it from GPU **occlusion (sample count) reports**
(a macro reads the visible-sample count). A report that reads 0, or is read before the GPU wrote it, means
"not visible": the object is not drawn and the sky behind it shows. There were two independent causes.

### Cause A: occlusion counts dropped past 256 slots (fix: 8192 slots)

* The occlusion counter query grows by one slot per submission/render pass while the game counts without
  resetting. Upstream drops the count (as a reset would) once it passes one bank (256 slots). A dropped count
  reads as "not visible" -> sky through rocks.
* Fix: on Intel's Windows driver the limit is **8192 slots** by default
  (`src/video_core/renderer_vulkan/vk_query_cache.cpp`, search `limit_setting`, `EDEN_NX2_EXP16`).
  `EDEN_NX2_EXP16=<slots>` overrides it. Idea from Lichii #1428.
* Evidence: the log line `Occlusion counter resets past N slots` dropped from 27 per session to only a few, and
  those only in the title/load menus. User eye test: "way rarer", but not gone -> cause B.
* Do not use 16,384: upstream notes it hung AMD with the split GPU thread.

### Cause B: the CPU read GPU-written data too early (fix: counter reports wait for their fence)

* With the **Default** fence behaviour, the guest passes its syncpoint before the host GPU has really run the
  work, so the game read occlusion results (and other GPU data) too early.
* Proof: `gpu_fence_behavior=Balanced` (delayed fences) made the sky glitch disappear completely (several
  runs, eye test), but cost ~40 % fps (Hyrule day 41 -> 24 fps) because every GPU release waited for the host GPU.
* Final fix (keeps Default fences, so no fps loss): **EXP33 = 3**, after Unfixedeward/Lichii (intel-arc branch,
  commit e8908408ef, `EDEN_NX2_FENCE_REPORTS=1`):
  * only occlusion **COUNTER** reports (sample counts) stay registered until their fence has run, and that
    fence is created and waited for even when it would be empty;
  * semaphore **payload** reports still land at once. Holding those too turns the game's ~10 GPU-side
    acquires per frame into host round trips: that was Balanced's fps loss.
  * Code: `src/video_core/query_cache/query_cache.h`, search `exp33` / `keep_fence_report`. Default set in
    `src/common/fritchii_switch_defaults.cpp` (`g_experiments[33].store(3)`). Related branches in
    `vk_rasterizer.cpp` (search `Experiment(33)`) and `nx2_runahead.cpp`.
  * Both halves matter: freeing the query at once while keeping only the fence left a stale operation behind
    (three channels waiting forever on a semaphore).
* Settings: **Default** fence behaviour (`gpu_fence_behavior=0`), set by Intel settings revision 4.
* Result (4 saves, B580): Hyrule day 28.5 -> 41.0, latest save 51.6 -> 59.6, Kakariko 24.0 -> 38.6,
  Zora's River 33.1 -> 45.4 fps vs Balanced, and the sky through objects stays fixed (eye test).

### Things that were tested for the sky glitch and did NOT help (save your time)

* `FRITCHII_OCCLUSION_VISIBLE=1` (draw everything): the whole view washes out with fog/sky layers, because OoT
  also hides those by occlusion. Not usable even as a test.
* Report-only read guards (`REPORT_READ_GUARD`), provisional occlusion results (`OCCL_PROVISIONAL` v1/v2),
  `ONEF_SIGNAL_ON_R` (worse + hang), `CPU_SYNC_DONE` (hang), compute-skip changes: no effect or worse.
* "Lost contents ... cleared to zero" log lines at area loads were a red herring for this symptom.

### If someone still sees rocks flicker with the sky behind them

1. Check the boot log: `Intel fork defaults:` must show `occlusion slots 8192`; the settings dump must show
   `gpu_fence_behavior: Default`; EXP33 must be 3 (built-in default; `EDEN_NX2_EXP33` must not be set to 0).
2. Search the log for `Occlusion counter resets past`: lines during gameplay mean cause A is back.
3. Quick confirmation: if switching to Balanced fences makes the glitch disappear, it is cause B (data read
   too early): the EXP33=3 path is not active.
4. Old Intel drivers (e.g. 101.9150) also showed a black screen; use 101.9175 or newer.

## 3. Other visual fixes (Intel defaults)

From the boot line `Intel fork defaults:`; each was judged by eye on the B580.

* `equal-depth-bias OFF` (`INTEL_FORK_NO_EQUAL_DEPTH_BIAS`): fixed broken geometry and menus.
* `vertex-input-dynamic OFF`, `safe-shuffle on`, `texarray-bound OFF`, `nan-guard on`: Intel driver workarounds
  (see the code comments at each switch).
* Async shaders were forced off on Intel at first: upstream's old draw-skip paths caused flashing (chimney smoke,
  walls). IntelFix10 turns async back on with **only Lichii build-386's skip rule** (section 4).
* Known remaining: grass-shadow flicker (Lichii has it too), a ~1 s black flash on some door entries.

## 4. Shader stutter (first visits)

* Root cause of the long freezes: single ~10-15 s compiles inside Intel's shader compiler (mostly OoT's mesh
  pipelines and a few compute programs) that a draw or dispatch waits for.
* Intel's driver keeps its own shader cache, one file per exe NAME, under
  `%USERPROFILE%\AppData\LocalLow\Intel\ShaderCache`, limited to 512 MiB. A full shader preparation fills it,
  after which it no longer stores anything. The fork therefore keeps its own Vulkan pipeline cache file
  (`vulkan_pipelines.fork-v54.bin`, up to 8 GiB), which holds both prepared and played pipelines: a second visit
  is smooth.
* `INTEL_FORK_ASYNC_MODE=lichii` (default since IntelFix10; `off` turns it off): an unbuilt draw is skipped only
  when Lichii build-386 would skip it: depth-tested, colour only into targets first written in an earlier frame.
  `INTEL_FORK_ASYNC_MESH_SKIP=1` (default) applies the same rule to mesh draws (most of the long compiles).
  Code: `src/video_core/renderer_vulkan/async_draw_policy.h`, `pipeline_build.h`, `vk_pipeline_cache.cpp`
  (`ClassifyLichiiDraw`). Cold first visit: held time 38.9 % -> 2.7-9.3 %, avg fps 21 -> 31-33.
* `INTEL_FORK_COMPUTE_PRECACHE` (default on on Intel): builds the known compute programs at the
  "Preparing shaders" gate before Play. In play: 30 -> 3 compute compiles.
* Not fixed: a few compute programs the preparation never saw still freeze once per install (12-16 s). They
  write through raw pointers, so skipping them is not safe.

## 5. Other fixes in the final build

* `INTEL_FORK_CLOSE_WATCHDOG=20` (seconds, 0 = off): once the window is closing, a teardown with no progress for
  that long ends the process (a long session once deadlocked at 0 % CPU while closing). Code:
  `src/common/shutdown_watch.*`.
* Ported from Lichii/Fritchii: only 32-hex user ID folders count as orphaned profiles (a hand-copied folder could
  be deleted as one).
* Separate A-series build (branch `intel/a-series`): the shader preparation uses at most 2 threads and the
  compute precache 1 (for A380/A750/A770, where the preparation stalled). The normal build can do the same with
  `EDEN_NX2_PRECOMPILE_THREADS=2` in `nx2_switches.txt`.

## 6. Build and test

* Windows, clang build: `tools\build.ps1 -Tree <tree> -Compiler clang` in the workspace (VS BuildTools + bundled
  CMake/Ninja/Qt). One build at a time.
* **Do not edit these without forcing everyone to prepare shaders again**: `shader_recompiler/`,
  `vk_graphics_pipeline.*`, `vk_compute_pipeline.*`, `fixed_pipeline_state.*`, `nx2_precompile.*` (their contents
  are part of the preparation's stamp).
* Measure warm and cold separately. "Cold" = delete the fork's own cache files; Intel's driver cache cannot be
  cleared while in use, and a full driver cache makes every cold run pay all compiles again.
* Judge visuals by eye on the real game (latest save + Zora's River for glitches); fps over several saves.

## 7. Pitfalls we hit

* Sources use CRLF line endings: scripted edits must keep them.
* Writing `'\0'` through shell/Python escaping can put a raw NUL byte into a C++ file (git then calls it binary).
  Check edited files for byte 0.
* The test harness skips the Intel settings revision (`INTEL_FORK_SETTINGS_REV=0`): pass the fence setting
  explicitly when comparing fps with a release.
