# Postponed for the user

## 1. Global-memory fallback compile cost (Intel)
~500 of OoT's compute shaders take ~4.7 s each to compile on an Arc B580 (prepare time, first-use
freezes). Cause and numbers: `OVERNIGHT_LOG.md`, "Compile-time survey". A redesign (buffer device
addresses) touches how every shader reads guest memory: it needs in-game A/B validation on NVIDIA
and Intel before it can ship. Not started.

## 2. 16-lane compute shaders on Intel (fix 10, off by default)
Forcing 16 lanes on the global-memory compute shaders makes them compile ~2.6x faster without
register spills (4.8 s -> 1.8 s each), which would shorten first-visit freezes and the shader
preparation. But they run slower: Hyrule Field ~35 instead of ~39.5 fps on the B580. So it ships
**off**. Try it yourself if freezes bother you more than ~4 fps: set the environment variable
`EDEN_NX2_INTEL_COMPUTE_SIMD16=1` before starting lichii.exe (`2` = all compute shaders without
warp operations). A per-shader choice (16 lanes only where it doesn't slow down) needs
measurements of each shader, which I could not do in one night.

## 3. HMMA zero-tile stores (small GPU gain)
Intel's matrix path (fix 8) rewrites constant zero words for every tensor multiply. Hoisting them
to shader start could save ~0.1-0.3 ms per frame in OoT's upscaler pass. Needs care in the SPIR-V
backend; low priority.
