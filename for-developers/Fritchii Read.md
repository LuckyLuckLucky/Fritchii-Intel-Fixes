# Intel Arc fixes for Fritchii: what we did

Hi Frizti. This package has the Intel GPU fixes that came out of testing Ocarina of Time on Intel
Arc. Each fix is its own commit on top of your latest code (`oot-gather-fix` @ `4f5c53149b`,
tag `fritchii-v5.8.4`), so you can take all of them or pick some. They apply with no conflicts.

**In one line:** all known Intel visual glitches in OoT are fixed. On an Arc B580 the slow outdoor
areas went from 20-30 fps to about 40 fps. "Preparing shaders" now works on Intel, and the lag
spikes are gone. NVIDIA and AMD render the same as before (details under "Effect on other GPUs").

## What's in this folder

| File | What it is |
|---|---|
| `Fritchii Read.md` | This file: what we did, why, how it was tested |
| `Fritchii agent Intel Fixes Read.md` | Step-by-step guide for your coding agent: how to apply, port by hand and verify each fix |
| `patches/0001-...0014-*.patch` | One `git format-patch` file per fix, base `4f5c53149b` |
| `intel-fixes.bundle` | The same 14 commits as a git bundle (branch `intel-fixes`) |
| `intel-fixes-all.patch` | All fixes as one diff (identical to the shipped IntelFix7 source) |
| `CHANGELOG-Intel.md` | The player-facing changelog we gave testers |
| `background/` | Full notes: `INTEL_CHANGES.md` (every fix in detail, test results), `OVERNIGHT_LOG.md` (measurements, things we tried that didn't help), `POSTPONED_FOR_USER.md` (open ideas) |
| `tools/` | Sources of the analysis tools we used (precompile-file stats, standalone Vulkan benchmarks for Intel compile and cache behaviour, HMMA difference test, debug-server scripts). Paths in the scripts point to `C:\work\Fritchii-intel` and need adjusting. |

## How to take it

Any one of these:

```bash
# A) Git bundle (keeps the 14 commits and their messages)
git fetch "path/to/For-Main-Dev/intel-fixes.bundle" intel-fixes:intel-fixes
git merge intel-fixes          # or: git cherry-pick <the commits you want>

# B) Patch files
git checkout oot-gather-fix
git am path/to/For-Main-Dev/patches/*.patch

# C) GitHub: once the owner has an account, the branch can also arrive as a pull request.
```

Commits 0011 to 0014 are optional (see the table). Leave them out if you don't want them.

## The fixes

| # | Commit | Problem on Intel | Fix | Tested |
|---|---|---|---|---|
| 1 | `vulkan: dynamic primitive topology follows the pipeline's tessellation stages` | Random light beams, stray stretched triangles | With extended dynamic state, `vkCmdSetPrimitiveTopologyEXT` got the guest topology (TRIANGLE_LIST) for tessellation pipelines. Intel uses it as given, NVIDIA doesn't care. Now it follows `FillStates`' rules (PATCH_LIST with tessellation) and is re-recorded when only the pipeline changes. | In game, B580 + 140V: **fixed** |
| 2 | `vulkan: task/mesh stages with warp ops require 32-lane subgroups` | Flickering and missing foliage | Intel runs task/mesh shaders at SIMD16 or SIMD32. At SIMD16 a guest ballot sees half the warp and the meshlet compaction breaks. The `requiredSubgroupSize = 32` struct was already built but never chained (`[[maybe_unused]]`). Now it is chained for stages that use warp-width ops and allow it. | In game: **fixed** |
| 3 | `shader_recompiler: keep shuffles in bounds on host subgroups narrower than the warp` | Flashing lights, NaN bloom | Intel's fragment and vertex shaders can't be pinned to 32 lanes. A shuffle from lane 16-31 read a lane that doesn't exist (undefined, NaN). Such a source lane now counts as out of range, as the guest does. This only applies where the minimum subgroup size is below 32. | In game: **fixed** |
| 4 | `vulkan_device: keep VK_EXT_vertex_input_dynamic_state on Intel 101.x drivers` | Prepared shaders useless on Intel, more pipelines | The "old Intel driver" check compared Intel's build number with `VK_MAKE_API_VERSION(27,20,100,0)`. **Every** Intel driver counted as old, so the extension was off. All 116k graphics records of the OoT recipe use it, so all were skipped. The build number is now decoded properly. | Log: `0 for another device` |
| 5 | `vk_pipeline_cache: prepare on Intel, into the driver's own shader cache` | "Preparing shaders" never ran on Intel | Intel's Windows driver has a persistent disk cache (a 9.7 s compile came back in 1.4 ms in a new process). Intel now counts as caching pipelines, the same way the AMD app-cache mode works. | Works in game |
| 6 | `vk_pipeline_cache: on Intel the preparation does not build beside the game` | Lag spikes after "Play now" | On Intel the preparation runs only on its screen, while paused, or from Prepare Shaders Now (the warm-up's rule). | Owner: no lag spikes |
| 7 | `vulkan_device: guest HMMA on 8x16x16 cooperative matrices on Intel's Windows driver` | One shader cost 13.5 ms per frame | OoT's tile pass `6bd913e1f34e83eb` emulated HMMA with shuffles (12 KB register spill per lane). The 8x16x16 matrix path (#779) was blocked on Intel Windows over float16, but the device keeps `shaderFloat16` for matrices anyway. Offline: 15.1 ms down to 1.25 ms. | **20-30 to ~40 fps** (owner, B580) |
| 8 | `spirv: Intel's 8x16x16 HMMA rounds after each K=8 half, as the guest` | Matrix results differed from the guest | HMMA.16816 rounds to fp16 after each K=8 half. One K=16 product rounds once: 42 of 55 OoT tensor shaders differed. Now it's two products, rounded in between. | Offline: matches the software path; in game: steady 40 fps in small areas |
| 9 | `vulkan_device: "Automatic" movie upscaler stays on the bilinear copy on Intel's 8x16x16 path` | Pink line and speckles on the title movie | The game's movie network misbehaves on the 8-row matrix path. Gameplay HMMA keeps the matrices. | In game: title screen clean |
| 11 | `spirv: native float32 storage atomic add on Intel` | 2 ms per frame in a compare-exchange loop | Uses `VK_EXT_shader_atomic_float` (`OpAtomicFAddEXT`), Intel only for now. | Shader gone from the GPU top list, same image |
| 10 | `vulkan: optional 16-lane compute shaders on Intel (off by default)` | Long compile freezes | Compiles 2.6x faster but runs slower (~4 fps), so it's **off** (env switch only). | **Optional** |
| D1 | `texture_cache: optional log of image read-backs` | n/a | Diagnostic log behind `EDEN_NX2_FLUSH_AREA_LOG=1`. | **Optional** |
| - | `shader-sweep: Intel profile switches` | n/a | `SWEEP_INTEL` / `SWEEP_COOP8` for offline analysis | **Optional**, tools only |
| B1 | `cmake: MSVC keeps the /we warnings as warnings with ENABLE_WERROR=OFF` | n/a | Lets the tree build with MSVC instead of clang-cl | **Optional**, build only |

Every Intel-specific behaviour has an off switch: `EDEN_NX2_INTEL_DRIVER_CACHE=0`,
`EDEN_NX2_PRECOMPILE_WHILE_PLAYING=1`, `EDEN_NX2_NO_COOP_MATRIX=1` and `EDEN_NX2_NO_FLOAT_ATOMICS=1`.
Change 4 can be undone with the existing *Vertex input dynamic state* setting.

## Effect on other GPUs

- **NVIDIA:** only fix 1 changes commands there. The dynamic topology for tessellation pipelines
  is now PATCH_LIST instead of the guest's value. This is the valid-Vulkan value, and NVIDIA
  treated those draws as patches anyway, so nothing should look different. Fix 2 chains a
  32-lane request, which is a no-op on NVIDIA. **Worth a quick NVIDIA check before release.**
- **AMD Windows:** same as NVIDIA. Only compute can be pinned there, so fix 2 does nothing.
- **AMD/Intel on Linux (Mesa):** fix 2 can now pin task/mesh shaders on drivers that allow it
  (RADV, ANV). That is the intended guest behaviour, but it was not tested on Linux. Fixes 8 and
  9 also apply to ANV's 8x16x16 path, which they make correct.
- **Shader stamp:** merging touches the shader translator and pipeline files, so
  `NX2_SHADER_STAMP` changes and **every user runs "Preparing shaders" once more**. That's the
  same as any shader-translator change.

## How it was tested

- **Hardware:** the owner's Arc B580 (driver 101.9150) with a Ryzen 5 5600G, in OoT. Testers
  with another B580 (i5-14400F, driver 101.9175), an Arc 140V laptop and an A750.
- **Fixes 1-3** shipped together in one build. Together they fixed every reported glitch
  (missing textures, light beams, flickering foliage, fog, flashes). We can't credit each glitch
  to one fix.
- **Builds:** IntelFix1 to IntelFix7 were shipped to testers. The commit boundaries match those
  builds byte for byte: commits 1-3 = IntelFix1, 1-5 = IntelFix2, 1-7 = IntelFix3, 1-10 =
  IntelFix6, and all commits = IntelFix7 (the optional commits were moved to the end).
- **Offline checks for the shader-side changes:** `spirv-val` and A/B output comparisons on the
  B580 (`tools/gplbench/difftest.cpp`, `hotrun.cpp`).

## What's still open (not in these commits)

- **Hyrule Field, looking into the distance:** ~25 fps on a B580 (40 when facing a wall). It's
  synchronization-bound, not one slow item. The front thread waits for the back thread per second
  on ~1,340 macro DMA reads (occlusion reports, ~270 ms) and ~1,400 fence releases (~190 ms).
  `EDEN_NX2_NO_REACTIVE=1` gives +2 fps but risks stale reads, so it's not enabled. Many EXP
  switches were A/B-tested with no gain (list in `background/OVERNIGHT_LOG.md`).
- **Grass shadows** still flicker slightly on Intel.
- **9 pipelines failed** in one Intel preparation run. They are logged only at Debug level, so
  they aren't identified yet.
- **Arc A750 (Alchemist)** has no 8x16x16 matrix shape, so it stays on the slow software HMMA.
- **Ideas not done:** the global-memory fallback costs ~4.7 s per shader to compile on Intel
  (a buffer-device-address redesign would fix it). Also, HMMA zero-tile stores could be written
  once per shader. See `background/POSTPONED_FOR_USER.md`.
- **Next:** we have a lot of new tester logs and will use them for further improvements once you
  have these fixes.

If anything is unclear, the full reasoning and numbers for every fix are in
`background/INTEL_CHANGES.md`.
