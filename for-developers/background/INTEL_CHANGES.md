# Fritchii: Intel Arc fixes, changelog

Base: `Frizti/Fritchii` branch `oot-gather-fix`, commit `4f5c53149b` (v5.8.4), in `Fritchii-src/`.
Pristine copy for diffs: `_download/orig/Fritchii-4f5c53149b/`.
Test hardware: Intel Arc B580, driver 101.9150 (this PC); logs also from Arc 140V (Lunar Lake).

Order of work: visual glitches first (foliage flicker has top priority), performance after.

## Status

| # | Change | Targets | Built | Tested on Intel |
|---|---|---|---|---|
| 1 | Dynamic topology follows tessellation | Random light beams, stray geometry | IntelFix1 | **yes, fixed** |
| 2 | Task/mesh shaders pinned to 32-lane subgroups | Flickering and missing foliage | IntelFix1 | **yes, fixed** |
| 3 | Shuffles bounded to the host subgroup | Flashing lights, NaN bloom (possible) | IntelFix1 | **yes, fixed** |
| B1 | MSVC build: `/we` warnings kept as warnings when `ENABLE_WERROR=OFF` | Build only, no runtime effect | IntelFix1 | n/a |
| 4 | Dynamic vertex input on Intel drivers 101.x (version decode fix) | Prepared shaders unusable on Intel, extra pipelines, stutter | IntelFix2 | no |
| 5 | Prepare step runs on Intel, into Intel's own driver cache | Stutter on Intel; prepare skipped or bloating our cache | IntelFix2 | no |
| 6 | On Intel the prepare doesn't run beside gameplay (only on the screen, while paused, or on request) | Lag spikes while the prepare runs in play | IntelFix3 | indirectly: the profile run with the prepare off had "basically zero lag spikes" |
| 7 | Guest tensor (HMMA) math on Intel's XMX matrix units (8x16x16 cooperative matrices) | Low FPS: one shader took 13.5 ms a frame | IntelFix3 | offline only: 12× faster dispatch on the B580; in-game check pending |
| 8 | Intel 8x16x16 HMMA rounds after each K=8 half | Wrong tensor results vs guest | IntelFix4 | offline: matches the software path; FPS up again in game |
| 9 | "Automatic" movie upscaler not on Intel's matrix path | Main menu red lines | IntelFix5 | **yes, in game (title screenshots clean)** |
| 10 | Optional 16-lane compute shaders (off by default) | Compile freezes | IntelFix6 | in game: ~4 fps slower when on, so off |
| 11 | Native float atomic add on Intel | 2 ms a frame in one compute shader | IntelFix6 | in game: shader gone from the GPU top list, image unchanged, FPS same within noise on the B580 |
| D1 | Diagnostic log `EDEN_NX2_FLUSH_AREA_LOG=1` (off by default, no runtime effect) | Finding per-frame image read-backs | IntelFix7 | ran: no image read-backs in the distance view |

## Builds

| Build | Folder | Contains | Notes |
|---|---|---|---|
| IntelFix1 (2026-11-11) | `Fritchii-v5-IntelFix1/` | 1, 2, 3, B1 | MSVC 14.51 Release, Qt 6.11.1 static, version string `intel-fix-4f5c53149b`, update checker off. Full diff: `intel-fix1.patch`. |
| IntelFix3 (2026-11-11) | `Fritchii-v5-IntelFix3/` | 1-7, B1 | `A-B old tensor path (compare).cmd` runs fix 7's old path (`EDEN_NX2_NO_COOP_MATRIX=1`) for a visual comparison; the profiling launcher no longer turns the prepare off. Cumulative diff: `intel-fix3.patch`. |
| IntelFix2 (2026-11-11) | `Fritchii-v5-IntelFix2/` | 1-5, B1 | As IntelFix1, plus `Profile run (send me the log).cmd` (sets `EDEN_NX2_GPU_PROFILE=1`, `EDEN_DKB_FRAME_TRACE=1`, and `EDEN_NX2_PRECOMPILE=0` so the preparation doesn't skew the FPS). Cumulative diff: `intel-fix2.patch`. |
| IntelFix5 (2026-11-11) | `Fritchii-v5-IntelFix5/` | 1-9, B1 | Same launchers as IntelFix3. |
| IntelFix6 (2026-11-11) | `Fritchii-v5-IntelFix6/` | 1-11, B1 (10 off unless `EDEN_NX2_INTEL_COMPUTE_SIMD16=1`) | Runs "Preparing shaders" once more. Cumulative diff: `intel-fix6.patch`. |
| IntelFix7 (2026-11-11) | `Fritchii-v5-IntelFix7/` | 1-11, B1, D1 | Plays as IntelFix6; shader stamp unchanged (`b2fd2d3b00a4bf2b`), so **no new preparation**. Player-facing changelog: `CHANGELOG-Intel.md` at the root (separate file, not in the build folders). Cumulative diff: `intel-fix7.patch`. |

The build uses the same `%APPDATA%\fritchii` data, config and saves as your normal build. On
first launch shaders are compiled again (changed SPIR-V), so expect stutter for the first minutes
in each area. The original `Fritchii-v5/` is untouched.

How it was built: `_tools/configure.cmd`, then `_tools/build.cmd` (output in `_build/bin`).

## Positive changes (confirmed by your feedback)

- **IntelFix1, changes 1-3 (2026-11-11, owner and a tester on Intel Arc):** every reported visual
  glitch is fixed: missing textures, random light beams, flickering foliage, fog glitches and
  flashing lights. The fixes were tested together, so the log does not credit any one change with
  a particular glitch.
- **Still open:** the grass *shadows* still flicker slightly.
- **IntelFix3, changes 6-7 (2026-11-11, owner on Arc B580):** "runs quite smooth now", **40 fps in
  the area that ran at 20-30 fps** before. A second new tester reports similar performance. The
  tester with the older GPU sees only a slight FPS increase.
- **New with IntelFix3 (low priority per owner):** the main menu shows red lines and sometimes a
  black background.

---

## 1. Dynamic primitive topology now follows the pipeline's tessellation stages

**Files:** `vk_rasterizer.cpp`, `vk_rasterizer.h`, `vk_state_tracker.h`, `vk_state_tracker.cpp`,
`vk_graphics_pipeline.h`, `vk_graphics_pipeline.cpp`

**Problem.** The game binds tessellation shaders while the guest's topology register says
"triangles". When a pipeline is created, `FillStates` already corrects that to `PATCH_LIST` (the
log line `Patch topology not used with tessellation, using patch list`, 606 times in one Intel
session). But with extended dynamic state on (EDS1 and up, your setting is EDS2), the topology is
a dynamic state. The rasterizer then called `vkCmdSetPrimitiveTopologyEXT` with the raw guest
value (`TRIANGLE_LIST`), which overrides the pipeline's fixed value. Drawing tessellation shaders
with anything other than a patch list is invalid Vulkan. NVIDIA hardware treats tessellated input
as patches no matter what, so it looked fine there. Intel reports
`dynamicPrimitiveTopologyUnrestricted = true` and uses the topology exactly as given, so triangle
lists went into the hull and domain shaders. Stretched, stray triangles are the usual result.

The tracker also re-recorded the topology only when the *guest* topology changed. Switching
between a tessellated and a normal pipeline with the same guest topology did not update it.

**Fix.**
- `GraphicsPipeline::HasTessellation()` is set at construction from the present stages. Mesh
  pipelines are excluded because they can use slot 1.
- `UpdateDynamicStates` works out the host topology the same way `FillStates` does: `PATCH_LIST`
  when tessellation is bound, `POINT_LIST` for guest patches without tessellation, otherwise the
  guest topology.
- `StateTracker::ChangeHostPrimitiveTopology` tracks the last Vulkan topology recorded, so a
  pipeline switch re-records it. It is reset wherever the guest topology is reset.
- `UpdatePrimitiveRestartEnable` checks restart support against that same host topology.

**Effect on other GPUs.** NVIDIA and AMD now get valid Vulkan for these draws. It should not
change what they render.

## 2. Task and mesh shaders that use warp operations run in 32-lane subgroups

**Files:** `vk_graphics_pipeline.cpp`, `vulkan_device.h`

**Problem.** Switch 2 shaders are written for NVIDIA's fixed 32-thread warp. Votes, ballots, lane
IDs, lane masks and shuffles all assume 32 lanes. The SPIR-V backend handles host subgroups
*wider* than 32 (AMD wave64), but nothing handles *narrower* ones. Intel Arc picks SIMD16 or SIMD32
per shader (B580: `minSubgroupSize 16`, `maxSubgroupSize 32`). At SIMD16 a guest ballot sees only
half the warp, and lane IDs wrap at 16.

Ocarina draws its high-detail foliage with native task/mesh shaders. A task shader typically culls
meshlets with a ballot and a prefix count (`popc(ballot & lt_mask)`) to compact the surviving
meshlets. At SIMD16 that compaction drops or duplicates meshlets from frame to frame, which shows
up as flickering and missing foliage.

`vk_graphics_pipeline.cpp` already built a `requiredSubgroupSize = 32` struct for graphics stages,
but it was marked `[[maybe_unused]]` and never attached. Compute pipelines did attach it. That
explains why AMD is fine: Automatic mode forces AMD onto the *compute* mesh fallback, and compute
is pinned to 32 lanes.

The B580's `requiredSubgroupSizeStages` covers compute, task and mesh (checked with
`vulkaninfo`), so the driver can pin task and mesh shaders to 32. The emulator just never asked.

**Fix.**
- `Device::CanRequireGuestWarpSize(stage)`: subgroup size control is enabled, the stage is in
  `requiredSubgroupSizeStages`, and 32 lies within [min, max].
- `MakePipeline` and `MakeShaderLibrary` (the GPL path) chain
  `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo{32}` to every stage that uses warp-width
  operations (vote, lane ID, lane masks, shuffles), when the device allows it for that stage.
  Shaders that only use quad operations are left alone. Quads need just 4 lanes, and forcing SIMD32
  can make Intel spill registers, as the compute path's comment about Mario Kart World notes.

**Effect on other GPUs.** NVIDIA always runs 32 lanes, so this changes nothing there. On AMD's
Windows driver only compute is pinnable, so nothing changes there either.

## 3. Shuffles from lanes past a narrower host subgroup stay in bounds

**Files:** `profile.h`, `vk_pipeline_cache.cpp`, `vulkan_device.h`, `spirv_emit_context.h`,
`spirv_emit_context.cpp`, `emit_spirv_warp.cpp`

**Problem.** Intel's fragment and vertex shaders *cannot* be pinned to 32 lanes. In them, a guest
shuffle from lane 16 to 31 (for example a butterfly `xor 16` reduction or `SHFL.DOWN` by 16) reads
an invocation that does not exist. SPIR-V leaves the result undefined, so it can be garbage or NaN.
Bloom and other blurred passes then spread it across the screen as flashes.

**Fix.**
- New `Profile::warp_size_potentially_smaller_than_guest`, set when the device's minimum subgroup
  size is below 32 (Intel, yes; NVIDIA and AMD, no).
- With it set, shaders that shuffle declare the `SubgroupSize` builtin. `ShuffleIndex`,
  `ShuffleDown` and `ShuffleButterfly` count a source lane at or past `SubgroupSize` as out of
  range. The lane keeps its own value and the guest's in-bounds predicate is false, the same
  result the guest gets for a lane outside its clamp. `ShuffleUp` cannot leave the subgroup, so it
  is unchanged.

**Effect on other GPUs.** None, because the flag is false on NVIDIA and AMD.

---

## 4. Dynamic vertex input turned on for current Intel drivers

**File:** `vulkan_device.cpp` (the `vertex_input_dynamic_state` driver check)

**Problem.** The check for the old Intel bug ("broken before 27.20.100.0") compared Intel's
`driverVersion` with `VK_MAKE_API_VERSION(27, 20, 100, 0)`. Intel's Windows driver doesn't encode
its version as an API version; it uses `(build_major << 14) | build_minor`. The B580's
`101.9150` is `1663934` (confirmed with `vulkaninfo`), about 1.66 million. `VK_MAKE_API_VERSION(27,
20, 100, 0)` is about 113 million, so **every** Intel driver counted as "old" and lost
`VK_EXT_vertex_input_dynamic_state`. The log line `Disabling broken
VK_EXT_vertex_input_dynamic_state` appeared on both Arc GPUs. Consequences:
- The precompiled pipelines didn't match the device. Reading your
  `precompiled.fork-v54.nx2prep` (120,124 records) with a small tool (`_tools/nx2prep_stats.cpp`)
  showed that **all 116,036 graphics records have dynamic vertex input set**. The preparation
  skips records made for "another device", so **on Intel every graphics record was skipped**, and
  a prepare there only built the 4,088 compute pipelines. That is why the prepared shaders didn't
  help on Intel.
- Each vertex layout was baked into its own pipeline: more pipelines, more compiles, more
  pipeline refreshes per draw (`vertex input 3506/s` in the 27 fps scene), and the vertex
  repacking path was on.

**Fix.** Decode Intel's build properly and disable the extension only on the old `100.x` driver
branch. Arc and Xe drivers (the B580's `101.9150`, the 140V's `101.8509`) keep it. If it ever
misbehaves, the existing setting *Vertex input dynamic state* (Graphics, Advanced) still turns it
off.

**Effect on other GPUs.** None (Intel only).

## 5. The prepare step runs on Intel and builds into Intel's own shader cache

**File:** `vk_pipeline_cache.cpp` (`DriverCachesPipelines`, new `IntelDriverCache` and
`AppCacheBesideDriver`), comment in `nx2_precompile.h`

**Problem.** The prepare step ("Preparing shaders", ~120k pipelines) only runs where "the driver
keeps a pipeline cache of its own", and that list had only NVIDIA and AMD. On Intel it was
skipped (`Precompiled pipelines skipped: the driver keeps no pipeline cache of its own`). Forced
with `EDEN_NX2_PRECOMPILE=2`, it would build into the emulator's own `vulkan_pipelines.bin`,
which is loaded whole at every boot.

But Intel's Windows driver *does* keep its own cache: `%LOCALAPPDATA%Low\Intel\ShaderCache`. On
this PC it is 2 GB, with a 476 MB file written the minute your last IntelFix1 session ended.
Measured with a standalone benchmark (`_tools/gplbench/`, B580, driver 101.9150), the cache
works across processes: a fragment shader that took 9.7 s to compile came back in **1.4 ms** in a
new process, even for blend states it had never seen.

**Fix.**
- Intel's Windows driver counts as caching pipelines, so the prepare runs there by default.
- As on AMD with `EDEN_NX2_AMD_APP_CACHE=1`, the emulator's own cache file stays on for pipelines
  built in play (boot rebuilt 3,014 of them in 7 s from it). The preparation's builds go only to
  Intel's cache, so the file doesn't grow by gigabytes. The existing 512 MiB cap applies.
- `EDEN_NX2_INTEL_DRIVER_CACHE=0` restores the old behaviour.

**Effect on other GPUs.** None (Intel only; AMD behaves as before).

## 6. On Intel the prepare doesn't compete with the game

**File:** `vk_pipeline_cache.cpp` (`PreparePrecompiled`, worker loop)

**Problem.** After you chose Play now, the prepare kept building beside the game on 3 threads.
On Intel, at 1-2 s per new shader on a 6-core CPU, that took CPU from the game. Your report:
the profiling launcher, which turns the prepare off (`EDEN_NX2_PRECOMPILE=0`), ran "basically
zero lag spikes", while a normal start stayed laggy.

**Fix.** On Intel the prepare follows the rule the emulator's older warm-up already used ("never
competes with the game"):
- On the **Preparing shaders** screen: full speed, all threads, as before.
- After **Play now**: it pauses, and continues **while the game is paused**, from **Emulation >
  Prepare Shaders Now**, or on the screen at the next start. Progress is kept.
- `EDEN_NX2_PRECOMPILE_WHILE_PLAYING=1` restores building beside the game.

**Effect on other GPUs.** None (Intel only).

## 7. Guest tensor math on Intel's matrix units

**File:** `vulkan_device.cpp` (cooperative matrix shape selection)

**Problem.** The profiling run showed one compute shader, `6bd913e1f34e83eb`, taking **13.5 ms of
GPU time every frame**, about 65 % of all measured GPU work in the slow outdoor scenes. Translated
offline (`shader-sweep`, with Intel's profile), it turned out to be the game's tensor-core matrix
math (HMMA), emulated with 32-lane shuffles. Intel's compiler statistics for it
(`_tools/gplbench/cstats.exe`, B580): SIMD32, 75,795 instructions, **12,480 bytes of register
spill per lane**. The B580's XMX matrix units sat unused, because the 8×16×16 matrix path (from
Lichii#779, Intel Linux) was explicitly off on Intel's *Windows* driver. The stated reason was
that Windows turns float16 off. But the emulator already keeps the float16 *device feature* for
matrices while translated shaders do no float16 arithmetic (`float16_math`), so that reason no
longer applied.

**Measured offline on the B580** (same shader, same inputs; `_tools/gplbench/hotrun.exe`):

| | Software HMMA (before) | 8×16×16 cooperative matrices (fix 7) |
|---|---|---|
| GPU time, 64×36 dispatch | 15.1 ms | **1.25 ms (12× faster)** |
| Instructions | 75,795 | 14,098 |
| Register spill per lane | 12,480 B | 3,136 B |
| Compile time (also helps the prepare) | 9.6 s | 2.0 s |

The driver accepts the matrix shader at the emulator's settings (32 full lanes), and `spirv-val`
passes. Where both versions wrote finite values they matched bit for bit, with NaN in the same
places. But synthetic inputs only produce degenerate output (NaN or 0.25) from this shader, so
**correctness on real game data still needs your in-game comparison** (see the IntelFix3 test
below).

**Fix.** Allow the 8×16×16 path on Intel's Windows driver (the B580 reports `M8 N16 K16`, f16 ×
f16 → f16, subgroup scope). Translated game shaders still use no float16 arithmetic.
`EDEN_NX2_NO_COOP_MATRIX=1` goes back to the software path.

**Expected effect.** About 12 ms less GPU time per frame in the slow outdoor scenes. Those ran at
26-40 ms of GPU time per frame (20-30 fps), so the slow scenes should get clearly faster. Not yet
confirmed in game.

**Effect on other GPUs.** None (NVIDIA and AMD keep their own matrix paths).

## What was measured on the B580 (and what it rules out)

`_tools/gplbench/gplbench.exe` is a standalone Vulkan program; it needs no game or emulator.

| Test | Result | Meaning |
|---|---|---|
| Same shaders, 5 other blend states | 9.7 s, then 0.0 ms each | Intel compiles each *shader* once; fixed-function state is free |
| New process, same SPIR-V | 1.4 ms (was 9.7 s) | Intel's disk cache works and is per shader |
| GPL fragment library vs monolithic | 9.3 s vs 9.6 s | GPL saves no compile time on Intel; libraries and monolithic pipelines share the cache |
| GPU time: fast-linked vs monolithic | +0.5 % | no GPU penalty for fast links on Intel |
| 12 shaders on 1 thread vs 12 threads | 20.0 s vs 3.9 s (5.2×) | Intel compiles in parallel; no global lock |
| Empty submit + timeline-semaphore wait | 2 µs | Intel's fence round trip is fast |

**Conclusions.**
- The prepare's cost on Intel is the number of *unique shaders* (15,609 vertex + 21,411 fragment
  in the recipe) times Intel's compile speed, which was 1.7 s for one of the test shaders. That is
  Intel's compiler and can't be removed from the emulator side. What could be fixed is that the
  work was wasted (change 4) or not done at all (change 5).
- Graphics pipeline libraries would not speed Intel up, so they stay off on Intel.
- The FPS gap is not slow fences. See "FPS analysis" below.

## FPS analysis so far (from the IntelFix1 logs)

- **B580:** about 27 fps in some outdoor scenes and a locked 60 fps in others. In the 27 fps scene
  the GPU front thread makes about 1,080 "full drains" a second (about 40 per frame) and waits
  about 310 ms a second for the back thread. An acquire takes about 11 ms to resume there, against
  0.75 ms in the 60 fps scene.
- **140V (tester):** the "30 fps" at the start was the intro movie. Gameplay runs at **5-7 fps**.
- Fences are fast (2 µs, above), so the waits mean queued work. Whether the GPU or the back
  thread is saturated needs GPU timestamps: the **profiling run** below gives them.

### Profiling run results (IntelFix2, B580, 2026-11-11)

| Scene | fps | GPU busy per frame | Submits per frame |
|---|---|---|---|
| Light scenes | 60 | 2-3 ms | 2 |
| Outdoor slow scene (t = 110-200 s) | 20-30 | **26-40 ms** | 117-193 |

- **The slow scenes are GPU-bound.** One compute shader, **`6bd913e1f34e83eb`** (128 threads
  per workgroup, 6.9 KB shared memory, 64×36 workgroups once per frame, which looks like a
  per-screen-tile pass), takes **13.5 ms of the ~21 ms** of timed GPU work in every frame. The
  next one takes 1.3 ms. This one shader is the main FPS gap.
- About 80 % of the ~3,600 submits a second come from one guest fence per submit
  (`vk_fence_manager.cpp:29`). Measured on the B580 that costs only about 5 µs of GPU time per
  extra submit (0.75 ms per frame), so it's not the main cost. The existing batching option
  (`EDEN_NX2_FENCE_FLUSH_US`) stays off because it caused white flashes in OoT.
- Next step: find why `6bd913e1f34e83eb` is about 4-6× slower than it should be on Intel.
  Suspect: it uses warp ops, so it is forced to SIMD32, and Intel spills registers at SIMD32
  (the emulator's own comment notes this for Mario Kart World). Being checked offline with
  Intel's compiler statistics.

## B1. MSVC build fix (no runtime effect)

**File:** `src/CMakeLists.txt`

The tree is built with clang-cl, but MSVC turns 16 style warnings into errors (`/we4459` and
others), so the MSVC build stopped on its first shadowed name, in `frizti_settings.cpp`. With
`ENABLE_WERROR=OFF` these warnings now stay warnings (`/w1NNNN`). Builds with the default
`ENABLE_WERROR=ON` are unchanged.

---

## 8. Intel matrix path rounds like the guest (main menu red lines)

**Build:** IntelFix4 (`Fritchii-v5-IntelFix4/`), not yet tested in game.
**File:** `shader_recompiler/backend/spirv/emit_spirv_tensor.cpp` (`EmitCooperativeTensorMulAdd`,
`CooperativeScratchWordsPerSubgroup`).

**Problem.** NVIDIA's HMMA.16816.F16 rounds the accumulator to fp16 after each K=8 group. The
emulator's software path reproduces that exactly. Fix 7's Intel 8×16×16 path did a K=16 product
with a single rounding. In an offline test of all **55** of OoT's tensor shaders (software vs
matrix path, same inputs, B580), 42 differed, in up to 25 % of their outputs, sometimes with an
infinity on only one side. That is the likely cause of the menu's red lines, which only appeared
with fix 7.

**Fix.** On Intel's 8-row matrices, a K=16 HMMA is two products: B with its upper K half zero,
then with its lower half zero, rounding in between as the guest does. NVIDIA and AMD are
unchanged.

**Verified offline:** the differences are gone, to fp16 rounding (≤ 0.3 % mean, mostly 0
differing values). Inputs within ±0.05 give **bit-identical** results. The leftover differences
with large random inputs also appear when the *software* path is run against itself (the shader
varies run to run then), so they are not from the matrix path. `spirv-val`: 55 of 55 valid.

**Cost:** OoT's hot 64×36 pass takes 2.0 ms instead of 1.4 ms (its HMMAs are all K=16), still
7.5× faster than the 15 ms software path. Expect a slightly lower FPS than IntelFix3's 40 in that
area, in exchange for correct results.

**Result:** IntelFix4 ran faster than IntelFix3 for the owner (constant 40 fps in small areas).
The menu lines stayed: they had another cause, fixed by change 9.

## 9. Title movie upscaler stays on the bilinear copy on Intel (main menu red lines)

**Build:** IntelFix5 (`Fritchii-v5-IntelFix5/`). **Verified in game on the B580** (title screen
screenshots at fixed frame counts, before and after).
**File:** `video_core/vulkan_common/vulkan_device.cpp` (`g_native_tensor_ops`).

**Problem.** With fix 7 the matrix extension is on, which also sets `g_native_tensor_ops`. The
movie upscaler setting "Automatic" then uses the game's own neural network for the title movie
(32-lane tensor compute programs f9ea14a22394f5b1, eff3ccbe1438679d, e440c9f7923c2973,
4c1ae2fe58f30a97, ...). On Intel's 8×16×16 path that network draws a pink line and speckled
columns over the movie. With `EDEN_NX2_NO_COOP_MATRIX=1` the title is clean.

**Fix.** `g_native_tensor_ops` is false on Intel's 8×16×16 path, so "Automatic" uses the bilinear
copy, as before fix 7. Gameplay HMMA shaders keep the matrix units (and their speed).

**Verified:** the title screen at the same frame count is clean; in Hyrule Field the game runs
with the matrices active (~38-39 fps on this PC at night).

## IntelFix3 results by setup (logs from 2026-11-11)

| Setup | Mode | Result | What limits it |
|---|---|---|---|
| Owner: Arc B580, Ryzen 5 5600G | Docked, Res1X | ~40 fps in the area that ran at 20-30; dips to 15-22 | first-visit compile freezes ("draws held"); a few spots at ~20-22 fps with no compiles, which need a profiling run |
| Tester: Arc B580, i5-14400F, driver 101.9175 | Handheld, **OoT 30 fps on** | steady 30 (the cap), freezes of 1-28 s on first visits | shader compiles: the preparation had just started (`preparing 120461 ... from record 0`) |
| Tester: Arc 140V (laptop iGPU), Core Ultra 9 288V | Handheld (1920×1080) | gameplay ~9 fps, moments of 1-6 | **RAM exhausted**: system commit 48.4 of 54.6 GB *before* the game started (other programs), 1.2-2.7 GB free in play, and the texture cache churns (2,327 images made per 600 frames, 2-4× the B580s) inside the 4 GiB budget the emulator gives integrated GPUs |

- **Change 4 confirmed in the owner's log:** `319 prepared ... 0 skipped (0 in the disk cache,
  0 for another device)`. The prepared shaders match Intel now.
- "Draws held" is the emulator waiting for a pipeline that is still compiling. This is by design
  (`EDEN_NX2_ASYNC_DISCARD=2`: skipping such draws lost text and lighting before). Pipeline
  builds already use one thread per CPU thread minus one. These freezes go away once the
  preparation has run through once.

## How to test IntelFix3

1. Run `Fritchii-v5-IntelFix3\lichii.exe`. The "Preparing shaders" screen still runs at full
   speed. After **Play now** the preparation now waits, so play should be as smooth as the
   profiling run was. It continues while you pause, from Emulation > Prepare Shaders Now, or on
   the screen at the next start.
2. Go to the outdoor area that ran at 20-30 fps and note the FPS (fix 7).
3. **Visual check for fix 7:** in that same spot, start `A-B old tensor path (compare).cmd`
   (the old path) and compare it with `lichii.exe`. If anything looks different between the two
   (lighting, shadows, fog, reflections, noise), tell me what and where.
4. Export the log from step 2 (optionally use `Profile run (send me the log).cmd`, which no longer
   turns the preparation off).

## How to test IntelFix2

1. Run `Fritchii-v5-IntelFix2\lichii.exe` and start Ocarina. A **"Preparing shaders"** screen
   should appear on Intel now (B580: estimated at over 2 hours in total). It doesn't need to
   finish: let it run about 10-15 minutes, then close the emulator. The next start resumes where
   it stopped.
2. Export that log. When the preparation stops it writes `Precompiled pipelines: N prepared ...
   skipped (... 0 for another device) ... not reached`. "0 for another device" is the proof that
   the prepared shaders match Intel now. Before this fix all 116k graphics records were skipped
   there.
3. Check that the visuals are still fine (change 4 turns on a Vulkan feature that was off on
   Intel until now).
4. **Profiling run:** start `Profile run (send me the log).cmd` in the same folder. Go to the
   scene that runs at about 27 fps, stay for about a minute, then export the log. It adds the GPU
   time per frame and the slowest shaders, which says whether the GPU or the emulator's thread is
   the limit.

## How to test IntelFix1

1. Run `Fritchii-v5-IntelFix1\lichii.exe`. Keep Mesh Shaders on **Automatic**.
2. Play the same spots as before: Kokiri Forest and Hyrule Field (foliage), places where light
   beams or flashes showed up, and foggy areas.
3. Report per glitch whether it is gone, better, the same or worse. Then send the log export,
   which must say `intel-fix-4f5c53149b` on its first line.

## Quick test you can do with the current build (no rebuild)

This checks the foliage diagnosis from change 2. In the Ocarina game properties, set
**Graphics (Extra) → Vulkan Extensions → Mesh Shaders: Compute Fallback**. Then look at foliage
(Kokiri Forest, Hyrule Field grass). If the flicker goes away or mostly goes away, the subgroup
diagnosis is confirmed. The compute fallback may be slower in dense foliage. Set it back to
Automatic for the new build.

## 10. Optional: 16-lane compute shaders on Intel (off by default)

**Build:** IntelFix6. **Files:** `video_core/vulkan_common/vulkan_device.h`
(`Device::NarrowComputeSubgroupSize`), `video_core/renderer_vulkan/vk_compute_pipeline.cpp`.

**Finding.** ~13 % of OoT's compute shaders are large (24-31k instructions, the global-memory
fallback). For each, Intel's driver compiles SIMD16 *and* SIMD32 (4.3-5.3 s) and often keeps
SIMD32 with 5.7 KB of register spills. Required 16 lanes: 1.8 s, no spills (56-shader sample).

**Measured in game:** slower at run time: Hyrule Field ~35 instead of ~39.5 fps on the B580
(f84d5909908cc5ea 0.14 instead of 0.07 ms a dispatch). So it is **off**; the pipelines are
exactly as before. `EDEN_NX2_INTEL_COMPUTE_SIMD16=1` turns it on for those shaders (`2`: every
compute shader without warp operations) if shorter compile freezes matter more than ~4 fps.

## 11. Native float atomic add on Intel

**Build:** IntelFix6. **Files:** `vulkan_device.h/.cpp` (VK_EXT_shader_atomic_float, Intel only),
`shader_recompiler/profile.h` (`support_float32_atomic_add`), `vk_pipeline_cache.cpp`,
`backend/spirv/spirv_emit_context.h/.cpp`, `emit_spirv_atomic.cpp`, `emit_spirv.cpp`
(`RewriteOpcodes`), `tools/shader-sweep/main.cpp`.

**Problem.** The emulator does every storage float32 atomic add as a compare-exchange retry loop.
OoT's 1e1964184a291f00 (96 workgroups summing into a few words) spent **1.8-2.0 ms of every
frame** in that loop on the B580, which has hardware float atomic add.

**Fix.** On Intel with `shaderBufferFloat32AtomicAdd`, those adds are one `OpAtomicFAddEXT` on a
float view of the same buffer. `EDEN_NX2_NO_FLOAT_ATOMICS=1` restores the loop.

**Verified:** `spirv-val` valid; offline the outputs match the loop (8 of 8.4 million values
differ in the last bit, from the order of the additions, which also varies run to run with the
loop); in game the shader is gone from the GPU's slowest list, the image is unchanged, and FPS
in Hyrule Field is equal within noise on the B580 (that scene waits on CPU<->GPU round trips more
than on the GPU). Expected to help most where the GPU is the limit (A750, 140V).

**Note:** changes 10 and 11 touch the shader translator and pipeline creation, so the
"Preparing shaders" step runs once more. Graphics pipelines Intel's driver already has come from
its cache.
