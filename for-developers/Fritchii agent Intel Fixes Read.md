# Agent guide: apply and verify the Intel Arc fixes

Audience: a coding agent working on the Fritchii tree. Goal: bring in the Intel fixes from this
folder, build, and verify them. The human-readable overview is `Fritchii Read.md`, and the full
reasoning per fix is `background/INTEL_CHANGES.md`.

## Facts you need

- **Base:** `Frizti/Fritchii`, branch `oot-gather-fix` @ `4f5c53149b7e17d1c24445f68d79a83a3b2a17f9`
  (tag `fritchii-v5.8.4`). The fixes were made and tested on exactly this commit.
- **Source of truth:** `patches/0001-...0014-*.patch` (`git format-patch`, `--base` recorded) and
  `intel-fixes.bundle` (branch `intel-fixes`, the same commits). `intel-fixes-all.patch` is the
  combined diff. All three give the same tree.
- **Files touched (21):** `src/video_core/vulkan_common/vulkan_device.{h,cpp}`,
  `src/video_core/renderer_vulkan/{vk_rasterizer.{h,cpp}, vk_state_tracker.{h,cpp},
  vk_graphics_pipeline.{h,cpp}, vk_compute_pipeline.cpp, vk_pipeline_cache.cpp}`,
  `src/video_core/nx2_precompile.h`, `src/video_core/texture_cache/texture_cache.h`,
  `src/shader_recompiler/profile.h`, `src/shader_recompiler/backend/spirv/{spirv_emit_context.{h,cpp},
  emit_spirv.cpp, emit_spirv_atomic.cpp, emit_spirv_tensor.cpp, emit_spirv_warp.cpp}`,
  `tools/shader-sweep/main.cpp`, `src/CMakeLists.txt`.
- **Line endings:** the repo stores LF. With `core.autocrlf=true` on Windows, `git am` still
  works, but don't compare files byte for byte against a CRLF checkout.
- Follow the repo's own `AGENTS.md`: keep comments short, and don't touch unrelated code.

## Step 1: apply

```bash
git fetch <path>/intel-fixes.bundle intel-fixes:intel-fixes
git checkout oot-gather-fix
git merge --no-ff intel-fixes        # or cherry-pick a subset, see the table below
# alternative: git am --3way <path>/patches/*.patch
```

If the tree has moved past `4f5c53149b` and a patch fails, run `git am --3way`. Port the failing
hunks by hand using the per-fix notes below; each note says what the code must do, not only
where it was.

| Patch | Fix | Required? | Depends on |
|---|---|---|---|
| 0001 | 1 topology / tessellation | yes | - |
| 0002 | 2 task/mesh 32-lane subgroups | yes | - |
| 0003 | 3 shuffle bounds on narrow subgroups | yes | - |
| 0004 | 4 vertex input dynamic state on Intel 101.x | yes | - |
| 0005 | 5 prepare into Intel's driver cache | yes | - |
| 0006 | 6 prepare holds while playing on Intel | yes | 0005 (`IntelDriverCache`) |
| 0007 | 7 HMMA on 8x16x16 coop matrices (Intel Windows) | yes | - |
| 0008 | 8 K=16 HMMA split for guest rounding | yes, with 0007 | 0007 to have an effect |
| 0009 | 9 movie upscaler off the 8x16x16 path | yes, with 0007 | - |
| 0010 | 11 native float32 atomic add | yes | - |
| 0011 | 10 optional 16-lane compute (off) | optional | - |
| 0012 | D1 image read-back log (off) | optional | - |
| 0013 | shader-sweep Intel switches | optional (tools) | 0003, 0010 (profile fields) |
| 0014 | B1 MSVC `/we` to `/w1` with `ENABLE_WERROR=OFF` | optional (build) | - |

## Step 2: build

Build as usual (the tree is normally built with clang-cl). To build with MSVC instead, keep 0014
and configure with `-DENABLE_WERROR=OFF`. Merging changes `NX2_SHADER_STAMP` (a hash of the
shader translator and pipeline sources), so every user's "Preparing shaders" runs once more.
That is expected.

## Step 3: verify

Log lines to check on an Intel Arc GPU on Windows (`Help > Open Log Folder`):

| Fix | Expected in the log |
|---|---|
| 4 | No `Disabling broken VK_EXT_vertex_input_dynamic_state`. After a preparation: `... 0 for another device` |
| 5 | `Our VkPipelineCache file: on for pipelines built in play (beside the driver's cache, up to 512 MiB)`; no `Precompiled pipelines skipped: the driver keeps no pipeline cache of its own`; the Preparing shaders screen appears |
| 7 | `Guest tensor operations run on 8x16x16 cooperative matrices`; no `Removing features for unsuitable extension VK_KHR_cooperative_matrix` |
| 1 | `Patch topology not used with tessellation, using patch list` may still appear at pipeline build (expected) |

In-game checks (OoT, Arc), with the switch that turns each fix off for comparison:

| Fix | Check | A/B switch |
|---|---|---|
| 1-3 | Kokiri Forest and Hyrule Field: no flickering or missing foliage, no light beams, no flashes | none (revert the commit) |
| 6 | After Play now, no lag spikes while the preparation is unfinished | `EDEN_NX2_PRECOMPILE_WHILE_PLAYING=1` |
| 7/8 | Hyrule Field ~38-40 fps on a B580; with `EDEN_DKB_FRAME_TRACE=1` + `EDEN_NX2_GPU_PROFILE=1`, `6bd913e1f34e83eb` is ~2-2.9 ms instead of ~13.5 ms | `EDEN_NX2_NO_COOP_MATRIX=1` |
| 9 | Title movie without a pink line or speckled columns | `EDEN_NX2_NO_COOP_MATRIX=1` |
| 11 | `1e1964184a291f00` no longer in the GPU top list; image unchanged | `EDEN_NX2_NO_FLOAT_ATOMICS=1` |
| 5 | Preparation runs on Intel | `EDEN_NX2_INTEL_DRIVER_CACHE=0` |

Regression check on **NVIDIA** (and AMD if available): play a scene with tessellation (fix 1
changes the dynamic topology for every vendor) and compare with the previous build. No
difference is expected.

---

## Per-fix notes (for porting by hand)

### Fix 1: dynamic topology follows tessellation (0001)
Root cause: with EDS1+, `UpdateDynamicStates` recorded `vkCmdSetPrimitiveTopologyEXT(guest
topology)`. Only the static state was fixed to PATCH_LIST in `FillStates`. Intel has
`dynamicPrimitiveTopologyUnrestricted` and uses the dynamic value.
1. `vk_graphics_pipeline.h`: add public `HasTessellation()` and private `bool has_tessellation{}`.
   In the constructor after `present_stages[...]` is filled, set
   `has_tessellation = !IsMeshPipeline() && (present_stages[1] || present_stages[2]);`.
2. `vk_state_tracker.h/.cpp`: add `VkPrimitiveTopology current_host_topology =
   VK_PRIMITIVE_TOPOLOGY_MAX_ENUM`. Reset it wherever `current_topology` is invalidated (both
   invalidation functions in the header plus `InvalidateState`). Add
   `ChangeHostPrimitiveTopology(VkPrimitiveTopology)`. Include `video_core/vulkan_common/vulkan.h`.
3. `vk_rasterizer.h`: add member `dynamic_host_topology` (default TRIANGLE_LIST).
4. `vk_rasterizer.cpp` `UpdateDynamicStates`: compute `dynamic_host_topology` =
   `MaxwellToVK::PrimitiveTopology(device, topology)`. Use PATCH_LIST if
   `pipeline->HasTessellation()`, else POINT_LIST where it would be PATCH_LIST. Then
   `host_topology_changed = !emulated_mesh &&
   state_tracker.ChangeHostPrimitiveTopology(dynamic_host_topology)`. Enter the block on
   `topology_changed || host_topology_changed`, and record the topology on `host_topology_changed`.
5. `UpdatePrimitiveRestartEnable`: use `IsPrimitiveRestartSupported(device, dynamic_host_topology)`.

### Fix 2: task/mesh stages pinned to 32 lanes (0002)
1. `vulkan_device.h`: `CanRequireGuestWarpSize(stage)` = subgroup_size_control extension &&
   `IsGuestWarpSizeSupported(stage)` && `minSubgroupSize <= 32 <= maxSubgroupSize`.
2. `vk_graphics_pipeline.cpp`: helpers `UsesWarpWidthOps(info)` (vote || invocation_id ||
   shuffles || mask; not quad ops) and `GuestWarpSizeChain(device, info, stage, ci)`.
3. `MakePipeline`: remove `[[maybe_unused]]` from `subgroup_size_ci`. Compute the stage flag once
   and set `.pNext = emulated_vertex ? nullptr : GuestWarpSizeChain(...)`.
4. `MakeShaderLibrary` (GPL): declare the same `subgroup_size_ci{32}` and chain it the same way.
On the B580, `requiredSubgroupSizeStages` = compute | task | mesh.

### Fix 3: shuffle bounds on narrower subgroups (0003)
1. `profile.h`: `bool warp_size_potentially_smaller_than_guest{}` directly after
   `warp_size_potentially_larger_than_guest`. The order matters because the profile uses
   designated initializers.
2. `vulkan_device.h`: `IsWarpSizePotentiallySmallerThanGuest()` = min subgroup size
   (`subgroup_size_control.minSubgroupSize`, or `subgroupSize` without the extension) is non-zero
   and < 32. Set it in `vk_pipeline_cache.cpp`'s profile initializer.
3. `spirv_emit_context.h/.cpp`: `Id subgroup_size{}`. In `DefineInputs`, inside the block that
   defines `subgroup_local_invocation_id`, define BuiltIn `SubgroupSize` when
   `uses_subgroup_shuffles && warp_size_potentially_smaller_than_guest`. Decorate it `Flat` in
   fragment shaders.
4. `emit_spirv_warp.cpp`: `InHostSubgroup(ctx, in_range, src)` ANDs `src < SubgroupSize`. Apply
   it to `in_range` in `EmitShuffleIndex/Down/Butterfly`, before `SetInBoundsFlag`. ShuffleUp
   needs nothing.

### Fix 4: vertex input dynamic state on Intel (0004)
`vulkan_device.cpp`, in the `vertex_input_dynamic_state` driver check: the Intel condition becomes
`(driverVersion >> 14) < 101`. Intel encodes `(build_major << 14) | build_minor`; `101.9150` =
1663934. Keep the NVIDIA condition.

### Fix 5: prepare into Intel's driver cache (0005)
`vk_pipeline_cache.cpp`, anonymous namespace:
- `IntelDriverCache(device)`: true on `VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS` unless
  `EDEN_NX2_INTEL_DRIVER_CACHE=0`.
- `AppCacheBesideDriver(device)` = `AmdAppCache || IntelDriverCache`.
- `DriverCachesPipelines`: add `case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS: return IntelDriverCache(device);`.
- Replace the other `AmdAppCache(...)` uses (the `use_vulkan_pipeline_cache` initializer, the "Our
  VkPipelineCache file" log line, and both checks in `PlayPipelineCache()`) with
  `AppCacheBesideDriver(...)`. Update the `Mode()` comment in `nx2_precompile.h`.

### Fix 6: preparation holds while playing on Intel (0006)
`PipelineCache::PreparePrecompiled`, after `full_threads`: set `hold_beside_game =
IntelDriverCache(device) && !EDEN_NX2_PRECOMPILE_WHILE_PLAYING`. Add a lambda
`allowed_threads(full)` that returns `full_threads` if full, else `0` if `hold_beside_game &&
!shader_notify.WarmMayBuild()`, else `threads`. The worker parks on `worker >=
allowed_threads(full)`.

### Fix 7: HMMA on Intel Windows coop matrices (0007)
`vulkan_device.cpp`, in the cooperative matrix block: remove `driver_id !=
VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS &&` from the `cooperative_hmma_8x16x16` condition and
update the comment. Nothing else is needed: `float16_math` keeps float16 arithmetic off for
translated shaders, and `shaderFloat16` is re-enabled for `cooperative_matrix` before device
creation. Compute pipelines with coop matrices already use `requiredSubgroupSize = 32` and
`REQUIRE_FULL_SUBGROUPS`.

### Fix 8: K=16 split on 8-row matrices (0008)
`emit_spirv_tensor.cpp`:
- `CooperativeScratchWordsPerSubgroup(profile)` returns 512 on the 8x16x16 path, otherwise 384.
  Use it for the scratch base and in `ReserveCooperativeHmmaScratch`.
- `EmitCooperativeTensorMulAdd(..., u32 depth)`, with `depth` passed from `EmitTensorMulAdd`.
  When `depth == 16 && types.rows == 8`, store B's lower K half in `tile_b` (upper zeroed) and
  the upper half in `tile_b_high = base + 384` (lower zeroed). Run the multiply loop twice with a
  `SubgroupBarrier` in between, so the accumulator in `tile_c` is rounded to fp16 between the halves.

### Fix 9: movie upscaler (0009)
`vulkan_device.cpp`: `g_native_tensor_ops.store(extensions.cooperative_matrix &&
!cooperative_hmma_8x16x16, relaxed)`.

### Fix 11: native float32 atomic add (0010)
- `vulkan_device.h`: add `FEATURE(EXT, ShaderAtomicFloat, SHADER_ATOMIC_FLOAT,
  shader_atomic_float)` to `FOR_EACH_VK_FEATURE_EXT`, and `IsFloat32AtomicAddSupported()`.
- `vulkan_device.cpp` `RemoveUnsuitableExtensions`: keep the extension only for vendor 0x8086
  with `shaderBufferFloat32AtomicAdd`, unless `EDEN_NX2_NO_FLOAT_ATOMICS=1`.
- `profile.h`: `support_float32_atomic_add`, set from the device in `vk_pipeline_cache.cpp`.
- `spirv_emit_context`: `native_f32_atomic_add = uses_atomic_f32_add && support_float32_atomic_add
  && support_descriptor_aliasing`. When set: add the F32 SSBO view, extension
  `SPV_EXT_shader_atomic_float_add` and capability `AtomicFloat32AddEXT`, and skip the
  `f32_add_cas` function.
- `emit_spirv_atomic.cpp` `EmitStorageAtomicAddF32`: emit `OpAtomicFMax` on the F32 pointer and
  push `{result, OpAtomicFAddEXT}` to `ctx.opcode_rewrites`; `emit_spirv.cpp` `RewriteOpcodes`
  also matches `OpAtomicFMaxEXT`. Sirit has no FAdd, and FMax has the same word layout.

### Optional: 0011 (fix 10), 0012 (D1), 0013 (shader-sweep), 0014 (B1)
- 0011: see the commit. `Device::NarrowComputeSubgroupSize` + `vk_compute_pipeline.cpp`, env
  `EDEN_NX2_INTEL_COMPUTE_SIMD16=1|2`; default off, so pipelines are unchanged.
- 0012: log lines only.
- 0013: offline analysis only.
- 0014: MSVC only.

## Tested and NOT worth porting (don't redo these)

These were measured in OoT's Hyrule Field distance view on a B580, ~26 fps each, and all were
within noise: fragment_barrier, EXP62, EXP40=3, EXP30=1, EXP31=1, EXP10=1/5, EXP12=1, EXP24=1,
EXP37=1, 0.75x resolution (worse). `EDEN_NX2_NO_REACTIVE=1` gives +2 fps but allows stale guest
reads. GPL on Intel gives no compile-time gain (measured), so leave it off. Rejected for known
upstream visual risk: `EDEN_NX2_FENCE_FLUSH_US` (white flashes) and `clean_words`
(zortos293/Lichii#161).

## Tools (`tools/`)

- `nx2prep_stats.cpp`: read-only stats for a `.nx2prep` file. It can also dump one record's
  environment for `shader-sweep --environment`.
- `gplbench/`: standalone Vulkan programs. `gplbench` measures compile reuse, GPL, fast-link cost
  and submit round trips. `cstats` prints the driver's SIMD width, instructions and spill
  (pipeline executable properties). `hotrun` and `difftest` time two SPIR-V versions and compare
  their outputs. `ctime` measures compile time.
- `hmma_difftest.ps1`, `sweep_*.sh`: the offline HMMA A/B used for fix 8.
- `dbg.ps1`, `field_run.ps1`, `distance_run.ps1`, `ab_live.ps1`, `launch_debug.cmd`: drive the
  emulator's debug server (`EDEN_NX2_DEBUG_SERVER`) for repeatable fps measurements. **Run only
  one emulator instance at a time;** `distance_run.ps1` closes running ones first.
- All paths point to `C:\work\Fritchii-intel`, so adjust them.
