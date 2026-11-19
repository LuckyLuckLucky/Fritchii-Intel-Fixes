# Fritchii switches

What the live switches and the numbered experiments do. The switches themselves are in
src/video_core/nx2_debug_control.h, the experiments' defaults (with the measurements behind
them) in src/common/fritchii_switch_defaults.cpp. This file is not compiled: editing it
rebuilds nothing.

## Named switches

### Profiling mode (Help > Profiling Mode)

For testers who send us a log (helper BP, 2026-11-16). Help > Profiling Mode (both the Fritchii
and the classic UI), then Restart Now: Fritchii starts again with the switches below; play, then
Help > Export Log and send the zip. Saved in qt-config.ini as [UI] profiling_mode (main.cpp reads
it there before the settings load). The switches are read once per process, so turning it on or
off asks to restart Fritchii (a running game closes: save first); the restart gets the launch
environment, not the switches this process added. While it is on:
- the switches: EDEN_NX2_GPU_PROFILE=1, FRITCHII_MEM_CENSUS=3, FRITCHII_HITCH_SPLIT=1,
  EDEN_NX2_HITCH_MS=40, FRITCHII_VRAM_REPORT=1, EDEN_NX2_IMAGE_ALLOC_TIMING=1, EDEN_NX2_TC_LOG=1
  (util/profiling_mode.cpp). A value from the environment or an nx2_switches.txt wins; profiling
  mode only beats a built-in value. MEM_CENSUS is 3, not 1: 1 also queries the working set, which
  holds its lock 150-270 ms every 30 s (a stutter). HITCH_MS is 40, not 33: with OoT at 30 FPS
  every frame is 33.3 ms and would log a Hitch line.
- the log: "Test switches: ...; profiling mode: ..." and "Profiling mode ON: A=1, ..." at each game
  start; system-info.txt in the export says "Profiling mode: on (...)" or "off".
- a red "PROFILING MODE ON  Turn it off in Help" banner at the top centre of the game, in a window
  and in fullscreen (a frameless tool window above the Vulkan surface, like the in-game controls;
  it steps below them while they show, hides while Fritchii is in the background or a dialog is
  open, and ignores the mouse); "[PROFILING]" in front of the window title; a red "PROFILING ON"
  label in the status bar.
Cost: small. The GPU profile times its zones with the CPU's time stamp counter (~150,000 zones a
second on the GPU thread); the rest are counters, a driver memory query per 30 frames and a few
log lines every 10 s. Expect about 1-3% fps (not measured yet); read fps records with it off.
### FRITCHII_GPU_PRIORITY, FRITCHII_THREAD_QOS, FRITCHII_FOCUS_GRAB, FRITCHII_QUEUE_PRIORITY

TEST, off by default (helper BN, 2026-11-16): the game loses 5+ fps while its window is not
focused, which also skews every automated test (they run unfocused). What is already ruled out:
every GPU thread, CPU core and timer thread runs at THREAD_PRIORITY_TIME_CRITICAL (15, the top of
the normal range), so Windows' foreground CPU boosts can't raise them; EXP111 (process priority
class) measured no gain on 2026-11-12; the process already opts out of power throttling and timer
throttling at start (common/windows/timer_resolution.cpp); nothing in Fritchii's own code changes
with focus except "Pause when not in focus" and "Mute audio when in background". Left for a
focused window: the GPU scheduler's treatment of the foreground process, the QoS level Windows
gives threads (High in focus, Medium visible, Low covered or minimized; processes playing audio
count as High, and tests mute the game), and how the present is shown (independent flip vs DWM
composition). Code: common/windows/fritchii_focus.cpp, video_core/fritchii_focus.h (called once a
frame from RasterizerVulkan::TickFrame). Windows only; nothing system-wide is read or changed.

- FRITCHII_GPU_PRIORITY (live): the process's GPU scheduling priority class
  (D3DKMTSetProcessSchedulingPriorityClass). 0 = the class the process had at start (normal),
  1 = above normal, 2 = high. Realtime is not offered (needs extra privileges, has hung NVIDIA
  drivers). Log on every change: "FRITCHII_GPU_PRIORITY=N: GPU scheduling class C asked (was W at
  start), status 0x..., now C2" (status 0 = taken; anything else = Windows refused).
- FRITCHII_THREAD_QOS (live): 1 = the process opt-out of power throttling is set again and every
  thread that asked for High priority or above (Common::SetCurrentThreadPriority) is tagged High
  QoS on its own (SetThreadInformation ThreadPowerThrottling, EXECUTION_SPEED in the control mask,
  clear in the state mask); threads starting later are tagged as they start. 0 = those threads go
  back to system-managed QoS (the process opt-out stays, as before). Log: "FRITCHII_THREAD_QOS=N:
  T of H hot threads tagged High QoS".
- FRITCHII_FOCUS_GRAB (live): 1 = the game's largest visible window takes the foreground (from a
  short thread of its own; attaches to the foreground thread's input for the hand-over); 0 = the
  window that had the foreground gets it back. For a focused vs unfocused toggle in one session.
  It moves the owner's focus: only while nobody types. Log: "FRITCHII_FOCUS_GRAB=N: taking the
  foreground done / refused by Windows".
- FRITCHII_QUEUE_PRIORITY (boot): 1 = the Vulkan queues ask for high global priority
  (VK_KHR_global_priority, else VK_EXT_global_priority), 2 = realtime. A refusal (not permitted,
  no extension) makes the device as before; the log says which happened.
- Always on, Windows: "Focus: game window focused/unfocused (GPU scheduling class C)", logged
  when it changes (checked once a second), so any log can be split into focused and unfocused
  stretches, and shows whether Windows itself moves the GPU class with the focus.

### FRITCHII_HANG_REPORT_S

ON by default at 5 in every build (helper BB, 2026-11-16, after helper AW's boot hang: 0 frames
from 17 s on, every GPU thread idle, CPU 0.1, and nothing in the log said what the game waited
on). Live, a number of seconds; 0 turns it off. Once a second (on the Pulse thread, or on a small
"FritchiiHangWatch" thread where the Pulse does not run: FRITCHII_PULSE=0, EDEN_NX2_HITCH_LOG=0,
Linux) the emulator looks at the game's presents. When none came for that many seconds while the
game runs (powered on, not shutting down, not paused, not stalled by the frontend, no GDB stub),
it logs one "Hang report:" block at Critical, then nothing more until a frame comes ("Hang report:
frames again after N s without one (re-armed)"). Pausing stops the count; it starts again from
zero on resume. Before the first present (the boot logo) nothing is watched. Everything it reads
is read without changing or waking anything, and every lock it needs is only tried (it says "lock
held for 200 ms" rather than wait). Lines, in order:
1. "no frame for N s while the game runs (S s since the last present, P presents ...)".
2. The stall snapshot (EDEN_NX2_STALL_SNAPSHOT) as a 100 ms present gap would log it: the span
   each GPU thread is in now and for how long, the longest span in the gap, the present thread,
   the master semaphore's ticks.
   Then, with FRITCHII_SLOW_GPU on (the default), "GPU breadcrumbs": the last 8 main-queue
   submissions, what they held and whether the GPU finished or started each.
3. "channels suspended on an acquire: yes/no; acquires waiting (N): device ADDR OP VALUE, word
   now W, for T s" for every semaphore acquire a channel (or the memory stage) is suspended on.
4. "syncpoint waits": each syncpoint something waits on, its guest and host values, the values its
   registered actions wait for (nx2:syncpt's armed slot waits, the decoder's and the present's
   fences) and any thread blocked in WaitGuest/WaitHost.
5. "GPU split: front ..." (the one follower watchdog's front line: the front's wait site, target,
   for how long, published/consumed/played) and, with FRITCHII_ONEF on, "one follower step ...;
   positions ...; E ...; R ...; locks ...".
6. "service calls in flight": every HLE service call not finished, as service, command, handler
   name, "waiting for the service's lock" or "in its handler", and for how long.
7. "game threads in kernel waits": the game's threads in a kernel wait, longest first: thread id,
   priority, core, what (a mutex at ADDR, condition variable key, WaitForAddress ADDR,
   WaitSynchronization on an object type, an IPC reply, SleepThread) and for how long.
8. With FRITCHII_HANG_DUMP=1, the minidump's path.
Cost while frames come: one clock read and one add per present, a few relaxed stores per service
call and per guest wait. A long loading screen without presents, or a game waiting on a frontend
dialog, can also trigger it: once, harmlessly.

### FRITCHII_HANG_DUMP

Off by default (helper BB, 2026-11-16). Boot; FRITCHII_HANG_DUMP=1 makes the hang report
(FRITCHII_HANG_REPORT_S) also write a minidump of the process: every host thread's stack, thread
times and start addresses, the modules (MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules, a
few MiB, no game memory) to fritchii_hang_<pid>_<n>.dmp in the log folder, at most three a
session. The game keeps running (dbghelp suspends the other threads for the write). A host thread
blocked on a host lock (a service handler, the GPU threads) shows its whole stack there; a game
thread blocked in a kernel wait is a parked fiber, whose stack is not in the dump: the report's
"game threads in kernel waits" line covers those. dbghelp.dll is loaded at run time. Windows only:
elsewhere the report says so in one line.
### FRITCHII_MEM_CENSUS

MEASURE, off by default (helper BE, 2026-11-16; the 16 GB RAM / 8 GB card goal). Boot. Why: the
"Memory owners" line left ~3.7 GB of the private working set unattributed at the Castle Town market
(working set 11.8 GB steady, 14.6 GB peak). With the switch on, a thread at low priority wakes
every second; every 30 s, and whenever the working set is the highest yet and 512 MiB above the
last census (at most every 5 s: load peaks), it walks the whole address space (VirtualQuery; one
QueryWorkingSet for residency, the same query the zero-page pass makes, ~150-270 ms of kernel time;
on Linux /proc/self/smaps) and logs four "Memory census (30 s|peak)" lines, committed/resident MiB:
1) the totals: guest backing and guest views (tagged by DeviceMemory), VMA host maps (VMA's
vkMapMemory calls, tagged while the switch is on), census scratch (its own page list), images,
other mapped files, executable private memory (the JIT code caches: one 512 MiB reservation per
core; driver code too), heaps (first segments of each Win32 heap; the CRT heap apart), thread
stacks, other private memory, and "resident not in any of them" (should be ~0);
2) images by name (nvoglv64, nvgpucomp64, Qt, ...); 3) other mapped files by name; 4) the exec
allocations and the other private memory by reservation size (<64 KiB, 64 KiB-1 MiB, 1-16, 16-64,
64-256, 256 MiB+) with the 10 largest allocations: base, reservation MiB, committed/resident,
protection; 5) "more": JIT blocks compiled per core (warm/on demand), the sampled guest residency
(backing + views - both = guest RAM: a guest page read through both addresses is counted twice in
the working set but is one page of RAM), and with 2 every heap's summary. Values: 1 = committed
and resident; 2 = also HeapSummary of every heap (locks each heap for its walk: ~250 ms on a 2.5 GB
CRT heap, game threads that allocate freeze meanwhile; only for a memory hunt); 3 = committed only
(no working set query). Also with the switch on: "Pipeline frees (FRITCHII_MEM_CENSUS)" when 32 or
more evicted graphics pipelines are destroyed at once (private commit and working set given back,
KiB per pipeline: the 0.45 MB in the memory lines is an estimate). Costs: the residency query
holds the working set lock while it runs (a Hitch line shows it as "memory sample"); read the fps
with the switch off.

Added 2026-11-16 (helper BE, after the first census run, b4-census-market-log): the first line also
names the Windows heaps' further segments ("heap segments CRT a/b (n), other heaps"; read from the
segment header, signature 0xffeeffee: the CRT heap grows in segments up to 1 GiB reservations,
which the first run listed as the two ~1 GiB "rw" regions), "heap big blocks" (heap blocks over
~508 KiB, each in an allocation of its own: read from its size header; the 8 largest are listed in
the private line), and "size matches": private allocations whose size (64 KiB granules) equals a
live VMA memory object's, under "device-local memory (driver copy?)" and "host-visible memory not
mapped" (the 315 private 16-64 MiB reservations of the first run, 8.4 GB committed, looked like the
driver's own copies of device-local memory: VMA's 32 MiB blocks and dedicated images). New tags:
"guest thread stacks" (the guest threads' fiber stacks, 2 MiB each), "GPU page entries" and "GPU big
page entries" (Tegra::MemoryManager's tables per GPU address space). 4 = as 1, and once, at the
first census, a walk of every block of the CRT heap ("CRT heap walk": busy blocks by power-of-two
size, and the 16 exact sizes holding the most bytes, so owners can be told apart by block size).
The walk holds the CRT heap's lock: every thread that allocates waits for it (~0.3-1 s); one
hitch, early in the run.

Added 2026-11-16 (helper CI, after the CENSUS=4 run b5-census4-log): the two ~1 GiB rw regions are
not CRT heap segments (the heap segment check found 64 segments, 833 MiB; the heap walk at 2 s saw
638 MiB busy). They are page tables: one existed at 2 s (27 MiB, 539 at 7.3 s, 1022 at 12.6 s, before
the GPU address space was made at 8.1 s), the other appeared after the GPU address space ("small
pages up to 0xc000000000") and grew with play (319 -> 986). New tags: "guest page table" (each
process's, 8 bytes per 4 KiB page, 1 GiB for 39 bits), "GPU page table" (the small-page table, 4
bytes per page, 1 GiB for 40 bits), "GPU big page table", "SMMU address map", "SMMU page entries".
The "more" line adds a row per table: reserved, committed, resident, and how much of the resident
part is all zero (guest) or has no mapped page (GPU): what FRITCHII_RAM_PT_TRIM or _LAZY can give
back. "Page table commit (n): <table> by <caller> committed K KiB of entries for a..b" lines log
every call that commits 1 MiB or more of a table at once (the first 64, then every 64th): they name
the caller that filled it (protect, walk, mark debug, mark cached, map; the GPU table's maps).

### FRITCHII_RAM_GPU_ENTRIES

TEST, off by default (helper BE, 2026-11-16). Boot. Each GPU address space (Tegra::MemoryManager)
keeps its small-page entry words, 2 bits per 4 KiB page: 64 MiB for a 40-bit space, made as a
zero-filled std::vector, so all of it was resident (the first census run: two 64 MiB allocations,
64/64 MiB). With 1 the words come from zero pages the host fills in only where they are touched
(VirtualAlloc, anonymous mmap; the vector if that fails). Same contents, same accesses. Expected:
~60 MiB less resident per address space (~120 MiB at the market, two spaces), minus the few MiB
of pages the game's GPU mappings touch. Judge: the census tag "GPU page entries" (resident) and
the private working set; nothing else changes.

### FRITCHII_RAM_PT_LAZY

TEST, off by default (helper CI, 2026-11-16). Boot. The guest page table is a 1 GiB reservation
(8 bytes per 4 KiB page of a 39-bit address space), committed a page at a time. Walks that only
read it (page protection, block reads and writes, cache operations, the debug and cached marks)
committed every table page of their whole range first, so a walk over unmapped address space left
zero pages in RAM. The census run at the market (b5) showed a 1 GiB private region 1022 MiB
committed / 871 resident by 12.6 s, while the guest had mapped only ~10 GB (~20 MiB of entries);
it was already 539 MiB at 7.3 s with 96 MiB of guest views. With 1 these walks read through the
committed bits instead (an uncommitted page is unmapped: same result) and commit nothing. Maps
still commit what they map. Expected: up to ~850 MiB less resident (and committed) if the walks are
what committed it; the census's "Page table commit" lines say which caller did ("protect", "walk",
"mark debug", "mark cached", "map").

### FRITCHII_RAM_PT_TRIM

TEST, off by default (helper CI, 2026-11-16). Boot. Unmaps give back the RAM of page table pages
they leave without a mapped entry: the guest page table's 4 KiB pages that hold only zeros after
the unmap, and the GPU small-page table's pages (1,024 entries, 4 MiB of GPU address space) with no
mapped page left. MEM_RESET + VirtualUnlock (madvise DONTNEED on Linux): still committed, no page
file write, a zero page reads zero again, and the GPU entries are read only for mapped pages. Done
under the lock every map of that table takes (the kernel page table lock, page_table_lock), so no
map lands between the check and the discard. The GPU small-page table is a 1 GiB reservation (4
bytes per 4 KiB page of 40 bits; Switch 2 asks for small pages up to 768 GiB) committed 64 KiB per
64 MiB of address space a map reaches and never decommitted; b5 had it at 986 committed / 859
resident. Expected: what the census reports as "all zero" (guest) and "without a mapped page"
(GPU), if the tables filled by maps that were unmapped since. Nothing to judge but the census rows
and that nothing breaks (a wrong discard would show as garbage GPU reads or guest crashes at once).

Added 2026-11-16 (helper CM, b12 Castle Town runs): with the page tables tagged (guest 22 MiB, GPU
12), the 1 GiB rw regions are still there, so they are not page tables. Region A exists at 2 s (28
MiB, before the Vulkan device), fills to 1021 by 12.5 s during the boot pipeline builds; B appears
by 12.5 s and grows to ~900 while the background prebuild runs (done at ~17.8 s), then barely moves;
a third appears at the area load (~45 s). They fill one after another like an allocator's arenas.
Nothing in our code reserves 1 GiB, and they carry no heap segment header. New census line "owner
hints" (any mode but 3): for the 4 biggest unnamed allocations (32 MiB or more resident) and for
the 16-64 MiB reservations together, up to 512 resident pages are read and counted: non-zero words
per page, words pointing into the allocation itself (allocator free lists), and words pointing into
each loaded image (vtables and callbacks: the driver's objects point into nvoglv64.dll or
nvgpucomp64.dll, ours into the exe), plus the first six words of the allocation. Read-only, a few
ms per census.

### FRITCHII_RAM_VK_HOST_ALLOC

TEST, off by default (helper CM, 2026-11-16). Boot. The Vulkan instance and device are created
(and destroyed) with allocation callbacks, so the driver's host allocations come from a heap of
our own (HeapCreate, low-fragmentation) instead of the driver's own reservations. Their children
(pipelines, descriptor pools, command pools...) are still created with null callbacks, which
drivers serve from the parent's allocator; nothing else changes, the game sees the same. Every
block is counted. Why: the census's two to three 1 GiB arenas (~1.75 GB resident at Castle Town)
fill during pipeline builds, like a driver allocator that keeps what it once used. If they are
the driver's, they move into our heap ("other heaps" in census line 1), the census names them,
and what the driver frees goes back to the heap (and from there to Windows) instead of staying
resident. Expected: between 0 (the driver ignores the callbacks for children, or uses all it
holds) and several hundred MiB; FRITCHII_RAM_PREBUILT_KEEP + EXP51=2 freed only ~170 MiB for
2,300 destroyed pipelines, which fits a driver arena that keeps freed memory. Census "more" line
(FRITCHII_MEM_CENSUS on): "Vulkan host allocations (FRITCHII_RAM_VK_HOST_ALLOC=1, ...): L MiB
live in N blocks (peak P), by scope command / object / cache / device / instance; the driver's own
(notified) M MiB; F failed, X foreign frees". Object scope near 0 = the driver does not route
pipelines through the device's callbacks (then the switch only names device memory). F and X
must be 0. Risk: a driver bug with callbacks (rare; Mesa and NVIDIA both support them); a small
cost per driver allocation (one heap call, as the driver's own). Logged once when on.

### FRITCHII_JIT_WARM_CORES

TEST, off by default (helper BE, 2026-11-16). Boot, a core mask (hex with 0x, e.g. 0x3 = cores 0
and 1); 0 or unset = every core, as before. Each guest core has its own recompiler with its own
code cache and block tables. EDEN_NX2_JIT_WARM copies every block one core compiles to every other
core, and FRITCHII_JIT_BLOCK_LIST=1 hands every core the whole list (OoT: 765,194 blocks, "warm-up
handed out 765194/765194/765194/765194"), so each core holds the code of every block any core ever
ran. Cores outside the mask take no part in either: they compile only what they run. Expected
saving: the census's exec private (code) and part of the CRT heap (block tables) for each core left
out; measure the per-core exec rows first (FRITCHII_MEM_CENSUS=1): if each core holds ~400 MiB of
code + tables, leaving two cores out saves ~800 MiB. Risk: a core left out compiles on demand the
first time it runs a block (the hitch the warm-up removed). Judge by the census's "JIT blocks
compiled per core, warm/on demand" (the on-demand count of the cores left out after the load),
"Recompile" in the Hitch lines, and the hitch score; pick the mask from the census (the cores with
few on-demand compiles in a normal run are the ones to leave out).

### FRITCHII_RAM_PREBUILT_KEEP

TEST, off by default (helper BE, 2026-11-16). Live: N > 0 = graphics pipelines built at boot from
the disk cache that no draw has used, unused for 9,000 frames, are destroyed past N kept (at most
2,000, the old budget), also while the cache is under the eviction threshold (16 GB PCs ~2,280:
the market's ~1,800 built never reached it, so nothing was ever evicted there). With the switch on
they also go first when the cache is over the threshold. Why: each holds an estimated 0.3-0.45 MB
of driver memory (NVIDIA). The "Memory owners" line now says how many built pipelines no draw used
("N never used") and how many compute pipelines no dispatch used (EXP51=2 already evicts those).
Expected saving: (never used - N) x ~0.3-0.45 MB, e.g. 1,000 unused at N = 200: ~250-350 MiB.
Cost: one destroyed and needed later (the player reaches that content) is translated and built
again on the GPU thread (~10 ms, or skipped draws with async shaders). Log: the eviction line adds
"N of them boot builds no draw used (FRITCHII_RAM_PREBUILT_KEEP=K: T this session, A needed
again)"; A must stay near 0 in a normal route, and the hitch score must not move.

### FRITCHII_ZERO_TRIM_BATCH

TEST, off by default (helper BE, 2026-11-16). Boot, Windows. The zero-page pass reads, through the
backing, every resident guest view page whose backing page is not in the working set, which puts
that backing page in the working set until the pass ends: the same RAM page counted twice. The
market's working set peaks (12.2, 14.1 and 14.6 GB at 28, 40 and ~64 s, prof-market-log) each fall
inside a pass, ~2 GB over the steady 12.4 GB. With 1 those pages leave the working set every 4,096
pages instead of at the end. What is trimmed stays the same; the unlock calls are the same count
in more batches. Expected: the peak working set ~2 GB lower (no RAM is freed: the pages were RAM
already; but Windows and the owner's tools stop seeing a 14.6 GB process). Judge: "Memory:" peak and
"Zero-page pass" lines (same MiB trimmed, similar ms), hitch score unchanged.

### FRITCHII_ZERO_TRIM_CHUNK

TEST, off by default (helper CR, 2026-11-16). Live (debug server `switch`), Windows; read once at
the start of each zero-page pass. The first big pass after the first area load (~530 MiB trimmed,
~88 s after boot, ~48 s into play) takes each run of zero pages out of the working set with one
VirtualUnlock call, and its longest runs take 30-46 ms. A guest VirtualProtect (the GPU cache's
write traps) that lands in that call waits for it: host protect longest 21.6, 19.1 and 45.7 ms in
the market, Castle Town and night market hitches of 62, 53 and 71 ms (b18-reg logs, the only long
frames of steady play there, and the frame behind their 0.1% low). 0 = as before. 1 = a run goes
in calls of at most 256 pages (1 MiB); N >= 2 = N pages a call. The same pages leave the working
set and their contents are untouched (no verify mode: nothing differs). The pass lines ("Trimmed
... MiB of all-zero guest pages" / "Zero-page pass") end with the mode, the unlock calls and the
longest one, and the guest map/protect calls made while the pass unlocked and the longest one (the
number to judge, with either value). Expected with 1: longest call ~1 ms, longest guest call a few
ms at most, no Hitch: line at the pass. Test: fresh launches (the big pass comes once), market pin,
with the flash watcher on (where it shows) and --noflash.

### FRITCHII_FORMATLESS_13

TEST, off by default (helper BH, 2026-11-16, port of zortos293/Lichii #1874 by KevinLny, open
there). Boot. 0 = as before. 1 = typeless storage image reads count as supported on a Vulkan 1.3
instance even when the device's shaderStorageImageReadWithoutFormat feature is false. Only devices
without that feature change (Intel on Windows; NVIDIA and AMD already have it, so nothing changes
there). Why: on Intel Iris Xe (Windows driver 101.7092) every typeless image read became a constant
0 and the scene rendered black (Pokopia; log line "Typeless image read not supported by host").
The driver reports formatless reads per format through format features 2. Check: an Intel Windows
tester with the switch at 1, the warning gone and the scene drawn.

### FRITCHII_AMD_NOPERSPECTIVE

On by default on AMD drivers only (RADV, AMD proprietary, AMDVLK); other vendors never change,
whatever the value (helper EA, 2026-11-18; port of zortos293/Lichii #820/#722, first added off by
default in 4cdd0231c3). Boot. Red flash fix over wet ground and water: OoT's water volumes write a
screen-space coordinate as a noperspective varying, AMD's driver interpolates it wrongly when a
primitive is clipped close to the camera, the half-res water target gets bad "no water" texels next
to water ones, and the puddle shader's exp2 overflows to the R11G11B10 maximum (pure red).
- 1 (the AMD default): generic outputs the pixel program reads noperspective are written times
  Position.w by the last pre-rasterization stage and read perspective-correct times FragCoord.w.
  The scaling stage is the vertex shader, the tessellation evaluation shader, a geometry shader (a
  lowered passthrough one too: it scales at each EmitVertex), or the mesh program: native mesh
  shaders in their epilogue (OoT's path; the first version missed it, so =1 changed nothing for
  OoT's mesh draws) and the mesh emulation's vertex shader. Per-primitive mesh outputs stay flat.
  Left native: transform feedback, NVIDIA-native passthrough geometry shaders, pixel programs with
  indexed attribute reads, and tessellation pipelines (an AMD tessellation crash in shader
  preparation was blamed on #820 elsewhere); the log says once when a tessellation pipeline kept it.
- 2: as 1, tessellation pipelines too.
- 0: off, noperspective varyings stay native.

Part of the SPIR-V settings stamp (bit 20 on, bit 21 for 2), so kept and warmed SPIR-V of the
other form is not reused: AMD players prepare shaders once more after updating or toggling it.
Other vendors keep the stamp they had. Log on AMD: "FRITCHII_AMD_NOPERSPECTIVE=N: noperspective
varyings ...". Why not every vendor: white fog at the Hyrule Field drawbridge on an RTX 4080.
Check: an AMD tester at a spot with the red line on the wet floor, the red gone.

### FRITCHII_TEST_MARK

Boot. Only for stamped test copies (video_core/test_watermark.cpp); plain builds have no mark,
create nothing for it and never log it. How the on-screen test mark is drawn:
- 1 (default; any value other than 0 and 3): Vulkan draws the mark as one textured quad over its
  bounding box. Its pixels (shadow, text, transparent elsewhere) go into a small float texture,
  uploaded only when they change (a new layout or window size; moving to the next spot every
  minute only moves the quad), sampled 1:1 with nearest filtering and blended "over", so colours
  and position are the same as with 3.
- 3: as before, two vkCmdClearAttachments calls inside the present render pass with one rect per
  block (~370 rects). For an A/B: on an AMD RX 9070 XT at 4K this cost ~10 ms (up to ~17 ms) of
  GPU time in the present submission every frame; NVIDIA barely noticed.
- 0: no on-screen mark. Only the drawing is skipped: the stamp stays in the exe and the log's
  "Build hash:" line is unchanged.

OpenGL draws the mark with scissored clears for 1 and 3 and skips it for 0. The log says once,
on the first frame with a mark, which way is used: "Test mark: one textured draw
(FRITCHII_TEST_MARK=1)", "Test mark: two clears (FRITCHII_TEST_MARK=3)", "Test mark: off
(FRITCHII_TEST_MARK=0)" or "Test mark: clears (OpenGL) (...)".

### FRITCHII_DEVICE_FAULT

On by default (helper BH, 2026-11-16, port of the device fault part of zortos293/Lichii #512).
Boot. When the driver offers VK_EXT_device_fault, Fritchii enables it (vendor binary dumps off)
and a device loss logs what the driver knows: "Device fault: <description> (N addresses, M vendor
records)", each fault address with its kind (invalid read/write/execute, instruction pointer) and
range, and the vendor fault codes. A driver without it logs "Device fault info: not supported by
the driver". Only the loss report reads it; nothing is drawn differently. FRITCHII_DEVICE_FAULT=0
leaves the extension off, as before. Why: two AMD testers hit device losses with no clue in the log.

### FRITCHII_PIPELINE_BINARY, FRITCHII_PIPELINE_BINARY_MAX_MB, FRITCHII_PIPELINE_BINARY_HARVEST, FRITCHII_PIPELINE_BINARY_SHARED_MB

Off by default, except 1 on AMD's Windows driver (helper CE, 2026-11-16; AMD default helper CW).
Boot. Fritchii keeps the compiled pipelines itself through
VK_KHR_pipeline_binary instead of trusting the driver's cache: 1 load and save, 2 load only, 3 load,
save and verify (every 16th pipeline built from the store is also compiled normally and the driver's
binary keys compared). The store is <shader dir>/<title id>/pipeline_binaries.fritchii.bin (+ .idx),
one per GPU and driver (vendor, device, driver id, driver version, pipeline cache UUID); another GPU
or driver starts it over. Entries are found by the driver's key of each create info
(vkGetPipelineKeyKHR), checked by hash when read, zstd-compressed. A saving store adds its id to the
Prepare Shaders stamp, so the first boot with it (or after it starts over) prepares once more and
fills it. FRITCHII_PIPELINE_BINARY_MAX_MB (6144) caps the file; FRITCHII_PIPELINE_BINARY_HARVEST=0
stops taking pipelines the driver's own cache still holds (otherwise taken from there, no compile).
A device loss in a session that used the store starts it over at the next boot. Graphics pipeline
libraries (EDEN_NX2_GPL) are left to the driver: only whole pipelines go through the store. Log:
"Pipeline binaries (...)" at boot, "Pipeline binaries so far/after the preparation/at exit: N hits
(ms each), M misses (ms each, K taken from the driver's cache), ~S s of compiles saved ...".
RAM cost (batch 14, 2026-11-16, RTX 4080, castletown 90 s): working set 10.7 -> 16.0 GB with =1 (620-931 pipelines stored, ~1.4 MiB each, 880-1293 MiB file); the driver's three 1 GiB arenas did not shrink.
What held it (helper CW): NVIDIA gives every pipeline one 2048 KiB binary with the same key, besides
its own 4-110 KiB ones (97% of every entry). Each pipeline built from binaries got its own copy of
that blob in the driver's arenas (two extra 2 GiB and one extra 1 GiB arena in the census, 2,369
pipelines x ~2.2 MiB). FRITCHII_PIPELINE_BINARY_SHARED_MB (64; 0 = as before) makes a binary that a
second pipeline names once and keeps it for the session, so every later pipeline names the same
handle (from the store, from the driver's cache or from a capture). Log every 10 s and at exit:
"Pipeline binaries RAM: store index X MiB, blobs held Y MiB (peak P), queued for the file Q MiB,
binaries alive N (K kept shared, S MiB, R reuses), captured data unreleased M". AMD's binaries are
~35 KB per pipeline (no shared blob seen), so its cost is the index (~7 MiB for 120k) plus the
writer queue (256 MiB cap, only while saving). The file still holds the blob in every entry (same
format): ~4,400 NVIDIA pipelines fit the 6 GiB cap.
Why: AMD's Windows driver keeps ~30% of the 120k prepared pipelines and compiles the rest again at first use
(0.2-1.1 s each, the 1 s stalls); NVIDIA on Linux prunes its cache.

### FRITCHII_SCALED_EXTENT_CAP

On by default since 2026-11-16 (helper BR; helper BH ported it off, port of zortos293/Lichii
#1890 by zortos293, still open there). Live, for images made after the change. 0 = as before
(the A/B and kill switch). 1 (default) = the texture cache leaves
an image unscaled when its scaled width or height would pass the device's 2D image or framebuffer
limit (the smallest of the three; 16384 on AMD and RADV, 32768 on NVIDIA). Rescaling is decided
per render target set, so that whole pass stays native while the rest of the frame scales. Why:
OoT's sun shadow map is a 12288x12288 D16 depth target; at 1.5x it is 18432 wide and at 2x
24576, past AMD's 16384, and drawing into it hangs the GPU: device loss entering Castle Town at
1.5x/2x on AMD (zortos #1885: semaphore waits for 2 s, then "Device loss occurred" with no fault
address). NVIDIA keeps scaling that map at 2x, so on NVIDIA the switch changes nothing at those
scales. Check: an AMD tester at 1.5x enters Castle Town with the switch at 1.
- Why on by default: it only changes an image whose scaled size is past the device's own
  limits, which is invalid Vulkan on every driver (no driver has to accept it). At 1x, 1.25x on
  every card and up to 2x on NVIDIA nothing changes. A scale that does not enlarge images (1x and
  the downscales) is never refused (idea from bakemono's handoff: without that guard a 0.75x
  downscale on an 8192-limit device would leave the image native, bigger than scaled).
- Checked before the cached IsRescalable answer, so changing the scale in game is covered.
- Log: "Image WxH (format F, N samples) stays at native size: scaled it would be ..., past the
  device's limit of L (FRITCHII_SCALED_EXTENT_CAP)" (HW_GPU, Info), once per image. At 1.5x/2x on
  AMD, expect it once for the 12288x12288 D16 sun shadow map.

### FRITCHII_SCALE_DEPTH_MAX

On by default (helper DA, 2026-11-17), value 8192; 0 = as before (the A/B and kill switch). Live,
read whenever a render target set is checked for scaling. A depth/stencil image (any format that
is not a colour format) whose scaled width or height would pass N stays at native size, on the
same path as FRITCHII_SCALED_EXTENT_CAP (that pass's render target set stays native, the rest of
the frame scales). Why: OoT's 12288x12288 D16 sun shadow map (288 MiB, made on every Castle Town
load) costs 648 MiB at 1.5x and 1.15 GiB at 2x on NVIDIA, where the extent cap (32768) lets it
scale, and 450 MiB at 1.25x on AMD; a shadow map that size gains nothing visible from scaling.
Only scales that enlarge images are affected: at 1x and the downscales nothing changes. Main
scene depth (1920x1080 or 2560x1440 native) stays far under 8192 up to 3x.
- Log: "Depth image WxH (format F, N samples) stays at native size: scaled it would be ..., past
  N (FRITCHII_SCALE_DEPTH_MAX, k so far)" (HW_GPU, Info), first 16 images; then "Scale depth max
  (FRITCHII_SCALE_DEPTH_MAX=N): x depth images kept at native size in the last 10 s, y this
  session" at most every 10 s while new ones appear.

### FRITCHII_OOT_BYPASS_SAMPLED

TEST, off by default (helper BH, 2026-11-16, port of zortos293/Lichii #1875 by zortos293). Live;
only matters with Ocarina of Time anti-aliasing at No sharpening (or the forced bypass on GPUs
that can't build the network). 0 = as before: the copy that replaces the network's output pass
takes render target 0 of the latest draw with one of three known tonemap shaders. 1 = it copies
the image the output pass itself samples (cb0 + 0xc0, B10G11R11 or its E5B9G9R9 view), and a
skipped prepass that no replacement followed runs the network again. Why: the game has a fourth
tonemap shader (96d6efdbfe52605c, seen in adult Zora's Domain, Lon Lon Ranch and Kakariko houses),
and there the copy kept an old image: the 3D view froze while the HUD and audio went on, until the
camera turned (zortos #1688, RADV and NVIDIA). Their tester confirmed the fix. Log: "anti-aliasing
network bypassed from the sampled image" once, and "the network's output pass was not replaced;
running the network" once if a frame ended with another output program. Check: No sharpening in
Zora's Domain facing King Zora, the view must follow the camera.

### FRITCHII_PROTECT_UNLOCKED

On by default on every driver since 2026-11-18 (owner: a briefly late object beats a freeze; RTX 4080 market toggle 70.1 / 71.3 fps, no cost); first AMD/Intel only. (Helper AK, 2026-11-15, port of zortos293/Lichii #1780 by Reiixan); live,
FRITCHII_PROTECT_UNLOCKED=1 turns it on (Windows only). HostMemory's Protect (the write traps on
GPU-cached guest memory, 12,000-28,000 calls a second in OoT) took the placeholder lock before every
VirtualProtect, and Map/Unmap hold that lock across their kernel calls, which slow from ~10 us to
~250 us while the game maps memory in bulk (zone changes): protections queued behind them and the
GPU thread stalled (their test: frames over 100 ms 37 -> 10 on one route). With the switch on,
Protect first calls VirtualProtect on the whole range without the lock. It refuses a range that
reaches a placeholder or a second view (a Map/Unmap under way, or a range across views); those take
the lock and are protected view by view as before. Any interleaving ends as a Protect done wholly
before or after the Map/Unmap. Log: "Host protect in the last 10 s (FRITCHII_PROTECT_UNLOCKED): N
without the lock, M took the lock". Measure with the "host protect" time and longest in the Hitch:
lines around area loads.

### FRITCHII_ASYNC_REDRAWN

TEST, off by default (helper AK, 2026-11-15, port of TheuZCoder's Fritchii fork 2e0cf33368; there
it is EDEN_NX2_ASYNC_REDRAWN, on unless =0); live, FRITCHII_ASYNC_REDRAWN=1 turns it on. With
EDEN_NX2_ASYNC_DISCARD=2 a native draw whose pipeline is still building is skipped only when every
target was fully cleared in this render frame and a recent earlier one. OoT's scene colour is
covered by the sky or terrain instead of a clear, so it counts as persistent and every new object or
effect drawn into it waited for its pipeline (Hyrule Field, the first hit on a new enemy). Images now
count the consecutive render frames they were written in (ImageBase::write_streak); a target not
cleared but already written in this frame and the 7 before counts as recurring for depth-tested
colour draws, which then skip while the pipeline builds (the object is missing for a frame or so).
Layers drawn once or now and then, depth-only passes and draws without a depth test still wait. The
10 s "Async discard" line counts these skips ("N of them into redrawn targets"); they stay in the
persistent skipped column too. Only matters with unbuilt pipelines: a warm shader cache sees few.

On by default on AMD's Windows driver (helper BM, 2026-11-16): there a pipeline missing from the
driver's cache takes 0.2-1.1 s to compile, and an RX 7900 XT session held 76 s of draws for unbuilt
pipelines, a ~1 s freeze whenever something new showed up (tester log t1858). The boot log says so
once; FRITCHII_ASYNC_REDRAWN=0 waits as before. Other GPUs keep 0.

Also turns on by itself, on any driver, once the session shows the driver compiling prepared
pipelines again (helper BV, 2026-11-16): a Linux NVIDIA tester (RTX 4070 SUPER, 615.71) saw ~650 of
1,128 prepared builds take over 20 ms, up to 2.5 s, 18 s of draws held in one boot (log t0419). The
rule, checked every 10 s with the "Driver compiles of prepared pipelines" line: 16 or more prepared
pipelines over 20 ms that are a quarter or more of the prepared builds, or 3 or more over 250 ms.
Where the driver cache holds the preparation (NVIDIA on Windows) prepared builds take 2-10 ms and it
never triggers. Decided once per boot, no time limit. Log: "Prepared pipelines are compiled again by
this driver (n so far, longest t ms): new objects pop in instead of waiting (FRITCHII_ASYNC_REDRAWN
auto)". A value in the environment (nx2_switches.txt) or set live wins; storing -1 live gives it back
to the default and the auto rule. Only prepared (kept SPIR-V) builds count now; runtime pipelines a
worker emits are new to every driver and no longer mix into the count. Compute dispatches still wait
for their pipeline (skipping one would lose its results), unless FRITCHII_ASYNC_COMPUTE_SKIP below
is on; FRITCHII_COMPUTE_FAST_FIRST below makes that wait shorter.

### FRITCHII_ASYNC_COMPUTE_SKIP, FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS

TEST, off by default (helper DU, 2026-11-17; idea and rules from zortos293/Lichii #2104, 75fb0b527e,
ffd0ebe246 and ce937db2d0, by zortos293). Live. A native dispatch whose compute pipeline is not built
yet records a wait on the Vulkan worker, and every GPU channel waits behind it: 0.6-5.5 s freezes on
cold shader caches, 21-55 s per session in tester logs t0357, t0419 and t1858 (grep "Compute dispatch
waited N ms for pipeline X"; the "Worst frame" line does not see them). Runtime-translated compute
programs (the sword-swing foliage culling variant 59c1699f529e7c08 and friends) are not covered by
Prepare Shaders.

- 0: waits as before.
- 1: such a dispatch is skipped while its pipeline builds, as if the guest had not run it this frame,
  when all four guards hold; otherwise it waits as before. On the first skip the build is handed to
  a spare thread ("VkComputeKick"), since the pipeline workers are first in, first out. Within
  FRITCHII_PIPELINE_WAIT_BUDGET (shared with the draws; 0 except on Intel) it waits for the build
  first.
- 2: dry. Every dispatch waits; the line counts what 1 would skip.

The guards, all on what the dispatch would mark written (its native storage write marks and the
guest ranges of its written storage images):
- G1, GPU data: every byte holds GPU writes (gpu_modified_ranges, full coverage) and no CPU write
  waits for upload; images: every image there GpuModified, not CpuModified, one covering the range.
- G2, every frame: each buffer range (exact address and size) was written by a dispatch in each of
  the last 8 frames; images by ImageBase::write_streak (as FRITCHII_ASYNC_REDRAWN). A pass the game
  runs once (a LUT at a scene change, a pool init) never qualifies. A range the same pipeline
  skipped in the frame before keeps qualifying (a skip writes nothing, so the streak it passed is
  kept, never extended).
- G3, no read-back: no page Preflushable, in cpu_read_ranges or a read download, and no guest read
  ever seen on its pages (GetFlushArea, sticky for the session). Feeders whose results the game
  logic reads (collision, the c0[0x150] binding writers) always wait.
- G4, shape: native, direct (no indirect grid), no texel buffers, no global stores, no packed R16
  image, not an OoT upscaler or effects pass, no frame dump, and known write extents: a 16 MiB
  (unproven) or capped mark, several device ranges or an image without one device range waits.

The decision comes before the dispatch's fill (MemStage::FillDispatch) and, with the one follower,
before E posts it (an unbuilt pipeline's dispatch is lent: "lent: unbuilt pipeline"), so a skipped
dispatch leaves no marks, uploads, image changes or EXP10 records (it confirms an empty record). Its
QMD releases and semaphores are the engine's (KeplerCompute, after DispatchCompute returns) and still
happen. A dispatch that runs takes Configure's own path instead of the seam's fill.

What the player sees for the length of a compile (not one frame): per-frame compute outputs stay as
last written (foliage culled for an older view, particles or poses frozen), or what a skipped append
pass draws is missing; the frame after the build ends is right again.

Log, every 10 s while anything happened: "Async compute skip (FRITCHII_ASYNC_COMPUTE_SKIP=n) in the
last 10 s: x dispatches/s skipped, y/s would skip (dry), p new programs, t ms of waits avoided;
unbuilt dispatches that waited: a shape, b unknown extent, c read back (c2 of them only by a guest
read seen here), d no GPU data, e not every frame; must be 0: s programs still skipping 10 s after
their first skip (since boot), guest reads of skipped output r in these 10 s (r2 since boot; h
ranges held now)". Then one line per list with the top 8 programs by dispatches: "Async compute skip
programs, waited, read back (n programs): hash xN, ..." for each refusal, for skipped, would skip
(dry) and guest reads of skipped output. Once per program (first 64): "pipeline X is still
building, its dispatches are skipped" and "pipeline X skipped N dispatches over T ms (frames f-g),
ran at frame h; W ms of wait avoided" (the wait avoided runs from the first skip to the build's end).

The skipped-output read count (round 2, helper DU) counts only reads of bytes whose last writer was
a skipped dispatch: each skip holds its written ranges, and any later write over them ends that (a
buffer write mark of any dispatch, draw or copy, an image modification, which counts for the whole
image, or a guest CPU write: VideoCommon::ComputeSkipHook). Ranges held 60 s without a write are
dropped. Before, it counted every read of a range a skipped pipeline had written until that pipeline
ran again, also when other passes had rewritten the range since. Dry runs hold nothing.

The OoT upscaler passes (the game's 1440p, scaled 1440p and 3/4 passes, the full-size pass and the
depth reader) never skip: skipping them keeps the whole picture still.

FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS (boot, ms, default 0; test only): every compute program counts as
unbuilt for that long after its first dispatch, to test the skip on a PC with warm caches (2000 =
a cold compile for each). A dispatch that runs meanwhile (mode 0, a refusal, mode 2) waits as a
building pipeline's does, since round 2: Configure records the build wait on the scheduler worker,
held until the delay ends, and logs "Compute dispatch waited N ms for pipeline X
(FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS)". With the one follower such dispatches are lent ("lent:
unbuilt pipeline") in every mode while the delay is set. The pipelines are built, so there is no
budget wait. Test: one session at a pinned save, mode 2 first (no visual change, read the refusal
split), then mode 1 with the delay; screenshot at 1 s and 10 s, compare the 10 s shot with mode 0;
both must-be-0 counts 0.

### FRITCHII_COMPUTE_FAST_FIRST

Live, default 0. A dispatch whose compute pipeline is still building waits for it on the Vulkan
worker, and every channel waits behind it: 0.6-5.5 s freezes on cold caches, many of them by
programs whose output the guest reads back, so FRITCHII_ASYNC_COMPUTE_SKIP refuses them. With 1,
that dispatch gets an unoptimized build of the same pipeline at once
(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT; same SPIR-V, layout and descriptors, so the same
results, only more GPU time), and dispatches use it while the optimized build runs on its pipeline
worker. Before its next bind after that build ends, the scheduler worker swaps the optimized
pipeline in; the unoptimized one is freed once the GPU passed the tick it was swapped out at. The
unoptimized build is never stored (no binary store, no pipeline cache), so the next session still
gets the optimized one from the caches.

- Only pipelines built at run time on a worker. Boot, warm-up and preparation builds, and the
  programs always built unoptimized (BuildsUnoptimized), keep their path. Prepared (precompiled)
  pipelines are included: their module is made by whichever build needs it first.
- If the optimized build ends first, the unoptimized one is thrown away unused. If the unoptimized
  build fails, the dispatch takes the optimized build as without the switch.
- The one follower's posted dispatches, lent ones and FRITCHII_ASYNC_COMPUTE_SKIP all see the
  unoptimized pipeline as built (IsBuilt, BuiltAt). The skip's kick makes the unoptimized build, so
  a skip lasts only that long.
- 2 (test): every program built at run time takes the path, waited for or not (its worker builds the
  unoptimized pipeline first, then the optimized one, then the swap). Exercises the whole path on a
  PC with warm caches; the compile times are only real on a cold driver cache.
- Log: "Compute dispatch waited N ms for pipeline X (an unoptimized build, FRITCHII_COMPUTE_FAST_FIRST)";
  per program (first 64 lines) "built unoptimized in A ms" and "optimized in B ms (unoptimized: A
  ms)"; every 10 s while anything happened "Compute fast-first (FRITCHII_COMPUTE_FAST_FIRST=n) in the
  last 10 s: f unoptimized builds served dispatches (x ms, y ms each), b beaten by the optimized
  build, e failed; o optimized builds followed (g failed), those programs p ms unoptimized vs q ms
  optimized; s swapped in; now w waiting for their optimized build, d built and not dispatched since;
  must be 0: u programs unoptimized over 30 s (t since boot)". A must-be-0 hit also logs a warning
  with the program. An optimized build that fails is logged and the program keeps the unoptimized
  pipeline.
- The optimized build waits its turn in the pipeline workers' queue (first in, first out). A busy
  queue shows as w, and over 30 s as the must-be-0 count.

### FRITCHII_COMPUTE_PREBUILD_FIRST

Boot, default 1 (0 = as before). The boot build of the disk cache queues its compute records before
its graphics records. The same builds on the same workers, only in another order. The boot waits for
all of them before the game starts either way, so a dispatch never waits for a program the disk cache
recorded in an earlier session (runtime-translated compute programs are recorded, as are prepared
ones the game used); this only makes the compute records end earlier within the boot.

### FRITCHII_PREP_COMPUTE_FIRST, FRITCHII_PREP_COMPUTE_CHECK

Why: a compute dispatch can't be skipped while its pipeline compiles, so a compute pipeline the
driver has to compile in play is a freeze (0.5 to 5 s for OoT's big ones). Tester logs (11-16 to
11-18): 105 dispatch waits (115 s) on prepared compute pipelines the driver had not kept, and 28
(50 s) on compute states the preparation lacks. The precompiled file holds its 4,088 compute records
at its very end, so a driver cache at its size cap (NVIDIA's default ~1 GiB) drops them, and a player
who presses Play now gets them last.

- `FRITCHII_PREP_COMPUTE_FIRST` (boot, default 1; 0 = as before): the preparation takes every compute
  record first, in any FRITCHII_PREP_ORDER mode (the used list's ages then rank after compute). Same
  records and builds, only the order changes, so the preparation takes no longer. A checkpoint of an
  older build resumes: positions before it as they were, the rest compute first. Logs "Precompiled
  pipelines in order (...): N compute first, ..." and, once the last compute record ends,
  "Precompiled compute: all N compute records done T s into the preparation (...)".
- `FRITCHII_PREP_COMPUTE_CHECK` (boot, default 1; 0 = off; 2 = test). When the stamp says "already
  prepared", the 6 biggest of 48 sampled kept compute pipelines are built the way a dispatch builds
  them (kept info and SPIR-V, same layout, flags and program hash) and timed. A driver cache hit takes
  2-10 ms; half or more over 40 ms means the driver's cache lost the prepared compute pipelines (new
  exe name, driver update, cleared or full cache). Then the compute records alone are prepared again
  before play (the Preparing shaders screen shows "n / 4,088"; Play now works as usual). No stamp,
  checkpoint or kept file is written by that pass. `precompiled_compute_pass.fork-v54.txt` counts the
  passes per stamp; after 2 the check only warns (a full cache drops them again). 2 = the compute pass
  runs at every boot (to time it, or to fill a cold driver cache on purpose).
- Logs: "Prepared compute check: the 6 biggest of 48 sampled kept compute pipelines built in a, b, ...
  ms (k over 40 ms, T ms in all): the driver's cache holds/lost ...", then "Preparing 4088
  precompiled pipelines from ... (its compute records only)" and "Precompiled compute pass finished".
- Run time, every 10 s while any compute pipeline is built in play: "Compute pipelines built at run
  time in the last 10 s: P from prepared SPIR-V (c compiled by the driver, x ms), T translated now (c
  compiled, x ms), longest compile L ms; this session N prepared ones compiled again (must be 0 after a
  finished preparation) for S s, M translated ones compiled for S s". The first 64 driver compiles get
  a line each: "Compute pipeline H compiled by the driver at run time in N ms (prepared, not in the
  driver's cache | translated now: a state the preparation lacks)".

### FRITCHII_OCCLUSION_VISIBLE

TEST, off by default (helper AN, 2026-11-15, idea from DahBard's Blitchii LICHII_FAKE_OCCLUSION);
live, not exact. OoT tests objects with two 64-bit sample count reports (type 21, 16 bytes each,
16 bytes apart: before and after the object's test geometry), and its 0xe2c macro reads the six
words and writes render_enable.mode = p0, or p0 ^ 1 when the counts differ (p0 = 0: draw when
samples passed). That read was the front's largest wait (FrontDmaRead, ~16% of the front with its
drains). With the switch on, nothing is measured:
- Front (fritchii_occlusion.h, Maxwell3D::FrontRunMacroFeaturesT's emit): the hash-matched 0xe2c
  call makes no read; its 0x158 write appends six made-up words. Ahead of FRITCHII_E2C_ON_E (no
  call is deferred while it is on); not with EXP53's predicate. The switch adds bit 16 to the
  front's macro features, so the features body runs (only it knows the macro's hash).
- Back (Maxwell3D::ProcessQueryGet): a type 21 report becomes a payload report of a made-up count
  before anything queues or posts it, so it is written as payload releases are (at once below High
  accuracy, on E with FRITCHII_ONEF_SEM_E): no host occlusion query, result copy, report
  registration or fence operation. Sample counter resets (clear_report_value) are dropped
  (ProcessCounterReset).
- Render thread (QueryCacheBase::CounterEnable, asked at every draw): the host sample counter stays
  stopped, so no occlusion query wraps the draws.
- 1: everything visible. The made-up count grows by 4096 at every report (never 0), so any two
  reports differ, whoever reads them (the front, the back's macro path with the split off, a CPU
  read), and the macro's words say "differ": mode p0 ^ 1.
- 2: every report reads 4096 and the macro's words say "equal": mode p0. Objects drawn on p0 = 0 are
  skipped. This is what a constant count (Blitchii's 4096) gives OoT's pair test; for comparison.
- Live: turning it off, the render thread starts the counter again at the next draw and the back
  makes real reports again; a report pair across the switch reads as different (visible) for that
  one test. Turning it on, reports already made complete as before. E's one-follower mirror of the
  counter state may say "started" while R's counter is stopped: the superset side, safe.
- Log, every 10 s while it works: "Occlusion (FRITCHII_OCCLUSION_VISIBLE=N) last 10 s: X sample
  reports/s answered without a host query, Y counter resets/s skipped, Z 0xe2c reads/s skipped on
  the front".
- Visual risks (1): objects behind walls drawn (more draws, and possibly lens flares, sun glare,
  light shafts or glow sprites showing through walls and hills if the game tests those); (2):
  tested objects missing.

### FRITCHII_UNSWIZZLE_IN_PLACE

TEST, off by default (helper AO, 2026-11-15, port of tomk134/lichii-custom e8e50e797 via
DahBard/Blitchii 43321b8db9, d05a021080 and their r54 crash fix d3251a67cf); live,
FRITCHII_UNSWIZZLE_IN_PLACE=1 turns it on. ExecuteUnswizzlePlan (the chunked upload of 2D images of
64 MiB or more that aren't converted or GPU-decoded) copied every guest run into a fresh vector
before untiling it. With the switch on, a run that is contiguous in host memory (MemoryManager::
GetSpan, which checks the device pages' host continuity, not just the GPU mapping) is untiled
straight from guest memory; other runs are copied into a grow-only thread-local scratch buffer. The
main upload path (GpuGuestMemory) already read in place. =2 also untiles every in-place run from a
copy and compares (exactness check, must be 0; a game write between the two reads could differ).
Log: "Unswizzle in place in 10 s (FRITCHII_UNSWIZZLE_IN_PLACE=N): R runs read in place (X MiB), C
copied first (Y MiB); verify V checked, M differ (must be 0)".

### FRITCHII_KEEP_BLOCKS

TEST, off by default (helper AO, 2026-11-15, port of DahBard/Blitchii's device memory reserve, r64
cccc1ff9fc, capped). Set at boot to enable (any value; then live): 0 = keep nothing, 1 = 256 MiB,
N = N MiB, at most 512 (no brute-force VRAM). Unset at boot, VMA calls the driver directly as before.
VMA's device-local blocks it frees (plain blocks only: power-of-two sizes 1-64 MiB, no dedicated
memory, nothing host-visible) are kept and handed back, without a driver call, to VMA's next
allocation of the same memory type, size and flags. Area loads at the texture cache's threshold
evict images (their blocks empty and go back to the driver) and then make new ones; on Blitchii
vkAllocateMemory took 29-50% of the back thread in OoT's transition frames. Limits: the cap, and
(texture cache budget check, every 30 frames) what the live usage leaves under the critical use
plus half the room between the critical use and the budget. Everything goes back when a driver
allocation fails (then retried), when an image is over VMA's budget (then retried before it counts
as memory pressure), when images stop fitting (the EXP42 pool's pressure test), and at once when the
switch goes to 0; over the limit one block a frame goes. The caches count kept blocks as free
(Device::QueryDeviceMemoryUsage). FRITCHII_KEEP_BLOCKS_PREMAKE=1 (boot) makes the cap's worth of
VMA default-size blocks at boot. Off on integrated GPUs. Log: "Kept blocks in 10 s
(FRITCHII_KEEP_BLOCKS cap N MiB, limit L): A driver allocations under VMA (X MiB) in T ms, longest
M ms; F driver frees in T ms; R blocks reused, K kept, S not kept, G given back; holds H MiB". Note
the Churn line's device allocations still count reused blocks (VMA's own callback).

### FRITCHII_KILL_RT_POOL

TEST, off by default (helper AV, 2026-11-15; the kill hitches in the Stalchild fight). Live: 0 =
as before, 1 = colour render targets (colour attachment, not depth, 2D, not sparse or disjoint, not
asked for dedicated memory) of up to 32 MiB are made with vkCreateImage, allocated from their own
VMA pool per memory type (64 MiB blocks, like EDEN_NX2_DEPTH_POOL for depth) and bound; anything
the pool refuses (over 32 MiB, no room within the budget, a failed bind) goes the old way. 2 = dry:
nothing moves, the line counts the colour targets the pool would try and how many of them got
memory of their own. Why: outside a pool the driver's preference gives a colour target its own
memory, a driver allocation on the GPU back thread each time. In the owner's fights
(fight-keepers*, 2026-11-14) the only frames over 50 ms after the load were single image creates
of 35 and 124 ms at kills (159 ms once in a playtest), the back blocked and every other GPU thread
idle behind it. Only memory placement changes, never what is drawn; images made before a switch
change stay where they are. FRITCHII_KILL_RT_POOL_BLOCKS (boot, default 2, at most 8 = 512 MiB):
blocks the pool makes when it is first used and never gives back. Log, every 10 s while the switch
is not 0, and at 0 only for a window with a create of 20 ms or more: "Kill image creates
(FRITCHII_KILL_RT_POOL=N) in 10 s: C made in T ms; per kind (colour in the kill pool, colour own
memory, colour in a block, depth own memory, depth in a block, other own memory, other in a block):
count (ms, over 5 ms, over 20 ms, longest); worst W ms: size, format, usage (kind); kill pool: P
placed, O over 32 MiB, R refused, B blocks, U of M MiB used; dry: D colour targets the pool would
try, K of them got memory of their own". Read the worst create's kind first: "colour own memory"
is what the pool removes; "depth in a block" points at EDEN_NX2_DEPTH_POOL_MIN_BLOCKS; "other in a
block" at FRITCHII_KEEP_BLOCKS.

### FRITCHII_DEPTH_POOL_BIG

TEST, off by default (helper BA, 2026-11-16; the Castle Town load cluster). Live: 0 = as before,
1 = a depth image the depth pool (EDEN_NX2_DEPTH_POOL, 64 MiB blocks) refuses is tried on the
normal heap with VMA's budget check before it counts as "over the memory budget". Why: VMA refuses
anything larger than a custom pool's block with VK_ERROR_OUT_OF_DEVICE_MEMORY before it looks at
the budget, and every Castle Town load makes a 12288x12288 D16 depth target (288 MiB) right in the
longest frame (raise-*-log.txt, 2026-11-16: "Image 12288x12288x1 ... format 124 is over the memory
budget; allocating it anyway", then "Out of device memory: keeping 512 MiB more free"). That is a
false pressure event: the texture cache keeps 512 MiB more free from then on (thresholds drop on
cards where the budget binds, so 8 GB cards evict and upload again mid-load) and the image was
placed with no budget check at all. Exact: only where the memory comes from. Log (first 8): "Depth
image WxHxL (format F) refused by the depth pool, outside it: placed within the budget
(FRITCHII_DEPTH_POOL_BIG, N so far)". How to judge: with 1 the "over the memory budget" and "Out of
device memory" warnings are gone from the load; the hitch score should not move on a 16 GB card
with EDEN_NX2_TC_CEILING_MIB set (the ceiling binds there), so this is a correctness fix first.

### FRITCHII_HITCH_SPLIT

Diagnostic, boot, default 0 (helper BA, 2026-11-16). 1 = the hitch line gets six more causes and
the GPU threads' CPU time. Why: in the Castle Town load cluster 1.4-1.9 s of "recompiling code
another core had" sat in gaps where the GPU threads were idle (the game waited on the recompiler),
the 730-780 ms gap had the back busy 300-400 ms outside any span with 600k page faults, and the GPU
threads never showed in "busiest threads" (only the first 48 named threads were measured; GPU,
GPUBack and GPUMem start later). The new causes, shown only when they happened:
- "recompiling code this core had compiled before (dropped since)": split out of "code another core
  had". Large = blocks are dropped and compiled again (invalidations, fastmem faults); small with a
  large "another core had" = the warm-up never reached those blocks on this core.
- "game code dropped by invalidations (blocks)": blocks removed by range invalidations (kernel code
  memory changes, instruction cache maintenance); count = blocks, time = the removal.
- "game code dropped by fastmem faults (blocks)": blocks dropped to be compiled again without
  fastmem for one access (guest writes to pages the GPU caches protect), once per access and core.
- "warm compiles refused (code cache full)": precompiles refused because the code cache keeps its
  free half; plus one line per core the first time: "JIT warm compiles refused from now on
  (FRITCHII_HITCH_SPLIT) core c: code cache keeps K MiB free, near N MiB used, far F MiB used".
  Refusals mean FRITCHII_JIT_CACHE_MIB is worth a test.
- "staging buffers made" (MiB = their sizes) and "texture uploads (CPU side)" (MiB = mapped sizes;
  includes the untiling and the first writes into new staging memory, so its page faults).
- busiest threads: GPU, GPUBack, GPUMem and GPUBackE2 are measured even past the first 48 threads.
Cost: a cached flag test per compile, staging buffer and upload when off; two clock reads each when
on. Exact (counting only).

### FRITCHII_JIT_SHARED_FASTMEM_MARKS

TEST, boot, default 0 (helper BO, 2026-11-16; the idea of zortos293/Lichii #1861, which does it on
the arm64 backend). 1 = the CPU recompiler's fastmem fault marks (an access that faulted on a
write-protected or read-trapped page is compiled again without fastmem) are one set per process
instead of one per core. An access that faulted on one core is then compiled without fastmem by
the other cores the next time they compile that block (on-demand compiles, warm compiles, blocks
compiled again after a drop or a cache clear), instead of faulting and recompiling there too.
Blocks another core already compiled with fastmem still fault once each, as before.
- Exact: a mark only sends that one access to the page table path, never changes a result.
- Cost: a shared lock around one set lookup per memory access while a block compiles (not while
  code runs); none when off.
- Log: "FRITCHII_JIT_SHARED_FASTMEM_MARKS=1: ..." at boot; "JIT shared fastmem marks ...: N
  accesses compiled without fastmem from another core's fault (M marks ...)" at N = 1, 2, 4, 8 ...
- Judge with FRITCHII_HITCH_SPLIT=1: "game code dropped by fastmem faults" (blocks) and the
  "dropped since" recompile time should fall; the hitch score must not rise. Up to 5/6 of the
  fastmem recompiles of blocks compiled after the first fault (6 cores). Fresh A/B (boot).

### FRITCHII_JIT_STATE_REFS

TEST, boot, default 0 (helper BQ, 2026-11-16; stage 0 of helper BO's one code cache for all cores,
helper_bo_answer.md). 1 = every per-core address the CPU recompiler used to write into a block as
an immediate is read through the state register (r15) instead: the callbacks object (SVC,
exceptions, cache ops, ISB, counter reads, memory slow paths, 128-bit and exclusive fallbacks), the
JIT (the dispatcher's block lookup), the config (exclusive accesses through the monitor in C++),
the TPIDR register addresses, this core's exclusive monitor slots, the fast dispatch table and
FRITCHII_JIT_INDIRECT_CACHE's slots and counters. A block's bytes then no longer depend on the core
that compiled it; caches stay per core. The inline exclusive store clears every reservation on its
address (this core's too, as the monitor's C++ path does), addressed from the monitor's lock.
2 = 1, and every block (near and far part) and the prelude are scanned for any 8 bytes that point
into this core's objects (JIT object with its state block and emitter, callbacks, TPIDR, own monitor
slot, fast dispatch table, indirect slots). Cost of 1: one load per call out (rare), a load instead
of an immediate at TPIDR reads/writes and exclusive accesses, and one load more per indirect branch
with FRITCHII_JIT_INDIRECT_CACHE=2/3 (the slot array, then the slot's offset in the cmp and jmp).
- Log: "FRITCHII_JIT_STATE_REFS=N: ..." at boot. With 2: "JIT state refs (FRITCHII_JIT_STATE_REFS=2)
  core C so far: N per-core constants left in M blocks of B scanned (must be 0); prelude P" at 256,
  512, 1024 ... blocks, at the first block with one, and at the end; the first 8 hits get their
  own line (block pc, offset, value, what it points into, the 2 bytes before it = the opcode).
- Judge: =2 at market and castletown must say 0 (stage 1 needs it); then fresh A/B 0/1/1/0 (boot)
  at the market for fps, plus the hitch score. Exact: the same values, read from the state block.

### FRITCHII_HOST_ZSTD

Boot, built in at 3 since 6.2 (helper FH, 2026-11-19; 0 turns it off). OoT's zstd decodes run on the host's libzstd
instead of the game's own zstd 1.4 translated by the JIT. Bit 0 (1) = the resource loaders'
whole-buffer decode (zstd's decompressMultiFrame inlined at main+0xEB380, the ARes_Loading
threads); bit 1 (2) = the stream decode (the game's buffer-less stream wrapper at main+0xF5C80,
ParallelDecompThread); bit 2 (4) = the stream decode in verify mode (see below); 3 = both on the
host. Meant for area loads and streaming (loading screens, load hitches); idle in steady play.
- How: the JIT compiles a `brk #0xF7A0+n` in place of one word of the game's code (MemoryReadCode,
  also for warm and block-list compiles; guest memory unchanged). The breakpoint calls
  core/arm/dynarmic/host_routines.cpp inside the block, which answers the call or declines it
  (the replaced word's effect, then the next word). A call costs one callback and a dispatcher
  lookup, nothing next to a decode. Each site goes in only for main build E0F3CCFA... when its
  checked words match ("Host zstd: main+0x... (...) hooked" or "... does not match"); never with
  the debugger on.
  - Whole-buffer: the BRK replaces the function's first word (a `pacibsp`, a no-op: declining
    goes on at +4); answered with x0 = the result, PC = x30. Its callers set up a static context
    in place right before each call and read only the result.
  - Stream: two sites. The entry (main+0xF5C80, a `pacibsp`) is never answered: it only notes a
    new frame (state 1) and ends any host stream of an object that starts or ends a frame. The
    game's code then runs its own frame set-up (header, the static context in the object's
    workspace, the dictionary, state 2), so the game's context is always one the game made.
    The loop site (main+0xF5CD0, `cmp w23, #1`, reached from the set-up and from a frame under
    way) takes a frame from its first byte when the set-up just ran, and answers through the
    function's own epilogue (main+0xF6084: x19..x30 and the stack restored, w0 returned).
    Declining sets the flags of that `cmp` and goes on at +4. A frame that began in the game's
    code stays there; a host frame stays on the host.
- Exact: the same results as the game's decoder (content size, or zstd's error code; the stream's
  state, output, room and bytes-to-come fields as the game writes them; -1, -2, -3 as the game
  returns them). The game's DDict is used through a libzstd DDict made from a copy of its
  content, kept while the game's DDict (address, content, size, ID, entropy tables) is unchanged
  and taken only when libzstd gives it the game's dictID. Output goes through Memory::WriteBlock,
  so pages the GPU caches track are marked as the game's stores would mark them.
- Verify mode (4, or 5 with whole-buffer decodes on the host): every stream frame the host could
  take is decoded on the host into a shadow (no guest write), the game's code decodes it for
  real, and the next call's entry compares the object's state, output pointer, room and
  bytes-to-come and the chunk's output bytes with the shadow. Mismatches are logged (the first
  16, "Host zstd verify mismatch #n ...") and counted in the log line: must be 0.
- Differences, none for a valid frame: on a decode error the game's output buffer is not written
  (the game's decoder leaves part of it; its callers only test the result); bytes past the
  content are left as they were (both decoders' fast copies may leave leftovers there); the
  game's staging buffer is not used (only this function reads it); a broken frame may fail with
  another error code where the game's would (corrupt input only).
- Left to the game's code (logged once each): a raw dictionary, the magicless format, a
  dictionary libzstd does not take as the game's, over 256 MiB in / 512 MiB out (whole-buffer) or
  512 MiB of content / a 64 MiB chunk (stream), 64 host streams or 1 GiB of stream buffers in
  use, staged bytes left in the stream object from an earlier frame, host memory running out.
- Cost: a stream keeps a host copy of its frame's content (the window later blocks copy from)
  until it ends; the decoding core is not preemptible while a call runs ("longest call" in the
  log line: a chunk or file of a few MiB is a few ms).
- Log: "FRITCHII_HOST_ZSTD=n: ..." at boot, the site lines, "Host zstd ... taken" per dictionary,
  and every 10 s while decodes ran: "Host zstd (FRITCHII_HOST_ZSTD=n) whole-buffer|stream: N calls
  on the host, X MiB out in Y ms (longest call Z ms), E errors as the game's, D left to the game's
  code, verify: C chunks compared, M mismatches in all (must be 0)".
- Judge: first 5 (verify, must be 0 mismatches), then loading screen time and load hitches on the
  same route, 0 against 3 (boot switch, fresh launches); the picture after each load must be the
  same.

### FRITCHII_JIT_SHARED

TEST, boot, default 0 (helper BX, 2026-11-16; stage 1 of helper BO's one code cache for all cores,
helper_bx_answer.md). 1 = every guest core of a process runs one CPU recompiler code cache: one
code space, one block map, one emitter, blocks compiled once under a lock (one compiler at a time)
and linked for all cores. Each core keeps its state block, run loop, fast dispatch table, return
stack buffer, FRITCHII_JIT_INDIRECT_CACHE slots and counters. Needs FRITCHII_JIT_STATE_REFS=1 and
compact links (FRITCHII_JIT_CHAIN=4/5/6/7 or 1 with FRITCHII_JIT_FAR_CODE=1); without them, with the
debugger, cycle counting or 32-bit games each core keeps its own cache (a warning says why).
Lock-free: running code, links (patched by one aligned 4-byte store of a jump's rel32, or an 8-byte
store of the return stack push's immediate), the per-core dispatch table and the shared map's
lookup. Removed blocks (code changed or unmapped, fastmem recompiles) keep their code until a
whole-cache clear; each core drops them from its own tables at its next entry. A whole-cache clear
(cache full with 32 MiB left, or a guest IC IALLU) halts every core and runs once all are out of
generated code. Not with FRITCHII_JIT_HOT_PACK (each core's pack would clear the shared cache).
FRITCHII_JIT_DUMP records blocks on the core that compiled them. FRITCHII_JIT_STATE_REFS=2 scans
shared blocks for every core's objects. 2 = 1 plus checks: 1 in 64 lock-free lookups compared with
the map under the lock, and the state block's own objects checked at 1 in 64 entries (both must be 0).
Also fixes FRITCHII_JIT_INLINE_CNTPCT's per-core address under FRITCHII_JIT_STATE_REFS (read through
the state register now, so stage 0's scan covers it).
- Log: "FRITCHII_JIT_SHARED=N: ..." at boot, "JIT shared code (FRITCHII_JIT_SHARED=N) core C made/joined
  the process's code cache" per core, then every 10 s: "JIT shared code (FRITCHII_JIT_SHARED=N) T s: B
  blocks compiled once (+b); blocks by cores that looked them up 1:a 2:b ...; first lookups of a block
  another core had L (+l); compile lock: W waits, X ms waited (+x), Y found compiled by the core waited
  for; removals R, whole-cache clears C; near/far MiB used, map K keys; patch shape errors P (must be
  0)" (=2 adds "checks, mismatches (must be 0)").
- Judge: boot =1 at market and castletown (with STATE_REFS=1 and the test set), RAM census (exec
  memory should drop by about 5 cores' code), hitch score fresh 0/1/1/0, fps A/B, a 10-minute run.
  Exact: the same blocks, compiled once.

### FRITCHII_JIT_CACHE_MIB

TEST, boot, default 0 = 512 MiB (helper BA, 2026-11-16). 512-1536: the CPU recompiler's code cache
per core in MiB. Why: precompiles (EDEN_NX2_JIT_WARM, FRITCHII_JIT_BLOCK_LIST's 649k-block warm-up)
stop while half the cache (half the smaller region with FRITCHII_JIT_FAR_CODE: 96 MiB of 512) is
free, so blocks the warm-up never reached are compiled at the area load, by each core that runs
them. Exact: the same code, more room. Memory is committed only as code is written (up to the size
per core, 4-6 cores). Log: "FRITCHII_JIT_CACHE_MIB=N: code cache of N MiB per core". Only worth a
test when FRITCHII_HITCH_SPLIT=1 shows "warm compiles refused" before the load; judge by the load
cluster's hitch score and its "another core had" time, fresh launches (boot setting).
### FRITCHII_TEST_COMMIT_REFUSE

Boot, Windows, TEST, default 0. Windows' commit limit is RAM plus the page files. A commit refused
there (error 1455) used to end the game: guest memory then faulted, a refused `operator new` was an
uncaught std::bad_alloc. Guest memory, page tables and `operator new` now try again every 50 ms for
up to 5 s (a system-managed page file grows meanwhile, the texture cache gives memory back), then
log "Out of memory" and fail as before; the crash report says the limit was hit. `operator new`
only waits with under 4 GiB of commit left, so a bad size still fails at once. Nothing changes
until a refusal happens. `<ms>` refuses every commit for that long from the first one on and starts
one impossible allocation: under 5000 the log shows "refused" and "went through"; above it, "Out of
memory" and then the game going on once the window ends (or the crash report's line).

### FRITCHII_TEST_SAMPLER_LIMIT

Boot, TEST, default 0. Drivers limit how many samplers exist (about 4,000 on NVIDIA) and the texture
cache never frees one; past the limit the game used to end. A refused sampler now borrows this
sampler's first variant or the first sampler made, and the log says "The driver refused a sampler"
(8 times at most). `<n>` makes the driver's refusal happen after n samplers: the game must keep
running, with some textures filtered or wrapped wrongly.

### FRITCHII_TEST_DEVICE_LOSS

TEST, off by default (live, debug server only). 1 = the next queue submission goes through the
device loss report without a real loss: the "Device loss" lines, every thread's stack, the "Host
crash" line, eden_crash.dmp from the helper process, then the process ends with code 2. It should
take a few seconds. 2 = the same, with the GPU's semaphore treated as unreadable, as on a real loss.
Never set it in nx2_switches.txt.

The report and the hang report name the submission holding the queue up: "GPU oldest unfinished
submission: tick N", the first after the GPU's last reported tick without an end stamp, out of the
last 256 kept (FRITCHII_SLOW_GPU), with what it held. The newest 8 follow as before.

### FRITCHII_SMALL_CARD, FRITCHII_SMALL_CARD_PARTS, FRITCHII_SMALL_CARD_BUFFER_BLOCK_MIB

TEST, off by default (helper BY, 2026-11-16). FRITCHII_SMALL_CARD (live): 0 off, 1 on, 2 auto = on
when the main device-local heap is under 9 GiB or its budget at boot under 7.5 GiB (8 GB and 6 GB
cards; never integrated GPUs; a simulated card's limit counts). The small card profile: an 8 GB
card should stop overflowing in real play (RTX 2080 at 1440p: 7.9 GB of a 6.7-7.2 GB budget, 32
pressure events, read-backs stalling for seconds; RTX 3070 Ti 6850 of 7181) and a 6 GB card should
fit at 1x, with nothing reserved for show. Big cards never turn it on in auto mode.
FRITCHII_SMALL_CARD_PARTS (boot, bit mask, default 0x2F) picks the parts:
- 0x1 fit (live): the texture cache's critical threshold = budget less 1/16 of it (at most 512
  MiB; was 1/8, at most 1 GiB) less the out-of-memory margin, the block slack and what the cache's
  usage leaves out: the image pool, deleted images still in flight, collector downloads, frees
  queued for the free thread ("uncounted", reserved at once when it grows, 16 MiB a check when it
  shrinks). No fixed 5 GiB cap. The out-of-memory margin stops at 1/8 of the budget instead of 3
  GiB (the RTX 2080's Castle Town load drove it to 3 GiB: critical ~3.6 GiB, the collector evicted
  the scene it drew). Expected stays 1/8 of the budget under critical.
- 0x2 pool (live): EXP42's image pool cap = 1/16 of the budget, at least 256 MiB (~450 MiB on 8 GB,
  ~340 on 6 GB) instead of EXP66's 1 GiB.
- 0x4 buffer blocks (boot): the buffer allocator's VMA blocks are 16 MiB instead of 32
  (FRITCHII_SMALL_CARD_BUFFER_BLOCK_MIB, 4-256, tries others), so buffers over 8 MiB get exact
  memory. At the market the buffer allocator held ~1.4 GiB of blocks for ~0.7 GiB of buffers (8 GB
  and 6 GB simulations): most of the block slack.
- 0x8 refuse scale (boot): a resolution scale over 1x runs at 1x for the session (the setting
  stays); the status bar reads "Scale: 1x (2x DOESN'T FIT)". Log: "Small card profile: resolution
  scale ... does not fit".
- 0x10 stream (boot, not in the default): stream buffer 64 MiB instead of 128.
- 0x20 buffer pool blocks (boot): the buffer cache's device-local pool uses 4 MiB blocks instead of
  16, buffers over 2 MiB take the default blocks (FRITCHII_BUFFER_POOL_BLOCK_MB overrides, 0 keeps
  16). Castle Town: ~140 MiB of small buffers held 50 blocks, 800 MiB, on every card size.
Logs: at boot "Small card profile (FRITCHII_SMALL_CARD=N): main heap H MiB, budget B MiB, a small /
not a small card; profile on/off (parts ...)"; the thresholds' "Memory margins" line adds
"uncounted U MiB (small card profile on/off)"; with FRITCHII_VRAM_REPORT=1 a second line "VRAM split
(FRITCHII_VRAM_REPORT): main allocator blocks / allocated, buffer allocator blocks / allocated,
outside the allocators MiB; small card profile ...". Judge under EDEN_NX2_VRAM_LIMIT_MIB=8192 / 6144
+ FRITCHII_VRAM_SIM_OTHERS=1 (work\helper_by_answer.md): "over the budget" samples 0, fps and 1% lows
not lower; with no limit on a 16 GB card, FRITCHII_SMALL_CARD=2 must log "not a small card" and
change nothing.

### FRITCHII_IMAGE_DEVICE_LOCAL

TEST, off by default (helper DQ, 2026-11-17; port of a low-VRAM fork's fix). Boot: 0 off, 1 on, 2
dry. Discrete GPUs only (integrated: logged and ignored). An image's first try in the default heaps
is "within the budget, prefer device memory" with nothing required, so once the device heap is over
its budget VMA moves on to a host memory type still within its budget and succeeds silently: the
image lives in system RAM, the GPU samples it over PCIe (the other fork measured 9.4 GB of images
there in the market, 150 ms frames), and the texture cache never sees pressure because the device
heap's usage does not count it. =1: the tries within the budget in the default heaps (the plain
first try, the depth pool refusal retry, the kept-blocks retry) require DEVICE_LOCAL; custom pools
keep their own memory type. A full device heap then refuses the image: that is a pressure event
like any refused image (with the native thresholds the texture cache keeps 512 MiB more free, "Out
of device memory: keeping N MiB more free"; the budget control and FRITCHII_TC_CRITICAL_ENFORCE
work as before), and
the old fallbacks place it as before (without the budget check, device memory first; host memory
only when the device really is out). =2: placement unchanged, only counts. Logs: at boot
"FRITCHII_IMAGE_DEVICE_LOCAL=N: ..."; then "Image device-local (FRITCHII_IMAGE_DEVICE_LOCAL=N,
event): first tries refused R, of those placed over budget in device memory D; host memory images
H (M MiB), by the first try F" each time R or H reaches a power of two, then every 1024. With =2, F
is the silent spill =1 removes. Test: 8 GB simulation (EDEN_NX2_VRAM_LIMIT_MIB=8192 +
FRITCHII_VRAM_SIM_OTHERS=1) at the market, first =2 (is F above 0 at all?), then =1 vs 0 fresh
(boot switch): host images and hitch lines down, pressure events up, fps and 1% lows not lower.
On a 16 GB card without a limit both modes must log nothing after boot.

### FRITCHII_BUFFER_POOL_BLOCK_MB

TEST, off by default (helper DQ, 2026-11-17; port of a low-VRAM fork's change). Boot: 0 = 16 MiB
blocks (as before); n = n MiB blocks (1-64) for the buffer cache's device-local VMA pool
(Device::SeparatePool::DeviceBuffers, both allocators when they are split), and buffers over half
a block take the default blocks. The other fork uses 4 (2 MiB cut-off): buffer cache buffers live
a few per 16 MiB block, and a block stays while any of them does, mostly empty. Not the same as
FRITCHII_SMALL_CARD_BUFFER_BLOCK_MIB (the buffer allocator's default blocks). Log at boot:
"FRITCHII_BUFFER_POOL_BLOCK_MB: device buffer pool blocks N MiB, buffers over K KiB take the
default blocks". Judge by the "buffer pool" slack in FRITCHII_VRAM_OWNERS / FRITCHII_VRAM_REPORT
and the Churn line's device allocations (smaller blocks = more vkAllocateMemory calls), fresh
launches.

### FRITCHII_BC1_KEEPS_ALPHA

Off by default (helper DQ, 2026-11-17; port of a low-VRAM fork's fix). Boot: 1 on. Only does
something when the player picked ASTC recompression BC1 (our default is BC3) on a GPU without
native ASTC: BC1 for every ASTC image turns alpha into a 1-bit cutout (dark faces and banding in
OoT). With 1 the session runs the BC3 path instead, whose opaque scan already sends the fully
opaque images to BC1 (EDEN_NX2_ASTC_BC1, on by default) and keeps the others in BC3. Log at boot:
"FRITCHII_BC1_KEEPS_ALPHA: ASTC recompression BC1 runs the BC3 path ...". A candidate for default 1:
it changes nothing for a player on BC3 or Uncompressed.

### FRITCHII_VRAM_POOL_FLOOR

Default 1 (helper FE, 2026-11-19). Live: 0 off. While
FRITCHII_VRAM_POOL_FIT or budget pressure sets the image pool's cap, the cap never goes under 1/24 of
the budget (64-192 MiB), and images only that floor keeps wait 16 frames. An image refused by the
budget halves the pool instead of emptying it; one that could not be made still empties it. Why: over
budget the fit gave 0, so every render target given to the pool was destroyed at once (simulated 4 GB
card at Castle Town: 0 taken, 2,800-5,400 missed per 600 frames; 16 GB: ~640 taken, 20-50 missed).
Cards with room never reach the floor. Judge by the "Image pool (EXP42)" line's taken and missed.

### FRITCHII_VRAM_POOL_FIT

TEST, off by default (helper BF, 2026-11-16; 8 GB cards). Live: 1 = EXP42's image pool (pooled
VkImages of deleted render targets, built-in cap EXP66 = 1 GiB) holds at most the room the rest of
the process leaves under the driver's budget less the texture cache's margin (1/8 of the budget, at
most 1 GiB), re-read every 30 frames. Why: the texture cache counts pooled images as free memory,
so its thresholds leave the pool out. Next to a full cache the pool pushed small cards past the
budget (6 GB card: critical ~4.2 GiB + 1 GiB of pool > a ~5 GiB budget), where Windows moves memory
to system RAM (the 16-24 fps cliff) or an image fails and the whole pool is dropped at once. Saves
0 on a 16 GB card (cap stays 1 GiB: ~7.7 GiB of room at the market), up to the whole 1 GiB on 6-8
GB cards when the cache is full. Log, every 600 frames while it is on: "Image pool fit
(FRITCHII_VRAM_POOL_FIT) last 600 frames: cap C MiB now (lowest L MiB, built-in B MiB); budget X
MiB, the rest of the process R MiB". Judge with FRITCHII_VRAM_REPORT=1 under
EDEN_NX2_VRAM_LIMIT_MIB=8192 / 6144 + FRITCHII_VRAM_SIM_OTHERS=1: "over the budget" samples go to 0,
fps and 1% lows not lower, the pool line's "taken" count tells what the smaller pool costs.

### FRITCHII_VRAM_CRITICAL_FIT

TEST, off by default (helper BF, 2026-11-16; use with FRITCHII_VRAM_POOL_FIT). Live (read at the
texture cache's budget check, every 30 frames): for budgets between 4 and 10 GiB the collector's
critical threshold may reach the budget less the margin (1/8, at most 1 GiB) less 512 MiB, instead
of the fixed 5 GiB (MaxCriticalMemory). Why: an 8 GB card (budget ~7.1 GiB with Windows and other
programs) collected from 5 GiB on while OoT's market sits at ~5.1-5.3 GiB in the cache's view on a
16 GB card, so it evicted and re-made textures (ASTC re-transcodes, downloads with a GPU wait) with
~2 GiB of budget left to the pool and margins. 8 GB card: critical 5120 -> ~5700 MiB, expected 4352
-> ~4940 MiB (+580 MiB for textures). 6 GB cards and 10 GiB+ budgets: no change. Judge at
EDEN_NX2_VRAM_LIMIT_MIB=8192 + FRITCHII_VRAM_SIM_OTHERS=1 with EDEN_NX2_TC_LOG=1: the collector
line's deletes (costly, downloaded) and uploads drop, "over the budget" stays 0, fps / 1% lows /
hitch score at the market and night market.

### FRITCHII_DEPTH_POOL_RETRY

On by default since 2026-11-16 (helper BJ; =0 turns it off; made by helper BF). Live: 1 = a depth target that does not fit
EDEN_NX2_DEPTH_POOL's 64 MiB blocks is made the normal way (within the budget) before anything
counts as memory pressure. Why: OoT's 12288x12288 D16 sun shadow map (288 MiB) never fits the pool,
so it went down the over-budget path with VRAM to spare: "is over the memory budget; allocating it
anyway" and "Out of device memory: keeping 512 MiB more free" on a 16 GB card at 1.5 GB used
(prof-market-log 41.0 s, also in the 8 GB run). Each such false event empties the image pool (up to
1 GiB of render targets to re-make), releases the kept blocks and lowers the thresholds by 512 MiB
for minutes on small cards. Log: "Depth image WxH (format F) did not fit the depth pool; made
without it (FRITCHII_DEPTH_POOL_RETRY, N so far)". Judge: the two warnings above are gone at the
market after load; nothing else changes (placement only).

### FRITCHII_GRID_AUDIT

TEST tool, off by default (helper BJ, 2026-11-16). Boot: 1 = every indirect compute dispatch (list
dispatches, GPU-sized grids) copies the 3 grid words it ran with to host memory, and once that
submission is done they are checked: "Dispatch grid audit: indirect dispatch of S ran grid XxYxZ
(over the device limit / y/z past 16 bits) ... N bad of M audited". The host never sees these
grids before vkCmdDispatchIndirect, so this is the only way to learn whether a bad grid reaches
the GPU (AMD device losses). Costs a copy and two barriers per indirect dispatch: a diagnostic, not
for fps runs. Must be 0 bad; any line names the shader to clamp on the GPU next.

### FRITCHII_ONEF_AMD

Boot, off by default (helper BJ, 2026-11-16). On AMD's proprietary Vulkan driver the one follower
(FRITCHII_ONEF / FRITCHII_ONEF_LEAN) stays off: the desktop RX 7900 XT lost the device in game with
it on and ran clean with every FRITCHII_ONEF_* line out. Log once: "One follower: off on AMD's
proprietary driver ...". 1 = keep it on there (tests). RADV, NVIDIA and Intel unchanged.

### FRITCHII_ASTC_HELD_DEFER

Boot, default 0 (helper BG's fix from 2026-11-16, ported by helper FJ). 1 = an ASTC GPU decode made
while this thread holds a filled draw or dispatch packet (EXP123 >= 2) does not submit: the decode
and its barrier stay in the current command buffer, ahead of the draw, and the next flush takes
them. Why: that submit splits a held packet across two command buffers ("Memory seam (5.5) ...
ASTCDecoderPass"), and both AMD cards lost the device 10-15 ms after one at the title. That loss
is why EXP123 is held to 1 on AMD's driver (FRITCHII_SEAM_AMD), and with EXP123=1 the one follower
posts nothing there ("lent: no ring", every draw a transient lend). Test on AMD with
FRITCHII_SEAM_AMD=1: the 5.5 lines from ASTCDecoderPass must be gone, "ASTC GPU decode inside a
held draw fill" shows once. Watch RADV for BC3 flashes (the submit once hid a race there).

### FRITCHII_VA_REPORT

TEST tool, off by default (helper FJ, 2026-11-19). Boot: 1 = the instance enables VK_EXT_debug_utils
and the device VK_EXT_device_address_binding_report (AMD's Windows driver has it since 24.20,
NVIDIA too), and a messenger of its own keeps the last 65,536 GPU address binds and unbinds the
driver reports (object type, handle, range, time, thread) in a ring. On a device loss, each fault
address VK_EXT_device_fault names is looked up: "GPU address report: #N bind|UNBIND buffer|image|...
handle H, [A, B) K KiB, T ms before the report, thread X (holds the page)". An UNBIND newest =
the GPU used a freed object (a lifetime bug); a live bind = a bad range inside a live object
(sparse page, out of bounds past the robustness checks); no holder = a driver internal range or
one older than the ring. Then, for every fault: the span of all ranges the ring saw, how many events were the
driver's own objects (VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT; reporting them is the driver's choice),
the events in the fault's 4 GiB window (and up to 8 internal ones there); with no holder, the 6
nearest ranges below and above within 256 MiB, nearest first. Two AMD losses read pages at
0xbef0'xxaf'0000 that no reported range was near: these lines show whether that window is a driver
region we never see. Costs a locked ring write per bind (allocations, not draws): for loss
hunts, not fps runs. Boot lines: "GPU address report (FRITCHII_VA_REPORT=1): messenger on" and
"the driver reports its GPU address binds", or why it is off.

### FRITCHII_VRAM_REPORT

TEST tool, off by default (helper BF, 2026-11-16). Live: 1 = every 30 frames the main device-local
heap's usage against its budget as the driver reports them (all process memory there, unused
block space included; the budget at most the simulated limit), one line every 600 frames: "VRAM
fit (FRITCHII_VRAM_REPORT) last 600 frames: main heap U of B MiB budget now, peak P MiB; over the
budget in O of S samples (most M MiB over); image pool, kept blocks, block slack, staging upload /
download / device, stream MiB; memory pressure events, failed images". Over the budget is where a
real card starts moving memory to system RAM: on an 8 GB card that is the fps cliff. One driver
memory query per 30 frames.

### FRITCHII_VRAM_BUDGET_MODE

On by default as vk (helper CX, 2026-11-16; zortos tester report test 4, VRAM). Live: where the
texture cache's live budget cap comes from. fixed (0) = today's thresholds only; vk (1, default) =
the driver's budget for the process (VK_EXT_memory_budget); os (2) = the same, but at most the main
heap less what the other processes hold on the adapter (PDH's GPU Adapter Memory less GPU Process
Memory, sampled once a second on a low priority thread of its own, started on first use). The
environment takes the name or the number, the debug server's `switch` the number. Once a second the
driver's count of the main heap for the whole process (pool, frees in flight, block slack and the
driver's own memory included) is held under the budget less FRITCHII_VRAM_HEADROOM_MIB: past it the
cache's critical memory comes down by the excess (at most 1 GiB a second, never under half the
budget; expected keeps its distance), with room again it goes back up 256 MiB a second (dead band
half the headroom) until the base thresholds rule again. While the usage is past the target or the
cap binds ("pressure"): the collector deletes past critical as with FRITCHII_TC_COLLECT_OVER, the
image pool fits the room as with FRITCHII_VRAM_POOL_FIT, kept blocks go back, and
FRITCHII_TC_CRITICAL_ENFORCE=1 acts. Why: the thresholds followed the budget only below Fritchii's
7,680 MiB ceiling and in the cache's view of the usage; a 16 GB RX 9070 XT at 4K reached the budget
with the cache at 7-10 GB, the driver lowered the budget and moved ~10 GB to system RAM (two 15-16 s
freezes); a 5-7 GiB VRAM hog dropped the budget to ~11.6 GB and nothing followed. Costs nothing
while the usage stays under the target (RTX 4080 at 1080p: ~7 GB under it). Log: "VRAM budget
control (vk|os): driver usage U MiB of budget B MiB (vk, os, other processes), headroom H MiB:
pressure on|off, cap on|moved|off; texture cache T MiB, expected, critical (base)". Under
EDEN_NX2_VRAM_LIMIT_MIB + FRITCHII_VRAM_SIM_OTHERS use vk: os would count the other programs twice.

### FRITCHII_VRAM_HEADROOM_MIB

Default 0 = auto (helper CX, 2026-11-16). Live: the margin under the budget where
FRITCHII_VRAM_BUDGET_MODE starts shrinking the caches. Auto: 1/16 of the budget, 256 MiB to 1 GiB
(~950 MiB on a 16 GB card, ~450 MiB on an 8 GB one).

### FRITCHII_TC_CRITICAL_ENFORCE

Default 1 (helper CX, 2026-11-16). Live: before an image is made with the texture cache past its
critical memory, clean images (no GPU writes: no download, no wait) idle for 10+ frames and not
bound as render targets are deleted until the cache is back under critical with room for the new
one (at most 32 a pass, 256 a frame, the 512 oldest looked at). 0 = off; 1 = only while
FRITCHII_VRAM_BUDGET_MODE reports pressure; 2 = whenever past critical. Why: the collector runs once
a frame with a bounded number of deletes; an area load makes dozens of images in one frame on a card
already at its budget (a 1365^2 image create took 11.9 s while the driver made room). Counted in the
FRITCHII_VRAM_OWNERS line: "evictions before creates P passes, N images, M MiB, S short, C at the
frame cap".

### FRITCHII_TC_THRASH_GUARD

Default 1 (helper FE, 2026-11-19). Live: 0 off, 1 on, 2 counts only (the log line, no floor). Stops
the texture cache deleting images the game keeps coming back to when it is over budget. Why: the
collector compares the driver's usage with its critical memory, and on a small card most of that
usage is not images (Castle Town, simulated 4 GB card: 4,100-4,900 MiB in the cache's view, 1,500
of it images, critical 2,100-2,400). It could never get there, stayed 1 GiB+ over and deleted
everything unused for 3 frames, ASTC included: 55-60 images made a frame instead of under 1, ~1,700
ASTC GPU decodes a second instead of ~1, 44 fps with 20 fps lows instead of ~75. Deleted images
are remembered by address and shape (8,192 slots, ~320 KiB of RAM). When 8 or more a second come
back after the collector took them under pressure, the collector and FRITCHII_TC_CRITICAL_ENFORCE
leave alone images used within 1.25x the 90th percentile of the gaps (at most 600 frames). The floor
halves when live images pass 3/4 of the budget, decays after 10 s without pressure, and is ignored
when an allocation has failed. Without pressure it never rises, so cards with room are unchanged.
It only keeps images longer, so it cannot change what is drawn. With it, FRITCHII_TC_CRITICAL_ENFORCE
stops walking the idle list for the rest of a frame once a pass found nothing.
Log every 600 frames, when anything was made again or a floor is set: "Image churn
(FRITCHII_TC_THRASH_GUARD=N) last 600 frames: M images made, R made again after a delete (MiB, costly
to load, frames after their last use); deleted by: collector under pressure, collector, cold sweep,
superseded sweep, slot retire, join, unmap, unmap sweep, other; collector age floor F frames (raised,
halved), live images MiB". Test: EDEN_NX2_VRAM_LIMIT_MIB=4096 + FRITCHII_VRAM_SIM_OTHERS=1 at Castle
Town: "Images made" under ~2,000 per 600 frames, "ASTC GPU decodes" under ~200 per 5 s.

### FRITCHII_VRAM_OWNERS

On by default every 10 s (helper CX, 2026-11-16). Live: seconds between lines, 0 = off. One line
with who holds device memory next to the budget: "VRAM owners: main heap U of B MiB budget (S MiB
heap): texture cache images, deleted images still allocated, image pool, frees in flight, buffers
and the rest (buffer allocator), unused space in allocator blocks, kept blocks, outside the
allocators (the driver's: pipelines, descriptors, swapchain, its own); staging pool (any heap);
texture cache N images (GPU-written, unbacked), its view, expected, critical (base, budget cap on);
budget mode, budget (vk, os, other processes), headroom, last 10 s: peak usage, lowest budget,
seconds over budget less headroom, seconds over the budget; evictions before creates". Costs a walk
of the live images every 10 s.

Since helper DC (2026-11-17) the staging part ends in "block slack by pool: main allocator default
D MiB (B MiB in N memory objects, H holes, largest on the heap L MiB), depth pool (type T) ...,
colour pool ..., sparse pool ..., buffer pool ...; buffer allocator default ..., buffer pool ...":
the unused space of each allocator's block vectors on the main heap (a pool shows only once it has
blocks). "default" is each memory type's own blocks plus dedicated allocations (which add no unused
space). A pool whose largest hole equals its block size (64 MiB depth/colour, 16 MiB sparse/buffer)
holds an empty block. The buffer allocator is walked only when the back is its user ("not walked
(the memory stage holds it)" otherwise). Costs one walk of every block (VMA's statistics) of each
allocator every 10 s.

### FRITCHII_VRAM_TRIM

Helper DC, 2026-11-17. Live: 0 off, 1 auto (default: 90 with the small card profile on, otherwise
off), 2-100 = percent of the budget, on any card. Every 60 frames, while the main heap's usage is
at or above that share of its budget, each allocator gives back the empty block VMA keeps in each
block vector (each memory type's default blocks, the depth, colour, sparse and buffer pools; up to
32-64 MiB each). VMA keeps one empty block per vector and frees it itself only once the usage is
past the budget, when Windows may already move memory to system RAM. How: a 256-byte allocation in
the fullest block with room (NEVER_ALLOCATE, best fit), freed at once: VMA then deletes the last,
empty block (VmaBlockVector::Free). Memory that held nothing; the cost is a driver allocation when
that vector needs a block again. The buffer allocator is trimmed only when the back is its user.
Log: "VRAM trim (FRITCHII_VRAM_TRIM at P% of the budget): main heap U of B MiB, gave back X MiB of
empty blocks (main allocator M MiB, buffer allocator N MiB); session S MiB in K trims" (the first
8 trims, then at most every 10 s). Test: FRITCHII_VRAM_TRIM=40 on the 16 GB card makes it act at
the market (usage ~7.5 of ~15.3 GiB).

### FRITCHII_DEPTH_POOL_FIT, FRITCHII_DEPTH_POOL_SMALL_BLOCK_MIB

TEST, off by default (helper DC round 2, 2026-11-17). FRITCHII_DEPTH_POOL_FIT (live, bits): how
depth targets are placed in the depth pool (EDEN_NX2_DEPTH_POOL). Why: batch 29 at the market had
the depth pool at 1152-1280 MiB in 18-20 blocks of 64 MiB with 529-727 MiB of it in 76-177 holes
(largest 33-62 MiB): OoT makes depth targets sized by screen bounds (per-NPC D32 targets, shadow
cascades) next to large long-lived ones, so a freed target's hole rarely fits the next one, and a
small long-lived target pins a whole 64 MiB block.
- 1 = size classes: the allocation is rounded up to one of four classes per power of two (at most
  a quarter more, ~1/8 on average), so the hole a freed target leaves fits the next target of its
  class. The padding counts as allocated to VMA; the owners line shows it ("depth fit ... padding
  of live images P MiB"): compare slack + padding, not slack alone.
- 2 = small depth targets (rounded size at most a quarter of FRITCHII_DEPTH_POOL_SMALL_BLOCK_MIB,
  boot, default 16, 4-64) go to a depth pool of their own with blocks of that size: they no longer
  fill holes large targets need, and a long-lived small one pins 16 MiB instead of 64. The owners
  line's split shows it as "small depth pool".
- 3 = both.
Images made before a change stay where they are. A depth image the driver requires in memory of
its own, or larger than a pool block, goes the old way ("handed back" in the owners line). Only
where the memory sits changes; the image, its contents and its layouts are the same. Cost: none
expected (the same requirement queries VMA makes); the small pool makes a 16 MiB block more often
early on (a driver allocation each), so read Hitch lines and EDEN_NX2_IMAGE_ALLOC_TIMING.

### FRITCHII_VRAM_HOLES

Diagnostic, off by default (helper DC round 2, 2026-11-17). Live: 1 = every owners line
(FRITCHII_VRAM_OWNERS) ends in "holes (FRITCHII_VRAM_HOLES) main allocator: <vector>: N blocks M
MiB; holes <64K a/b MiB, <256K ..., ...; allocations by the same classes; blocks at least half
free K (S MiB) held by A allocations B MiB: depth, colour, other images, buffers and untyped; their
sizes ...; their ages <10 s, <1 min, <5 min, older, unknown" for each block vector of the main
allocator on the main heap ("default (type T)", "depth (type T)", "depth small", "colour",
"sparse", "buffer"). The half-free blocks' allocations are what keeps unused memory from going
back: their kind, size and age say which images to move or separate. Ages come from a stamp every
image allocation carries in its VMA user data. Images placed by FRITCHII_DEPTH_POOL_FIT or the
colour pool count as "untyped" (made with vmaAllocateMemory); their pool label says what they are.
Cost: builds VMA's detailed map (JSON of every allocation) each line: several ms on the GPU back
thread every 10 s. Only for measuring.

### FRITCHII_VRAM_PACK

Helper DC, 2026-11-17. Live: 0 off, 1 on, 2 auto (default: on with the small card profile on).
Buffers (both allocators, the separate buffer pool included) and memory commits take the
best-fitting hole of a block (VMA's MIN_MEMORY strategy, which images always use) instead of VMA's
default, which cuts them from a hole of the next larger size class and so splits the big holes the
next large buffer needs. Only the place in a block changes; the search is CPU time (no new device
memory). Read the owners line's "buffer allocator default / buffer pool" slack with 0 and 1.

### FRITCHII_VRAM_SIM_OTHERS

TEST, off by default (helper BF, 2026-11-16; boot, with EDEN_NX2_VRAM_LIMIT_MIB). 1 = the simulated
card's limit becomes the limit less what Windows and the other programs hold on the real card,
measured once at boot (heap size less the driver's budget; 16 GB card: 16376 - 15297 = ~1.1 GiB).
Why: EDEN_NX2_VRAM_LIMIT_MIB=8192 alone gave the simulated card a budget of the whole 8 GiB, but a
real 8 GB card next to the same desktop has ~7.1 GiB: the thresholds, VMA's budget and its heap
limit then sat ~1 GiB too high. VMA fails past the limit where a real card would spill into system
memory; images then go to host memory, as slow as the spill, so fps cliffs show up too. Log:
"FRITCHII_VRAM_SIM_OTHERS: others hold N MiB of this card's H MiB heap; the simulated card's limit
and budget are L MiB". The result depends on what else runs (OBS, browser): read the log line.
Known gaps: each of the two VMA allocators (images, buffers) gets the limit on its own, and driver
memory outside VMA (DLSS, pipelines, swapchain) counts in the budget but not in VMA's hard limit.

### FRITCHII_AMD_OVERALLOCATE

TEST, off by default (helper BF, 2026-11-16; boot; port of zortos293/Lichii #1335 via
Frizti/Fritchii #61). 1 = on AMD's own drivers (AMD proprietary / AMD open source, not RADV) with
VK_AMD_memory_overallocation_behavior, device memory may be overallocated: allocations past a full
heap spill into system memory instead of failing. Why: upstream traced 27 failing sessions (24 AMD,
mostly 8 GB cards in OoT) to failed device-local allocations with host memory free. Fritchii
already retries out-of-budget images and then places them in host memory, so this mostly removes
those fallbacks' "placed in host memory" / failed image lines. Log: "FRITCHII_AMD_OVERALLOCATE:
VK_AMD_memory_overallocation_behavior on". Judge on an AMD 8 GB card only (no effect elsewhere):
fewer "Out of device memory" and failed-image lines, no crash at full VRAM.

### FRITCHII_STAGING_RING

TEST, off by default (helper AO, 2026-11-15, port of DahBard/Blitchii's staging ring, r64). The
size is read at boot (1 = 256 MiB, N = N MiB, 16-1024; host memory, not VRAM), its use is live (0
stops it). One host-coherent upload buffer for the texture cache's staging pool, made and written
once at boot. Uploads too big for the stream buffer's region (8 MiB on a discrete GPU) that find no
idle cached staging buffer take a slice of the ring before a new staging buffer is made (often a
driver allocation, then first-touch page faults). Like the stream buffer, the ring has 16 regions,
each reused once the GPU is past its last stamp; an upload over half the ring needs all of it idle.
The last upload's regions are stamped again at the next request (its copy may be recorded after the
request). Log: "Staging ring in 10 s (FRITCHII_STAGING_RING=N): U uploads (X MiB), L over half the
ring; R refused (B while an upload over half the ring was in flight), C staging buffers made after a
refusal".

### FRITCHII_ZERO_UPLOAD

TEST, off by default (helper CJ, 2026-11-16, Castle Town load burst). Live. 1 = a whole texture
upload whose guest bytes are all zero becomes a GPU clear to zero; 2 = count only (the check runs,
the upload runs as before). Exact: zero bytes untile to zero texels, and the clear writes zero in
every level and layer, as the upload's copies do. Only plain uncompressed single-sample images of
256 KiB or more whose host image is the guest image (not converted, GPU-decoded, sparse, scaled,
padded, or with trimmed levels); a slice of a deep 3D texture (guest footprint over 2x its texels)
is not checked. The check reads the bytes the upload would read and stops at the first non-zero
4 KiB. Why: every Castle Town load made a 12288x12288 D16 target whose 288 MiB of never-written
guest memory was uploaded (102 ms on the front, 9 new 32 MiB staging buffers, 132k page faults),
plus fresh render targets uploaded from zero memory. The clear saves the staging buffers, their
first-touch faults, the untiling and the GPU copy; the read of the guest pages stays (their own
faults included). Log: "Zero uploads in 10 s (FRITCHII_ZERO_UPLOAD=N): Z uploads all zero (X MiB
guest, checked in T ms), cleared on the GPU; D had data (Y MiB looked at before the first non-zero
byte, T2 ms); S not checked (3D slices)", a "Zero upload:" line for each of the first 40 zero
images of 4 MiB or more, and "zero checks of texture uploads" in the hitch line.

### FRITCHII_STALE_ZERO

On by default (helper CV, 2026-11-16, dialogue text corruption, zortos293/Lichii #1310). Live. An
image made where a GPU-written image was dropped without a write-back skips its upload (the stale
skip, EXP78 / EDEN_NX2_TC_SKIP_STALE): the guest bytes there are older than the GPU's writes. It
started undefined, which in practice is whatever its device memory held last: with the EXP42 pool,
another deleted target's texels (other text, symbols, noise). With 1 it is cleared to zero in every
level and layer instead (uncompressed, unconverted, single-sample images; others stay undefined).
Still not the GPU's data (that was dropped; see FRITCHII_LOST_WRITEBACK), but transparent black
instead of another image, and it no longer changes between re-creations. Cost: one transfer clear
per skipped upload, which replaced a full upload. 0 = undefined as before.

### FRITCHII_LOST_WRITEBACK

On by default at 1024 (images up to 1 MiB; helper CV, 2026-11-16, round 2; zortos293/Lichii #1501's
idea). The default was 1 (every size) for one build: the market toggle 0/1 measured 72.1 -> 68.9 fps,
1% 54.1 -> 42.6, up to 6.1 GiB written back per 10 s (big targets the cold sweep drops); 0/1024 measured
73.2 / 73.6, 1% 54.8 / 56.3, and still covers the text targets (~0.5 MiB). Live. A correctness
fix: a GPU-written image that the cold sweep (EXP58's superseded deletes included),
FRITCHII_SWEEP_SUPERSEDED, EXP64's slot retire or the collector (bad overlaps) is about to delete
is written back to guest memory first, on the collector's download queue (EXP121's): the landing
deletes it and clears its stale marks, so an image made there later uploads the GPU's pixels
instead of skipping the upload (the stale skip) and showing nothing or other memory. The owner's
gallery session (build 21): OoT draws each dialogue/choice button's text once into a 1136x120 target
and samples it as 1133x120; hidden UI (left stick) left it untouched for 4 frames, the cold sweep
dropped it, and on unhide the text was junk (FRITCHII_STALE_ZERO=0) or blank (=1).
Used again before the landing (bound, sampled, drawn), the image simply stays. With no room in the
queue (64 entries, 256 MiB) the image is kept for a later pass. Deleted as before: images the CPU
wrote since (nothing GPU-only in them), allocation failures, a newer write-back over some of its
bytes (recent ring of 256), or an overlap downloaded since its queueing (refused). Relocation
(EXP43) is not written back (the image moves, ~10 a frame while the camera moves); instead a loss
the counter blames on a relocation guards that shape and address from relocating again.
1 = every image; N > 1 = only images up to N KiB (a cost cap for A/B: 1024 covers the text
targets); 0 = off (drops as before). Cost: one GPU copy plus a CPU swizzle per written-back image,
on the GPU thread at the landing; the "Lost contents" line gives MiB per path.

### FRITCHII_LOST_KEEP

On by default (helper EK, 2026-11-18; a tester's one-frame black flash while swapping items fast,
RX 7900 XT, up to 26 "SAMPLED with lost contents" per 10 s). Live. 0 = the old behavior. Three
parts, all about GPU-written images dropped without their data reaching guest memory:
1. Refused write-backs no longer blank newer bytes. OoT draws R8 HUD masks of several sizes at one
address; the newest one's write-back landed first, the older one's was refused (it would land
older bytes) and its drop then marked every page stale, the newer write-back's too. The next image
there skipped its upload and was cleared to zero (the counter blamed "cold sweep, 448x97, 3 frames
ago" for a 128x126). MarkStale now leaves pages a newer write-back (ring of 256) holds unmarked.
2. An image the CPU wrote whole since its last GPU write is not marked stale and not written back:
its GPU data was void already (its next use would have uploaded the guest bytes), so marking it
skipped the CPU's bytes and a write-back would have landed older bytes over them.
3. Memory the game read after a loss is learned (ring of 128 ranges, the sampled image's range), and
a later drop of a GPU-written image over it is written back whatever its size (over
FRITCHII_LOST_WRITEBACK's 1 MiB cap: OoT's 960x540 post chain, the exposure buffers). Images read
in a later frame than their write (the relocation guard bits) get the same, from half the budget.
Budget: N MiB/s of guest bytes, at most a second's worth saved (1 = 32 MiB/s, ~1/20 of what
FRITCHII_LOST_WRITEBACK=1 wrote back in the market, where it cost 3 fps); past it they are dropped
as before (counted "over budget"), so device memory stays where it was on 8 GB cards. Cost: the
learned/guarded write-backs (one GPU copy + one CPU swizzle each at the landing, GPU thread), a
ring scan per GPU-written image drop.

Lost contents counter (always on, needs the stale page marks, i.e. EXP78 on, the default): every
10 s with anything to say, "Lost contents in 10 s (FRITCHII_STALE_ZERO=Z, FRITCHII_LOST_WRITEBACK=W):
images made over memory whose GPU data was dropped: S upload skipped (C cleared to zero), U uploaded
stale guest bytes; O overwritten whole first; SAMPLED with lost contents: N (A not drawn into, B
drawn over; K KiB; G were read in a later frame before the drop) by drop: collector, cold sweep,
superseded sweep, slot retire, join, relocation, unmap/unmap sweep, other, not recorded; relocation
guards R; write-backs queued/landed: collector Q/L (M MiB) refused F kept K over cap C; cold sweep
...; superseded sweep ...; slot retire ...". SAMPLED is the number that shows on screen: an image
sampled before anything overwrote it whole. The first 40 get a "Lost contents sampled:" line with
the image (format, size, address) and the drop that left its memory stale (path, the dropped
image's format, size and address, frames since, its guard bits, why no write-back saved it); after
those, 2 per 10 s window. FRITCHII_LOST_KEEP adds to each path "(written back all the same: learned
L, guarded G; over budget B)" and at the end "FRITCHII_LOST_KEEP=K: sampled ones' drops without a
write-back because: no write-back on that path, over cap, refused, not downloadable, off, over
budget, refused at landing; learned ranges, pages kept from stale under newer write-backs,
CPU-written images not marked stale (not written back)". With it on, SAMPLED "not drawn into" and
the over cap / refused / over budget reasons should stay near 0; what is left on "no write-back on
that path" (joins, unmaps, relocation) is the next lead.

### FRITCHII_SIBLING_CENSUS, FRITCHII_GPU_SIBLINGS_FREE

TEST, off by default (helper DE, 2026-11-17). The pinned GPU threads own a whole physical core each
(front 0xc, back 0x30 on a 7800X3D) and use one of its two logical processors. The other one is in
no thread's mask, so it is often the only idle logical processor, and unpinned busy threads
(VulkanWorker ~400 ms/s at Critical priority, GPUFencingThread, VulkanPresent) land on it and share
the GPU thread's core.

- FRITCHII_SIBLING_CENSUS=1 (boot, a dry counter, changes nothing): the Pulse line (once a second)
  ends with "; GPU cores (FRITCHII_SIBLING_CENSUS): front core 0xc busy B, GPU T, others O ms/s; back
  core 0x30 busy B, GPUBack T, others O ms/s; R core ... ; VulkanWorker N/s on front/back/R cores
  a/b/c%; GPUFencingThread ...; VulkanPresent ...; FRITCHII_GPU_SIBLINGS_FREE=V". busy = both logical
  processors of the core (NtQuerySystemInformation, DPCs and interrupts included), T = the pinned
  thread's own CPU time, others = what else ran on the core. The noted threads count where they woke
  up for each unit of work (a worker chunk, a fence batch, a present).
- FRITCHII_GPU_SIBLINGS_FREE (live, default 0): 1 = VulkanWorker, GPUFencingThread and VulkanPresent
  keep off the pinned GPU threads' cores (BackgroundWorkCpuMask, as HostTiming's spin and the
  pipeline workers already do); back to 0 = every CPU the process may use. Applied by each thread at
  its next unit of work. Judge it on the back's and front's ms/s and us a draw (2 cycles), then fps.
  2 (round 2) = 1, and once a second the Pulse thread also moves every other thread of the process
  that nobody placed (still on the whole process mask: driver threads, audio, VSync, image frees,
  the Pulse thread...) to the same mask; the pinned GPU threads, anything with a narrower mask
  (guest cores, E2) and the threads that place themselves (HostTiming, VkPipelinePrebuild,
  VkPrecompile, the three above) are left alone. Back to 0 or 1 gives the moved threads their old
  mask (unless something changed it since). Needs the Pulse thread (on by default). 1 and 2 are
  refused (one log line, nothing changes) when the mask would leave fewer than 8 fast logical
  processors (MinAutoGuestProcessors + 2): a 7800X3D keeps 11 (on), a 6-core with SMT 7 (off), 8
  cores without SMT 5 (off).
- Round 2 census: each core also shows "(DPC+ISR D)" (that core's DPC and interrupt time, part of
  others) and ", own threads allowed there T ms/s in N: name a, name b, name c": the process's
  busiest threads whose mask lets them onto the core's free hardware thread (named by their
  description, else "tid N module.dll" from their start routine). Windows does not say where a
  thread ran without a kernel trace, so these are the candidates, not a split. The pinned threads
  are read by thread id, the latest thread pinned to a role (round 1 read the first: R showed 0).

### FRITCHII_CB_MERGE

TEST, off by default (helper DE, 2026-11-17). Live, 1 = dry count (changes nothing). The game
writes constant buffer data as an offset write plus 1-4 words, so FRITCHII_CB_WORDS_BLOCK's blocks
hold ~1.9 words: ~1M blocks a second, each run by E on its own. When a block opens, the front looks
at the ring words since the previous block (at most 32): joinable when they are only constant buffer
offset (or address/size) writes. E times 1 block in 16 by size and the offset-only Pairs entries.
Line every ~10 s after the CB words block line: "CB merge (FRITCHII_CB_MERGE=1, dry) S s: blocks
opened after a block N/s: joinable J/s (offset only, with address/size; W writes between), nothing
between, blocked by other entries; E: blocks of 1/2/3-4/5+ words a/b/c/d ns, offset-only pair
entries P/s at G ns; fixed part a block F ns; E saving if joined L to H ms/s". F = 2a - b (the
per-block cost a merged entry would pay once), L = J x F, H = J x (F + G). Gate 1 needs L >= 5 ms/s.

Round 3 (the lever itself): 2 = verify, 3 = on. A constant buffer size/address/offset register
write that would go as a Pairs entry right after an open CbWords block (the block still the ring's
last entry, same engine, no CB_WORDS_BLOCK=2) goes into the block instead, as a Sink-tagged Method
word; the block's header gets CbJoined. E runs a joined block as runs of constant buffer words
(exactly as ExecuteCbWords runs them) and each joined write as its Pairs entry ran (ConsumeSink,
ApplyPairs), in ring order. Same words, same values, same order, same registers: only the entry
boundaries move. 2 also puts a marker and the front's address of each joined run's first word in
the block; E compares it with where that word goes on its registers (const_buffer address +
offset after the joined writes): "differ" must stay 0, as must "bad words" (anything else in a
joined block). Not joined (sent as before) while the memory stage's ring or the E split runs:
their readers take every block word as a constant buffer word. At 3 the dry counters stay off.
Line every ~10 s: "CB merge (FRITCHII_CB_MERGE=N) S s: front joined writes J/s, refused R/s, run
marks M/s (not marked K/s); E: joined blocks B/s, joined writes W/s; session: runs checked C, must
be 0: differ D, bad words X". Judge on E's ms/s ("method" and "cb upload") and us a draw.

### FRITCHII_QUERY_FLUSH_DRY

TEST, off by default (helper DE, 2026-11-17). Live, a dry timer that changes nothing. The front's
report DMA flush (QueryDma::FlushPending in a split Run, the 0xe2c path) lends
RasterizerVulkan::FlushQueryResults to R: E parks until R reaches it and asks its query cache
(SemiFlushRegion). kaksample2 (unprofiled CPU sample, Kakariko): ~30 ms/s of E parked there,
while R finds reports to wait for under once a second; the profile's lends line misses these.
Per lend: E's parked time, whether R found reports in the range, and whether E's own
report-landing pages (POST_FENCE, OneFollower::ReportLandingClear) call the range clear. Line every
10 s: "Query flush lends (FRITCHII_QUERY_FLUSH_DRY=1) S s: N/s, E parked T ms/s (U us each); R found
reports in F/s; E's landing pages clear for C/s (ms/s), R found reports in X/s of them (must be 0
to skip on it; total since boot); ceiling (clear, nothing found) Y ms/s". Gate 1: Y >= 5 and X = 0.

### FRITCHII_QUERY_LEND_DRY

TEST, off by default (helper DE, 2026-11-17, round 5). Live, a dry timer that changes nothing.
kaksample2 (unprofiled CPU sample, Kakariko): E parked ~23 ms/s in Query lends. Per lent Query on
E: why it was lent (which rule kept it from the posted paths: fence payload with SEM_E's path off
or not clean, fence counter report with POST_FENCE_REPORT off, of a type that lands at once, or
not clean, plain payload release, plain report, report on a registered buffer page), E's parked
time, and whether the report's page is one the front's DMA reads snapshot (FrontReadPagesAny; a
posted report there is read stale by the front unless the front also waits for R). Line every 10
s: "Query lends (FRITCHII_QUERY_LEND_DRY=1) S s: N/s, E parked T ms/s by why lent; <why> n/s t ms/s
(front read page f/s); ...". Gate 1 per kind: t >= 5 ms/s; f must be 0 for that kind to be posted
without a front wait.

Round 5 also adds to EDEN_NX2_PROTECT_STATS=1 (boot) a line "Page protection on GPUBack (E, 5 s,
per second): protect N calls T ms, unprotect ...; protects of a page E unprotected 0-1 frames
before R calls T ms, with their unprotects ~P ms": E's share of the protect/unprotect cycle and
EXP89=1's (lingering protection, live) ceiling on E. EXP89 changes nothing the guest reads (page
contents are never touched; a guest store to a lingering page traps and goes through the
handler, then the page is released at the frame's sweep): its cost is in the Store memo line's
trapped stores, and "left after unmap" must stay 0.

### FRITCHII_EXTENT_CENSUS

TEST, off by default (helper DK, 2026-11-17; design: helper DJ, flower read-back dip). Live, log
only. A native compute program's storage pointer has no size, so a dispatch whose store extent is
unknown marks the whole FRITCHII_BARE_SSBO_MIB window (16 MiB) GPU-written. The store analysis
(StoreExtentAnalysis) now keeps its first exit per buffer (Shader::ExtentUnknownWhy, not stored
with the info: programs from the precompiled cache have "not recorded", so boot with
EDEN_NX2_PRECOMPILED_FAST=0 for the reasons). Set at boot it also turns the read ring on
(FRITCHII_READ_RUNS=1's) to rank by read-back waits; live it only ranks by bytes. Every 10 s:
"Extent census (FRITCHII_EXTENT_CENSUS) last S s: N native written bindings a second, F of them
marked in full (M MiB/s) over B program bindings; extent unknown U/s, X/s of those with no reason
recorded (must be 0 with EDEN_NX2_PRECOMPILED_FAST=0) | read-back waits W ms a frame over K
writers", then the top 8 writers by read-back wait ms a frame ("Extent census read-back i/n: ...",
any writer: a bare native binding gets the full line below, others a short line naming their kind),
then the top 8 others by bytes marked in full ("Extent census marked i/n: ..."). Full line:
"program <hash> binding <ssbo> (pointer cX[0xY]) | read-back R reads, W ms a frame | D
dispatches/s, F full-marked/s, M MiB marked/s (K KiB binding) | why unknown: <reason> | full mark
because: unknown, indirect grid, 16-bit guard or dims, distrusted, checking new dims, proof covers
the binding | cbuf extent: <terms or none> | shared extent: <FRITCHII_EXTENT_SHARED's proof, not
tried or refused: reason> | guard extent: <FRITCHII_EXTENT_GUARD's proof, not tried or refused:
reason>". Reasons: "index from a constant buffer word cI[0xO]" (which word),
"index loaded from memory (<opcode>)", "loop-carried index", "index from an atomic", "signed or
64-bit math (<opcode>)", "variable times variable", "instruction not bounded (<opcode>)", "a store
stays global", "pointer picked at run time", and the analysis' other exits by name. "cbuf extent"
is what FRITCHII_EXTENT_CBUF would use (each word with its stride). Cost off: two atomic loads per
native written binding.

### FRITCHII_EXTENT_CBUF

TEST, off by default (helper DK, 2026-11-17; design: helper_dj_answer.md fix 1). Live for the
marks; =3's read counts need it at boot (the read ring turns on at its first use, as
FRITCHII_READ_RUNS=1 does, and logs its own lines too). For written native buffers whose store
extent is unknown, a second store analysis (terms mode, translation only, plain extents unchanged)
takes constant buffer words read at an immediate offset as terms: offset <= ids bound + sum(stride
x word). At bind the words are read from game memory like the pointer word; a word on a GPU-written
page is "fallback" (full mark, as today); a 16-bit guard past 2^16 or an indirect grid is
"unresolved". Not stored with the info: boot with EDEN_NX2_PRECOMPILED_FAST=0.
- =3 dry: marks stay full; each write mark carries where the proof would end it. Reads that go to
  the back are sorted by their newest mark: inside the proof, would be freed (past it and no older
  mark in the ring covers the bytes, marks with a proof cut at their proof), or past it but under an
  older mark (same writer / another). Must be 0: "freed reads whose bytes changed at landing" (the
  read's own bytes, before the wait against after it: a change means the GPU wrote there and the
  proof would have served stale bytes).
- =1 live: the proof becomes the mark through the ExtentTrust checks (each program marked in full
  and checked at new dims, distrusted for the session on a write past it: "Compute write extent too
  small" in the log, "distrusted" in the census). Must be 0 there, then the flash watch and the
  white-leaves spot.
Line every 10 s per top program binding: "Extent cbuf (FRITCHII_EXTENT_CBUF=M) i/n last S s:
program <hash> binding <ssbo> | cbuf extent <terms> | dispatches/s: proof P, fallback F,
unresolved U, marked by the proof (=1) L | proven K KiB of B KiB on average | reads to the back by
its marks (=3): inside the proof I (ms a frame), would be freed R (ms a frame), past the proof but
under an older mark of the same writer / another writer | must be 0: freed reads whose bytes
changed at landing X of Y compared (inside: ...)". The freed ms a frame is the ceiling of =1 for
that program. With no binding to try: "Extent cbuf (...) last S s: no native written binding had a
cbuf extent; extent unknown U dispatches/s, X/s of them with no reason recorded".

### FRITCHII_EXTENT_SHARED

TEST, off by default (helper DO, 2026-11-17; design: helper_dn_answer.md section 3, the flower
writer 989e109982cb409b). Live for the marks; =3's read counts need it at boot (as
FRITCHII_EXTENT_CBUF=3). Some programs park an offset they computed from their ids (a record
offset) in their own shared memory slot and load it back before storing through it; the store
analysis saw "index loaded from memory (LoadSharedU32)" and the dispatch marked the whole 16 MiB
window. For written native buffers whose plain and cbuf extents both stayed unknown, a third store
analysis (translation only, plain and cbuf extents unchanged) follows such a load to the values
stored there, when (SharedSlots, global_memory_to_storage_buffer_pass.cpp):
- every shared write and atomic of the program has an exact address c . tid + k (no 32-bit wrap
  for any invocation), all with the same non-negative coefficients (multiples of 4), and the slots
  never overlap: widened to whole words, with `span` from the lowest to the highest byte, the
  dimensions of more than one invocation sorted by coefficient each step past what the smaller ones
  reach (c1 >= span, c2 >= c1 (w1 - 1) + span, ...) and the last slot ends inside the program's
  shared memory. Else "shared memory not one slot per invocation";
- the load (32-bit) has the same coefficients, and only 32-bit stores of exactly that word write
  it (else "loaded shared word also written another way": another width or an atomic), one of
  them before the load on every path (else "loaded shared word not stored before the load").
Then the loaded value is the newest of those stores' values on the invocation's own path (no other
invocation writes the slot, an invocation sees its own writes in order), so it is bounded by the
largest of them. The result is a cbuf extent flagged via_shared: this switch, not
FRITCHII_EXTENT_CBUF, decides whether it is used. Not stored with the info: boot with
EDEN_NX2_PRECOMPILED_FAST=0. FRITCHII_STORE_FOOTPRINT's footprints follow the same loads (always:
the footprint is only used under that switch).
- =3 dry: as FRITCHII_EXTENT_CBUF=3 (marks stay full, the read ring counts the reads the proof
  would free). Must be 0: "freed reads whose bytes changed at landing".
- =1 live: the proof becomes the mark through the ExtentTrust checks (new dims marked in full and
  checked, a write past the proof distrusts the program: "Compute write extent too small" in the
  log, "distrusted" in the census). Must be 0 there; then the flash watch and the flower spot.
Lines every 10 s per top program binding: "Extent shared (FRITCHII_EXTENT_SHARED=M) i/n last S s:
program <hash> binding <ssbo> | shared extent ids only (end E + local X/Y/Z + group X/Y/Z) | ..."
(the rest as the "Extent cbuf" line). With none to try: "Extent shared (...) last S s: no native
written binding had an extent proven through shared memory slots; refused R dispatches/s (the
census' shared extent says why); ...". The census lines end in "| shared extent: <proof>", "not
tried" or "refused: <reason>". 989e's expected proof: end 0x138 + local 0x140 + group 0x5000, so
0x5000 x grid_x - 8 bytes past the pointer (20 KiB a workgroup) instead of 16 MiB.
Test block (nx2_switches.txt): EDEN_NX2_PRECOMPILED_FAST=0, FRITCHII_EXTENT_CENSUS=1,
FRITCHII_EXTENT_SHARED=3 at boot (flower save, a few cuts), then live 0/1 with profile_toggle.
=3 at boot also turns on the write marks' ticks (FRITCHII_READ_RUNS=2's, counter only: a read's
host wait is split at older ticks, the total stays). For the reads past a proof but under an older
mark, each such "Extent shared"/"Extent cbuf" line is followed by "Extent ... i/n with the proof:
its reads would wait X ms a frame instead of Y (freed reads as 0, covered reads until their cover
ran: timed T, untimed U kept as waited; the timed ones A instead of B) | covered by N writers:
<kind> <program> binding <b>: R reads, W ms a frame, timed T would wait A ms a frame instead of B
(why unknown: <census reason>, F full-marked/s, pointer cI[0xO])" for the top 4 covers. The cover
is the newest mark still over the read's bytes once marks with a proof are cut at their proof;
"would wait" ends the read's host wait when that mark's submission ran (an older tick of the same
queue, timed on the reading thread before its own wait). With FRITCHII_EXTENT_CBUF=3 alone the
covers are named but untimed unless FRITCHII_READ_RUNS=2 is on at boot.

### FRITCHII_EXTENT_GUARD

TEST, off by default (helper DT, 2026-11-17; design: helper_do_answer.md round 2, the 8 covering
writers of the flower reads). Live for the marks; =3's read counts need it at boot (as
FRITCHII_EXTENT_SHARED=3, write ticks included). GPU append lists (ballot, popc, the leader's
atomic on a counter, a shuffle broadcasting the old count, index = old + the lane's place) store
behind a capacity check (`if (index < cap) store [list + index * 4]`, or an early exit), so the
index has no bound of its own ("index from an atomic", "instruction not bounded (ShuffleIndex)")
and the dispatch marked the whole 16 MiB window. For written native buffers whose plain, cbuf and
shared extents all stayed unknown, a fourth store analysis (terms mode, translation only, the
other extents unchanged; GuardFacts in global_memory_to_storage_buffer_pass.cpp) adds:
- path facts: the structured control flow's conditional edges (If, Break, Repeat, each checked
  against its header's two successors). An edge H -> T holds for a store when T dominates the
  store's block and every other predecessor of T is dominated by T, so every path to the store
  entered T through H -> T and the condition held there. `x < K`, `x <= K`, `K > x`, `!(x >= K)`,
  `x == K` (unsigned) bound x by K - 1 (or K) when x is defined outside every loop (one value per
  invocation, the one compared) and K is bounded (an immediate, a constant buffer word read at
  bind time, ...). `x - d < K` bounds x by K - 1 + d (a wrap makes the compare fail). Refused:
  signed compares ("signed guard on a possibly negative index"), `x + c < K` with c > 0 or a
  variable ("guard on the index plus a value that may wrap"), a limit loaded from memory ("guard
  limit not bounded (LoadGlobal32)"), x defined in a loop ("guarded index redefined in a loop"),
  a compare whose edge does not cover the store ("a branch compares the index but the store is
  reachable around it");
- phi pruning: a phi's incoming edge that needs an atom (a compare, ...) to have the other value
  than one of the store's path facts is dropped when that atom has one value per invocation
  (predicated `@!P mov` before `@!P st`: the old register value never reaches the store);
- a subgroup shuffle is bounded by its operand's bound at the largest local ids (operand evaluated
  without the path facts: the source lane may not run the store; a shuffle from an inactive lane
  is undefined, as in the guest program); a bit count is at most 32;
- clamps: word - v (also -v + word) is at most the constant buffer word while the word is at least
  v's largest value (a floor, checked at bind: below it the extent is unknown), as in
  min(atomic, capacity - count).
The result is a cbuf extent flagged via_guard: this switch, not FRITCHII_EXTENT_CBUF, decides
whether it is used. Not stored with the info: boot with EDEN_NX2_PRECOMPILED_FAST=0.
- =3 dry, =1 live: as FRITCHII_EXTENT_SHARED (ExtentTrust checks for =1, must be 0: "Compute write
  extent too small", "distrusted" in the census, "freed reads whose bytes changed at landing").
Lines every 10 s per top program binding: "Extent guard (FRITCHII_EXTENT_GUARD=M) i/n last S s:
program <hash> binding <ssbo> | guard extent end E + local X/Y/Z + group X/Y/Z + words: <terms>
(needs cI[0xO] >= min) | ..." (the rest as the "Extent cbuf" line, then the "with the proof" line).
With none to try: "Extent guard (...) last S s: no native written binding had an extent proven
with its stores' path facts; refused R dispatches/s (the census' guard extent says why); ...".
The census lines end in "| guard extent: <proof>", "not tried" or "refused: <reason>".
Test block (nx2_switches.txt): EDEN_NX2_PRECOMPILED_FAST=0, FRITCHII_EXTENT_CENSUS=1,
FRITCHII_EXTENT_SHARED=3, FRITCHII_EXTENT_GUARD=3 at boot (flower save, a few cuts).

### FRITCHII_STORE_FOOTPRINT

TEST, off by default (helper DM, 2026-11-17; design: helper_dl_answer.md item 1; live modes:
helper_dm_answer.md round 2). Arm at boot, then live:
- Arm: any non-zero value at boot (read at the first native written dispatch). Armed, the buffer
  cache keeps a shadow of gpu_modified_ranges (store_footprint_live.h FootprintShadow, told about
  every Add, Subtract and Clear of the set): `unclaimed` = bytes some mark without a footprint made
  GPU-modified since they were last clean; one entry per footprint (program binding + pointer
  phase) = bytes its marks made GPU-modified. Only 3 at boot turns the read ring on (the dry lines
  below, and FRITCHII_READ_RUNS=1's); 1 or 2 at boot arm without the ring. Not armed: nothing
  changes, whatever the live value.
- Live (debug-server switch FRITCHII_STORE_FOOTPRINT, read at each guest read; starts at the boot
  value): 0 = as before; 1 = a guest read whose GPU-modified bytes are all held only by entries
  whose footprints exclude them (none unclaimed) is answered from guest memory at once: no GPU
  write since those bytes were last clean touches them. Nothing else changes (sets, downloads,
  fences as before the read); the read's pages get their read traps back (the core took them for
  this read) so a later read of a GPU-written byte there still traps; the core may keep only the
  read's own bytes as its read area. 2 = as 1, plus every 16th such read waits as before and
  compares its bytes before the wait with after the landing (must be 0: "would have differed"),
  plus every locked download landing compares what it writes over claimed-outside bytes with guest
  memory (first 256 KiB of each landing; must be 0). 3 = dry count only (needs 3 at boot).
- A/B in one session: boot 1 (or 2), then live 1 <-> 0 (no ring on either side); or boot 3 and
  live 1 <-> 3 (ring on both sides, the dry lines too).
- Footprints with a modulus that isn't a power of two (0x110, 0x120 records) are used as they are
  only when every store offset comes from the ids alone and the plain store extent at the
  dispatch's dims keeps pointer offset + extent within 4 GiB (no 32-bit wrap); otherwise the bind
  uses the footprint modulo the modulus' power-of-two part (counted "reduced").
- Line every 10 s while armed: "Store footprint live (FRITCHII_STORE_FOOTPRINT=M, armed) last S s:
  reads asked A | answered from guest memory N (n a frame) | waited as before: inside a footprint,
  a byte no footprint claims, own bytes not GPU-modified | read trap pages armed again | binds:
  footprint, reduced to its power-of-two part (wrap not ruled out), none | shadow entries | =2
  verify: sampled, compared, would have differed (must be 0) | =2 landings: checked (KiB), would
  overwrite guest bytes outside a footprint (bytes, must be 0)". Errors for each must-be-0 hit (16
  at most each).
Dry part (3; waiting and marking unchanged; the marks carry the same footprint the live part
uses). At translation
every written storage buffer gets a store footprint (Shader::StorageStoreFootprint, kept with the
extras: not stored with the info, so boot with EDEN_NX2_PRECOMPILED_FAST=0): each store's offset
from the pointer word is followed as an exact congruence c + k*m (ExactCongruence in
global_memory_to_storage_buffer_pass.cpp). Adds, subtractions, products and shifts by constants keep
it; an unknown value (loaded from memory, a cbuf word, a loop counter, an id) is "anything", and
times a stride K it is a multiple of K; and/or/xor/shift right/bit fields keep the known low bits;
select, phi and min/max join both sides. The modulus is the gcd over the buffer's stores, the
footprint the set of byte residues they hit (up to 8 intervals, gaps filled past that). A modulus
that is not a power of two (0x330 records) holds only while the 32-bit offset doesn't wrap: the
landing check below catches it. No footprint when a store stays global, the pointer is picked at
run time, the program stores to global memory, or a store's address doesn't add the pointer once.
At bind, every native written binding (one device range) carries its footprint and the pointer's
device address into its write marks. A read that goes to the back is classed by the newest mark
over its bytes: inside its footprint; outside it with no older mark in the ring covering the bytes
(would be answered from guest memory); outside it but under an older mark (inside that one's
footprint / without one); or a newest mark without a footprint. Must be 0: outside reads whose own
bytes (up to 64) changed between before the wait and the landing. Lines every 10 s: "Store
footprint (FRITCHII_STORE_FOOTPRINT=3, dry) last S s: R reads to the back with a write mark over
their bytes, waited W ms a frame | outside ... (would be answered from guest memory) O (ms a frame)
| inside I | outside, but an older mark covers them: inside its footprint / without a footprint |
newest mark without a footprint N | must be 0: outside reads whose bytes changed at landing X of Y
compared (inside: ...) | program bindings bound B: with a footprint F (claim nothing, power-of-two
moduli, ids only), without: <reasons>", then the top 8 writers by wait ms: "Store footprint i/n:
<writer kind> program <hash> binding <b> | waited W ms a frame | outside, inside, covered (inside
its footprint / without one), no footprint (each with ms a frame) | must be 0: ... | read offsets
mod the modulus: +0x10 (n), ... | footprint: modulus 0x100, stored bytes +0x20..+0x2f ... | S
stores, ids only / with loaded/cbuf/loop multiples". The outside ms a frame is the ceiling of a live
mode for that writer. Cost off (not armed): one atomic load per native written binding, a bool test
per guest read of GPU-modified pages and per marked binding, a null test per change of
gpu_modified_ranges.

### FRITCHII_FEEDER_LANE

TEST, off by default (helper DM, 2026-11-17; design: helper_dl_answer.md item 2). Boot (read at
first use). =1 dry; 2-9 are not built and count as 1. Turns FRITCHII_EARLY_COMPUTE=1's dry count,
its rings and FRITCHII_READ_RUNS=2's write stamps on (their lines show up too). Would a feeder
dispatch (a program whose writes the guest reads back) on a second lane, started after its real
dependencies, finish sooner than on the main queue? At each feeder's bind JudgeEarlyCompute also
works out D, the newest unrun main-queue work it depends on: the newest unrun write mark over its
inputs (uniform and texture buffers whole; a native storage binding up to its proven read extent,
Shader::Info::storage_read_extents, plus its write mark when written; the whole binding when the
read extent is unknown), the newest unrun use of the ranges it writes (write after read), and its
own chunk when its bind uploads CPU data (D_copy leaves the upload out: the lane would take the
copy too). A mark or use of an earlier judged feeder gives that feeder's D, not its tick (chains
stay on the lane in order). If either ring wrapped before a run entry, D is its own chunk ("ring
wrapped", not eligible). Images are not in D (a compute-family lane can't take them; counted per
program). The marks carry D; a read whose pages' newest mark is such a feeder's waits for D_copy,
then D, then the writer (older ticks of the same queue: the total wait is unchanged). "Predicted
saving" = from D having run until the writer ran, for reads whose D tick is before the writer's
(eligible). Lines every 10 s: "Feeder lane (FRITCHII_FEEDER_LANE=1, dry) last S s: P feeder
programs, J dispatches judged (... uploaded; storage bindings narrowed to a proven read extent k of
n) | D at the bind: nothing unrun, draw-written buffer, earlier dispatch on the main queue, earlier
feeder on the lane, copy/query/other, unrun use of its written range, its own upload, ring wrapped
| reads that waited for them R (W ms a frame), cut at the writer's tick C | eligible E = f% of the
cut reads | predicted saving X ms a frame (x a read), with the upload on the lane too Y ms a frame |
D not submitted when waited U | must be 0: D after the writer Z", then the top 6 feeders by wait:
"Feeder lane i/n: program <hash> | reads waited, cut | eligible share f, predicted saving (and with
the upload on the lane) | D set by (cut reads): <kinds as above> | D k submissions before the
writer | binds (uploaded, read extents) | images (not in D): configured, with images, an image's
last write unrun | D not submitted, must be 0: D after the writer". Uses noted while the bind
uploads keep D without the upload (a small under-count of chains that upload). Cost off: one atomic
load per native storage binding.

### FRITCHII_HOSTTIMING_OFF_GPU

TEST, off by default (helper DE, 2026-11-17). Live. 0 (as before): the timer thread's spin goes to
BackgroundWorkCpuMask at the first spin after any GPU thread pinned, and never again; the GPU
threads pin one after another at boot, so a spin between the front's pin and the back's leaves it
allowed on the back's core (b37 census: HostTiming 295-496 ms/s allowed on E's core 0x30). 1: every
logical processor except the physical cores (both hardware threads) of the pinned front, back and
R, placed again whenever another GPU thread pins; refused (one log line) when that would leave
fewer than MinAutoGuestProcessors + 2 fast logical processors (7800X3D: 10 left, on; 6 cores with
SMT, 8 cores without: off). Back to 0 restores 0's placement. Each placement logs "HostTiming placed
(why): mask M, GPU threads' cores G, running on processor P" (0's first placement too).

### FRITCHII_FRAME_END_TIMES

TEST, off by default (helper DE, 2026-11-17). Boot, a dry timer that changes nothing. Where
RasterizerVulkan::TickFrame's time goes (with one follower it runs on R in a lend while E is parked:
"TickFrame (frame end)" in the lends line, 5 ms/s at Kakariko, 7-31 ms/s at the market). Line every
10 s: "Frame end times (FRITCHII_FRAME_END_TIMES=1) 10 s: N calls/s (P% on GPURender), T ms/s, U us
each, longest L us; by part ms/s: head, pipeline cache, descriptor queues, fence manager, staging,
texture cache, memory seam, buffer cache (E's)". Everything but the buffer cache part is what a
posted frame end could take off E's parked time.

### FRITCHII_HOSTTIMING_SPIN_BUDGET

TEST, off by default (helper AS, 2026-11-15; idea from zortos293/Lichii #1733). Live, a percent of
the timer thread's time; 0 = no limit (as before). EXP75 (600 us by default) makes the HostTiming
thread spin the last 600 us before a guest timer. OoT's present poll sleeps 250 us at a time while it
waits for a frame, so during a long wait (an area load) a timer is always that close and the thread
spins a whole core (2.3 s of CPU in a 2.5 s Castle Town load freeze, 70-100% of every load hitch).
With the switch, spins are paid from a budget that grows by N% of the time that passes and holds at
most 2 ms; with less left than EXP75's window the thread spins only what is left and sleeps on the
OS timer before that. 20 is #1733's value. Log, every 10 s (always on): "HostTiming in 10 s: spun X
ms/s before guest timers (S spins, longest L us; EXP75=600 us, FRITCHII_HOSTTIMING_SPIN_BUDGET=N%: C
cut by it); W OS waits; E events, callbacks M ms/s, longest T us (event name), K of 1 ms or more;
most in one pass P; J ran 1 ms or more late (worst Z ms, event name)". The hitch line also gets
"timer thread spin" and "timer events over 1 ms" (callbacks of 1 ms or more), so a hitch where
HostTiming is busy says whether it spun or ran a slow callback.

### FRITCHII_HOSTTIMING_PRIORITY

TEST, off by default (helper CG, 2026-11-16). Live. 0 = the HostTiming thread at highest (as
before), 1 = time critical, the priority the guest cores and the GPU threads already run at. The
timer thread answers guest sleeps and timeouts (KHardwareTimer) and the syncpoint polls the game's
GPU waits end on (nx2:gsv:SyncptPoll, SyncptPollNow). In market logs of 2026-11-16 (b10, b8),
10 s windows where other processes took 2.5+ CPUs had 400-900 callbacks run 1 ms or more late
(worst 8-14 ms, mostly SyncptPoll) and single callbacks of 4-9 ms of wall time, against 0-150
late in quiet windows; per window, late callbacks and fps correlate at -0.4 to -0.6. Only the
thread's OS priority changes, applied by the thread itself at its next pass (one relaxed load a
pass while off); a line "HostTiming priority (FRITCHII_HOSTTIMING_PRIORITY=n): ..." says when.
The "HostTiming in 10 s" line ends with "FRITCHII_HOSTTIMING_PRIORITY=n". Judge it on that
line's "ran 1 ms or more late" and "longest" (expected: the busy windows' counts drop toward the
quiet ones'), then on fps.

### FRITCHII_ONEF_RAISE_AFTER_LEND

TEST, off by default (helper AS, 2026-11-15). Live. R raises `played` after every packet it plays
except a lent one, and its raise before a lent packet stops at that packet's floor. A lend E makes
after its last consumed position (the frame end op when E goes idle) then leaves `played` short of
what E and R have both passed, and nobody raises it until E consumes more. If the front is waiting
on R's position to send more (a map at an area load), nothing moves until the hang watchdog's 2 s
rescue: "One follower hang watchdog: a lost raise of `played` (... R with nothing unplayed): raised
now", the 2.5 s Castle Town load freeze of 2026-11-15. With 1, E raises `played` itself after every
lend's park (min of its consumed position and R's oldest unplayed packet, as every raise). The check
is counted with the switch off too. Log, every 10 s when it happened: "One follower raise after lend
(FRITCHII_ONEF_RAISE_AFTER_LEND=N) 10 s: X lends/s left `played` short of what E and R had both
passed (W ring words short on average; raised after the park / left to E's next consumed position
or the hang watchdog), T this session".

### FRITCHII_SYNCPT_MIRROR

Fix, on by default (helper AH, 2026-11-15, port of zortos293/Lichii #1512); read once when the
nx2:gsv session sets up its syncpoint page (TIPC 204), FRITCHII_SYNCPT_MIRROR=0 turns it off.
Switch 2 clients read syncpoint values from a page nx2:gsv fills; before, only the 500 us poll
(and EXP37's poll-now) wrote it. A wait armed through nx2:syncpt 201 is answered the moment the
value is reached, so the woken client could read the old value: nn::movie's TryWaitNativeSync then
aborts on purpose with 2162-0001 (seen with the 3440x1440 ultrawide mod in the guard house). With
the switch on, SyncpointManager calls a mirror under its lock with each guest increment's new value
before the value is published and before the actions it meets run; the session stores it straight
to the page's host memory, only ever forward (wrap-aware), and the poll stores forward too, so it
cannot put the page back. Cost: guest increments take the manager's lock before the add instead of
after it (it was taken right after anyway) plus one host store.

### FRITCHII_SYNCPT_REARM_NOTIFY

Fix, on by default (helper DF, 2026-11-17, port of zortos293/Lichii #2052); read once,
FRITCHII_SYNCPT_REARM_NOTIFY=0 turns it off. The nx2:gsv syncpoint poll signalled the client's
notify event only when an armed bit changed. A client that wakes, finds its threshold not reached
and rearms leaves its armed bit as it was, so its next wake-up had to come from another syncpoint's
bit changing or from its own timeout (zortos's reported one-second OoT pause). Now the poll keeps
its own snapshot of every value (the page itself is already current through
FRITCHII_SYNCPT_MIRROR, so it shows no change) and also signals when an armed syncpoint's value
moved since the previous poll. The log says "nx2:gsv rearmed syncpoint waits notified" at 1, 2, 4,
8... polls that only this answered. Cost: a 192-entry compare per poll (500 us, or EXP37's
poll-now), and a client wake-up per such poll.

### FRITCHII_SPARSE_RESERVE

Fix, on by default (helper AH, 2026-11-15, partial port of zortos293/Lichii #1413); read once,
FRITCHII_SPARSE_RESERVE=N keeps N whole 4 MiB sparse blocks ready (default 4, at most 64), 0 = off
(every block backed by the thread that writes it, as before). BackSparseWrite gives a lazily backed
region's block memory before a GPU write lands in it, usually on a GPU thread that holds a cache
lock. Allocating and mapping that memory waits for the kernel's pool and page table locks, and the
page table lock can be held by a guest thread that needs a core blocked on that same cache lock (a
three-way deadlock: Mario Kart World hung loading a race; OoT backs its six lazy 300 MB GPU-only
regions the same way). Now an nx2:GpuBacking kernel thread maps whole blocks ahead (once a lazy
region exists) and BackSparseWrite takes one under m_region_mutex without a kernel call; a stale
block goes back to the reserve. When none is ready (or the block is shorter than 4 MiB, a region's
last), the old path runs and the log says "no reserve memory ready, backed on this thread". Also:
nx2:gpu 214/215/216 no longer hold m_region_mutex across kernel calls (released regions are taken
under the lock and freed after it; 214's whole backing, EDEN_NX2_GPU_ONLY_LAZY=0, is allocated
before it). Unlike zortos, a block with no memory ready never drops or parks its write. Cost: up to
N x 4 MiB of system pool memory mapped ahead (not committed on the host until written).

### FRITCHII_WRITE_REMARK

Fix, on by default (helper AH, 2026-11-15, port of zortos293/Lichii #1829); live,
FRITCHII_WRITE_REMARK=0 turns it off. A CPU write to a page a GPU cache tracks marks the page first
(HandleRasterizerWrite: collected write or OnCPUWrite) and stores its bytes after. A cache that
takes the mark in between (the back's gather of the collected writes at FlushCaching, an upload
that tracks the pages again) reads the old bytes and treats them as current; the stored bytes then
never reach it until the page is written again (stale vertex, uniform or texture data). The window
is wide when the mark waits for a cache mutex the GPU threads hold. Now each take moves
Core::g_pages_tracked_generation (UpdatePagesCachedCount with a positive delta, and
GPUDirtyMemoryManager::Gather when it took writes); Write, WriteBlock, ZeroBlock and the exclusive
writes read it before the mark and again after the store (with a fence), and when it moved and a
page of the write is still tracked they mark the write again. The log counts these writes ("CPU
writes marked again after a cache took their marks before the store", at powers of two). Cost: a
fence and a load after each trapped store; the second mark only when a take happened in the window.

### EDEN_NX2_MEM_SAMPLE

TEST, off by default (2026-11-14): EDEN_NX2_MEM_SAMPLE=1 (read at startup) lets the MemoryProbe
thread sample which guest pages are resident every 10 s (HostMemory::SampleResident) for the
"Memory owners" line. The walk holds kernel locks for 35-48 ms and showed up inside a 217 ms hitch
in the owner's playtest, so it only runs for a memory hunt; without it the line's resident and
guest-unique figures read 0. EDEN_NX2_MEM_HEAPS=1 (the heap walk, ~250 ms) is separate.

### EDEN_NX2_STALL_SNAPSHOT

On by default since 2026-11-14; EDEN_NX2_STALL_SNAPSHOT=0 turns it off (read as each thread is
named, so nx2_switches.txt applies). A present gap of 100 ms or more logs a "Stall snapshot:"
line after its Hitch line: for the GPU back, front, Vulkan worker, fencing, present and GPUMem
threads the span each is in now and for how long, the longest span each spent in the gap, the
master semaphore's ticks, and whether the present thread is between acquire and present
(common/nx2_hitch.h).
- Cost (2026-11-14, fable_front_drains 8.4): it no longer opens the profiler's zone gate and reads
  no clock. Spans take their times from a tick the OS keeps (Windows: the kernel's interrupt time,
  0.5 ms steps with Fritchii's timer resolution, 15.6 ms at worst; Linux: the coarse monotonic
  clock), so marking a span is a few loads and stores. Before, every coarse profiler zone called
  out of line and read the wall clock twice (83 ms/s on the front at the market).
- Only waits mark the slots: the timeline's spans (fence, worker, present and host GPU waits),
  the profiler's wait zones (idle, pending sleep, split drain, writer wait, list waits, query DMA
  wait, read flush, image create, pool destroy here) and the front's per-list "push". The
  per-draw, per-dispatch, per-macro and per-fence zones no longer do: time in them reads as the
  span around them ("push" on the front) or "(no span)" (busy outside every wait, the back's
  draws and dispatches mostly). Times are good to about one tick.
- With EDEN_NX2_GPU_PROFILE or a timeline recording the zones are timed exactly as before.

### EDEN_NX2_DMA_WAIT_WRITERS

EXPERIMENT (not for nx2): EDEN_NX2_DMA_WAIT_WRITERS, read at startup and settable live by the
debug server's `switch` method, so one session can A/B it.

### EDEN_NX2_SEM_FAST

EXPERIMENT, off by default: Ocarina of Time flickers white on the bottom half of Link's house
ladder with it (confirmed toggling it live, 2026-11-06), for +1.4 fps. EDEN_NX2_SEM_FAST=1,
semaphore acquires without draining the back
when the value is already met and no queued write overlaps it (puller.cpp).

### EDEN_NX2_LIST_VERIFY

TEST: EDEN_NX2_LIST_VERIFY=1 (live), each command list the split's front reads on GPU-written
pages is read again as before EXP40 and compared (video_core/nx2_list_verify.h; "list
verify" every 10 s). Changes timing: the front waits for every writer again for each list.

### EDEN_NX2_LOOP_SKIP

EXPERIMENT (not for nx2): EDEN_NX2_LOOP_SKIP=1, the split's front does not send the instance
loop writes the back would ignore (draw.end in an instance loop, unchanged vertex and index
buffer firsts and counts) (maxwell_3d.cpp).

### EDEN_NX2_DMA_SNAPSHOT

EDEN_NX2_DMA_SNAPSHOT (from a ported dma-snapshot fix): macro DMA reads of pages a drained
read flushed whole skip the drain while nothing sent since may write them (Maxwell3D::
FrontDmaRead). 9 = on (default: Lake Hylia 46.5 -> 52.3 fps; verify 0 differences in 235k
reads), 10 = on and verify every skipped read against the drained path, 0 = off.

### FRITCHII_DELAY_MACRO_DRAIN_US, FRITCHII_DELAY_POLL_DRAIN_US, FRITCHII_DELAY_BEFORE

Delay tests (live, default 0 = nothing changes; Coz-style causal profiling, fable_front_drains
section 3): each adds a busy spin of n microseconds on the front (steady_clock with a pause, not a
sleep) at one kind of front drain, to see whether that wait is on the frame's critical path. If
fps drops by about what the added ms a second predict, it is; if not, shortening it won't help.
- FRITCHII_DELAY_MACRO_DRAIN_US=n: a macro DMA read snapshot miss (Maxwell3D::FrontDmaRead, the
  split.Drain(MacroDmaRead) of EDEN_NX2_DMA_SNAPSHOT's miss path, the 0xe2c occlusion reads'
  "noted write" misses among them). Fable's T1: 50 us adds about 120 ms/s at the market.
- FRITCHII_DELAY_POLL_DRAIN_US=n: a suspended acquire's poll that falls through to the full drain
  (Puller::CanResumeAcquire's DrainBack, the "acquire" site). Fable's T2: 600 us adds about 111
  ms/s. The spin is outside the poll drain's own time in the "Fence writer resumes" line.
- Only drains that wait spin: the back had not run everything sent when the drain began.
- Where: right after the drain returns (the test: the back has nothing queued then, so the spin
  is added front time). FRITCHII_DELAY_BEFORE=1 moves both spins to right before the drain (the
  control: the spin overlaps the back's backlog, which keeps shrinking, so it should cost little).
  Nothing is published before a spin.
- "Front waits for the back by site" (every 5 s) ends with "delay tests added (after/before the
  drain, FRITCHII_DELAY_BEFORE=b): FRITCHII_DELAY_MACRO_DRAIN_US=n x/s y ms/s,
  FRITCHII_DELAY_POLL_DRAIN_US=n x/s y ms/s" and the three work tests below, while a delay
  switch is set or spins were counted.
- Measure with profile_toggle.py in one session (0 against n).

### FRITCHII_DELAY_E2C_US, FRITCHII_DELAY_DISPATCH_US, FRITCHII_DELAY_DRAW_US

Three more delay tests (live, default 0 = nothing changes; fable_front_drains section 6, Fable's
W, K and D), the same busy spin of n microseconds. The drain spins above can't tell "the front
paces" from "the back paces" (right after a full drain the back is empty, so any front spin there
is critical by construction); these add work to one thread in the middle of its pass:
- FRITCHII_DELAY_E2C_US=n ("W", front): every call of the 0xe2c occlusion macro (hits and
  misses, about 33,000 a second at the market), at the start of the call, before its read,
  whether the OoT handler or the JIT runs it (Maxwell3D::FrontRunMacroFeatures, where the call's
  macro slot is identified; with every macro switch off the plain body runs and there is no
  spin). n = 3 is about 100 ms/s of front work.
- FRITCHII_DELAY_DISPATCH_US=n ("K", back): every compute dispatch, at the start of
  RasterizerVulkan::DispatchCompute. n = 12 is about 96 ms/s.
- FRITCHII_DELAY_DRAW_US=n ("D", back): every 3D draw call (not per instance), at the start of
  RasterizerVulkan::Draw and DrawIndirect (list draws included; not DrawTexture blits). n = 1 is
  about 110 ms/s.
- K and D spin only on the GPU back thread (GpuSplit::OnBackThread): never on GPUMem with
  FRITCHII_MEMSTAGE_THREAD=2 (a draw GPUMem serves is still played on the back, which spins
  there), and not at all without the split. The spin sits outside the profile's "draw" and
  "compute" zones.
- Counted in the same "Front waits for the back by site" ending: "FRITCHII_DELAY_E2C_US=n x/s
  y ms/s (front), FRITCHII_DELAY_DISPATCH_US=n ... (back), FRITCHII_DELAY_DRAW_US=n ... (back)".
- Reading them: a spin on the thread that paces a pass costs about its ms/s in fps; one on a
  thread with slack there costs little.
- Off (0), a call, draw or dispatch pays one relaxed load of its switch and a branch, inline at
  the call site (GpuSplit::DelayWorkIf), no function call.

### FRITCHII_DELAY_RUN_US, FRITCHII_DELAY_RUN_KIND

The memory stage's delay test (live, default 0 = nothing changes; fable_stage_levelpoints 5.1),
with FRITCHII_MEMSTAGE_THREAD=2: GPUMem busy-spins n microseconds right after a level run's end
(its StageWaitFor of the run returned, before the return's flag take), so the back stays parked
that much longer. It tells whether GPUMem's time parked in runs is on the frame's path: if fps
drops by about what the added ms a second predict, every ms/s of parked time cut is a ms/s of
frame.
- FRITCHII_DELAY_RUN_KIND: 0 (default) spins after runs without a compute launch, 1 after runs
  with one. The launches are counted with the GPU profile on (EDEN_NX2_GPU_PROFILE) or while
  FRITCHII_DELAY_RUN_US is set (memstage_shadow.cpp's count_launch).
- Fable's reading: 5 us at kind 0 adds about 47 ms/s (9,471 runs a second); a frame 0.85 ms
  longer means parked time is fully on the path. 20 us at kind 1 adds about 21 ms/s.

### FRITCHII_DELAY_HANDOVER_US, FRITCHII_DELAY_HANDOVER_KIND

The hand-over delay test (live, default 0 = nothing changes), with FRITCHII_MEMSTAGE_THREAD=2:
the thread that was handed work busy-spins n microseconds before it starts on it, so each
hand-over arrives n us late. It checks Fable's "stage on reaches 76 if the hand-over losses
go": if fps drops by what n x the hand-over rate predicts, the hand-overs are fully on the
frame's path and cutting them pays one for one.
- KIND 1: GPUMem after a wait for the front that waited (the front -> GPUMem hand-over).
- KIND 2: the back after its wait for the stage (GPUMem -> back, BackWaitForStage).
- KIND 3: GPUMem after its other waits that waited (the back, the window, ring B room).
- Counted in the "Memory stage passes" line's delay test field (rate and ms/s).
- The spin is outside the run's own time ("runs ... us each on GPUMem"); counted at the end of
  the "Memory stage passes" line ("delay test FRITCHII_DELAY_RUN_US=n kind k: x/s y ms/s").
- Off (0): one relaxed load and a branch after each run.
- Measure with profile_toggle.py in one session (0 against n).

### FRITCHII_DELAY_BACK_US, FRITCHII_DELAY_BACK_KIND

The back's zone delay tests (live, default 0 = nothing changes; work\fable_stage_levelpoints.md
18.3, the share of a stage-off cut before it is built): the GPU back thread busy-spins n
microseconds at one kind of site, as FRITCHII_DELAY_DRAW_US does at every draw.
- KIND 1 ("M", rows 3 and 4): once every 64 ring entries of the method zone, after the entry
  ran (GpuSplit::BackLoop, after Execute). A run of constant buffer words or a FRITCHII_CB_WORDS
  block counts each word, a pairs block each pair; a multi-word method, state delta or draw
  batch one; deferred calls, engine switches and wraps none. n = 1 is about 71 ms/s.
- KIND 2 ("S", rows 1, 2, 5): every 4th BindHostStageBuffers in GraphicsPipeline::ConfigureImpl,
  before its "bind stage buffers" zone. n = 1 is about 61 ms/s.
- KIND 3 ("F", the fences of row 6): every made fence in FenceManager::SignalFence, right after
  the skip test (skipped fences don't spin). n = 5 is about 79 ms/s.
- On the back thread only (never GPUMem, not without the split). The spin is counted in the
  "Front waits for the back by site" line's ending: "FRITCHII_DELAY_BACK_US=n x/s y ms/s KIND=k
  (back)" (the spins of whatever kind is set).
- Off (0): one relaxed load and a branch at a stage bind and at a made fence; the method entries
  pay a test of a local the back loads once a batch.
- Measure with profile_toggle.py in one session (0 against n, KIND fixed in the test block).

### FRITCHII_DELAY_FRONT_US, FRITCHII_DELAY_FRONT_KIND

The front's delay tests (live, default 0 = nothing changes; work\fable_stage_levelpoints.md
18.3, rows 7 and 8 and the front's macros): the GPU front thread busy-spins n microseconds.
- KIND 1 ("A", row 7): a suspended acquire found met, as the channel resumes
  (Puller::ResumeAcquire's resume from a suspension; once per resume). n = 100 is about 60 ms/s.
- KIND 2 ("Q", row 8): a macro memory read's wait behind a queued dispatch (GpuSplit::
  DispatchesMayWriteSince's WaitFor, the "macro dma dispatch" site), right after the wait
  returned. n = 200 is about 48 ms/s.
- KIND 3 ("C"): once every 64 3D macro calls on the front (Maxwell3D::FrontRunMacro, before the
  call; with the split only). n = 2 is about 78 ms/s.
- KIND 4 ("P", the front split's FS0, work\FRONT_SPLIT_DESIGN.md 4.3): once every 16 list
  entries (DmaPusher::Step, an entry with words, before its checks; resumed entries count
  again). At about 84,000 entries a second n = 8 is about 42 ms/s.
- FRITCHII_DELAY_BEFORE=1 (the existing switch, which also moves the drain tests) moves kinds 1
  and 2 before their wait: kind 1 spins at the acquire's suspension, kind 2 right before the
  wait begins. The control: after the wait the back has reached what was waited for, so a spin
  there is on the path by construction; a spin before it overlaps the back's backlog, and if it
  costs little the wait is backlog that back cuts already shrink.
- Not on the back thread. Counted in the "Front waits for the back by site" ending:
  "FRITCHII_DELAY_FRONT_US=n x/s y ms/s KIND=k (front)".
- Off (0): one relaxed load and a branch at each site (both sides of the waits).

### FRITCHII_DELAY_WORKER_US

FRITCHII_DELAY_WORKER_US=n (live, default 0 = nothing changes; work\fable_stage_levelpoints.md
19.3 and 19.4): the Vulkan scheduler's worker busy-spins n microseconds after each chunk it ran
(Scheduler::WorkerThread, after ExecuteAll and the command buffer reallocation, still holding
the execution lock, so a Finish or WaitWorker waits for the spin as for the chunk). It prices
the worker's room before work is moved into its recorded lambdas: if fps holds while the
worker's added ms/s grows, the worker is off the frame's path and has that much room.
- Counted in the "Front waits for the back by site" ending: "FRITCHII_DELAY_WORKER_US=n x/s y
  ms/s (Vulkan worker, per chunk)".
- Off (0): one relaxed load and a branch a chunk.

### The price lines (dry counters, EDEN_NX2_GPU_PROFILE)

The dry counters of work\fable_stage_levelpoints.md 18.4 (video_core/fritchii_price.h): each adds
the time of exactly the items a cut would remove (the cut's size, X), where a delay test above
gives its share. Only with the GPU profile on (EDEN_NX2_GPU_PROFILE=1 or 2); off, each site pays
the profile's own enabled test and a branch. One line per group every 10 s, from the back thread,
only when the group counted something. Times are time stamp ticks converted with the wall clock.
- "Price uniform binds (back ...)", row 1: every compare of BindSmallUniformReusing on the back
  (the stream path's reuse test, in place or through the scratch copy), by verdict: changed (or
  no copy to compare with); unchanged and unflagged since its last bind (the cut: same range, no
  mark on its pages since); unchanged but flagged; unchanged, another range than the slot's last
  bind; unchanged with the bank marks off. The time is the guest read and the compare. "Flagged"
  means a mark of either flag FRITCHII_BANK_FLAGGED takes (Common::g_collected_write_pages,
  g_buffer_write_pages) on one of its pages since the last bind, kept as page write generations
  (Common::PriceGenSum; a hash collision or a mark over 64 pages only makes more "flagged"). The
  writers mark only while the bank marks are on: run with FRITCHII_BANK_CENSUS=1 (counters only)
  or FRITCHII_BANK_FLAGGED=1 in the test block, else every unchanged bind reads "marks off".
  fable_b_one_follower 14.4 cut 7 splits the unflagged ones (dry): "pages tracked (the cut)"
  when the buffer cache's tracker holds the range's pages clean (a guest store there traps and
  marks), "pages CPU-modified" when it does not (a guest store there raises no flag, so
  unflagged does not prove the bytes untouched: a skip would need EXP92's page watching).
- "Price handle reads (back, draws ...)", row 2: draws by their handle range since that
  pipeline's last draw: unflagged (the cut), flagged, another range (a constant buffer moved, the
  first draw, or a handle program, which reads storage too), marks off. The range is each
  descriptor's words in its constant buffers. Two sites: FillHandleValues (EXP123 3, its time
  alone) and ConfigureImpl's handle loop (EXP123 below 3, the stage-off default: the time also
  holds the storage binds and the samplers' and views' index work done in that loop, so it is an
  upper bound of what a kept-handles cut removes). Draws a TEX_BANK_GEN hit served are not
  counted (they read nothing).
- "Price constant buffer word calls (back ...)", row 3: by run length (1, 2, 3-4, 5-8, 9-16,
  17+ words), calls and words a second and the write half's time: CallConstBufferWords and
  ProcessCBMultiData from past the BackServed test to their end (the compare, the write and the
  invalidation; zone "cb upload" and its tests).
- "Price back ring entries by kind ...", row 4: the back's ring entries by kind (constant buffer
  word runs counted per word, 3D single, other engine single, multi, call, pairs per pair, state
  delta, draw batch, words blocks per word, engine/wrap) and the "method" zone ms/s each kind
  added (the zone's own estimate: 3D methods are sampled about one in 32).
- "Price handle values (E, unlent draws ...)", row 2b (fable_b_one_follower 23.1 cut 4): each
  unlent draw's handle values (OneFollowerDraw::FillRing, the fill timed) against the same
  pipeline's last unlent draw: changed or first; same, unflagged, pages tracked (the cut: same
  constant buffer ranges, no flag on their pages since, the buffer cache's tracker holds them
  clean, so a guest store would trap and flag); same, unflagged, a page CPU-modified (a guest
  store there raises no flag: not provable); same but a page flagged (page grain: another word
  on the page was written); same values from other ranges; handle program (reads storage, no
  key); and "unflagged and tracked but changed", must be 0 (a keep would have been stale).
- "Price back 3D methods ...", row 4b (fable_b_one_follower 22.3 step 2 and 23): row 4's "3D
  single" entries by method, the 24 costliest by the "method" zone time each added (the same
  sampled estimate), with the state writes each found sunk and consumed first (ConsumeSink:
  the macro's sunk register writes are paid by the next executable method, so a draw method's
  time holds them, and on E with one follower also the draw's unzoned work: the pipeline
  lookup, the handle values, the per-draw FlushCaching, the pack and the push). Methods are
  register indices (draw.end 0x585 and so on).
- "Price front waits split ...", rows 7 and 8: each wait of the front for the back by site (the
  drain sites of "Front waits for the back by site") and the suspended acquires, split into
  "until the back reached it" and "after". The back stamps each position it stores as consumed;
  the wait's end looks up the first stamp at or past its target (a drain: what was sent; a
  WaitFor: its target; an acquire: what was sent at its suspension, to its resume). "After" is the
  front's wake for a drain; for an acquire it is the host GPU, the fence thread, other channels'
  work and the wake. A stamp is stored at most every 4,096 ring words or at a waited target, so
  "until" can run late by that much; targets older than the 256 stamps count as "not in the
  stamps" (all "until").
- "Price Vulkan worker ..." (19.4): the scheduler worker's busy ms/s (ExecuteAll, which records
  the chunk's commands and, for a chunk with one, submits; plus the command buffer
  reallocation), split by chunks without and with a submit; idle = the rest of the second (the
  wait for work, FRITCHII_WORKER_SPIN's spin, the lock); FRITCHII_DELAY_WORKER_US's spins apart.
  Then, on the back: the time in Scheduler::Record calls (the recorded lambda's capture, the
  chunk append, a full chunk's hand-off to the worker; about one call in 8 timed), inside
  RasterizerVulkan::Draw or DrawIndirect and outside, against the draws' whole time (from past
  the D spin to the end). This is the recording-only part the back pays today; the code that
  only computes what a record captures (dynamic state compares, descriptor packing) is not in it
  and stays in its own zones (dynamic state, push descriptors, configure draw, draw record).

### FRITCHII_NULL_RENDER

FRITCHII_NULL_RENDER=1 (live, default 0; test only, the picture is broken: nothing is drawn;
work\fable_b_one_follower.md section 7, run 1): with the stage off, each 3D draw does only the
part the one-follower design gives the engine thread E, then returns. The fps it shows is the
design's ceiling (the front and the game threads); the back's us a draw is E's budget (a low
bound: with nothing drawn every occlusion test fails, so the game may send fewer draws).
- Kept (E): PrepareDraw's flush work, the pipeline lookup (the fill needs its layout; the design
  has E find the draw's facts too), the repack layout, and in GraphicsPipeline::ConfigureImpl the
  uniform state, each stage's storage binds, the handle words read as the seam's fill reads them
  (FillGraphicsHandleValues, not resolved), the texture buffer unbinds, UpdateGraphicsBuffers and
  the geometry binds, the descriptor reservation the buffer binds write into, each stage's
  BindHostStageBuffers and the post-copy barrier. The read-back flushes after the draw
  (TickWork) still run.
- Left out (R): the descriptor pools' sync, image views and samplers (with the EXP32 memo, EXP87
  templates and TEX_BANK_GEN), texture buffer binds (they need the views), image descriptors and
  rescaling, render targets, configure draw, dynamic state, the query segment and the record.
- Where the cut sits: with the stage off the seam is at EXP123 = 0, where the buffer fill is
  inside Configure (interleaved with the texture work), so the cut is a separate path at the top
  of ConfigureImpl in today's call order, not MemStage::FillDraw (which needs EXP123 2 or 3 and
  FRITCHII_MEMSTAGE_TICKS, a different code path than the stage-off zones the budget is from).
- Not null-rendered: draws with a held packet (EXP123 2 or 3), the stage on, delta draws (EXP118)
  and mesh draws. Dispatches, clears and DrawTexture blits run as usual.
- "Null render (FRITCHII_NULL_RENDER=1, 10 s): x draws/s null-rendered (buffer side only), y
  draws/s drawn in full" every 10 s while on.
- Off (0): one relaxed load and a branch a draw.

### FRITCHII_MEMSTAGE_RUN_SPIN_US

FRITCHII_MEMSTAGE_RUN_SPIN_US (live, default 0 = today: about 2,048 pauses, then a sleep;
work\fable_stage_levelpoints.md 14.4 row 4), with FRITCHII_MEMSTAGE_THREAD=2: GPUMem's waits for the
back in a run (the run's end) and inside the window (level inside the window) spin n
microseconds before they sleep, as the wait for the front spins 200 us. Each wake from a sleep is
latency on a path the delay test puts at 100% (14.8 kind 3). Clamped to 10,000 us. A served call
in the wait restarts the spin, as in the wait for the front.
- The link line counts GPUMem's sleeps in its waits by reason ("in its waits by reason,
  FRITCHII_MEMSTAGE_RUN_SPIN_US=n: the front, the back (level mode), the run, the window, room in
  ring B, level inside the window", a second). The run's should fall towards 0 at 200.
- Costs a core spinning while GPUMem waits for the back (it spins 2,048 pauses today anyway).
- Measure 0 against 200 with profile_toggle.py in one session. Its worth falls as P3 and compute
  part 2 remove the stops it shortens.

### FRITCHII_MEMSTAGE_SETUP_KEEP

FRITCHII_MEMSTAGE_SETUP_KEEP (live, default 0 = today; work\fable_stage_levelpoints.md 15.3 row 3),
with FRITCHII_MEMSTAGE_THREAD=2: GPUMem's draw setup (RasterizerVulkan::StageFillDraw's join 1)
keeps the facts of the last lookup while nothing it read changed, instead of hashing the program
registers and looking the facts up at every served draw.
- 0: today: the program id (49 words hashed) and LookupFacts at every draw.
- 1: keep. The lookup's inputs are the copy's program registers (the six pipeline slots and the
  program region, the 49 words the id hashes) and the facts table, whose generation moves under
  its lock at every write. When the words equal the last lookup's and the generation is the one
  read before it, the facts it found are still in stage_facts: no hash, no lookup, no copy of the
  facts. The words are compared themselves (a few ns), not a write counter, so no write path of
  the copy (the entry loop's fast paths, resyncs, the sink) can be missed. A lookup that missed
  keeps nothing.
- 2: verify. Every setup 1 would keep does the full lookup anyway, uses its answer and compares
  it with the kept facts (every field; the programs' infos by pointer). A difference or a miss is
  "a kept draw setup whose full lookup gave other facts or none" on the link line's must-be-0
  list (logged with "other facts" or "the lookup missed"), unless the table's generation moved
  between the keep's check and the end of the lookup: the back published another pipeline of
  the same programs in those few ns (a new infos pointer, maybe another dynamic vertex input), so
  the lookup answers for a later table than the check, and today's lookup made at the check
  would have answered as the keep. Those are "raced", not compared (the first gate's 21-32
  disagreements a window fit this: about 100 ns between the check's generation read and the
  lookup's, a few hundred table writes a second; the field line below confirms it).
  "Memory stage draw setup verify" (HW_GPU, Warning, the first 16 raced and the first 16 not
  raced that differed) names the fields that differed and both generations.
- "Memory stage draw setup (FRITCHII_MEMSTAGE_SETUP_KEEP=n)" (HW_GPU, Info, every 1,048,576
  setups at 1 or 2, about 10 s at the market): setups, facts kept (1), verified (2), disagreed
  (must be 0), raced with a table write (not compared) and how many of those differed.
- Test: one window at 2 (disagreed 0, verified close to the kept count 1 gives), then a 0/1
  toggle with the stage on. The GPU profile at 2 shows the setup's own split ("setup program").

### FRITCHII_E2C_CENSUS (the "Front 0xe2c misses" line)

FRITCHII_E2C_CENSUS (live, default 0 since 2026-11-14 = no work at all on the miss path; 1 = the
line below). It cost the front about 20 ms/s at the market's 2,600 misses a second (the walk of
the noted writes and the query cache look at each miss; fable_front_drains 8.4). A miss more than
10 s after the previous counted one starts a new window, so turning it on live gives honest rates.

"Front 0xe2c misses (noted write) 10 s" (HW_GPU, Info): the macro DMA reads of the 0xe2c
occlusion macro (hash 0x3b26ab7eb3fc9863, its 0x158 read of six words; OoT macro slots, so only
with a macro switch on, as by default) that miss their page snapshot because a write the front
noted since overlaps the read's bytes (EDEN_NX2_DMA_SNAPSHOT's "noted write" misses). Counted only
at those misses (fable_front_drains 1.3 A: the counter that tells its answer from the second
answer's same-frame sample counters). Per second:
- the misses; how far behind the back was at each (ring words between `consumed` and what the
  front sent: average and maximum); the misses with a report the query cache still lists on the
  read's bytes (the hit path's own test, 12 bytes back); the misses where more writes were noted
  since the snapshot than the table remembers (the per-range answers are then "none").
- For bytes 0-8, 8-16 and 16-24 of the read (the macro compares the 64-bit words at 0 and 16;
  8-16 is the first report's timestamp): the newest noted write since the snapshot over them, by
  kind: "payload report (16 bytes)" (report Payload with the timestamp; the front had the payload
  in its registers but notes 16 unknown bytes), "payload release (4 bytes)" (a short payload
  release, its word known), "counter or sample report" (any other report type: written when the
  host query completes), "other" (puller semaphores, uploads, unknown ranges), "none" (no note
  over that range; the miss came from another one). Each as "known a/b unknown c/d": the front
  knows its words or not, the back had run it (a, c) or not (b, d) when the read missed.
- Reading it: misses blocked by payload reports with the back far behind are answer A
  (bookkeeping on the front); counter or sample reports the back had not run, or a listed report
  on the bytes, are answer B (the value does not exist yet at the front).
- With FRITCHII_FRONT_RAN_READS on, it counts only the misses that switch did not spare (with 2:
  would not spare).

### FRITCHII_E2C_ON_E (the "0xe2c on E" lines)

FRITCHII_E2C_ON_E (live, default 0 = exactly as before; fable_b_one_follower 19.5 step 4,
fable_front_drains_b 1.4). The 0xe2c occlusion macro (hash 0x3b26ab7eb3fc9863, with a macro
switch on, as by default) reads six words (two sample counters 16 bytes apart) and writes
render_enable.mode = p0, or p0 ^ 1 when the counters differ. Its read was the front's largest wait
(the "macro dma read" drain, about 120 ms/s at the market). With the switch on, where the front
would wait for the back, the macro gets six zero words instead (so it writes p0) and that write
goes to E as a call at the same stream place (Maxwell3D::FrontSendE2cCondition). E, there, does
what the drained read did: with one follower it first waits for R to play the newest report op
it pushed and the reports a posted fence lands on the bytes (the rule of the front's E-position
waits), flushes the buffer cache's dirty pages of the read and the listed reports of its bytes
(EXP73's test), reads the words, and writes the mode the macro's formula makes of them
(Maxwell3D::BackE2cCondition), the sink first as for a Direct entry.
- 1: deferred only where the read would wait: a page snapshot miss (the drain), the dispatch
  check that would wait (asked without the wait; Queued defers), a listed report on the bytes or
  no snapshot path. The pages of a deferred read get a pending snapshot, current once the back ran
  the call (GpuSplit::Ran); until then reads of those pages are deferred too.
- 2: verify. The front reads as before; every call is also evaluated by E, which compares its
  mode with the front's and writes the front's. "differed" must be 0.
- 3: every call deferred (no snapshots used). For pricing E's side only.
- Not with EXP53's predicate, a replaying shadow RAM, EDEN_NX2_MACRO_DMA_DRAIN,
  EDEN_NX2_CMDPAGE_DIAG or EDEN_NX2_QUERY_DMA_TRACE, and FRITCHII_FRONT_RAN_READS=2's compared
  reads keep their drain.
- The front's copy of render_enable.mode (and its shadow word in a tracking mode) holds p0 until
  the next write of the register (the 0xe2a after the object's draws). A macro reading it, or a
  replaying write, fetches E's value first ("mode fetched from E", expected 0; none of the
  transcribed OoT macros reads it). EDEN_NX2_SPLIT_VERIFY takes E's value without a difference.

"0xe2c on E (FRITCHII_E2C_ON_E=n) front 10 s" (HW_GPU, Info), per second: reads deferred
(snapshot misses, of them on a still pending snapshot; other drained reads; every call with 3),
pending snapshots found run, calls sent, verify calls; mode fetched from E (expected 0); p0
differs (must be 0: the macro's write from zero words was not its p0).

"0xe2c on E: E 10 s" (HW_GPU, Info), per second: calls evaluated (and how many gave True), how
many first waited for R to play its report ops, buffer flushes, report flushes; verify compared
and differed (must be 0, with the session's total).

Read it with the "Front waits by site" / profile's "macro dma read": it should fall to the
forwarded and snapshot hits' cost, and E's time grows by its waits for R.

### FRITCHII_STAMP_READ_PAGES (the "Stamp read pages" line)

FRITCHII_STAMP_READ_PAGES (live, default 0 = exactly as before; fable_b_one_follower 21.2 cut 2).
With the write stamps on, GpuSplit::RangeLastWrite stamps every ranged 3D write (each constant
buffer upload word, each report) by GPU and by device address (EXP126) for the stamp readers
(WriteStamp, WriteStampWhy, BytesStamp: the QMD reads, the acquire lagged check, the QMD
prediction), about 25 ms/s of the front at the market. Those readers ask about few pages.
- 2: a ranged write's bytes are stamped only on device pages a stamp reader ever asked about.
  Where it skips, it raises a skip floor (the newest skipped write). A page asked about for the
  first time takes that floor as its add floor, a stamp over all of its bytes (the reader then
  waits for every skipped write sent before, once); from then on writes there are stamped. The
  ranged ring (ForwardRead, WaitForMemoryWriters, the exact scans) is untouched. Exact: a read of
  a page finds each write sent after the page was first asked about in the tables and each one
  before in the add floor. The page table is cleared at 65,536 pages (a page asked about again
  takes the newest skip floor: still exact). Only for writes up to 4 KiB with a translation
  (the device stamps' own rule: no queued function, not sparse) and EXP126 on; else as before.
- 1: dry. Everything stamped as before; the pages asked about are kept and the skips counted.
- Turned off after 2: readers keep taking the add floors (the skip floor stays for the session).

"Stamp read pages (FRITCHII_STAMP_READ_PAGES=n) 10 s" (HW_GPU, Info), per second: ranged writes
it handled, their pages on pages asked about (stamped), on pages never asked about (skipped, or
would skip with 1), writes stamped whole as before; pages asked about (and new ones a second),
reader answers raised by an add floor, table clears.

### FRITCHII_MACRO_SAME_DRY (the "Macro repeats" line)

FRITCHII_MACRO_SAME_DRY (live, default 0; counters only, nothing is skipped; fable_b_one_follower
21.2 cut 3). Prices skipping a 3D macro call that repeats its slot's last call. Each call of the
front's macro body (Maxwell3D::FrontRunMacroFeatures, handlers and JIT alike) is compared with the
last call of the same macro slot.

"Macro repeats (FRITCHII_MACRO_SAME_DRY, counters only) 10 s" (HW_GPU, Info), per second:
- calls and their writes;
- same arguments as the slot's last call; of them, that call wrote state only (no executable
  method: no draw, report, upload); and every register it wrote still holds the value it wrote
  ("none of its registers written since"; a same-value write in between counts as none); of
  those, it read memory (an MME DMA read, such as 0xe2c: its result may differ) and, the check of
  the prediction, this call wrote exactly the same writes again;
- calls whose every write was a state write of the value the register held, whatever their
  arguments: the upper bound of what a skip could remove, with their writes;
- the five macros with the most held repeats: method, held repeats and calls a second.

Not counted: the macro's inputs besides its arguments (the data RAM, registers it reads but does
not write). A skip would need those too.

### FRITCHII_MACRO_SEQ_DRY (the "Macro sequences" line)

FRITCHII_MACRO_SEQ_DRY (live, default 0; counters only, nothing is skipped; fable_b_one_follower
26.4 lever 1). Prices replaying the front's work between two draws instead of running its
macros. A run is everything the front's macro body (Maxwell3D::FrontRunMacroFeatures) and the
command lists do between two draw.end writes (by a macro or the command list). Its key is the
sequence of macro calls (method and parameter count) and command-list writes (method only),
hashed, the token lists compared on a hash match. For each run it keeps the arguments of every
macro call (and the words their MME DMA reads appended: game memory), the registers the macros
read with their values (the JIT's read callback and the handlers' Read), and the macro writes;
one run in 16 also keeps the data RAM at its start. Each run is compared with the last run of
the same sequence.

"Macro sequences (FRITCHII_MACRO_SEQ_DRY, counters only) 10 s" (HW_GPU, Info):
- runs (draws) a second, tokens a run, the share with command-list writes and how many a run;
- distinct sequences, and how many (most frequent first) cover 50/90/95/99% of the runs;
- macro writes a run, the share that were state writes of the value held, the share of
  executable writes (draws, reports, uploads: always sent);
- against the last run of the same sequence (share of runs that had one): same arguments; same
  arguments and register reads (the inputs a replay must match besides the data RAM), and of
  those how many wrote the same again (less than 100% = an input not tracked, the data RAM);
  same writes whatever the inputs; writes differing a run;
- the data RAM at the start, sampled: equal to the newest sampled start of the same sequence,
  and words differing a compare;
- the three most frequent sequences: share, first 12 tokens ("e52(6)" = macro 0xe52 with six
  parameters, "l586" = a command-list write of 0x586), length.

With FRITCHII_E2C_ON_E deferring, the 0xe2c reads append zero words, so their real values are not
among the inputs counted.

### FRITCHII_FRONT_RAN_READS

FRITCHII_FRONT_RAN_READS (live, default 0 = exactly as before; the market's "Front 0xe2c misses"
line: about 90% of the noted-write misses wait for writes the back already ran). A macro DMA read
(any macro, EDEN_NX2_DMA_SNAPSHOT's 0x158 reads) that misses its page snapshot only because a write
was noted over its bytes since the snapshot (reason "noted write") reads guest memory at once,
without the drain, when all of these hold (Maxwell3D::FrontDmaRead, RanReadCheck):
- (a) the back ran every write noted over the read's bytes since the snapshot: none of those notes
  is past the back's position (`consumed`, loaded first). The note table is in ring order, so the
  scan stops at the first note at or before that position; every older one ran, the ones the table
  dropped included. If the table ends before the scan gets there (the "notes past the table"
  case), a dropped note may still be queued: it drains (GpuSplit::NotedWritesRan);
- (e) the read is on one 4 KB device page;
- (f) the page never held GPU-modified buffer data (the never-cleared GPU-written bit), or no
  indirect grid placeholder was ever handed out: a 3D inline upload with placeholders is the one
  noted write whose run leaves its bytes in the buffer cache only (the grid's DMA copy), which the
  drained path's FlushRegion would bring back. Every other noted write writes guest memory itself:
  3D reports and payload releases, puller semaphores (both through the query cache, at their run
  or when the host query or fence completes), inline uploads (guest memory first, then the caches),
  KeplerMemory data noted by EXP120;
- (c) no async report is owed to the bytes: every report issued to their 16-byte slots completed
  (Nx2AsyncReportsSettled, the rule SEM_FAST uses for semaphore words) and the newest async note
  over them ran. This covers every way a report's value lands after the back ran it: counter and
  sample reports, timed and fence releases, any accuracy or fence setting, EXP33's kept fence
  reports, EXP119's report runs (all issue on the back before it passes the report's entry, and
  complete only after the words are in guest memory). A report starting up to 15 bytes before the
  read counts;
- (b) the query cache lists no report on the bytes (the hit path's own test, 12 bytes back);
- the other miss reasons (compute and copies, unranged writers; with EXP10 the dispatch check and
  byte lists, which come after the noted-write test) are asked as for a hit and still drain.

The page snapshot is left alone: the checks hold for the bytes read, not for the rest of the page,
where a noted write may still be queued or owe a report, and the hit path asks neither (a) nor (c).
So later reads of the page take the same check (a short scan) until a drain renews the snapshot.

- With FRITCHII_MEMSTAGE_THREAD=2 the back still runs every report and semaphore entry (GPUMem only
  fills draws; a lone gather or puller release it serves runs on the back before the back passes
  it), and `consumed` is stored after the entries before it ran, so it is the right position.
  Notes and the async slots are by device address, so another GPU address of the same bytes finds
  them.
- 2 = verify: the same decision, then the read drains anyway; the bytes read at once are compared
  with the drained ones, which are used. "Front ran reads (must be 0): macro m read of n words at
  a (device d) differs from the drained read: word i x, drained y" for the first 16. A guest CPU
  write to the bytes between the two reads would also show (none expected on report memory).
- "Front ran reads (FRITCHII_FRONT_RAN_READS=n) 10 s" (HW_GPU, Info): noted-write misses checked
  a second, passed (with 2: would be served), of those drained after all for another writer (the
  EXP10 dispatch check or byte lists), and the drained ones by first failed check: a note still
  queued, notes past the table, two pages, GPU-written page, an async report owed, a listed
  report; then the compute/copy checks by cause (unbounded writer, writer range overlap, writer
  ranges past the scan, writer ranges left the ring, dispatch queued, dispatch write overlap,
  unbounded dispatch, the dispatch scan's limit passed by the stamps / refused by a block stamped
  since / by a wide write since, listed byte overlap); the reads served (with 2: that would be)
  by what was spared (a noted write, the dispatch scan's limit, both); verify compared and
  differed (must be 0, with the session total).
- Served reads count as "skipped the drain" in the "DMA snapshot" line.
- Expected at the market: about 1,500 to 2,000 of the 2,300 drains a second go (the 1,520 counter
  or sample reports and 548 "other" notes the back had run, minus the ones that owe a report or
  are listed), 45 us each: about 70 to 90 ms/s of front waiting.

The dispatch scan's limit (any read, with the switch on; EXP10's "past the scan"). EXP10's
dispatch check compares the dispatch writes the back recorded since the page was last checked, at
most 2,048 of a ring of 8,192; past that, or when the ring dropped some, it answers "may write"
and the read drains (about 1,660 a second at the market once the noted writes pass, because the
pages are read rarely). By then the check has made every dispatch sent since the snapshot run
(it waits for that, or answers "queued" first), and every write the back recorded for them
(GpuSplit::AppendDispatchWrite: storage and texture buffers the buffer cache marks written,
written images' guest ranges, indirect arguments, QMD releases) stamped its 64 KB blocks with the
record's position before the record was counted. So the read passes when no block stamp of the
page is at or after the snapshot (GpuSplit::DispatchStampsMissSince): the answer the scan would
give without its limit, or stricter (the table is hashed by block, 65,536 slots; a collision only
makes a stamp newer). It then counts as the scan's miss (the page's checked count moves on).
- Still drains: a block stamped since, and a write that stamps everything (an unbounded or
  unconfirmed dispatch, an image without a guest range, a range over 4 GB). Unbounded compute and
  copy writers, writer ranges, unranged writers and dispatches still queued are asked before and
  drain as before.
- Not the GPU-written page bit: it is set for the buffer cache's GPU-modified ranges only, not for
  images a dispatch writes (texture cache) nor QMD releases (written into guest memory by the
  fence callback), and it is never cleared, so a page written once would never pass.
- The stamps start over with each split session (positions start at 0).
- 2 verifies these reads as the others.

### FRITCHII_DISPATCH_WALK_SUMMARY (the "DMA snapshot EXP10 dispatch walk" line)

FRITCHII_DISPATCH_WALK_SUMMARY (live, default 0 = exactly as before; fable_b2 2.4). With EXP10 a
macro DMA read's page snapshot asks GpuSplit::DispatchesMayWriteSince whether a native dispatch
sent since may write the page: it walks E's dispatch records from the newest down to the page's
cursor. With FRITCHII_ONEF_DISPATCH_RANGES the records are bounded, the page's snapshot is never
renewed by a drain, and each read walked every record made since its page's last read (about
8,500 records a second at the market, +33 to +38 ms/s of front macro time).
- 1: after the dispatches since the snapshot ran (the wait or the Queued answer, unchanged), the
  dispatch stamps answer first (DispatchStampsMissSince: the newest record position per 64 KB
  block, hashed, and one for the records that cover everything). No stamp at or after the
  snapshot over the page's block: Miss, the page's cursor moves to the record count loaded before
  the stamps, no walk. A stamp there: the walk, as with 0.
- Exact: E raises a record's stamps before it stores the count past it (release), the front loads
  the count (acquire) before the stamps, so the stamps cover every record below that count. The
  walk misses when no record at or after the snapshot overlaps the page; the stamps miss only when
  no such record touches the page's 64 KB block (collisions only make a stamp newer). So a stamp
  miss is the walk's miss over the same records, or stricter, and nothing is listed either way.
- 2 (verify): after a stamp miss the walk runs too and its answer is used; a write it finds over
  the page (a slot the back reused meanwhile excepted) is a disagreement, logged up to 16 times.
  Must be 0.

"DMA snapshot EXP10 dispatch walk (FRITCHII_DISPATCH_WALK_SUMMARY=n)" (HW_GPU, Info, with the
EXP10 line every 5 s), per second: records the walks loaded (counted with the switch off too),
summary misses (reads answered without a walk), summary refusals by a block stamp and by a wide
stamp; then the session's verify disagreements (must be 0). Expected with 1 at the market:
records walked near 0, "skipped by ranges" unchanged, the front macro zone back near 134 ns a
call.

### FRITCHII_LAGGED_DEFER (the "Lagged check defer" line)

FRITCHII_LAGGED_DEFER (live, default 0 = exactly as before; helper BD). With EXP12=5/6 a channel
suspended on a semaphore acquire gets a "lagged check" every 1 ms (Puller::CanResumeAcquire): the
front waits until the back ran what was sent at the previous check (DrainSite::LaggedCheck, or
LaggedCheckEngine with FRITCHII_ONEF_SEM_E), then reads the latch and the word again. At the
heavy market that wait is ~50 a second, ~1.2 ms each, ~14,000 ring words behind (55-60 ms/s of
front time, the second biggest front wait), and while it waits the front decodes nothing: not
the other channels, not new command lists.
- 1: when the back is short of the check's target, the check is put off: what was sent is
  published, the poll answers "not met yet" and the front goes on with other channels and new
  command lists. The first poll that finds the back at the target runs the check unchanged (no
  wait left). When the front would otherwise sleep (no command list queued, every channel
  suspended), it waits for the smallest target put off right there (LaggedDefer::WaitBeforeSleep,
  gpu_thread.cpp) and polls again: the wait it would have made, now with nothing else to do.
- Exact: the same checks decide the resume, in the same order within the channel; a check runs
  only once the back reached its target, as before. Only how other channels' work interleaves
  with the suspended channel moves, which the 1 ms timer already made timing-dependent; the
  acquire still resumes only when met. No hang: the work sent is published at each put-off poll,
  and the front waits for the target before any sleep.
- Why: the front is the wall at the market (fps follows its time); a wait that blocks other work
  for 1.2 ms is front time the frame pays for whenever there is other work to do.

"Lagged check defer (FRITCHII_LAGGED_DEFER=n)" (HW_GPU, Info, every 10 s while it acts), per
second: polls that put their check off (and how many acquires), checks run later with the back
there (each is a wait saved), waits made where the front would sleep (and their ms/s: the part
of the old wait that had nothing to overlap).
How to judge: a profile_toggle 0/1 in one session (pin market, --cycles=2 --warmup=20). On the
front: "Front waits for the back by site" lagged check ms/s must drop (the waits made where the
front would sleep stay in that line, the defer line gives them apart; the drop is the part that
overlapped other work), and the front per draw (macro + resume pending) must not rise. Then fps (cycles=4) only if the front
wait drops: a keeper when fps rises; drop if the "checks run later" count is near 0 (the waits
had nothing to overlap).

### FRITCHII_DISPATCH_WAIT_CENSUS (the "Dispatch wait census" line)

FRITCHII_DISPATCH_WAIT_CENSUS (live, default 0 = nothing counted; helper BD). A dry count, no
behaviour change. A macro DMA read (0xe2c's occlusion report reads, mostly) whose page snapshot
passed every other check waits for the native dispatches sent since the snapshot
(DrainSite::DispatchWait, "macro dma dispatch": ~250 a second, ~68 ms/s at the market, the
biggest front wait), because what a dispatch writes is only known once E ran it (E records its
write marks). At the market none of those dispatches ever wrote a page read (the EXP10 line's
dispatch overlaps stay 0), so the wait is pure latency. Removing it needs the front to bound
what the queued dispatches write (their QMD, the shader's storage buffer descriptors, the
pointer words in their constant buffers, image descriptors). This census says how much of the
wait each level of such a bound would answer, before anyone builds it.
- 1: each such wait that really waited is classified, once the dispatches it waited for ran, by
  their records (the ones E appended for positions from E's position when the wait began):
  buffers only (storage/texel buffer write marks: a front bound of the SSBO writes answers it),
  buffers + QMD releases (the front predicts those already), with indirect arguments (list
  dispatches, GPU-sized grids), with an image (a bound needs the image descriptors too),
  anything / records lost (unbounded or past the record ring), no record. E tags each record
  with its kind (DispatchWriteKind) whatever the switch says.

"Dispatch wait census (FRITCHII_DISPATCH_WAIT_CENSUS=1)" (HW_GPU, Info, every 10 s of waits), per
second and ms/s per class, the records per wait, and the waits whose dispatches did write the
page read (expected 0, as the EXP10 line says).
How to judge (Gate 1 for the front dispatch bound): the ms/s of "buffers only" + "buffers + QMD
releases" is the ceiling of an SSBO-only bound; add "with an image" for a bound that also reads
image descriptors. Build the bound only if the first is >= 10 ms/s on the front.

### FRITCHII_FRONT_CALL_LEAN (the "Front call lean" line)

FRITCHII_FRONT_CALL_LEAN (live, default 0 = exactly as before; fable_b 27.2 B, about 30 to 35
ms/s of the front). Every 3D macro call on the front read ten live switches, MacroTraced, built
an empty trace string and copied the call's words from the command list into the FIFO vector.
- 1: the features body takes the switches from the front state. FrontRefreshSwitches reads them
  where it reads the feature byte: when the switch generation moved, and every 1024 calls. A
  whole call that arrives in one run (FrontCallMultiMethod) keeps its words in the list: the
  handlers read them there. The words go into the vector only where a path needs it: the JIT
  (no handler, or a declined call), 0xe2c (its FIFO read appends six words), EXP53's predicate,
  EDEN_NX2_MACRO_TRACE, FRITCHII_MACRO_SAME_DRY / SEQ_DRY, FRITCHII_ONEF_DEAD_STORES, the list
  guard's checked body and the plain body. The trace string is made only for a traced call.
- Exact: the same switch values (every store of a live switch bumps the generation, which the
  next call sees before it picks a body) and the same words in the same order; only 0xe2c's
  handler appends to the FIFO (its 0x158), and it gets the vector.
- 2: as 1, and every call reads the switches again as before and counts the calls where any
  differs. Must be 0 (a switch stored between a call's refresh and its reads can count once).
- Off: one predictable branch in FrontRunMacro and one in FrontCallMultiMethod's macro path.

"Front call lean (FRITCHII_FRONT_CALL_LEAN=n), last 10 s" (HW_GPU, Info): calls through the
lean body, of them with their words read in the list, of those copied after all; mode 2: calls
compared and switch differences (must be 0). Judge it by the front's macro zone per call.

### FRITCHII_FRONT_LIST_LANE (the "Front lane" line)

FRITCHII_FRONT_LIST_LANE (live, default 0 = exactly as before; fable_b 27.3, about 36 ms/s for the
list words and 42 for the handler writes). 56% of the front's register writes end at
FrontWrite's same-value exit, after the list parse's CallMethod, FrontCallMethod and Front() or
the macro emit. The lane makes the same test where the write starts. Bits:
- 1: command-list words (DmaPusher::ProcessCommands, the incrementing and the inline branch).
  Decided once per chunk: the switch, the split running with front macros, no semaphore watch,
  the channel's 3D engine, Maxwell3D::FrontLaneReady (the switch generation the front read its
  experiments at, and the lane state). Per word: `block == 0 && lane method && shadow == value &&
  register == value`, then the word is dropped.
- 2: a handler's writes in the lean macro body (needs FRITCHII_FRONT_CALL_LEAN=1), not while
  the call records an op (FRITCHII_ONEF_FRONT_OPS) or a tool watches its writes.
- 4: verify instead of drop for the paths bits 1 and 2 turn on: each write goes the old way and
  a lane drop the old path sent (a ring commit, or a draw packet write) is counted. Must be 0.
- Lane methods: not executable, no memory write, not render_enable.mode (the deferred 0xe2c
  notes come before the test), not 0x157 or 0x158 (the DMA reads run after it), not a puller
  method.
- `block` (Maxwell3D::FrontLaneSync, kept where each changes): the front exists, the shadow RAM
  tracks, same-value skipping is on (EDEN_NX2_SPLIT_SKIP, EDEN_NX2_SKIP_SAME), EXP2 off, no list
  draw left stale registers, no FRITCHII_MACRO_SEQ_DRY run.
- Exact: it is FrontWrite's own condition, and with `block` 0 nothing FrontWrite, FrontCallMethod,
  Front() or CallMethod does before that exit acts on a lane method; a dropped write sent no
  entry before, so the ring and the memory stage see the same entries. The profile counters
  (method writes, same value) are added per chunk and per call.
- Off: one predictable branch per list word (a null engine) and one switch load per chunk.

"Front lane (FRITCHII_FRONT_LIST_LANE=n)" (HW_GPU, Info, every 10 s), per second: list writes and
handler writes dropped where they start; verify: writes checked, sends the lane missed (writes
the old path dropped that the lane did not: information), lane drops the old path sent in the
window and since boot (must be 0). Judge it by (macro + resume pending + push) per draw.

### FRITCHII_FRONT_OP_LEAN (the "Front op lean" line)

FRITCHII_FRONT_OP_LEAN (live, default 0 = exactly as before; fable_b2 1.4, about 60 to 80 ms/s
for the op ids). Needs FRITCHII_ONEF_FRONT_OPS. Each write of a recorded op still ran the whole
FrontWriteDecide. The op path takes a call only under its per-call conditions (the shadow RAM
tracks; EXP22, EXP2, the draw packet, the CbWords block and the list guard off; no stale list
draw registers; no deferred 0xe2c mode; no tool), and a fold case already goes the old way.
- 1: FrontOpDecide: the shadow compare and store, the same-value test, the register store and
  the executable cases of the methods ops write, by a per-method kind filled when the front
  starts: a state register, a draw manager method (the instance loop skip and tracking, the
  index draw tracking, draw_pending, effect Gpu), a constant buffer word (its range, its address,
  the offset step) and a bind group's raw_config (stored, effect unchanged). Any other method, a
  shadow RAM that stopped tracking, and a Subsequent draw.begin while EDEN_NX2_FOLD_INSTANCES is
  on take FrontWriteDecide. The profile counts as FrontWriteDecide does.
- 2 (verify): per write the lean decide runs on the touched state (the register, its shadow word,
  the constant buffer offset, draw_pending, other_index_draw, instance_loop), the state is put
  back, FrontWriteDecide runs from it and its answer is used; a difference in the sent bit, the
  decision (value, effect, constant buffer address, written range) or the state they leave is
  counted and logged (16 times). Must be 0.
- No ring or entry change: the op entry, E's apply and the memory stage see the same sent and
  pair masks and notes.
- Off: one predictable branch per recorded write, only while FRITCHII_ONEF_FRONT_OPS is on.

"Front op lean (FRITCHII_FRONT_OP_LEAN=n)" (HW_GPU, Info, with the "Front ops" line, counts since
boot): writes by the lean decide, writes left to FrontWriteDecide; compare: writes, differing
(must be 0).

### FRITCHII_FILL_GEOM_KEEP, _UPDATE_KEEP, FRITCHII_HANDLE_PROG_KEEP, FRITCHII_FILL_BRACKET_LEAN (the "Fill ... " lines)

fable_b 25.2 cuts e to h: the rest of E's fill. Each is live, default 0 = exactly as before (one
relaxed load and a branch a draw). Values: 1 = on; 2 = on, with the old path run as well and
compared ("must be 0" in its line); 3 = dry count: the old path only, with the calls the cut
would skip counted and their time measured (1 call in 64 timed and scaled by 64, so the line's
ms/s is the cut's ceiling, the memo's own look already subtracted in "net"). One line per cut
every 10 s (HW_GPU, Info). Test order: 3 first (a cut whose net is under ~3 ms/s at the market is
not worth turning on), then 2 (must be 0), then 1.

- FRITCHII_FILL_GEOM_KEEP (cut e, "Fill geometry keep"): the vertex and index binds'
  synchronize (BindHostVertexBuffers, BindHostIndexBuffer) skipped while the slot's memo holds:
  the range and the dirty generation sum of its tracker managers when a synchronize left it
  clean, as EDEN_NX2_STORAGE_SYNC_MEMO does for storage binds (a page that gains a CPU or
  Untracked bit moves the sum; nothing else makes a page dirty). Not with hot pages or CleanWords
  on (their walks may leave a range dirty). The read log's range is logged as before. Verify: a
  hit's range holds no CPU or unprotected page and its synchronize uploads nothing. The usage
  marks stay: their stamp is the packet's, new every draw.
- FRITCHII_FILL_UPDATE_KEEP (cut f, "Fill update keep"): UpdateIndexBuffer runs at every indexed
  draw (the draw methods raise its dirty flag) and UpdateVertexBuffer at every dirty slot. The
  binding they make is a function of the start address and the size (index) or the start and end
  addresses and the enable (vertex): the translation, the mapped range, then FindBuffer. Kept
  while those inputs, the page tables (MapSerial, taken before the resolve), the address space,
  the channel and the set of buffers (binding_epoch, taken after it, so a buffer the resolve made
  is part of the memo) are unchanged, as EXP18's memos. Not with accurate GPU fences (FindBuffer
  waits there). The dirty flags and the binds after it are untouched. Verify: the resolve runs
  and must give the kept binding. At 1 the disabled-stream translation skip count
  (FRITCHII_BIND_ADDR_MEMO) no longer counts kept slots.
- FRITCHII_HANDLE_PROG_KEEP (cut g, "Handle program keep"): E's fill (OneFollowerDraw::FillRing,
  with E's pipeline) of a draw whose pipeline has handle programs: each program evaluation (a
  single element or a whole table) keeps a record per pipeline, in fill order, of the constant
  buffer addresses it read through and each word it read (GPU address, size, value). While all of
  them read the same again, the record's results are pushed instead of evaluating: the program is
  a pure function of what it reads, and every address it reads is a constant buffer address or
  comes from earlier reads, all equal, so it would make the same reads. The compare makes the
  same memory reads as the evaluation (no page flags, no tracker state), so it saves the
  interpreter (its 1.3 KB of zeroed stack and slots), the repeated storage base reads and the
  rest; the dry count says how much. Classes: same words, constant buffer rebound, words changed,
  no record. Verify: a same-words program is evaluated and its results compared.
- FRITCHII_FILL_BRACKET_LEAN (cut h, "Fill bracket lean"), E's ring fills only:
  - The trackers' record (only made while the OoT trackers are on): E fills only the slots R can
    read on that draw, from E's registers: slot 0 only when one of the eight render targets is
    B10G11R11_FLOAT (TrackOcarinaCamera reads it only when the first color target's view has
    that format, which only that render target format gives), slots 1 and 2 only for an indexed
    draw of the actor's vertex program (TrackOcarinaActor). At 1 the others stay unbound; at 2
    and 3 the whole record is made. At every value the left-out slots are marked in the packet,
    and R counts reading one ("R read a slot the lean record leaves out", must be 0; at 1 the
    first one turns the cut off for the session).
  - Ring B's push: the room check reads ring_b_freed (the back's line, written for every packet
    it takes) only when the packet does not fit against the last value E read (it only grows),
    and the link counts the packet and its bytes but not the window counters (packets in ring B
    after the push, another read of the back's line; GPUMem's window question). Verify: the line
    is read every time and a fit by the kept value that the real one refuses counts (must be 0).
  - The fill bracket itself (fable_b 14.4 cut 3) is already lean on E: its brackets leave the
    shared watch count alone (MemStage::Fill::Open). Nothing more is cut there.
### FRITCHII_REPORT_WAIT (the "Report waits" line)

Helper CN (work\helper_cn_answer.md). Live, default 0. The wait a macro DMA read of report
memory makes for query results the host GPU still owes (RasterizerVulkan::FlushQueryResults,
reached through QueryDma::FlushPending from FrontDmaRead: the front's "macro dma flush" site).
With one follower it is lent: R waits, E is parked, the front is stopped, so every millisecond of
it is lost on all three threads. Kakariko (unprofiled): 25 to 30 waits a second of ~1.8 ms,
~48 ms/s; the market ~11 a second, ~28 ms/s.

- 0 = as before: EXP30 decides (EXP30 off = the old wait: an operation queued behind a new
  fence, i.e. every piece of GPU work sent so far, including what was sent after the reports).
- 1 = the lean wait (what EXP30=1 does): a fence for the reports that still wait for one, then
  Tegra::Nx2WaitAsyncReportsSettled over the read's range (from 12 bytes before it). Every
  counter report is issued and completed through those slot counters (QueryCacheBase::
  CounterReport: run entries and fence operations alike), so a settled range holds the words
  the old wait would leave there. A slot shared with another address only waits longer.
- 2 = verify: the lean wait (polled; a range that has not settled after 200 ms counts as a
  difference and goes on to the old wait instead of hanging), the read's words, then the old
  wait, the words again. "words changed in the old wait's rest" must be 0; the first 16 are
  logged as errors. Both waits are timed: "lean ... ms/s" against "old wait ... ms/s" is the
  saving 1 gives, measured in one session.

"Report waits (FRITCHII_REPORT_WAIT=n)" (HW_GPU, Info): every 10 s, and at the first wait after
the switch changes (so profile_toggle windows hold one value each), when there were waits:
waits/s, ms/s, us each, longest, lean waits/s; with 2 the verify numbers. Every value times its
waits (two clock reads a wait, ~30 a second), so 0 and 1 windows compare directly.

Risk to watch: the #215 family (white flashes on leaves and walls, a read that returned before
its report landed). Verify first; the flash watch in the 1 windows.

### FRITCHII_LOCK_CENSUS (the "Lock census" lines)

Helper CP (work\helper_cp_answer.md). Live, default 0. Who holds a cache lock when a thread has
to wait for it. The drain sites line already times the waits per role ("cache lock waits: ... GPU
back 18 ms/s (2,900/s)" at Kakariko); this names the holder. Only the two cache locks (the
buffer cache's and the texture cache's SpinRecursiveMutex) and only contended waits are seen
(common/fritchii_lock_census.cpp).

- 0 = off: one relaxed load in the contended path, nothing on an uncontended lock.
- 1 = every contended wait counted by waiter thread, holder thread (the owner when the wait
  starts, by its thread name) and lock. "Lock census (FRITCHII_LOCK_CENSUS=1) 10 s, contended
  cache lock waits, waiter <- holder (lock): GPUBack <- GPUFencingThread (buffer cache) x ms/s
  (n/s, y us each); ..." (top 12 pairs), plus the waits whose holder had let go before the
  waiter looked.
- 2 = as 1, plus the holder's stack for GPUBack's (E's) waits: the holder is paused (at most 40
  a second, 20-50 us each, added to that one wait), its stack read if it still holds the lock,
  then resumed. Second line "Lock census holder stacks": where the holders were (thread and
  first 5 functions, % of the stacks and E's wait in those waits) and the functions on their
  stacks (% of stacks). The first report loads the exe's symbols (DbgHelp, the .pdb next to the
  exe) on the census thread: a few seconds and a few hundred MiB. Windows only.
- 3 = as 2, for every waiter.

Run 2 for one short session (60 s is plenty); it perturbs E by at most ~1-2 ms/s.

### FRITCHII_FENCE_YIELD_E (the "Fence yield to E" line)

Helper CP. Live, default 0. The fencing thread releases fences in batches and lands each
batch's buffer downloads (PopAsyncFlushes) and image downloads with the cache lock held for the
whole batch (FenceManager::ReleaseThreadFunc's pop_all). E takes the buffer cache's lock at every
draw fill and counter report (~330,000 a second at Kakariko), so a pop under way stops E.

- 0 = as before.
- 1 = while E (GPUBack) waits for the lock it marks it (SpinRecursiveMutex::BackWaiting); between
  two pops of a batch the fencing thread then lets the lock go, waits until E has taken it (E's
  mark gone, at most 50 us) and takes it back for the next pop. The same pops in the same order,
  each whole under the lock, as EXP74's hand-back, which only lets go after a time; between two
  pops any other thread could already take the lock. The batch's operations and query pops run
  after all its pops, as before.
- 3 = dry: E marks, the fencing thread only counts the chances.
- "Fence yield to E (FRITCHII_FENCE_YIELD_E=n)" every 10 s from the fencing thread: pop batches
  and pops a second; the pops E waited behind with pops left (the cut's chances: yielded at 1,
  counted at 3); E waiting at the batch's last pop (out of the cut's reach: a single pop, or the
  last one); the time handed to E.
- Not on the EDEN_NX2_FENCE_RELEASE_STATS path (that one is left as it was).

Read with FRITCHII_LOCK_CENSUS=1: only worth turning on if the census names the fencing thread
as E's holder and the dry line shows chances at a good share of E's waits.

### FRITCHII_FENCE_COPY_UNLOCKED (the "Fence copy unlocked" line)

Helper CQ. Live, default 0. At Kakariko E waited ~17 ms/s behind the fencing thread, which held
the buffer cache's lock while it copied finished buffer downloads into game memory
(PopAsyncBuffers: the copies, the set of collected CPU writes, the frees). With it, each pop
(BufferCache::PopAsyncBuffersUnlocked) does under the lock what reads the cache's state: the
landing, the collected CPU writes over the landing's range only, the ranges the fence still
writes. It lets the lock go for the copy (marks after the bytes), takes it back, clears the
GPU-modified state where the fence still writes and marks the download landed. The ordering
argument is in work/helper_cq_answer.md. Landings under 512 bytes are copied under the lock as
before.

While such a copy is under way: another landing waits for it (one landing at a time, as before);
ClearFenceDownloads waits if it overlaps (a writer that takes ranges from the fence downloads
writes newer bytes next); a device unmap waits if it overlaps.

- 0 = as before.
- 1 = on.
- 2 = on and verify: every landing copied outside the lock, then compared under the lock (in a
  landing of its own) with what the locked pop would have written. Must be 0.
- "Fence copy unlocked (FRITCHII_FENCE_COPY_UNLOCKED=n)" every 10 s from the fencing thread: pop
  batches and pops a second; landings copied outside the lock and small ones under it; copies,
  MB/s and ms/s out of the lock; the fencing thread's hold of the buffer lock and its wait to take
  it back (1 and 2 only); lock holders that waited for a copy; E's cache lock waits that began
  behind the fencing thread (ms/s, count, us each); at 2 the verify counts. Once the switch is set
  (in the environment, or turned on live) the line runs at 0 too, so both arms of a toggle have
  E's number.
- Not on the EDEN_NX2_FENCE_RELEASE_STATS path; EXP74 and FRITCHII_FENCE_YIELD_E do nothing while
  it is on (the lock is let go at every landing anyway).

### FRITCHII_OCCL_LEAN (the "Occlusion lean" line)

Helper CS (work\helper_cs_answer.md). Live, default 0. Needs one follower with E's pipeline
lookup (FRITCHII_ONEF_E_PIPELINE=1, the default under FRITCHII_ONEF_LEAN); does nothing else.

Kakariko sends ~2,570 occlusion test draws a frame (sample counting on, every colour write off)
of ~3,420, and E fills each in full. For a draw that writes no colour, the pixel program can only
change depth, stencil, the counted samples or memory through a discard, a depth or sample mask
write, a store or atomic, or its alpha (alpha to coverage, the alpha test emulated in it). When it
has none of them, E swaps the pipeline it looked up for the same key with the pixel program hash 0
(the pipeline a draw with the pixel program disabled gets). E's fill then has no pixel stage (no
pixel uniform buffers, storage binds or texture handle values) and R binds no pixel textures;
depth, stencil and the samples counted are the same (Vulkan runs the per-fragment tests and the
query count on the rasterized coverage and interpolated depth with no fragment shader).

- 0 = off (one relaxed load a draw).
- 1 = on. A draw is taken when: no render target with a format has a colour mask bit on (by
  target index and by rt_control slot, both readings), alpha to coverage off, the alpha test off
  or Always (registers), and the pipeline (classed once): has a pixel program, no task/mesh, key
  alpha test Always, no dual source blending, and a pixel program without demote, depth or
  sample mask writes, storage/image/global writes or atomics. The pixel-less pipeline
  ("sibling") is translated on E from the draw's own environments, at most one a frame, kept
  out of the disk cache, and used only when built and its vertex side (stages 0-3: uniform masks
  and sizes, storage, texture, image and texel descriptors, handle programs, the vertex program)
  equals the full pipeline's. A difference refuses that pipeline for the session.
- 2 = verify: as 1, but the full pipeline still draws (picture unchanged); the siblings are made
  and compared. "differs" must be 0 for the cut to cover everything it counts.
- 3 = dry: the census and the timer only, no sibling made.

"Occlusion lean (FRITCHII_OCCL_LEAN=n)" (HW_GPU, Info) every 10 s and at a switch change (a gap
of half a second ends a 1 window, so 0/1 toggles keep one value a line): E draws, counting,
colour off and test draws a second; colour off but alpha to coverage or alpha test; draws the
cut can take (and how often the same pipeline as the previous one), with the pixel uniform
buffers, textures, storage buffers and handle programs each one skips; draws kept full by reason
(no pixel program, discard, depth or sample mask, stores, mesh, alpha test or dual source key,
vertex side differs, failed); drawn lean a second, sibling not ready, verify ready; siblings made
(and E's ms translating them), found, failed, compared, differs; E's fill and push time of a
timed draw (1 in 16 of the takeable ones), lean vs full. Use 3/1 for the thread metric (both
timed), 0/1 for fps.

Costs: the register test (~30-50 ns) on every E draw while on; a sibling translation on E (a few
ms, once per pipeline a session) when a takeable pipeline first shows up, at most one a frame.
Lent draws keep R's own lookup (full pipeline). Not compatible with the one-follower build
compares (step 3 BuildPipeline would see the sibling); FRITCHII_ONEF_LEAN=1 turns those off.

### FRITCHII_OCCL_FLAG (the "Occlusion flags" line)

A player setting since 6.2: Settings > Graphics > Advanced > "OoT occlusion speed-up", Off /
Standard / Full (Settings::values.occlusion_speedup, global only, takes effect when the game
starts). Standard, the default, is the core (3); Full is everything (9). When a switch sets
FRITCHII_OCCL_FLAG (environment or nx2_switches.txt) the switch wins, for tests; it is no longer
in the built-in switches. At each game boot the device logs "Occlusion flags:
FRITCHII_OCCL_FLAG=N (set by a switch | the Occlusion speed-up setting)". Live between its values
(debug server), but the device keeps the buffer device address feature for the flag area only
when the value is not 0 at boot ("Occlusion flags (FRITCHII_OCCL_FLAG): buffer device address
enabled for the flag area").

Values: 0 off; 3 the core (Standard); 9 everything (Full: the core, colour flag variants, the
discard pre-pass, the colour pre-pass); 1 the core as well (6.2's test builds baked 1 for the
core, so a 1 left in a switch file or script never turns the rest on; the contributed package
numbered "everything" 1, here it is 9); 2 verify (every flag store of 9, the host queries still
decide; any game); 5 as 3 with the discard pre-pass and 8 as 9 without the colour pre-pass (A/B).
Any other value is off.

Full is offered once to players it can help (fritchii/occlusion_offer.cpp): when a game ends in
which the flags acted in OoT on AMD's Windows driver with shaderInt64 (the rasterizer sets
OcclFlag::g_full_could_help), the setting is still Standard and no switch sets
FRITCHII_OCCL_FLAG, the offer is marked pending (UISettings fritchii_occl_offer) and shown 0.8 s
after the game list is back, unless another window is up; else at the next plain launch. "Faster
mode for AMD / Can give more fps on AMD cards. / Change it any time in Graphics > Advanced.
[Not now] [Turn it on]". The answer is kept and logged ("Occlusion speed-up offer: ..."); it is
never asked again. FRITCHII_OCCL_OFFER=0 never asks; =ask asks at every plain launch on any PC
(screenshots). In the settings, Full is greyed out where it can't act (the picked card runs one
follower: every card but AMD's Windows driver by default), with one tooltip line.

Preparing shaders: the extensions change files the stamp hashes, but no prepared pipeline (every
new SPIR-V path runs only for a variant made during play, never stored on disk; any other module
is byte-identical; vk_graphics_pipeline.cpp gained draw-time code only). Nx2ShaderStamp.cmake maps
the new hashes to 6.1's stamp (SameAsReleased, with that reason), so nobody prepares again, with
Full on or off. Vulkan 1.2 or later; one follower off (the draw path it hooks is the
one-thread path, which is the path on AMD's Windows driver). With one follower on, no draw takes a
flag variant, lent draws included (FRITCHII_ONEF_AMD=1 runs on AMD lent every draw and stored flags
through them; a played draw's pipeline from E falls back to its parent too). Code: fritchii_occl_flag.h,
vulkan_occl_flag.frag, PipelineCache::OcclFlagPipeline / FindOrMakeOcclFlag, SamplesStreamer.

Why: OoT's occlusion tests run as host occlusion queries, about 1,100 begin/end pairs a frame at
the market (~55,000 reports a second). On an RX 9070 XT (OCuLink eGPU) a microbenchmark measured
about 4.3 us of GPU time per pair around real work, rising to about 6.2 us under 3 GiB/s of PCIe
traffic (the query end waits for the enclosed work; precise or not makes no difference); one
query around the same work costs nothing extra. The 0xe2c pair test only asks "did any sample
pass" (census: all calls p0 = 0, one count of every pair 0), and writing sample reports as 0 or
4096 (FRITCHII_OCCL_TEST=2 on the experiments branch) kept the picture correct day, night and
with the sun in view.

What it does: a counted draw that writes no colour and whose pixel program can change nothing
(FRITCHII_OCCL_LEAN's Lean class; not mesh) is drawn with its pipeline's flag variant: the same
key, a whole pipeline (no libraries) whose pixel stage is the host program
vulkan_occl_flag.frag. With early fragment tests it runs only for fragments that passed depth
and stencil and stores 4096 into the 64-bit slot of the open query window, at a buffer device
address pushed at push constant offset 40 (bytes 40 to 47 are otherwise unused). The variant is
owned by its pipeline (never in the cache or on disk), made one a frame (shared with the lean
siblings' budget), and used once built; until then the draw keeps its pipeline and a real query.
The flag area is device-local memory (not host visible when the device has such a type, so the
small BAR heap is left alone), one u64 per slot for the first 1,024 banks plus a sink, cleared
with a fill in the upload command buffer when a bank is reserved. Each sync copies the synced
slots, and the slots of the running window it carries over, into a host-visible mirror (cached
memory preferred) that the fencing thread reads.

- 0: off (a relaxed load per draw; counted draws also touch the streamer's mark, which returns at
  once without the flag area).
- 2: verify. The host queries stay and decide everything. Flag draws store into the open query's
  slot; at each report the fencing thread compares "the query counted samples" with "a flag was
  stored" for reports whose counted draws were all flag draws. Must be 0 disagreements. Reports
  whose banks were reserved again before the compare are skipped ("bank reused").
- 3 (and 1, 5, 8, 9): on, Ocarina of Time only (its program ID) and not with precise counts (accurate part 2): a
  flag slot reports 4096 when any sample passed, not the count, which is all OoT's 0xe2c pair test
  asks. A query window's first counted draw opens a flag slot (no host query) when it is a flag
  draw; any other counted draw (colour on, not Lean, texture draws, draw clears) closes it and
  opens a query slot, and later flag draws stay in that query slot (the host query counts them
  exactly), so a window gains at most one slot per segment (the window's value is the sum of its
  slots, as for a window across submissions). The result copies (GPU) take flag runs from the
  flag area and query runs from the pool; the fencing thread reads flag runs from the mirror and
  calls vkGetQueryPoolResults on
  query runs only, so nothing waits on a query that never ran. A fragment-write to
  transfer/host-read barrier runs at each sync while the flag area exists.

Log every 10 s: "Occlusion flags (FRITCHII_OCCL_FLAG=N) per second: counted draws F with a flag
pipeline, O without (not Lean L, regs refused R, not ready Y); flag pipelines made M, failed X;
flag draws with no open query S; reports compared C: agree A, query 0 flag 1 D1, query 1 flag 0
D2 (must be 0; T since boot); mixed X, no flag N, bank reused B; slots opened: FS flag (no host
query), QS with a host query". A second line, "Occlusion flag census per second", counts the
counted draws without a flag pipeline by reason (colour Lean: colour on, pixel program in the Lean
class; colour discards / key state / depth or mask / stores / other; alpha test or coverage; off
not Lean: colour off, pixel program not Lean), the distinct colour Lean pipelines in the window,
and the query slots closed with only flag or colour Lean draws ("all flag or colour Lean": they
would need no host query if colour Lean draws stored flags too) vs with another counted draw
("blocked"). With the switch at 0 the census counters are not touched.

Measured (RX 9070 XT over OCuLink x4, ReBAR off, Core Ultra 7 255H; OoT market fountain,
1440p -> FSR 4 4K; live toggles in one session, three rounds, screenshots each window):
- Verify: about 200,000 reports compared, 0 disagreements; picture unchanged.
- On vs off: GPU busy -1.65 ms a frame idle (17.3/17.1/16.1 -> 15.6/15.4/14.5), -3.1 ms on average
  under 3 GiB/s of PCIe copies; frame time -1.5 ms idle (about +8% fps); picture correct by day,
  dusk and night. About 46% of the query slots become flag slots; the rest are windows with
  counted draws that write colour (about 51,000 a second at the market), which keep real queries.
- Flag area moved to device-local memory (2026-11-18, three rounds over two sessions): verify about
  146,000 reports, 0 disagreements, bank reused 0; GPU busy -2.2 ms idle, -1.9 ms under PCIe load;
  frame time -1.7 ms idle. Within run-to-run noise of the host-memory area.
- Census at the market fountain (mode 1): about 67,000 slots a second, 45% flag slots. Of the
  ~37,000 query slots, ~22,000 hold only flag and colour Lean draws (~44,000 colour Lean draws a
  second from ~28 pipelines); the rest are blocked by colour draws that discard (~12,000 a
  second) and colour-off draws outside the Lean class (~18,000 a second).

Risks and limits: a flag variant writes no colour by construction; a draw whose regs or pipeline
fall outside the Lean class keeps its query. The GPU-side value of a window with several slots goes
through the existing prefix scan, as windows split by a render pass already do; OoT's 0xe2c
reads the CPU-written value. No flag area on Qualcomm, ARM or Turnip drivers. The flag area is in
device-local memory (see above). Not tested on NVIDIA or Intel, nor with one follower on. Not with
the memory-stage ring (facts are the parent's key, but not checked).

#### Colour variants and pre-passes (2026-11-18; Full, =9, and verify, =2)

Full and verify also cover counted draws that keep their queries otherwise (the contributed
package called this value 1):
- Colour flag variant: a colour-writing draw whose pixel program is Lean and uses no subgroup,
  quad or helper operations (and no alpha to coverage) draws with its guest pixel program
  translated with RuntimeInfo::occl_flag_store: early fragment tests and, at the start of main,
  the flag store through the buffer device address in push constant bytes 40..47 (a u64 member of
  the program's rescaling or render area block, or a block of its own; PhysicalStorageBuffer64,
  Int64; the store is in a function of its own and runs only while the slot reads 0). Programs
  without the option are byte-identical. Needs shaderInt64.
- Discard pre-pass: a counted colour-off draw whose pixel program discards (otherwise Lean, no
  subgroup/quad/helper operations) is drawn twice: first a variant (the guest pixel program with
  early fragment tests and the flag store at the end of main, after the discards:
  RuntimeInfo::occl_flag_store_at_end) with depth writes off, then the draw itself with its own
  pipeline bound again (GraphicsPipeline::ConfigureAfterOcclPrepass: the variant's descriptors
  and push constants for this draw). The pre-pass tests the depth before the draw; with a
  monotonic compare "some sample passed and was not discarded" is the same.
- Colour pre-pass: a counted colour draw whose pixel program is Lean but uses subgroup or quad
  operations is drawn first with the host flag program, colour and depth writes off (with
  extended dynamic state 3 blending the dynamic masks are cleared; without it, dyna_state 2, a
  host variant whose key has every attachment's writes off), then as it is.
- Both pre-passes only where the draw can be recorded twice and nothing in it can differ: not for
  indirect or list draws, the delta path or draw counting, frame dump probes; not with stencil
  writes, depth bounds, transform feedback, stores in any stage, mandated early Z (discard
  pre-pass), EQUAL, or NOTEQUAL with depth writes; dynamic depth writes needed. A variant whose
  translation's descriptors differ from its pipeline's is refused. In mode 1 a host query
  already open in the window counts the real draw (no pre-pass); verify ends the open query
  around the pre-pass so it compares the pre-pass flag with the real draw's count.
- Test values (exact picture, for A/B): 3 colour-off flag variants only, 5 as 3 with the discard
  pre-pass, 8 as 9 without the colour pre-pass.

Logs: the flag line adds "colour flag draws, colour flag pipelines made; pre-pass draws (between
host queries, skipped: a host query open), made, refused at the draw, refused for their
descriptors, real draws lost (must be 0); colour pre-pass draws". The census line adds the
discarding draws' writes and the subgroup operations of colour Lean draws refused for them.

Test switches (development): FRITCHII_OCCL_FLAG_DUMP=<folder> writes the first 64 colour or
pre-pass flag pixel programs; FRITCHII_SPIRV_DUMP_DISCARD=<folder> the first 200 discarding
programs emitted; FRITCHII_TEST_KILL_FALLBACK=1 translates new programs with the backend's
no-demote path (OpKill). (The package's 64 hang report breadcrumbs are not taken: Fritchii's
report keeps 256 submissions and names the oldest unfinished one, since dfba1c5c30.)

Measured (same rig, alternating rounds): verify about 235,000-360,000 reports with the
pre-passes, 0 disagreements, no real draws lost; host query slots ~36,000 -> ~14,500 a second;
9 vs 3 (the package's =1 vs =3) frame time -0.6 to -1.0 ms. One GPU hang (device lost) under PCIe load in about 50
loaded windows with the pre-passes, not reproduced since (22 min playtest and ~30 more windows
clean); cause unknown. Flag variants are not in the disk cache: each session translates them again
on the GPU thread (one a frame) and the driver compiles them on the workers.

### EDEN_NX2_GPU_PROFILE=2: E's draw method zones (helper CP)

E's draw method outside the fill (0x585's "method" share, ~1.1 us a draw at Kakariko, 218 ms/s
profiled) is split into fine zones on E: "draw route" (UnlentBlocker), "draw flush caching"
(FlushCaching before the fill), "pipeline lookup" (E's CurrentGraphicsPipeline, rule 3),
"draw fill setup" (FillRing's own work: facts, room tests, the buffer cache lock, the repack
layout, the record), "draw handle values" (the raw handle values read), "draw push" (R's
packet packed and pushed), "tick work" (TickWork after the push). Exclusive times as all zones;
"memory fill" and its fine zones stay nested inside "draw fill setup". Profile 2 only.

### FRITCHII_ACQUIRE_MET_BY (the "Acquire resumes met by" line)

FRITCHII_ACQUIRE_MET_BY (live, default 0 since 2026-11-14 = one relaxed load per resume; 1 = the
line below). On, each resume pays a page table lookup, the release tables, a scan of the noted
writes and a clock read. A resume more than 5 s after the previous counted one starts a new window.

"Acquire resumes met by" (HW_GPU, Info, every 5 s; fable_front_drains 2.3 items 2-3): for every
suspended semaphore acquire that resumes (Puller::ResumeAcquire), the release that met it, as the
front's tables tell, by kind, each as "known/unknown" a second: whether the front knew the value
when it sent it. Two groups: resumed by the back's latch (it ran a release that met the value),
and by the other routes (word read, front answer, poll drain, lagged check).
- Which release, in order: with EXP13 on, the newest release the back made for the word since
  the suspension (the QMD copy's table, as its own "met by" asks), when it meets the acquire or is
  a reduction; else the release recorded for the word (GpuSplit::NoteRelease: puller releases,
  3D payload releases and reports, compute 0x6c3 releases) when its payload meets it; else the
  newest write noted over the word in the last 2^18 ring words; else "not seen by the front" (the
  guest CPU, or a writer the front does not note). It looks, it does not prove: a table slot
  shared with another word, or an older release with the same payload, can name the wrong one.
- Kinds: puller release, 3D payload release (4 bytes), 3D payload report (16 bytes), compute
  0x6c3 release, QMD release, QMD reduction, copy semaphore, counter or sample report, other
  noted write, not seen by the front.
- Known at send: payload releases the front records (it had the payload in its registers), QMD
  and copy releases while the QMD copy predicts them (EXP13 2-4; reductions never: bounds only),
  noted writes with known words. Counter and sample reports never.
- Reading it: latch resumes by releases known at send are what resolving acquires at the front
  (resume when the newest queued release over the word has a known value that meets it) would
  take off the front's blocked time.
- With FRITCHII_ACQUIRE_TABLE_FIRST on, its resumes count under "the other routes".

### FRITCHII_ACQUIRE_TABLE_FIRST

FRITCHII_ACQUIRE_TABLE_FIRST (live, default 0 = exactly as before; fable_front_drains 2.3 item 2
and 6.3). A suspended acquire's poll (Puller::CanResumeAcquire) asks the front's release table
right after the latch: before the word read (SEM_FAST's read, which waits for the noted writers
of the word when it can't overlay them) and above the unmet-poll throttle (16 polls or 1 ms, and
EXP12=5/6's 1 ms lagged check), which held MetByQueuedRelease back until then. Every poll point
the front has asks (each command list's end, the idle loop), so the first poll after another
channel sent the release asks too. Never inside a list: that would change the channels' order.
- 1: met when the newest release recorded for the word (GpuSplit::NoteRelease) is a puller
  release (WriteLong, SEM_EXECUTE) or a 3D payload release (4 bytes) or report (16 bytes), all
  known at send, still queued, with a payload that meets the acquire and nothing sent after it
  that may write the word: MetByQueuedRelease's rule, which the acquire's parse and the throttled
  poll already use. The resumed channel's entries go into the ring after the release, so the
  back runs the release first, and the answer is the one a later poll would give from the word
  once the back ran it. Off with delayed fences (the release lands at a later fence), and for the
  16-byte report with accurate queries (its operation waits for a fence too). Compute 0x6c3
  releases stay with the throttled path.
- 2: also the QMD feed (EXP13=3/4, MetByQmdPrediction) at the same place, before the word read
  (today it is asked after it, also above the throttle). Its own rules, unchanged. When a
  prediction can be wrong: the QMD words the front's copy read differ from what the back reads at
  the launch (a writer of the QMD the copy did not see, an overlay from guest memory), the
  copy's scheduler differs from the back's (dependency counts, chained dependents, decrements),
  or a max release or reduction (a bound, not a value). The checks: the back compares every
  release it makes with its record (the feed turns itself off for the session at the first
  mismatch, an ERROR line) and the feed audits each acquire it met ("acquire wrongly met"). Both
  find a wrong answer only after the resumed channel ran, so 2 stays a test mode. When the table
  does not meet, the poll asks the feed again after the word read (that read may have waited), so
  the feed's gate counters count both asks.
- Must be 0 (1's resumes): each resume watches its word with a waiter of its own
  (Nx2SemaphoreWaiters, added before the last check); once the back ran the release (`consumed`
  past it, its async report settled) the waiter must have latched, that is the back wrote a value
  that meets the acquire. If the word already met when the waiter was added (the release landed
  first), the watch is not judged. 16 watches at most; one not settled within 2 s is dropped
  ("unresolved"). "Acquire table first (must be 0): device d op o value v: the release it was
  resumed on (payload p, ring r) ran, and no write that met it was seen; the word is w now" for the
  first 8.
- "Acquire table first (FRITCHII_ACQUIRE_TABLE_FIRST=n) 5 s" (HW_GPU, Info): resumes a second by
  kind (puller release, 3D payload release (4 bytes), 3D payload report (16 bytes), QMD feed
  exact, QMD feed other), the watches (met at the write, landed before the watch, unwatched,
  unresolved after 2 s, open) and the must-be-0 count with the session total.
- Expected: small. At the market the 256 3D payload releases and 228 QMD releases a second that
  met parked acquires were resumed by the latch: the back had run them by the next poll point,
  which the front reaches only at a list's end. What 1 can take are the polls where the release
  was queued but the word read could not show it, or waited for it (a 16-byte note over the word,
  the throttle).

### FRITCHII_GATHER_ONE_LOCK

FRITCHII_GATHER_ONE_LOCK (live, default 0 = exactly as before; fable_front_drains 8.2 G1). The
game's collected writes, applied at a gather on the back (RasterizerVulkan::OnCacheInvalidations,
EXP88's path), took the buffer cache's lock and opened and closed a memory seam bracket
(MemStage::FillPlay) once per range: 63,000 to 73,000 ranges a second at the market, 2.5 ms/s in
the lock and 7.3 ms/s closing brackets (5.4 of it one shared atomic in Fill::~Fill).
- 1: two passes over the gather's ranges. First the texture cache's WriteMemory and the pipeline
  cache's InvalidateRegion for every range, as before. Then the buffer cache's part (the reach
  test, NoteWriteUnlocked or WriteMemory, the verify check) for every range, under one hold of its
  lock and one bracket, taken at the first range that needs them (a gather whose ranges all skip
  takes neither, as before).
- Why it is exact: each cache gets the same calls in the same order. Across caches nothing
  depends on order. The texture and pipeline calls change only their own state. The buffer
  cache's WriteMemory changes its own state, guest memory (a page download, after the claim) and
  records commands; the other two read none of that. The shared part is the device pages' cached
  counts, which all three only lower here: the sum does not depend on the order, and a page is
  unprotected only when its last user lowers it, in the same call that marks that user's data
  CPU-modified. The bracket plays its entries before anything else is recorded (SeamHook::OnUse),
  so one bracket records the same commands in the same order as one per range. Other threads that
  want the buffer lock wait for the whole buffer pass instead of one range.
- What changes besides time: the seam's counters (fills per site; one Invalidation fill per
  gather) and, with packet ticks, one operation packet number per gather instead of per range.
- Count: "N gathers under one buffer lock (FRITCHII_GATHER_ONE_LOCK)" in the "Constant buffer
  path (EXP88=n, 5 s)" line.
- Expected: about 8 to 9 ms/s off the back at the market (stage off), the per-range lock and
  bracket close; the bracket's atomic runs about 1,400 times a second instead of 68,000.

### FRITCHII_GATHER_EMPTY_TEST

FRITCHII_GATHER_EMPTY_TEST (live, default 0 = exactly as before; fable_front_drains 8.2 G3). About
20,000 gathers a second reach GPU::InvalidateGPUCache at the market, 93% of them with nothing
collected; asking the four game cores' managers (two mutexes each, a std::function, a vector)
was 4.4 ms/s of the back.
- 1: before the gather (after run-ahead's own check), each manager answers
  GPUDirtyMemoryManager::NothingCollected, one lock-free 8-byte load of its `current` word. When
  all four say nothing, the gather is not made. Nothing is added on the game threads' side: the
  flag is `current` itself.
- Why it is exact: a manager holds a collected write that no gather took only while `current` is
  not the empty word {~0, 0}. Collect leaves `current` holding the write (its CAS) or, when it
  moves to another 2 KB block, pushes the old word to the back list and stores the new one in the
  same locked section; only Gather stores the empty word, in the locked section that also takes
  the back list. A write collected before the test (any chain of locks or atomics from the
  game thread's Collect to the back, which the game's submit always is) is, by the atomics'
  coherence, seen by the test as that Collect's store or a later one: a later Collect (not empty)
  or a Gather's empty word, which took it. A write collected after the test is taken by the next
  gather, as today. A second gatherer (a FlushRegion off the back) no longer waits for another
  one's collection in progress; it never waited for that one's application either, and the
  pending-write list (EXP49) still closes the gap for landing downloads.
- "Gather empty test (FRITCHII_GATHER_EMPTY_TEST=n) 10 s: gathers asked N/s, skipped as empty
  M/s (P%)" (HW_GPU, Info).
- Expected: most of the 4.4 ms/s.

### FRITCHII_FENCE_COMMIT_FAST

FRITCHII_FENCE_COMMIT_FAST (live, default 0 = exactly as before; fable_front_drains 8.3 row 3).
Every fence's FenceManager::CommitCacheFlushes takes the texture cache's lock and then the buffer
cache's (with a memory seam bracket) to find out whether a download waits to be committed:
10,606 fences a second in the market's compute stretch found none (5.3 ms/s).
- 1: each half is skipped, lock and bracket included, when its cache's note says it has nothing:
  TextureCache::MayHaveAsyncFlushWork (a download or download buffer queued since the last commit)
  and BufferCache::MayHaveUncommittedFlushes (a fence download range queued, QueueFenceDownload).
  Both notes are kept always (also at 0): raised where the work is queued, under the cache's
  lock, and set again from the containers under the lock after each commit.
- Why it is exact: a note is raised in the same locked section that adds to the containers its
  cache's test reads (texture: uncommitted_downloads, uncommitted_async_buffers; buffer:
  uncommitted_gpu_modified_ranges, which AccumulateFlushes moves to committed_gpu_modified_ranges)
  and lowered only under the lock from the containers themselves, so "note down" means the
  containers were empty at that point. With the containers empty the half does nothing but take
  the lock and open an empty bracket (should_flush false, nothing committed). Work queued on the
  fencing thread is seen in program order; work queued on another thread that does not happen
  before the fence is ordered after it, as when that thread takes the lock second. The skipped
  bracket held no entries; pending entries are played before anything else is recorded either way.
- "Fence commit fast (FRITCHII_FENCE_COMMIT_FAST=n) 10 s: commits N/s, texture half skipped T/s,
  buffer half skipped B/s" (HW_GPU, Info).
- Expected: about 3 to 4 ms/s (both lock takes at most fences in the compute stretch).

### FRITCHII_CB_WORDS_BLOCK

FRITCHII_CB_WORDS_BLOCK (live, default 0 = exactly as before; fable_perdraw_b 3.1). About 1.36
million 3D constant buffer data words a second (0x8e4 to 0x8f3: the command list's increasing
uploads, the draw macros' base vertex and instance words) each went through the split's ring as
its own Method entry.
- 1: Maxwell3D::FrontWrite puts such a word in the open CbWords block (GpuSplit::CbWord) as the
  very Method word its entry would have carried (Direct and last-call from a macro, the command
  list's last-call bit otherwise), up to 64 words a block. Everything else in FrontWrite stays:
  the front's register copy and offset per word, the NoteWrite, RangeLastWrite and
  NoteMethodWrite per word, each at the word's own position (every word is committed as its
  entry was). The block ends at anything else reserved (another entry, an engine switch, held
  back work first), at a publish, and when the sent position is read (SentPosition). The back
  runs the block as one entry: its words go through Execute as the entries in a row did, whose
  constant buffer gather takes them in one CallConstBufferWords (or word by word where the
  gather is off). The memory stage follows the block as one entry, each word through the same
  CallMethod as a Method entry.
- 2: as 1, and each block carries the front's address of its first word. The back compares it
  with its registers after the sink (the address the front's notes used), then applies the
  block, records the offset, the 16 data registers, their shadow RAM words and the dirty flags,
  checks memory holds the words' values (when it wrote them itself, not served by the memory
  stage), puts the state back and applies the words one entry at a time, and compares. Not
  applied twice: one word, a deferred draw waiting (it can draw once), the gather off.
- Why it is exact:
  - Order and position: the words sit in the ring in the same order between the same other
    entries. The back stores `consumed` only at entry ends, so a note at a word's position inside
    a block counts as run exactly when the whole block ran: waits end at the block's end, never
    before the word ran. The notes themselves (range, data, position) are the same calls.
  - Dirty flags: each word still goes through ProcessShadowRam and ProcessDirtyRegisters, in
    order; the first word issues a waiting deferred draw before any word is uploaded, as its own
    entry did. Checked by =2.
  - Same-value skip: constant buffer data is executable, so neither the front's skip nor the
    back's (state registers only) ever applied; EXP107's memory compare runs per
    CallConstBufferWords call on the back, in order, as with the gather (a call over different
    words only changes whether equal bytes are written again).
  - Shadow RAM: the block carries the raw argument as the Method word did; the back and the stage
    apply the shadow RAM per word. The mode cannot change inside a block (shadow_ram_control is
    another method, which ends it).
  - Macro reads of the offset and data registers read the front's copy, which FrontWrite still
    updates per word. Macro DMA reads see each word's note at its position, as before.
  - EXP88: WriteBoundConstantBuffer gets the same calls the gather made; its device ranges merge
    where they touch, and FlushCaching invalidates them at the next draw, which cannot come
    inside a block.
  - FRITCHII_FRONT_RAN_READS: the noted writes it scans (reports, inline uploads) are never made
    for constant buffer words, and a page snapshot's position is SentPosition, which ends the
    block, so no snapshot or noted write position falls inside a block.
  - Packet ticks: no draw, closure or seam bracket can fall between a block's words, so the
    words' ranges reach the same draw's FlushCaching (back) or the same fill (stage) as before.
  - Memory stage (FRITCHII_MEMSTAGE_THREAD=2): the block is one entry, served or lent as a whole.
    A word that makes it hard (a watched page, GPU-written data, EXP107) makes all of it hard:
    the back then writes every word, the ones the stage wrote already being the same bytes.
- "CB words block (FRITCHII_CB_WORDS_BLOCK=n) 10 s: front blocks B/s with W words/s (W/B a
  block), ring entries saved S/s, blocks ended full F/s; ranged write overflows O/s; back
  totals: blocks, words; must be 0: broken blocks, bad blocks; verify (=2): addresses checked,
  differ; applied both ways, differ, not applied twice" (HW_GPU, Info). Broken blocks (a commit
  after a block that did not end it), bad blocks (words that are not constant buffer data) and
  both differ counts must stay 0.
- Expected: the back's constant buffer gather already took words that arrived together, so the
  back saves the per-entry decode and the splits at early publishes (the front published every 8
  words to a waiting back); the front saves the engine select and reserve per word. The ranged
  write overflows are not expected to drop: RangeLastWrite already extends the last range for
  each next word, so they count runs to other places (the line shows them for the A/B).

### EDEN_NX2_ACCURATE_PARTS

EDEN_NX2_ACCURATE_PARTS (bit mask, 0 = none, read once at boot): parts of GPU accuracy High in
Fast mode, the split GPU thread kept, to find which one stops a flicker that Accurate mode
stops. Not live: switching the counter query mode mid-game crashed (queries opened in one
mode were closed in the other).
1 = counter reports written once their host query completes (synced), 2 = precise occlusion
queries, 4 = CPU reads of GPU-written images download them, 8 = CPU writes flush GPU data
before invalidating it, 16 = fences wait for the GPU work before them.

### EDEN_NX2_HOST_PACE

EDEN_NX2_HOST_PACE: the host frame pacer (vk_host_pacer.h). 1 = on (default), 0 = only
measure how long each frame was shown.

### FRITCHII_PACE_FIX

FRITCHII_PACE_FIX (live, default 0; bits): fixes for the capped 60 FPS judder at 120 Hz (~1.5% of
frames shown 3 refreshes, 25 ms; capped 1% low 40). The pacer's lead (how long before the
intended refresh a held frame is presented) adapts +0.5 ms per miss and -0.01 ms per hit, which
settles where ~2% of frames miss by design; every miss is a 25 ms frame and moves the schedule a
refresh later (the backlog that the catch-up and EXP70's drops then clear).
- 1 = present early: hold a frame only until just after the refresh before its intended one
  (lead = a refresh minus a guard of 15% of a refresh, 1-3 ms, raised 0.25 ms whenever a frame
  shows a refresh early). The frame is still shown at the same refresh (no extra input lag), but
  the copy and the compositor get ~7 ms instead of ~3. The catch-up looks at when the frame
  reached the present thread instead of when the thread woke.
- 2 = copy before the hold: the present thread copies the frame to the swapchain image and
  submits that before the hold and holds only vkQueuePresentKHR, so the copy's GPU time and
  the submit lock are out of the lead.
The "Host pacing" line ends with the proof: "misses: catch-up / woke late / slow copy / after
present (time left at present)", "copy to present" mean/max, "game late" (frames that reached
the present thread after their refresh) and "early" (shown a refresh early).

### EDEN_NX2_ARB_SPIN

EDEN_NX2_ARB_SPIN (ported; off by default here, the original uses 8): microseconds a
guest thread about to sleep in WaitForAddress first watches the word (KAddressArbiter), 0 =
off. Live: read on every wait, so profile_toggle.py can A/B it in one session.

### EDEN_NX2_IDLE_PAUSE

EDEN_NX2_IDLE_PAUSE=1 (ported; off by default here, on in the original): the guest
core's idle poll (PhysicalCore::Idle, EDEN_NX2_IDLE_SPIN_US) runs PAUSE and reads the clock
every 16 pauses, instead of a SwitchToThread and a clock read per iteration. Live: read on
every Idle.

### EDEN_NX2_GPU_WAKE

EDEN_NX2_GPU_WAKE=1 (live, ON by default since 2026-11-14, built in with EXP12 = 4; ported): the GPU
thread (the split's front) waits for suspended channels in a wait that ends on a wake, instead
of polling them (ResumePending, then yield or sleep per EXP6, ~120-150 ms a second of front
time in Ocarina of Time). A new command (PushCommand) or a GPU write that latches a waiter
(Nx2SemaphoreWaiters::OnWrite) wakes it at once (Tegra::Nx2PendingWake); a guest CPU store to
the word a channel waits on ends the wait within ~2 us (Nx2SemaphoreWaiters::WordStored). For
500 us after the last progress it waits in hardware (MWAITX/UMWAIT, Common::WaitForFlag) with
a full poll every 20 us, then blocks in the OS 500 us at a time (Common::Event::SleepFor).
0 = the polling loop as before (EXP6 applies), and the wake sources return at once. Read on
every wait, so profile_toggle.py can A/B it in one session.

### EDEN_NX2_BACK_FENCE_STATS

EDEN_NX2_BACK_FENCE_STATS=1 (live, diagnostic, off by default; fable52_answer_fences2 4): the
back's SignalFence timed in five parts (CommitCacheFlushes with DownloadStagingBuffer, guard,
the fence's operation, FlushCommands, InvalidateGPUCache), summed per 5 s ("Back fence split
(5 s"), and every fence over 1 ms with its parts ("Back fence over 1 ms", 30 lines per 5 s at
most). Also counts the fences that committed no download (FenceReleaseStats::BackFence).

### EDEN_NX2_RUNAHEAD

EDEN_NX2_RUNAHEAD (live, Ocarina of Time only, default 0; video_core/nx2_runahead.h): 1 = the
split's front answers acquires a release it sent meets without draining and writes the word
early, with the guest's CPU writes snapshotted per boundary for the back; 2 = verify (old path,
chunks compared with guest memory at their stamp); 3 = early answers without writing the word.

### EDEN_NX2_RUNAHEAD_MS

EDEN_NX2_RUNAHEAD_MS (live, default 16): how far the back may lag the oldest early answer.

### EDEN_NX2_SWEEP_UNMAPPED

EXPERIMENT, off by default (2026-11-14): EDEN_NX2_SWEEP_UNMAPPED (read at startup;
TextureCache::SweepUnmapped). An unmap of GPU memory flags the images in it Remapped and
CpuModified; lookups skip them, so most sit until the collector's age limit (50 frames, 10 near
the budget). Ocarina of Time at 3440x1440 unmaps and maps its render targets again every frame
or two in the Castle Town guard house and the dead sets overflowed a 12 GB card. 0 = today. 1 =
at each frame end, delete flagged images untouched for 2 frames, without a download, when
nothing can still need their contents: still CpuModified (nothing uploaded into it or wrote it
whole since the unmap, so neither the collector nor a guest read would ever download it), no
aliases, not among the last 8 images shown, not a bound render target, no download queued, no
decode in flight, not sparse or unbacked, not taken by EXP121. 2 = verify: deletes nothing,
marks the images 1 would delete and counts what reached them later (found again by EXP76 & 1,
uploaded into, and of those GPU-written before the unmap, presented, written whole, deleted
unused by another path). The "Texture cache last 600 frames: GPU memory unmaps" line (with
EDEN_NX2_TC_LOG or this switch) counts unmaps that hit images, images newly flagged, found
again, uploaded into, downloaded, deaths by path (collector, join, unmap, other) with how many
were unused since the unmap, frames and KiB of the collector's unused deaths, and the memory
flagged images hold (now, average, peak). With 1 or 2 a second line, "unmapped sweep", gives
deletions and MiB, entries looked at, and 2's counts. Judge with 2 first: "uploaded into ...
GPU-written before the unmap" and "presented" should stay 0 (an upload into a deleted image's
replacement may be skipped as stale where the kept image would load guest memory, both garbage
next to the lost GPU data), then 1 against 0 for device memory and fps on 8 GB cards. With
EDEN_NX2_IMAGE_HASH_LOG, "unmap sweep" in the 10 s delete lines shows sampled images made again
where the sweep deleted one.

### EDEN_NX2_BUFFER_CHURN_STATS

EDEN_NX2_BUFFER_CHURN_STATS=1 (live, diagnostic, off by default; Fable's ideal pipeline design
8.3): buffer cache creates and deletes per second by cause, and staging buffer creates by
caller (video_core/buffer_churn_stats.h). "Buffer churn (5 s" every 5 s while on.

### EDEN_NX2_CB_DIRECT

EDEN_NX2_CB_DIRECT (live, default 1; ported): a run of constant
buffer words within one device page outside Switch 2 sparse memory is written through one
translation and a memcpy instead of WriteWordsCached's page walk (Maxwell3D::
CallConstBufferWords). Exact: the same bytes and the same accumulated invalidations; 0 = the
page walk.

### EDEN_NX2_RT_DEPTH_NOUPLOAD

EDEN_NX2_RT_DEPTH_NOUPLOAD (live, bits, default 0; ported): new
depth/stencil images skip the upload from guest memory when the exact rules (EXP56's clear,
EXP8 bit 2's copies) did not already (TextureCache::JoinImages). 1 = render targets with
nothing to join, 2 = images whose every join copy is GPU-written, 3 = both.

### EDEN_NX2_FOLD_INSTANCE

EDEN_NX2_FOLD_INSTANCE (live, default 0; ported): an instance loop
of Ocarina of Time's instanced draw macros (0xe52, 0xea0 by code hash) does not draw its
first instance alone; the loop's deferred draw covers it (Maxwell3D::FrontRunMacro,
DrawManager::FoldFirstInstance). 1 = on.

### EDEN_NX2_OOT_MACROS

EDEN_NX2_OOT_MACROS (live, default 1 since 2026-11-12; ported, its macro_hle_oot.h): Ocarina
of Time's hot 3D macros, found by code hash, run through native C++ handlers on the split
front instead of the MME JIT (Maxwell3D::FrontRunMacro). 1 = handlers, 2 = verify (the JIT's
writes count; the handler runs on copies for the first 4096 calls of each macro and then one
call in 64, and differences are logged: "OoT macro verify"), 3 = verify every call.

### EDEN_NX2_DRAW_PACKET

EDEN_NX2_DRAW_PACKET (live, default 0; Fable's back-thread plan, Build C step 1): the
register writes of an OoT draw macro call (0xe04, 0xe52, 0xea0, identified by code hash) that
the front would send as single Direct entries go as one ring entry the back applies in one
call, in the same order through the same CallMethod (Maxwell3D::ApplyDrawPacket). Constant
buffer words and anything else still go one by one, splitting the packet. 1 = on, 2 = on and
the back checks each packet's registers against the values the front wrote ("Draw packet
verify").

### EDEN_NX2_BANK_SLOT_STATS

EDEN_NX2_BANK_SLOT_STATS (live, default 0; recomp research idea 1, step
1): with the tracked native bank check (EXP9 != 0), counts per constant buffer index (c0, c1,
c2, ... as the shader numbers them; NVN2: c0 driver storage table, c1 program constants, c2
texture handles, c3+ the game's uniform buffers) the bank binds, the pages they cover and
the pages compared and found changed for each reason, logged every 5 s next to the "Native
bank check" line ("Native bank slots"). Counting only (BufferCache::
CheckNativeBankPagesTracked). 1 = on.

### EDEN_NX2_BANK_PAGE_TABLE

EDEN_NX2_BANK_PAGE_TABLE (live, default 0; the recomp's page table,
research idea 1 step 3): the native bank check's page snapshots live in a page-indexed table
(2 MiB regions of 512 page slots, the last region kept) instead of a node map hashed per page
(~1.9% self of the back in the 8 Nov sampler, 720k lookups/s at Castle Town). Exact: the
same snapshots, the same compares; a change of the switch empties both stores, so the next
bind of each page takes a new snapshot and marks it changed, as after the 16384-page reset.
1 = on.

### EDEN_NX2_BANK_SLOTS

EDEN_NX2_BANK_SLOTS (live, default 0; recomp research idea 1): a bit
mask of constant buffer indices (bit n = cn) whose 64 KiB native bank binds rely on the write
flags alone, for the banks NVN2's driver fills only through the command stream (c0 storage
table, c2 texture handles) or never rewrites (c1 program constants). For those binds, with
EXP9=3 (the default): (a) the "hot" rule (pages that changed lately are compared on every
bind even with no flag set) is dropped, and (b) with EDEN_NX2_BANK_PAGE_TABLE=1 a bind whose
whole range has no collected CPU write, no buffer cache write (our own constant buffer words
among them, at FlushCaching), no recheck pending and no CPU-modified page, with every page
already snapshotted, skips the page loop at once (BufferCache::CheckNativeBankPagesTracked).
The game's own banks (c3+) keep the full check: never set their bits (the white flashes,
d79572d28a). Pick the mask from EDEN_NX2_BANK_SLOT_STATS: indices whose "hot" changes stay 0.
0 = off. Suggested after that check: 0x7 (c0, c1, c2).

### EDEN_NX2_BANK_SKIP_VERIFY

EDEN_NX2_BANK_SKIP_VERIFY (live, default 0): with EDEN_NX2_BANK_SLOTS, every bank the skip
passes over and every hot page the slot rule leaves alone is still compared; changes are taken
as before (the page is marked) and counted as "missed", which must stay 0, and pages of a
skipped bank that the page loop finds a reason for are counted as "disagreed" (must stay 0).
Logged every 5 s ("Native bank slot skip"). Costs what the skip saves. 1 = on.

### FRITCHII_BANK_CENSUS

FRITCHII_BANK_CENSUS (live, default 0; counters only, nothing the check does changes;
fable_perdraw_b cut 5 step 1). The tracked native bank check (EXP9) compares a page on every bind
for HotFrames frames after any change ("hot"), because some writer of the game's banks sets no
flag: 637,555 hot compares a 5 s window found 328 changes (21.7 ms/s of compares on the back).
The census says, for each change the check finds, what touched the page since its last compare.
- 1: changes by the reason that found them, and for the unflagged ones (hot, frame check, verify
  only, EDEN_NX2_BANK_SKIP_VERIFY's missed) the evidence: binds since a collected CPU write or
  buffer cache write flag was last taken on the page (a trapping writer marks before it stores:
  its bytes can land after the compare that took the flag and after the one recheck), an upload
  (UploadMemory) that read the page from guest memory after the last compare (a guest store
  between that compare and the upload: the host buffer may hold it already), a GPU write mark
  ever on the page, and "unseen" when no late flag, no upload and (at 2) no emulator write
  explains it (a GPU write mark explains nothing by itself). Per constant buffer index (c3+
  are the game's), unflagged and unseen.
- 2: 1, and the write ring turned on (all emulator writes to game memory, with their site): the
  newest ring entry over the page since its last compare, by site and write function. The ring
  also marks each page it records (Common::g_ring_write_pages, taken at each compare), so an
  emulator write whose entry the ring wrapped over counts as "ring writer lost", not as
  unseen. The ring costs every emulator write a few stores: fps are not comparable. Not
  together with FRITCHII_MEMSTAGE_READ_LOG (the ring is left on until the census goes below 2
  only when the census turned it on).
- When no change is found it adds one flag take and two stores per compared page (one more
  shared load at 2), and one load per upload. At 0, one load per bank bind.
- "Native bank census (FRITCHII_BANK_CENSUS=n, 10 s): changes: collected C, buffer cache W,
  recheck R, CPU-modified M, hot H, frame check F, verify only V (slot skip missed S); unflagged
  U: flag taken <=4 / <=32 / <=256 / more / never binds before a / b / c / d / e (late flag L, of
  them buffer cache B), uploaded since the last compare P (that compare CPU-modified Q), ever
  GPU-written G, ring writer named X, ring writer lost Z (ring wrapped Y), unseen N (first
  compare this frame N1, ever GPU-written N2); slots (unflagged / unseen): cK n / u ... | site /
  via: n ..." (HW_GPU, Info). Counts start at the census' first bind; pages first seen before
  it count their flags from their next compare.
- First run (market, build 5ffb7a1d35, 2): ~1,385 unflagged changes per 10 s, c6 1,362, c7 16;
  ring writer 1,044-1,072, all "GPU write: query-completion" (report values the query cache
  writes into the game's c6 bank when a host query completes, which set no bank flag);
  uploaded since the compare ~1,005; late flag 7-8; unseen 8-9 with 322-334 ring wraps. Fixed
  by FRITCHII_BANK_FLAGGED's marks (below), whose gate the census opens too: a census run sees
  the marked writers as flagged.

### FRITCHII_BANK_FLAGGED

FRITCHII_BANK_FLAGGED (live, default 1 since 2026-11-14 via the built-in switches in src/yuzu/main.cpp; fable_perdraw_b cut 5 step 2). The game's banks keep
the hot rule (every page that changed in the last 120 frames compared at every bind: ~600,000
compares per 5 s for c6, 21.7 ms/s on the back) because writers set no flag. The census named
them; these marks set the bank check's flags, each after its bytes landed, while the bank marks'
gate (Common::g_bank_marks) is open, which is while FRITCHII_BANK_FLAGGED or
FRITCHII_BANK_CENSUS is non-zero (the bank check sets it). Shut, each site is one relaxed load
and a branch:
- the write ring's Note (every emulator write to game memory, any thread: query completions,
  immediate and semi-flush report writes, device and GPU block writes, span stores, our own
  constant buffer words, downloads, DMA, blits) marks g_buffer_write_pages;
- MemoryManager::Write (the 3D engine's stamps, compute and QMD releases) marks it;
- unsafe guest memory writes (Core::Memory::WriteBlockUnsafe) and in-place span stores
  (GuestMemoryScoped, Core::Memory::NoteHostStore) on rasterizer-cached pages go through the
  ring's Note also while the gate is open, not only while the ring or a uniform keep is on;
- the guest store's after-store point (UniformKeepPages::AfterGuestStore) marks
  g_collected_write_pages again for the ranges HandleRasterizerWrite recorded before the store.
  A store whose ranges did not fit (more than 4) moves Common::g_collected_overflows.
With both switches at 0 nothing changes. Open, the marks only add compares that find what the
hot rule would have found (or a real change no rule saw). A writer that loaded the gate shut set
no mark, so no snapshot from before the gate opened is trusted: two frames after it opens (every
writer's next load sees it by then) the bank check drops every snapshot once, and the flagged
rule below works only from then on.

With the switch on (Vulkan's mapped uploads only):
- 1: the constant buffer indices of FRITCHII_BANK_FLAGGED_SLOTS (live, bit n = cn, default 0x40
  = c6, the only slot the first census measured) get EDEN_NX2_BANK_SLOTS' rule: no hot rule,
  and a bank with no flag, no recheck, no CPU-modified page and every page snapshotted is
  skipped whole. Not while CPU inner invalidation is skipped.
- Every upload that stages guest bytes for a host buffer (MappedUploadMemory, and the inline
  writes of InlineMemoryImplementation) copies them into the bank snapshots of those pages
  (RefreshNativeBankSnapshots): a snapshot holds what its page's host copy holds. A refresh that
  changed bytes makes the page hot, as the change found at its next bind used to. The bytes come
  from cached memory, never back out of the staging buffer (write-combined: the first version
  read it back and spent 60% of the back thread there, 59.7 -> 28.4 fps). A copy that covers no
  snapshot page (the table's present bits, 64 pages a word) goes straight to staging as before;
  one that does is read from guest memory once into a scratch buffer, copied to staging, and
  compared and copied into the snapshots of the pages it covers, only the overlapping bytes.
- When it turns on, and whenever g_collected_overflows moved, every snapshot is dropped: each
  page's next bind takes a new one and marks the page changed (as the 16384-page reset does).
- 2: 1, and every page of the flagged slots without a reason is compared anyway (not only the
  hot ones): a change is taken as any change is and counted as "missed". "never flagged" and
  disagreed "recheck" / "CPU-modified" must stay 0; "then flagged" and disagreed "collected" /
  "buffer cache" are writes racing the bind (below) and grow with fps. Costs more than the hot
  rule did.
- "Native bank flagged (FRITCHII_BANK_FLAGGED=n, slots S, 5 s): B binds, K skipped whole, H hot
  pages not compared; verify: V compared, M missed (then flagged F, never flagged U), D
  disagreed (collected a, buffer cache b, recheck c, CPU-modified d); uploads refreshed R pages
  (C changed); snapshots dropped N times so far" (HW_GPU, Info).
- What a non-zero "missed" or "disagreed" means. Writers on other threads mark the page flags
  without the bank check's lock, after their bytes land. "Missed": a verify compare found
  bytes that landed during this bind, before their writer's mark; the mark reaches the page at a
  later bind (the page is watched for 8 visits: "then flagged"). That bind's draw is not ordered
  after the write, so either contents are right for it (the hot rule caught such writes only by
  timing). "Never flagged" is a writer no mark covers and must stay 0. "Disagreed": the
  whole-bank skip's flag look (Any) and the loop's take (Take) are two reads; a mark set between
  them shows as a collected or buffer cache reason in the loop, and stays set for the next bind
  when the bank is skipped. Recheck or CPU-modified there would be a real disagreement. First
  verify run (market, d5d510bb9c, 26 fps): 2 missed and 5 disagreed in 28 windows of 5 s, the
  census at the same build 1 unflagged change per 10 s, a late flag. With the refresh from
  cached bytes (0d73686ef2, 61 fps): 12 missed, all then flagged, never flagged 0, and 200
  disagreed, all collected, in 29 windows (16x per bind: the back no longer stalls the guest, so
  more guest stores overlap the bind loop). The refresh cannot cause either: it runs on the
  bank check's thread under the same lock, gives the snapshot exactly the bytes it stages, and
  touches no flag, recheck or present bit (the whole-bank skip's inputs); its present-bits gate
  (NativeBankAnySnapshot) covers every page FindNativeBankSnapshot would find.

Why no stale bank reaches a draw on a flagged slot. The bank check marks a page CPU-modified
(so the bind's sync uploads it) when the page's guest bytes differ from its snapshot. The rule
compares a page only when a flag, a recheck or the tracker's CPU-modified state says so. So it
is exact if (a) the snapshot equals the host copy whenever no flag is pending, and (b) every
change of the page's guest bytes is followed by a flag or CPU-modified state that a later bind
sees. Every bind ordered after the write sees it: flags are set after the bytes land, a flag
is cleared only by the Take that reads it before the compare, and a mark that is set after a
Take's look stays set for the next one. A bind that runs at the same time as the write and is
not ordered after it may run before the mark, and so before the bytes; the hot rule saw such a
write only when the timing happened to fall its way. (a): snapshots are taken from guest memory
and the page marked changed, so the sync uploads at least those bytes; after that every upload
or inline write copies the bytes it gave the host into the snapshot. GPU-written pages are not
checked (the bind skips the bank while any of its range is GPU-modified). (b), writer by writer:
- guest CPU stores to protected pages trap: HandleRasterizerWrite marks before the store, the
  after-store point marks again after it (the late landings the census saw, 7-8 per 10 s);
- guest CPU stores to unprotected pages: the page is CPU-modified until a sync re-protects it
  (protection is set before the upload reads the bytes, so a store after the read traps);
- every emulator write the write ring hears of (the ring's contract: every write emulator code
  makes to game memory, after it landed) and every MemoryManager::Write;
- our own constant buffer words: the ring's Note at the write, and WriteMemory at FlushCaching.
What is not covered is a writer outside all of these: a guest store through a mapping the
rasterizer does not protect, or emulator code storing through a raw pointer without a Note. The
census at 2 counts those as "unseen" per slot, which is why a slot goes into
FRITCHII_BANK_FLAGGED_SLOTS only after a census run shows unseen 0 for it, and why =2 compares
every page.

### EDEN_NX2_TEX_BANK_GEN

EDEN_NX2_TEX_BANK_GEN (live, default 0; the recomp's per-stage
texture bank generations, research idea 2): a graphics pipeline whose bindings are all sampled
textures with handles read straight from constant buffers (no handle programs) keeps the view
and sampler ids its last draw resolved, with the generations of the pages that draw read
handle words and TIC/TSC entries from (TextureCache::TexBankPage: watched pages trap CPU
writes; our constant buffer words, buffer cache writes and InvalidateRegion mark them too).
A draw with the same handle constant buffer bindings, the same TextureMemoState (pools,
limits, channel, mappings, no view removed), the same sampler binding mode and every page at
its generation takes those ids: no handle reads, no table reads, no GetSamplerId; the views
are still prepared (PrepareImageView). A miss costs the compares. Vulkan GraphicsPipeline::
ConfigureImpl; not with EXP32. 1 = on, 2 = on and every 16th hit resolves the full way and
compares ("Texture bank memo mismatch", must stay 0). Logged every 10 s ("Texture bank memo").

### EDEN_NX2_STORAGE_SYNC_SCAN

EDEN_NX2_STORAGE_SYNC_SCAN (live, default 0): storage buffer synchronizes (graphics, and
compute while EXP59 is off) walk the memory tracker only from the first to the last word of
each 4 MiB manager that holds a CPU-modified or unprotected page, found by reading the words
(MemoryTrackerBase::ForEachScannedUploadRange). The words left out hold neither, so the full
walk would upload nothing there, re-protect nothing and store back what they hold: the same
result without the stores. Not the CleanWords walk (that one trusts clean_words). The sample
capture of 10 Nov had 72% of the graphics storage binds in that walk over clean words.
1 = on, 2 = the full walk as before, after counting the CPU and unprotected pages the scan
would have left out ("Nx2 storage sync scan verify" every 10 s, must stay 0).

### EDEN_NX2_STORAGE_SYNC_MEMO

EDEN_NX2_STORAGE_SYNC_MEMO (live, default 0): a graphics storage binding synchronized while
the dirty generations of the 4 MiB tracker managers it covers stayed put (WordManager::
dirty_gen: bumped whenever a page gains a CPU-modified or unprotected bit) is clean after it,
and stays clean while that sum holds: the next bind of the same range skips SynchronizeBuffer,
whose walk would find nothing (BufferCache::SynchronizeStorageMemo; 512 entries keyed by
range). Not with EXP48's hot pages (they stay dirty) nor with the CleanWords walk unless
EDEN_NX2_STORAGE_SYNC_SCAN=1 picks the walk. A miss costs a sum over the managers twice.
1 = on, 2 = every hit synchronizes anyway after checking that no word of the range holds a
CPU-modified or unprotected page ("Nx2 storage sync memo verify" every 10 s, must stay 0).

### EDEN_NX2_FEED_MEMORY_OVERLAY

EDEN_NX2_FEED_MEMORY_OVERLAY (live, default 1; fable_answer_pair_2 section 2): the EXP13=3/4
feed takes records whose QMD the copy read from an overlay built from guest memory with a
small upload patched over it (QmdPrediction::Source::OverlayFromMemory; OoT's per-dispatch
12-byte release patch at QMD word 52). Only overlays built while the feed and this switch were
on and no unbounded writer was queued over the QMD (GpuSplit::QueuedGuestWriters) count; the
back verifies every release either way (QmdPrediction::Verify). 0 = refused as before. EXP127 = 2
adds sources under this switch (EXP127).

### FRITCHII_FEED_REARM

FRITCHII_FEED_REARM (live, default 1): the EXP13=3/4 feed that turned itself off comes back on
after 10 s with no new divergence (a bound violation, payload mismatch, unpredicted back release,
phantom record: each one while off starts the 10 s again), when the audit says nothing it met
was wrong: no acquire judged wrongly met since the turn-off, none named by the scans at the
turn-off or since, every decision made before it judged, no violated bound still being re-read.
The words a turn-off named stay out of the feed for the split session. Off for good: the third
turn-off in a split session, lost events, more than 64 words kept out, a scan naming an acquire
(for the split session); an acquire the audit judges wrongly met (for the run: it also turns the
feed off). A new split session (the game booted again) starts with the feed on. Lines: "feed
(EXP13=3/4) off (n of 3 this split session)", "back on (FRITCHII_FEED_REARM=1)", "stays off
for the ...", and every 5 s after a turn-off "feed re-arm (FRITCHII_FEED_REARM=1)". 0 = off for
the rest of the run, as before (an EXP13 change still turns it back on).

Not this switch: a new split session's ring positions start at 0, and the predictor's horizon
(QmdPrediction::Settle) now starts over with them (QmdPrediction::StartSplitSession). Kept, it
stopped every back release from waiting for its record until the new ring passed the old one,
so the first one that ran before its record turned the feed off minutes after a game reboot.

### FRITCHII_TEST_FEED_OFF

FRITCHII_TEST_FEED_OFF (live, default 0, test only): every N seconds of feed on, the feed turns
itself off as for an unpredicted back release of no word ("test: FRITCHII_TEST_FEED_OFF=N, no
real release"), to see FRITCHII_FEED_REARM bring it back (and stay off after the third).

### FRITCHII_TEST_SPLIT_RESTART

FRITCHII_TEST_SPLIT_RESTART (live, default 0, test only): every N seconds of a GPU thread split
session, the GPU thread drains and stops the split (the stop an accuracy change takes mid-game)
and the next loop starts a new session, ring positions from 0, as a game reboot does. For the
predictor's session start (QmdPrediction::StartSplitSession). Line: "GPU thread split: stopped
for FRITCHII_TEST_SPLIT_RESTART".

### EDEN_NX2_QMD_3D_PATCH

EDEN_NX2_QMD_3D_PATCH (live, default 0; fable_answer_pair_2 3.1): a 3D inline upload over a
QMD the EXP13=2-4 copy holds retires its own 3D method-write mark (GpuSplit::
RangedMethodWrite, which runs right after anyway) before the copy patches the holders
(Nx2PatchQmdCopy). Without it the holder's write-stamp check counts the upload's own method
write as a later writer, so no holder ever takes a 3D patch and the QMD read waits. 1 = on.

### EDEN_NX2_LIGHT_TABLE_ARRAYS

EDEN_NX2_LIGHT_TABLE_ARRAYS=1 (boot, default 0; ported):
a texture handle the shader reads from a storage buffer table at an index each invocation
reads for itself (OoT's clustered lights: each light's shadow map handle, read per pixel from
the light table) becomes a descriptor array of the table's first 64 entries, indexed
NonUniform, instead of binding the null image, whose depth-compare sample is 0:
fully shadowed, the dark grid that follows Link over water. With it on also: depth-compare
samples of uncomparable images (garbage table entries) bind the null image, view lookups skip
repeated table elements, a pipeline's texture arrays fit the sampler limits too, and dual
vertex programs join their handle programs. Read once, on first use (PipelineCache's
constructor, after nx2_switches.txt is applied): it changes what shaders translate to, so it
is not live. Off, translation and binding are exactly what they were before the port.

Safe to turn on (helper AF, 2026-11-15): kept pipelines (precompiled_infos,
EDEN_NX2_PRECOMPILED_FAST) are stamped with it when on (" light-tables 1" in KeptStamp), so the first boot after turning
it on or off prepares them again instead of drawing with the other setting's translation.
handle_is_pointer_word and is_descriptor_array are kept with the infos (bits 1 and 2 of
is_depth's byte; both false with it off, so kept files are unchanged). Delta draws: a pipeline
with a handle table never takes one (GraphicsPipeline::HasHandleTable in DeltaPathDecide;
EXP118 only takes pipelines that sample nothing anyway). The texture memos: EXP87 templates and
TEX_BANK_GEN refuse pipelines with handle programs, EXP32 compares every element's handle, and
FRITCHII_HANDLE_PROG_KEEP never keeps a table (more than its 32 reads), so each draw evaluates
the table again. The fast draw count's ComputedHandlesChanged checks every element.

### FRITCHII_BARE_SSBO_MIB

FRITCHII_BARE_SSBO_MIB=<n> (live, default 16; EDEN_NX2_BARE_SSBO_MIB, CheezyEmu's name, is read
when it is unset): how far a storage binding of a bare pointer (a Switch 2 program's pointer,
no size word after it) reaches past the pointer, in MiB, 8 to 1024 (0 or 8 = the old 8 MiB
cap). Written bindings stop at 16 MiB at most. OoT's light/decal cluster list is mapped about
9.5 MiB past its pointer: a compute program writes it and the water and the mist over it read
it, so at 8 MiB the far entries were never written and read zero or old values (stair-stepped
tiles around Link in water, waterfalls and river mist). Port of CheezyEmu 6aaf9f243f and
5532ec295e (CheezerWheezer). The first 24 different bindings the old cap cuts are logged
("Bare compute/graphics storage binding at ...: mapping ..., bound ..."). Cost: read-only
bindings past 8 MiB synchronize up to twice the range per bind (clean ranges are skipped by
EDEN_NX2_STORAGE_SYNC_MEMO); a written graphics binding is marked GPU-modified in full; a
written compute binding only over its proven write extent. Measure 8 vs 16 live.

FRITCHII_BARE_SSBO_CLAMP=1 (live, default 0): a bare pointer's binding stops at the first device
page with no host backing (pages the unmapped device read lines report "read as zeros"); none
backed at its start makes it the null binding. The answer then also holds only until the device
backing changes. The first 24 are logged ("Bare compute storage binding at ...: host-backed ...:
clamped / null binding"). Not exact for written bindings (writes past the end are dropped, and
reads past it are robust-access values instead of the zeros the host buffer held): a test for
the AMD tester's market load, which read a 16 MiB bare compute window of unbacked pages
(0x1aeaf4000) on every load. The unmapped device read lines now name the read's site and context
("buffer upload in compute storage bind", "extent probe", "immediate buffer") and its caller
(exe+offset, for the PDB or map).

### FRITCHII_MEMSTAGE_SPLIT_ALLOC

FRITCHII_MEMSTAGE_SPLIT_ALLOC (boot, default on, =0 off; Fable's ideal pipeline design 2.4,
build step 3): the buffer cache runtime gets a VMA allocator of its own (Device::
GetBufferAllocator) and a staging pool of its own on it, so the future memory stage can own
them on its thread. On it: the buffer cache's buffers (EXP104's pool included), quad and
uint8 index buffers, vertex repack and multi-range buffers, the null buffer, the uniform
stream and buffer uploads and downloads. The two list draw buffers (list draw arguments, kept
list words) belong to the memory stage's player (EXP123) and are on the main allocator. Images, the texture cache, query cache, descriptors,
mesh emulation, upscalers and diagnostics stay on the main allocator and staging pool. The
back's pool keeps its full stream buffer (128 MiB, texture uploads up to 8 MiB stay in the
ring); the buffer cache's pool takes half that (64 MiB), so a discrete card reserves 64 MiB
more. On integrated GPUs the two split the one stream buffer instead. EXP104's byte caps count
both pools' recyclers together, and the new allocator takes no memory until used. Read once
when the device is made.

### FRITCHII_MEMSTAGE_SEAM_STATS

FRITCHII_MEMSTAGE_SEAM_STATS=1 (boot, diagnostic): the memory seam's stats line (EXP123) every
10 s also with EXP123 = 0, which shows what the seam carries at today's timing.

Without it, and without FRITCHII_MEMSTAGE_TICKS, EXP123 = 0 runs the seam lean (no switch): a fill
bracket only counts its depth (no arena claim, no watch on the scheduler, no site counts), and the
stats counts are skipped. Entries that run as pushed (EXP123 = 0, and outside a deferring bracket
at 1 to 3) go straight to the player's typed call without being written to the scratch arena.
The same calls in the same order: nothing defers at 0, so what the lean drops is the deferring
modes' checks and the stats. With SEAM_STATS set, EXP123 = 0 keeps the full bracket, so its line
still counts the records inside fills.

### FRITCHII_MEMSTAGE_READ_LOG

FRITCHII_MEMSTAGE_READ_LOG=N (boot, diagnostic, default 0 = off; the memory stage's sub-step 2b,
fable_memstage_step2_spec.md 5.3): with EXP123 = 2 or 3, every Nth draw, dispatch and operation
packet logs what its fill read from game memory (uniforms copied to the stream, buffer uploads
with their staged bytes, cached ranges bound without an upload, storage and table addresses and
size words, compute pointers, the inline page write), and at the old read position (a draw's or
dispatch's first play; an operation's end) reads it all again: bytes and values are compared,
bound ranges are asked whether the CPU wrote them since. Differences are counted by kind; the
first 16 of each class (below) are logged as "Memory seam (5.3) <class>" with the kind, address,
size, the changed bytes, the device range, the place, the writer, the pending mark answer, the
gathers since the fill and the programs. Reads past 1 MiB a packet are not compared (counted as
"not logged"; the ranges still count for the over-read test). The counts are in the "memory seam
read log" line of EXP123's stats every 10 s, with the reads compared by kind. N = 8 is the spec's
value; N = 1 checks every packet (slower). Not FRITCHII_MEMSTAGE_VERIFY, which is 2c's by-value
checks (bit flags).

Game threads write the pages the cache reads all the time, so the gate is "0 unexplained", not 0
(fable_memstage_readlog1.md; step 6 spec 10). With READ_LOG on, from boot, every write emulator
code makes to game memory (device and GPU memory managers, span stores, the GPU write listener,
run-ahead release words, Core::Memory's unsafe writes) goes to a ring of 4,096 entries with its
site, thread and sequence (vk_mem_stage.cpp, "Who wrote", lists every hook). Each difference gets:
- a class: "emulator write, download landing" or "emulator write" (the ring names a write newer
  than the fill over the changed bytes; counted by site), "game write, marked" (a game-side mark
  is pending, GPU::ForEachPendingCpuWrite; for a bound range, the tracker holds it), "game write,
  mark already taken" (a tracked read, an upload or the inline page write, with no mark left: the
  store landed after its mark was gathered, and nothing repairs it), "untracked read changed" (a
  stream uniform or a value: no tracker, no mark expected, the next fill reads it again), "gather
  on the back inside Configure" (a bound range, and the drawing thread applied a gather since the
  fill; the first gather's stack is logged), or "unclassified" (unmapped, or the ring wrapped);
- a place: "over-read" (an upload's changed bytes outside every range the packet binds) or
  "inside a range the packet binds" (for a dispatch, whole storage buffers).
The "memory seam read log classes" line has the counts by class and site, over-read and changed
inside a bound range, ring wraps, and on the holding thread the gathers applied and FlushRegion
calls by cache. The "read log gate" line says passed or FAILED: it fails on unexplained (emulator
writes that are not download landings + unclassified) and on ring wrapped (the run is not valid;
make the ring larger). "mark already taken", "untracked read changed" and "changed inside a range
the packet binds" are reported beside it and are not failures (fable_memstage_readlog2.md). Off,
every hook costs one relaxed load and a branch.

### FRITCHII_MEMSTAGE_VERIFY

FRITCHII_MEMSTAGE_VERIFY (boot, bit flags, default 0; spec 5.4): with EXP123 = 3 the back also
makes its own read and compares it with the packet's value (not the early fill's read log: that
is FRITCHII_MEMSTAGE_READ_LOG). 1 = handle values, 2 = the QMD, 4 = null pointer slots (7 = all).
Checks and mismatches are counted by kind in the "memory seam by value" line; the first 16 mismatches are logged as "Memory seam by value (5.4)" with the
address (a handle word's address, or for a handle program bit 63 set, the program in bits 32-62
and the element in bits 0-31), the kind and the shader (the stage's hash; the program address
for the QMD and null slots). Must read 0: same thread, same instant. "count" (the back took a
different number of handle values than the fill made) is counted always and must be 0 too. The
own reads double the handle reads EDEN_NX2_MEMSTAGE_STATS counts. Later sub-steps may add bits.

### FRITCHII_MEMSTAGE_TICKS

FRITCHII_MEMSTAGE_TICKS (boot, default 0; the memory stage's build step 4,
fable_memstage_step2_spec.md 7.2): what the buffer cache side's stamps are.
- 0: scheduler ticks, as before (SeamClock forwards to the scheduler).
- 1: packet ticks. The memory stage numbers its packets (each draw, dispatch and operation
  packet takes the next number) and every stamp the buffer cache side takes is the number
  current then: buffer usage (and so CanReorderUpload's "used since"), staging buffers and
  the stream ring's lazy region stamps, EXP104's recycler, write ticks, extent probes, list
  page copies, host read flushes. The GPU thread publishes at the end of every scheduler
  tick "every number up to N was recorded in tick K"; a stamp is free when its tick is
  complete. Numbers whose entries still wait for their play stay open past a tick's end (a
  held packet's until its PlayRest), which is what makes EXP123 = 2 and 3 safe: they need
  1 or 2 here and run as EXP123 = 1 without it. Deleted buffers wait their 8 frames and also
  their packet. The stream uniform reuse compares a close count instead of the tick. On one
  thread every answer is the scheduler's. With FRITCHII_MEMSTAGE_SPLIT_ALLOC=0 the texture
  cache's staging (the shared pool) gets packet stamps too, right on one thread.
- 2: as 1, and every answer is checked against the scheduler ticks' answer. The line
  "Memory seam ticks (FRITCHII_MEMSTAGE_TICKS=n)" every 10 s gives packets/s, tick closes/s,
  the published ticks waiting, carried closes and deleted buffers held past their frames (with
  1 too); with 2, "memory seam ticks check" gives early (the packet answer frees or waits
  less: unsafe) and late (holds longer) per check (is free, stream known, wait, host read),
  carried skipped, split (a number handed out in two ticks without a carry), unchecked
  (older than the 2^18-number ring) and epoch off. Early, late, split, epoch off and held
  must all read 0. Disagreements log the first 16 lines with the packet and both ticks.
- With FRITCHII_MEMSTAGE_AHEAD (the window open, the ring running) each thread stamps with its
  own packet instead of the newest number anyone opened: GPUMem with the draw it fills or the
  operation it runs (under its mark), the back with the packet it holds or fills and after it
  the last one while not published. The check then also counts "stamps on a number already
  published" (must be 0: such a use would be freed early), "splits that raced the tick advance"
  (a split by one tick of a number above what the closing tick's close saw: a stamp taken on the
  other thread between a close and the tick's advance; reported) and GPUMem's "epoch reads
  racing a close" (reported). Carried ranges are kept until older than the stamp ring. Late is
  a warning with two threads (it is the conservative side).

### FRITCHII_MEMSTAGE_SHADOW

FRITCHII_MEMSTAGE_SHADOW (boot, default 0; the memory stage's piece 5b,
fable_memstage_step6_spec.md 6.2): the future memory stage's inputs, run on the back thread and
compared, never used. Off, each hook is one test of a flag (one per ring entry, per draw, per
FlushCaching, per channel bind).
- 1: before the back executes each ring entry, the stage's code classifies it (pass, work or
  level: spec section 3, the Call entries by their stage tag in header bits 32-39) and applies
  pass and work entries to its own copy of each Maxwell3D's registers, shadow RAM, constant
  buffer bindings, root tables and draw manager. The draw manager is the same code on both
  (DrawManager is a template on the engine, instantiated for Maxwell3D and for the stage's copy),
  so the stage finds its draws with the back's own lines. Constant buffer words are computed
  (range and offset), not written. After a level entry the copy is taken from the back again,
  with the back's waiting method sink applied. At every draw the back issues: (a) the stage drew
  at the same entry; (b) the stage's state at its draw equals the back's (the registers the
  buffer cache reads through CacheInputs, the draw's registers, the constant buffer bindings, the
  root tables, the draw state, execute_on, the channel, the engine; every dirty flag the stage
  raised is raised on the back); (c) at the pipeline lookup, the facts table (keyed by the program
  registers) against the pipeline's facts digest; (d) at FlushCaching, the device ranges of the
  stage's constant buffer words against the back's invalidation ranges.
- 2: as 1, and (b) compares the whole register file and the shadow RAM (slower).
Every 10 s, "Memory stage shadow": entries/s pass, work, level; level runs/s and resyncs/s;
draws/s compared (served, render-enable off, flushed from the method sink) and in level entries;
the draw kinds of spec 3.4 (indirect, list, GPU count, texel buffers, diagnostics, host
conditional); facts misses/s; watched-page hits (constant buffer words onto image pages or the
texture/sampler tables: counted, and the entry is treated as level). "Memory stage shadow
compare" gives a (missing, extra, position), b (dirty flags apart), c (conflicts; program keys
that changed code; pipelines without a compared draw), d (missing, extra; flushes, strict ones)
for the window and the session: **all must read 0**. "Memory stage shadow inside draws": what
runs on the back between a draw's fill and its record, FlushRegion by the cache halves that ran
(texture, buffer, query; the buffer half must stay 0 in play) and collected game writes applied.
"Memory stage shadow level entries by kind and channel": the top 12, named by the 3D method, the
engine, the closure (`call +0x...`: its address in the executable, for the symbol file), or the
constant buffer case; this table is the go/no-go for piece 6b. The first 16 differences of each
compare are logged as "Memory stage shadow (a/b/c/d)", (b) naming the registers. The stage
implements the default constant buffer write; with EXP107 on, constant buffer words are level.
Not covered yet: the watched pages leave out shader code and query addresses.

Piece 5c (fable_memstage_6b_plan.md sections 2 to 4 and 6.1), in every mode:
- Classes: pass, work, level ("hard": needs the buffer cache on the back) and neutral (another
  engine's plain register write). clear_report_value, fragment_barrier, tiled_cache_barrier and
  report_semaphore.query are pass, except a fence report (Release, short query), which is hard.
  render_enable.mode is work when the registers decide it (an override, or mode False / True: the
  stage sets execute_on), else hard. Other engines: only their executable methods are hard. The
  front's EXP105 command list check is tagged pass; the list draw's address and keep id are a
  tagged closure (work); the 3D macro data closure (dma_pusher.cpp, IsMemoryDirty) is not sent
  when the front runs the macro (front macros and the native Turing macro engine: the back never
  reads it then). A new tag, StageTag::Cache (hard, no copy after), is on no closure yet.
- Follow: hard entries are applied to the stage's copy as well (the store; falcon[4]'s scratch
  word), so the copy is taken from the back again ("full copy") only after an entry it cannot
  follow: an untagged closure, an unknown tag, a 3D macro run on the back, methods before any
  engine entry. After an undecided render_enable.mode the copy takes execute_on from the back, and
  after a draw the back issued in a hard entry, an indirect draw or a GPU-counted draw the draw
  manager's scalar state, both at the next entry.
- Runs: a run starts at a hard entry and ends after the last hard entry reached through pass and
  neutral entries; a work entry or a draw ends it.
- Compare b's dirty flags: only flags 15 to 48 (the buffer cache's: VertexBuffers, VertexBuffer0
  to 31, IndexBuffer). The copy keeps them as the cache's own state (its stores raise them, a full
  copy takes the back's, what the back's fill clears is cleared on the copy), and they must be
  equal at each draw both ways, or a subset of the back's when the state tracker raised them since
  the back's previous draw. Counted per flag.
- 3: as 2, and the buffer cache's fills read the stage's copy outside runs (CacheInputs pointing
  at the copy's registers, bindings, root tables, draw state and flags): at every draw the back
  issues outside a hard entry whose shadow draw matched, that is not indirect, and whose copy has
  not changed since the shadow's draw. Otherwise the fill reads the engine as before. The cache's
  flags 15 to 48 are then the copy's: at a run's start the back's are set to the copy's, inside a
  run the copy takes the back's at every entry. Needs FRITCHII_MEMSTAGE_FULL_BINDS=1 (the
  scheduler's flag raises on the back are dropped at the hand-over); while it is off, 3 runs as 2.
  The flag part of compare b is skipped at 3. A check of the stage's inputs with the real
  picture: flash-watch route, compare b and the seam's own counters must stay clean.
Lines every 10 s, with the others: "Memory stage 5c runs" (runs/s, entries per run, neutral
entries/s, served draws between two runs as a histogram 0, 1, 2-3, 4-7, 8-15, 16+, full copies/s
and MB/s, the takes, 3D macros run on the back/s, fence reports/s); "Memory stage 5c runs by first
hard kind" (top 12); "Memory stage 5c full copies by cause" (top 8); "Memory stage 5c
render_enable.mode by override and mode" (must show only False / True or an override: the
undecided count must be 0); "Memory stage 5c flags 15-48" (compares, mismatches by flag, at 3
where the fills read from and why not the copy, and the OoT tracker record check below); and
"Memory stage 5c gate: pass/fail", the stop thresholds of plan 6.1, read in the market's window:
runs above 15,000/s, full copies above 2,000/s, or any undecided render_enable.mode in the session.

The OoT temporal trackers (TrackOcarinaCamera, TrackOcarinaActor; plan 5.1) no longer ask the
buffer cache after the fill when the draw has a packet (EXP123 >= 2): while the trackers are on
(published at each frame end), the draw's fill adds a record of the vertex stage's uniform slots
3, 4 and 6 (address, bound size, and whether the bytes the trackers read are GPU-modified) to the
packet, and the trackers read that. Without a packet (EXP123 < 2, texel buffer draws, the first
frame) they ask the cache as before. With FRITCHII_MEMSTAGE_SHADOW on, every such draw compares
the record with today's query ("OoT tracker record vs today's query": mismatches must be 0; the
first 16 log "Memory stage shadow (OoT trackers)"). The shadow's "diagnostics" draw kind is now a
frame dump or a trace only.

### FRITCHII_MEMSTAGE_THREAD

FRITCHII_MEMSTAGE_THREAD (live, read at a frame end, default 0; the memory stage's step 6, pieces
6a and 6b, fable_memstage_step6_spec.md 6.1, 6.3 and section 5): 1 runs the buffer cache's two big
fills on a thread of their own by hand-off; 2 lets that thread read the ring itself (below).
- The thread, "GPUMem", exists while the GPU thread split runs (started and stopped with the
  back) and sleeps at 0. It is not started with FRITCHII_MEMSTAGE_SPLIT_ALLOC=0 (logged).
- 0: the fills run on the back (EXP123 = 2's early fill, as before).
- 1: the draw's fill (MemStage::FillDraw) and the dispatch's (FillDispatch), and the buffer
  recycler's idle destroys, run on GPUMem: the back posts the fill, GPUMem runs it, the back
  waits (about 2,000 pauses of spinning, then a sleep). Same order and timing as 0, another
  thread. The buffer cache's lock goes with the fill. Every other cache call stays on the back,
  where it was. A sync inside a fill (finish, wait, submit for host read, the multi-range bind)
  is posted back to the back, which serves it inside its wait. Needs EXP123 >= 2 (so
  FRITCHII_MEMSTAGE_TICKS >= 1) and the buffer cache's own allocator; without them, or without
  the split, it runs as 0, logged once.
- Expect fps lower than 0: two thread wake-ups a draw. The point is the thread rules, not speed.
- "Memory stage link (FRITCHII_MEMSTAGE_THREAD=1)" every 10 s: hand-offs a second (draw fills,
  dispatch fills, idle destroys), the back's wait in ms/s and per hand-off, GPUMem's run time,
  sleeps on both sides (GPUMem's at THREAD=2 also by the wait's reason, see
  FRITCHII_MEMSTAGE_RUN_SPIN_US), served syncs a second, and the counters that must be 0: a buffer
  allocator call, a buffer staging request or a seam push by a thread that is not the cache's
  user; a VMA heap budget read off its allocator's user; a scheduler or descriptor queue use on
  GPUMem; an entry GPUMem would run at once (served on the back instead); an entry run at once
  while entries are pending; the memory player reached on GPUMem; the front's waits reached on
  GPUMem; a fill handed off while the back did not hold the buffer cache's lock. The first 16 of
  each log "Memory stage link (must be 0)" with the place. The seam's
  own 5.1/5.2 counters (EXP123's line) must stay 0 too.
- EDEN_NX2_GPU_PROFILE: GPUMem reports as "GPU memory thread" with its "memory fill" zone (the
  fill ms/s of EXP123 = 2, now off the back) and "memory sync wait"; the back shows "memory
  hand-off" for its waits. FRITCHII_BACK_READ_AUDIT adds a "Stage read audit" table for GPUMem.
  The hitch line, the stall snapshot and the CPU trace list GPUMem as a thread of its own.
- Not pinned to a core (spec 5.8, until step 7 measures it).
- 2: the ring-fed stage, level mode (piece 6b, fable_memstage_step6_spec.md 6.3 and
  fable_memstage_6b_plan.md 6.2; the code: memory_stage_link.cpp, memstage_shadow.cpp, ring B in
  vk_mem_stage.cpp). GPUMem reads the ring itself, before the back, with its own cursor
  ("staged"); the back runs only what GPUMem passed on. GPUMem runs piece 5c's code on every
  entry (classify, apply, follow) on its own copy of the 3D registers, and that copy is what the
  fills read:
  - a draw it serves is filled on GPUMem right where its draw manager issues it (the pipeline's
    facts come from a table the back fills; constant buffer words are written to game memory by
    GPUMem, their buffer half applied before the fill), and the packet goes to ring B (4 MiB);
    the back takes the packet at its draw and plays it, without entering the buffer cache;
  - everything GPUMem does not implement is a level run: the cache, its flags and the words'
    ranges are lent to the back, which runs those entries exactly as today, then GPUMem takes the
    flags back (and a full copy only after a closure it cannot follow). Indirect and list draws
    (list draws are served with the window open, FRITCHII_MEMSTAGE_LIST_DRAWS below),
    GPU-counted draws, texel buffers, a facts miss, a frame dump or trace, host conditional
    rendering and constant buffer words onto a watched page make their entry level (a lone
    gather does not with FRITCHII_MEMSTAGE_GATHER=1 and the window open: GPUMem comes level at it
    and the back runs it there, below);
  - level mode (AHEAD = 0): GPUMem works only while the back is parked (it ran everything passed
    on and is not in its idle hook), so the two threads still take turns at every draw. Expect fps
    lower than 0; the point is correctness and what a run costs. Step 7 opens the window.
  - Needs what 1 needs, plus EXP123 = 3 (values by value: a served draw's texture handles are
    read by GPUMem at its fill and come in the packet) and EDEN_NX2_EXP53 off (it runs as 1
    otherwise, logged once); forces FRITCHII_MEMSTAGE_FULL_BINDS on. An exception on GPUMem ends
    the ring cleanly where the back is parked and THREAD = 2 runs as 1 for the rest of the
    session (logged, counted).
  - Ending the ring (2 to 1 or 0, at a frame end) hands the cache's flags and GPUMem's last
    constant buffer ranges back to the back, and plays whatever is left in ring B. Step 7's
    window is FRITCHII_MEMSTAGE_AHEAD (below).
    The constant buffer write modes other than the default (EXP107) make every constant buffer
    entry level. EXP118 (delta draws) is untested with it.
  - What the back would do in the buffer cache inside a served entry (a read flush from a
    texture upload in Configure, its gather, a MustFlushRegion question) runs on GPUMem as a
    served call while the back waits, counted by cause.
  - "Memory stage ring" every 10 s (with the link line): entries a second by class (pass, work,
    level, neutral), runs a second with entries per run and microseconds per run (GPUMem, from the
    lend to the return; the back, from the lend seen to its next packet), packets a second and
    their size, the back's waits for the stage (times and ms/s), GPUMem busy and its waits by
    reason (the front, the back, runs), full copies and flag hand-overs a second, served calls by
    cause, level draws by kind, and the verify. Piece 5c's lines (runs by first hard kind, full
    copies by cause, render_enable.mode, ...) keep coming every 10 s, from GPUMem's state.
  - Must be 0, beside 1's: the back in the buffer cache while not lent (its entry points, by
    function), InnerInvalidation with ranges on the back while not lent, a packet asked for at a
    draw the stage sent none for, a packet left unplayed at a run's start, ring B full, the draw
    counts of both threads differing at a run's end, a packet played whole for a draw the back
    did not prepare, a packet whose entry is past the back's draw (dropped unplayed, the back
    fills the draw), a served draw whose texture handles the back read from game memory, and an
    exception on GPUMem.
  - FRITCHII_MEMSTAGE_RING_VERIFY=1 (boot): compare b with two threads: GPUMem puts a hash of
    compare b's words (the narrow set) in each packet and the back hashes its own at the take;
    differences are counted on the ring line, the first 16 logged. Off for fps runs. The flags
    15-48 are not in it (in served mode the back's are not the cache's: GPUMem's copy holds them,
    and they are handed over at each lend and return).
  - Profile: GPUMem's "stage wait" and "level run" zones; on the back the "split wait" span's
    phase 3 is its wait for the stage.
  - The read log (FRITCHII_MEMSTAGE_READ_LOG) and the texture-write check do not cover ring
    packets in level mode; with FRITCHII_MEMSTAGE_AHEAD the read log does (below).

### FRITCHII_ONEF (one follower, steps 1 to 4)

The one-follower renderer front end (work\fable_b_one_follower.md, sections 8 and 8.1;
src/video_core/one_follower.h). The back thread becomes E, the engine thread; a new thread R,
"GPURender", owns the pipeline, texture and query caches, the fences and the Vulkan scheduler and
plays packets E sends. Anything R cannot do from a packet runs "lent": E parks, R runs E's code
reading E's engine, then releases E. The switches nest: each refuses to turn on without the one
before it and logs why ("FRITCHII_ONEF_VIEW=1 needs FRITCHII_ONEF=1; running 0"). Turn them on in
this order in one session: FRITCHII_ONEF=1 at boot, then VIEW, BUILD, UNLEND live.
- FRITCHII_ONEF (boot, default 0): 1 = step 1, R exists and every draw and every R-owned
  operation is lent. The picture is today's; it prices the hop (one round trip a draw). 0 is
  today's code path, untouched. Not live: ownership of the caches and the scheduler cannot change
  in a session. Needs the GPU thread split.
- FRITCHII_ONEF_E_PIPELINE (boot, default 1 with FRITCHII_ONEF=1; fable_b_one_follower 10.2
  rule 3; 0 goes back to the facts table path): 1 = E owns the
  pipeline lookup. The pipeline cache always reads E's engine (R's view never replaces it); at
  step 4 E looks each unlent draw's pipeline up before its fill (a miss or a build not ready is a
  lent draw, "pipeline miss"), fills from that pipeline's own facts and puts the pipeline in the
  packet; R binds it and looks nothing up. The facts table then plays no part in unlent draws.
  R calling the lookup outside a lent call counts "owner on R".
- FRITCHII_ONEF_QUERY_OPS (live, default 0; fable_b_one_follower 11.1 item 1): 1 = queries stop
  parking E. A render enable with an override or mode True/False sends nothing while no
  Conditional/IfEqual/IfNotEqual one was ever lent (it could only end host conditional
  rendering, which nothing began; E still runs the call's FlushCaching); counter resets and plain
  reports (not fences, not payload releases) are render ops R runs at their place while E goes
  on, the report's address translated on E. A plain report on a page the buffer cache has
  registered stays lent (a lend reason, "report on buffer page" in the kinds line; common in
  OoT). 2 = those are render ops too when the report's type has a streamer (it only queues in
  CounterReport and lands at a fence or flush, both lent, where its buffer side runs); a type
  without one (a payload write inside CounterReport) stays lent. Counted as "report on buffer
  page as op". Run it with EXP12=5 set from boot (the lagged acquire check, 11.3); with another EXP12
  it logs a warning (it no longer switches EXP12 itself: the first QUERY_OPS hang came with that
  live 4 -> 5 flip). Every 10 s "One follower queries by kind" logs each call site's
  rate and parked ms/s (condition skipped/override/true/false/conditional/if equal/if not equal,
  reset, plain report, report on buffer page, fence report, payload release), with the switch off
  too.
- FRITCHII_ONEF_INVAL_E (live, default 0; fable_b_one_follower 11.1 item 2; needs
  FRITCHII_ONEF_E_PIPELINE=1 from boot and step 4, else it logs and runs 0): 1 = E runs the
  draw's invalidation (FlushCaching's ranges) itself, the buffer half, when no range touches a
  page an image, a shader or a query was ever on, nor a report op E pushed; such a range keeps it
  lent. E stamps the pages it wrote that way with the next packet number (a 16 MiB table, made at
  the first use), fences and looks at the filters again; R, before an image upload, reads the
  stamps: a page E wrote after R's place counts "texture read ahead of R" (must be 0) and the
  image takes that write as the invalidation would have (it uploads again at its next use). The
  "One follower invalidations" line (every 10 s) splits them: run on E, lent by texture, shader
  or query page, lent at the second look; and the uploads checked per second.
- FRITCHII_ONEF_COPY_E (live, default 0; fable_b_one_follower 11.1 item 6, first piece; with
  FRITCHII_ONEF_INVAL_E's conditions): 1 = inline uploads (AccelerateInlineToMemory, deferred
  path) run on E: the guest write (its stream-write guard drains R on a texture or shader page),
  the buffer cache, the accumulated invalidation. Lent whole when a range is on a query page or a
  report op E pushed; after an in-place upload the texture and shader half is lent alone where
  their filters are hit. E stamps the pages before the write (item 2's pair). Counted in the
  invalidations line ("inline uploads on E / with R's half lent / lent: query page").
- FRITCHII_ONEF_POSTED (live, default 0; fable_b_one_follower 12.2): posted is the default,
  lent the exception: an R-side operation that returns nothing to E goes by value. Applied so
  far, in 12.3's order: fix 1, plain reports on buffer cache pages as render ops (what
  FRITCHII_ONEF_QUERY_OPS=2 does; POSTED also turns QUERY_OPS on). The "One follower posting
  (dry)" line prices the next fixes: lent fences a second and how many have a buffer flush
  pending (the others need no E half), lent gathers and their ranges each; copies by class are
  the "lends by site" line's BufferCopy, BufferClear, DmaBufferImageCopy, AccelerateSurfaceCopy
  and AccelerateInlineToMemory rows.
- FRITCHII_ONEF_GATHER_E (live, default 0; fable_b_one_follower 12.3 fix 5; with
  FRITCHII_ONEF_INVAL_E's conditions): 1 = InvalidateGPUCache's gather of the guest's writes
  runs on E: the take, then the buffer half (ApplyGatherBufferHalf). A range on a texture or
  shader page lends the gather (R's half); the query cache has no part in a gather. Runahead
  keeps it lent. Counted in the invalidations line ("gathers on E / gathers lent").
  FRITCHII_ONEF_COPY_E also runs AccelerateInlineWords on E (no piece on a query page; its
  pages stamped first), and FRITCHII_ONEF_POSTED posts FragmentBarrier and FlushCommands as
  render ops (R's scheduler and counters only).
- FRITCHII_ONEF_OP_INLINE (live, default 0; fable_b_one_follower 13.1, 13.4 group 1; step 4):
  1 = the buffer commands E's inline uploads record (FRITCHII_ONEF_COPY_E runs them on E) are
  posted: at the play of the pending entries E writes them by value as one ring B op packet
  (kind 2, its number the entries' packet tick) and pushes a BufferOp naming it; R plays it
  through MemPlayer::Exec, after any held draw packet's rest, with no fill state of E's and no
  lock handed over. Lent as before: a sync inside a held fill, E's own draw fill, no room, no
  packet ticks. Must be 0: "ring B pairing" (R found another packet at the head), "owner on R"
  (an entry's play entered the buffer cache). The "One follower op packets" line counts them.
- FRITCHII_ONEF_OP_INVAL (live, default 0; fable_b_one_follower 13.4, step 4): 1 = the buffer
  commands E's own invalidation work records (INVAL_E's draw invalidation, GATHER_E's gathers:
  the Invalidation site) are posted as ring B op packets, as FRITCHII_ONEF_OP_INLINE does for
  inline uploads, instead of a lent seam play (which E made holding the buffer cache's lock).
- FRITCHII_ONEF_OP_LISTKEEP (live, default 0; fable_b_one_follower 15.4, step 4): 1 = kept
  list words (Nx2KeepListWords) are posted: E obtains the buffer (its sync's commands an op
  packet, the ListWords site), R records the copy into the keep buffer as a render op. The
  front's read of kept words (Nx2ReadKeptListWords) is lent now, so R has played the copies
  before it (it ran on E before, reading R's player).
- FRITCHII_ONEF_OP_DMA (live, default 0; fable_b_one_follower 13.4, step 4): 1 = the DMA
  engine's buffer-to-buffer copies and clears (AccelerateDMA BufferCopy, BufferClear) run on E
  (the buffer cache is E's, the result E's own), their commands posted as op packets (the Dma
  site); the submit after list page copies is a render op. Copies with an image stay lent.
- FRITCHII_ONEF_OP_DISPATCH (live, default 0; fable_b_one_follower 11.1 item 5, 13.4 group 6;
  step 4, needs FRITCHII_ONEF_E_PIPELINE=1 from boot, else it logs once and every dispatch stays
  lent): 1 = compute dispatches as posted packets. E runs the stream's invalidations, the compute
  pipeline lookup when it is a plain hit (the shader known, its entry built, no rebuild pending
  or due; the OoT network, DLSS and trap guard hooks then run on E, once), the buffer side by
  value into a ring B packet of its own kind (3: uniform and storage binds, uploads, the
  GPU-written marks the storage binds make), the handle values, and what R reads of the compute
  engine (launch description, pools, code address, native mode); then it pushes a dispatch op and
  goes on. R takes the packet by name and records the dispatch on its view of the compute engine
  (image views, samplers, descriptors, pipeline bind, vkCmdDispatch), with only the texture
  cache's lock. Stays lent, counted by reason on the "One follower dispatches" 10 s line (with
  the posted share): indirect dispatches, dispatches that write an image, texel buffers, the OoT
  upscaler passes and the depth reader, a lookup that is not a plain hit (the lent call looks up;
  a lent dispatch E looked up takes E's pipeline, no second lookup), diagnostics (frame dump,
  traces, flash watch, the handle verify, the stage's shadow or ring, memstage stats), no ring
  room or values by value, the view not seeded. A posted dispatch whose fill noted command list
  page writes or write extent probes lends those records after it ("posted + ... lent").
  Must be 0: "ring B pairing", "played dispatch" (R's Configure failed or threw), "played
  dispatch off view", "owner on R", "R waits for E's lock"; any of the dispatch ones stops
  posting for the session (logged at Critical). The QMD release after a dispatch is unchanged.
  The value is a bit mask (any non-zero value posts the plain ones):
  - +2: dispatches that write images are posted too. R resolves and records the written images
    (its texture cache marks them, as in the lent path). What E needs of those writes in its own
    stream: (a) EXP10's dispatch write records, made at E's place: the images' guest ranges are
    the texture cache's, so E records "may write anything" (DispatchUnbounded::Image, the record
    the lent path makes for an image without a guest range): a superset, a front DMA read
    across it drains. (b) Nothing in E's buffer side: image writes make no buffer marks in the
    lent path either. (c) E's own texture-side tests already wait for R: FLUSH_E's texture half
    needs R caught up, INVAL_E/GATHER_E lend on a page an image was ever on, and an image R
    first creates at the dispatch's play after E wrote its pages is the "texture read ahead of
    R" must-be-0 (its stamps), as for a played draw's render target. Written byte-addressed
    images (the packed R16 shadow, which can throw) stay lent ("byte-addressed image write").
    Must be 0: "played dispatch image" (R's image writes differ from what E saw in the program).
  - +4: indirect dispatches are posted too. Their arguments are read by the GPU only (nothing
    in DispatchCompute reads them on the CPU but the frame dump, a diagnostic, lent): E obtains
    their buffer in its fill as DispatchCompute does (FullSynchronize, DiscardWrite; a section
    after `before`, played at the packet's rest after Configure's bind record, as there), the
    packet carries it, E records the 12 bytes for EXP10, R records vkCmdDispatchIndirect. Must
    be 0: "played dispatch indirect" (R's view and the packet disagree on the dispatch).
    With the switch on, R also copies every indirect dispatch's three grid words on the GPU
    right before it (a 16-byte download slot) and checks them against the device's workgroup
    limit once the submission is done: over it on a posted dispatch is the must-be-0
    "indirect grid over the limit" (seen after the fact; posting stops), on a lent one the dry
    count "lent indirect grid over the limit (dry)" on the dispatches line (the game's own).
  Fixed 15 Nov (of7-flash-log, flashes and seconds-long GPU stalls with everything on): E read
  the rasterizer's `kepler_compute`, which R points at its view while it plays a posted dispatch,
  so E could fill the next packet with the played dispatch's launch description, pools and
  indirect address. E now reads the channel's own compute engine.
  So 7 posts everything but the upscaler passes, texel buffers, lookup misses and diagnostics.
  "lent: view not ready" is the window after a channel bind until the next lent packet seeds
  E's tracking and R's view; FRITCHII_ONEF_BIND_SEED=1 seeds inside the lent bind itself and
  closes it (the line prints both switches).
- FRITCHII_ONEF_DISPATCH_RANGES (live, default 0; fable_b_one_follower 27.9; with
  FRITCHII_ONEF_OP_DISPATCH): 1 = dispatches record their image writes for EXP10's dispatch
  checks by range, not as "may write anything" (which made every front DMA / 0xe2c read of a
  report page drain, about 2,500 a second against 2 in normal mode):
  - a posted image-writing dispatch: E reads each written image's descriptor at its place (the
    TIC entry the handle's index names, TextureHeaderIndex, the view header) and records the
    image's guest range as the texture cache computes it (its address less the base layer,
    ImageInfo, CalculateGuestSizeInBytes), translated to device memory. Not one mapped device
    range, a buffer image, or more than 8 written elements: "may write anything", as before.
  - a lent dispatch: the lent call records on R with E's recording state (GpuSplit's flag is per
    thread), so the lent path's own block records its images' backing ranges and confirms (the
    lent upscaler passes were "unconfirmed", about 115 a second).
  Must be 0: "dispatch image range": R, after it played the dispatch, reads each descriptor E
  recorded again and compares (and the number of written images, and an image where E saw none);
  a difference stops the switch for the session. Counted on the dispatches line: "posted images
  ranged" / "unbounded", "images wider than E's record" (the texture cache's backing image
  reaches past the descriptor's range: a view into a larger image; the lent path records the
  larger one; not a fault), and, with OP_DISPATCH alone, "images on a front read page" (every
  played image-writing dispatch whose backing image touches a page the front's DMA reads took a
  snapshot of; fable_b 27.9's count first, expected about 0).
- FRITCHII_LIST_COPY_SKIP (live, default 0; fable_b_one_follower 27.9 item 3; the front, with the
  split): the "list writers" wait in DmaPusher::Step for a CPU-written entry that begins between
  methods, kept only because a copy is queued (CopyWritersPending), is skipped when no writer sent
  and not yet run has a noted range on the entry's pages and none was unbounded
  (GpuSplit::WritersMayWriteSince per page; MaxwellDMA::FrontNoteLaunch notes each copy's
  destination). The deferred check on the back at the entry's place runs for every skipped one
  (EXP105's sampling does not apply to them): a GPU-written entry is the must-be-0 and stops the
  switch for the session. The "List writer waits" 10 s line counts the waits by reason (inside a
  method's data, a copy queued), the answer after the wait, whether a queued writer's range was
  on the entry's pages, the skips, and the must-be-0, with the switch off too.
- FRITCHII_ONEF_FRONT_OPS (live, default 0; fable_b_one_follower 27/28, stages S2 and S3; the
  front, with the split and EDEN_NX2_OOT_MACROS=1): seven hot OoT macros (0xe02, 0xe1c, 0xe36,
  0xea0, 0xe08, 0xe52, 0xe3a; macro_ops_oot.h) go as one ring entry per call, "op + resolved arguments"
  (the data RAM words the handler read), instead of one entry or Pairs word per register write.
  The front still runs the handler on its own state: each write is decided by FrontWriteDecide
  (shadow, same-value skip, executable bookkeeping) and recorded, the sent set and its lanes
  (Pairs word or Direct entry) kept as masks, the notes made after the op entry. Only where
  every write takes those plain lanes: shadow RAM Track, EXP22 / EXP2 / the draw packet off
  (FRITCHII_CB_WORDS_BLOCK 1 and 2 are fine: the op's data words are its Direct writes, the op
  closes an open block, E groups them as the block would; with 2 the addresses are compared), EDEN_NX2_MACRO_SINK only with EDEN_NX2_RING_PAIRS, no list guard, no
  macro trace / dry / verify tool, no EXP53 predicate; the instanced draws only up to 1 instance
  while EDEN_NX2_FOLD_INSTANCES counts later instances (16 otherwise; its held count would go out
  before the op). Anything else, the old
  path. 1 = E rebuilds the writes and applies the sent ones as Execute applied their entries
  (S2); 2 = 1 + check; 3 = E calls the leaf each write reaches through CallMethod directly
  (S3: WriteStateRegister, CallMethodFast, ApplyPairs' store, CallConstBufferWords, by
  execution_mask, never by op), only where CallMethod would take those fast routes, else S2; it
  stays on with EDEN_NX2_GPU_PROFILE (one sampled "method" scope, CountRegisterWrite per write);
  4 = 3 + check. The memory stage follows an
  op entry write by write (StageTag::Ops).
  Must be 0 (check modes, "Front ops check" once a second): E's expansion against the front's
  write list word for word; each written register on E against the front's copy after the call;
  "front skipped E differs": at each state write the front dropped, E's register and shadow word
  hold its value (every op in 2 and 4, every 64th in 1 and 3); each constant buffer word's
  address on E against the front's noted one; with 4, each Direct write's route (CallMethod's
  fast leaf) and index_buffer.count's indexed flag, and for ops that only store state (no
  deferred draw, no write tracking, profile off) S2 and S3 applied from the same state compared
  register, shadow word and dirty flags. Also always: "count
  differences" on the front's line (recorded writes against the expansion's number). Any
  mismatch stops ops for the session. The front's "Front ops" line (once a second) counts op
  entries, writes sent and dropped, per op sent / sent nothing / old path, the old path by
  reason (shadow, exp22, same_experiment, packet, list_draw_stale, e2c unknown,
  guard_list_reads, sink without pairs, tools, stopped, declined or too long), fallbacks.
- FRITCHII_ONEF_DEAD_STORES (live, default 0; fable_b_one_follower 27.7; count only): 1 = the
  "Dead stores" line counts 0xea0 calls whose trailing c[0][0x640] resets (8e3 = 0x640, 8e4 = 0,
  8e5 = 0) the next 0xea0 overwrites with nothing sent between (same ring position, no held
  work, same buffer), and of those, the ones uploading the same two words as the call before.
  Dropping them is not built: front-side guest reads between the calls could see the bytes.
- FRITCHII_ONEF_FILL_COUNT (live, default 0; fable_b_one_follower 28.5, S4's count; with
  FRITCHII_ONEF_FRONT_OPS): 1 = every 16th bind op (0xe02, 0xe08) hashes the first 4 KiB of the
  bound range on the front (raw read, after the op) with whether noted work that may write it is
  still queued; E hashes the same bytes when it runs the small entry right after the bind. The
  "Fill count" line: sampled, same, differ with a queued writer, differ with none (expected 0).
- FRITCHII_ONEF_FLUSH_E (live, default 0; fable_b_one_follower 17.3 (a); step 4): 1 = a
  FlushRegion or MustFlushRegion E tests clean is answered on E, not lent: no page of the range
  GPU-written (E marks those when it fills the writes), none a query's or a posted report's,
  and for the texture half none an image was ever on with R caught up (R registers the images
  of the draws it plays). Anything else is lent as before.
- FRITCHII_ONEF_SYNC_MIRROR (live, default 0; fable_b_one_follower 16.2, dry): 1 = E keeps a
  mirror of R's samples counter "started" bit (set by every draw, indirect draw, draw texture
  or clear E issues with the count enable register on; cleared by a samples counter reset) and
  R compares it with its streamer at every samples report, posted or lent: must-be-0 "sync
  mirror", and a 10 s line with the reports compared and the two kinds of mismatch. It prices
  the by-value query sync of posted fences: the sync needs this mirror exact. Clears count as a
  superset (R starts the count only for a clear it cannot defer).
- FRITCHII_ONEF_POST_FENCE (live, default 0; fable_b_one_follower 16.3; step 4, immediate
  fences only): 1 = a sync point, a reference or a forced fence release is posted when E finds it
  clean: no query sync owed (no samples report since the last lent fence or wait for idle while
  E's mirror had the counter started, and no report of another kind), and nothing for the
  buffer cache to commit. E runs the fence's closing gather itself first; R runs the fence with
  its buffer half and closing gather skipped, so it never enters the buffer cache. Fences with
  a caller's callback (SignalFence) stay lent. A "sync mirror" mismatch with R's counter
  started and E's mirror not stops it for the session (the test reads the mirror as a
  superset). With immediate fences the forced release's wait ends inside its own SignalFence
  (the callback runs there), so posting it changes no landing time.
  With fable_b 16.2 a fence that owes query syncs posts too: E obtains the buffers for the
  slots R will sync there (samples reports made with the mirror started, cleared at every
  fence; payload releases, kept until a lent fence) at the fence's place, their uploads an op
  packet before it, and the op names them; R's SyncValues copies into them without the buffer
  lock. Must be 0: "sync target missing" (a value no target holds: not copied, posting stops),
  "sync target unused" (a samples target R had no value for). Only other report kinds still
  make a fence lent. 13.0: every report's page then waits, in E's buffer cache reads of game
  memory, for the posted fence after it to be played by R (a folded fence lands the queued
  reports at R's place); counted in the op packets line.
- FRITCHII_ONEF_LOOKUP_CHECK (live, default 0 = off, N = every Nth unlent draw; fable_b 18.4):
  at an unlent draw's pipeline lookup on E (rule 3), the key parts
  the lookup kept because their flag was clear (the six program hashes with Shaders clear, the
  blend attachments with Blending clear, the vertex attributes and divisors with VertexInput
  clear) are compared with E's registers as the lookup judges them: the blend by
  BlendingMatches (canonical for native keys; skipped with dynamic blend state, whose key never
  holds the blend), the rest computed fresh and canonicalized as the lookup leaves a native key:
  must-be-0s "lookup programs", "lookup blend", "lookup vertex input", one per part. The engine's flags are left as they were, except that a differing program refresh
  leaves Shaders raised so the next lookup takes the right key.
- FRITCHII_ONEF_DRY_RELEASE (live, default 0; fable_b 19.5 step 3's price, counting only): E
  stamps each release it passes (payload Query, fence-flagged payload, counter report, QMD
  reduction or max callback) by its word; every GPU semaphore write stamps when it ran; the
  front's pending sleeps and lagged check waits add up its stalled time. At each resumed
  acquire the release that met it after the suspension gives "E ahead of the write" (the time
  from E's pass, or the suspension, to the write) and the front's stalled time in it: the line
  "One follower dry release price" every 5 s, by kind, with the sum as the price of running
  those releases at E's place. Racy per word (a newer pass overwrites an older unwritten one).
  The QMD prediction line's "other writer" count also splits off the resumes met by the
  release recorded for the word: puller payload releases and other payload releases.
- FRITCHII_ONEF_SEM_E (live, default 0; needs FRITCHII_ONEF_POST_FENCE; fable_b 19.4, 19.6,
  20.3): the semaphore and sync point writers run at E's place, fences not delayed:
  - a sync point E posts makes both increments itself (guest, then host after the buffer half)
    and R makes an empty fence: the game's wait on it ends when E passes it. The frame end
    stays lent, so E is at most a frame ahead of R;
  - a SignalFence with a callback (QMD reduction and max releases, the GP sync) runs the
    callback on E between the buffer half and the gather, then posts the fence empty;
  - a payload release (not Accurate) is written on E (after its page's landings) and latches
    its waiters there; R only notes the streamer's pending sync, synced at the next fence into
    the buffers E obtains (16.2);
  - a synced fence-flagged payload report (EXP33 off) is a posted fence whose write E makes;
  - one home: whatever cannot go to E is lent (a fence not clean, Accurate payloads, a report
    type without a streamer), never posted to R. Must-be-0 "semaphore order": a write on E or
    R to a word last written from a later stream place;
  - the front's lagged check on a word whose newest release is a payload one waits for E's
    position ("lagged check (E)" in the waits by site).
  With POST_FENCE on, E also runs the commit's buffer half itself when flushes are pending
  (R's fence carries the downloads), so pending flushes no longer keep a fence lent; marks
  ahead still due do. The fence decisions line counts every fence E could post: posted, posted
  with E's commit, lent because POST_FENCE is off, fences delayed, stopped, a sync owed, or
  marks ahead due; and the samples sync targets R did not need because E's mirror was ahead
  (no longer "sync target unused").
- FRITCHII_ONEF_SIGNAL_ON_R (live, default 0; with FRITCHII_ONEF_SEM_E): SEM_E's
  game-visible signals happen at R's played place, not E's (AMD tester, test 4 §5c: POST_FENCE
  plus SEM_E lost the device at the market fountain load on an RX 9070 XT, POST_FENCE alone did
  not). A payload release is a posted op (E goes on) whose write R makes when it plays it; the
  front's E-position waits wait for it (a guest-writing op) and E's reads of its page wait until
  R played it. A synced fence-flagged payload is written by R inside its posted fence. A sync
  point is posted as without SEM_E (R increments both). A SignalFence with a callback is lent
  (the callback may touch E's caches). The fence decisions line gives SIGNAL_ON_R and "signals
  made on R for E" per second.
- FRITCHII_ONEF_WAITS_R (live, default 0): every front wait ends at R's played position: it
  overrides FRITCHII_ONEF_WAITS_E (a preset turns it on) and SEM_E's lagged acquire check
  ("lagged check (E)"). The same as WAITS_E=0 with SEM_E's lagged check on R.
- FRITCHII_ONEF_MIRROR_STOP (live, default 0): the must-be-0 "sync mirror: a samples report: E's
  mirror started, R's counter not" (R skipped a draw E's mirror counted: no pipeline, configure
  failed). 1 = FRITCHII_ONEF_POST_FENCE stops for the session, as the opposite case does
  (logged at Critical); 2 = counted as explained, no note, when R skipped a draw with the samples
  count on since its last samples report ("E's mirror ahead after a skipped draw" in the fence
  decisions line). The superset itself is harmless: E lists a sync R then does not need.
- FRITCHII_ONEF_SOFT_RELEASE_E (live, default 0; fable_b 22.3): a non-forced ReleaseFences
  (GPU::OnCommandListEnd, about 1,400 a second, the lent "ReleaseFences" site: the forced ones
  are posted by POST_FENCE) is answered on E: with the fencing thread WaitPendingFences(false)
  does nothing.
- FRITCHII_ONEF_POST_IDLE (live, default 0; needs FRITCHII_ONEF_POST_FENCE; fable_b 22.3): a
  WaitForIdle with no report of another kind owing the query sync is posted: E obtains the
  buffers for the values R syncs (16.2) and runs SignalOrdering's flush grouping at its place;
  R records the barrier and syncs into the named buffers. 1 = only with R caught up (R played
  every packet E pushed: the lent form's order, E just does not wait for the wait for idle
  itself); 2 = also with R behind when no posted fence with report landings is unplayed (a
  test: posting with R behind flashed the image dark in the market, the lent form kept R
  caught up 5,900 times a second). The fence decisions line counts each wait for idle: posted
  with R caught up or behind, lent because R was behind, a landing was due, or a sync owed.
  Must-be-0 "signal ahead of landing" (SEM_E): E made a game-visible signal (sync point
  increment, callback, payload write) while R had not played a posted fence that lands
  reports E queued before it.
- FRITCHII_ONEF_POST_FENCE_REPORT (live, default 0; needs FRITCHII_ONEF_POST_FENCE): a clean
  fence's counter report (a type with a streamer: its value is R's; EXP33's kept reports and
  payload reports excepted) is posted as the rasterizer's fences are: E runs the buffer half,
  obtains the sync buffers (the report's own samples value included) and the gather; R runs
  CounterReport and its SignalFence. Its page waits for the op (13.0). Counted as query kind
  "fence report posted". With POST_FENCE on, E also lands the guest reads' downloads over the
  report pages a posted fence lands, and R's report writes inside a posted fence never wait for
  the buffer lock: must-be-0 "R read landing" when one still finds a download (landed only with
  the lock free) or the lock busy (skipped).
- FRITCHII_ONEF_OP_CLEAR (live, default 0; fable_b_one_follower 13.4 group 3; step 4, view
  tracked): 1 = clears are render ops. E has decided the render enable (DrawManager tests
  ShouldExecute) and runs the clear's FlushCaching; R plays Clear from its view (every clear
  register is a watched word) without it. The op carries its R-only dirty flags by rule 1, as a
  draw packet does (PacketRenderState). Counted as "clear" lends gone from the lends-by-site line.
  Known fault (15 Nov): with unlent array draws (UNLEND bit 0x2) the market renders about 15%
  darker (mean luminance 100-104 against 119). Fixed in c1472cfd23 (fable_b 18.3: a posted
  clear keeps the flags E's pipeline lookup reads on E's engine); measured 117.4 with clears
  posted, 114.5 without.
- FRITCHII_ONEF_CLEAR_NODEFER (live, default 0; diagnostic for that fault): 1 = a posted clear
  on R clears at once instead of becoming the next render pass's load op (deferred clear).
- FRITCHII_ONEF_UNLENT_RESET (live, default 0; fable_b_one_follower 19 step 2; step 4): 1 = a
  non-indexed single-instance native draw of points is no longer lent as an indirect-reset
  candidate. E runs IsNx2IndirectReset's whole test itself at its place on its own engine (a
  mesh pipeline is never one; otherwise it reads the program's code with ReadBlock, whose flush
  is lent or answered on E as any E read) and lends only a draw that is a reset. The others are
  pushed with PacketNoReset and R's Draw skips the test (it would read game memory on R). The
  forced-lent verify draws compare E's answer with the test on E's engine inside the lent call:
  must-be-0 "unlent reset", which stops the switch for the session. "indirect reset" on the
  transient lends line is what it removes (the market's mesh draws, about 290 a second, E).
- FRITCHII_ONEF_UNLENT_VALUES (live, default 0; fable_b_one_follower 19 step 2; step 4): 1 = what
  R's view took only at lent packets besides the registers (SyncFromEngine: the engine hint
  engine_state, the native macro mode, the constant buffer bindings state.shader_stages) goes
  in the packets by value when it changes: SpanEngine (8 bytes) and SpanBinds (each binding
  ProcessCBBind wrote since the last packet, 16 bytes each), after the pools span. A draw after
  an HLE macro or a macro mode change is then not lent to sync the view ("engine hint" and
  "native mode" on the transient lends line). E takes the mode on at a lent packet (R's view
  takes the engine's values there) and gives it up the same way: after the switch goes off,
  draws are lent until the next lent packet. Must be 0: "view values", at lent draws and every
  16th other lent packet R compares the view's by-value values with E's engine before taking
  the engine's; a difference stops the switch for the session.
- FRITCHII_ONEF_BIND_SEED (live, default 0; fable_b_one_follower 19 step 2; step 4): 1 = a lent
  channel bind (BindChannel, about 800 a second in the market) re-seeds at its own end, E still
  parked: E's tracking starts over from the new channel's engine (PackSpans' seed), the generic
  view is seeded from it, and R's part (c) view follows the new channel (SyncFromEngine in full,
  then R names it again). Before, the view kept the old channel until the next lent packet, so
  the first draw after every bind was lent only to carry that seed ("channel" on the transient
  lends line) and paid R's whole backlog. R's view engines are kept per channel (at most 4), so
  a bind back costs a full copy, not a new engine. Must be 0: "bind seed view" (a played draw
  found the view on another channel), which stops the switch for the session.
  The bind itself stays lent: the rasterizer's channel fields (channel_state, maxwell3d,
  gpu_memory, kepler_compute) are read by E (RouteDraw, the posted clear's FlushCaching, the
  query and flush tests) and by R (played draws, ops) at their own places, and the view scope
  saves and restores maxwell3d on R; a posted bind needs those split into E's and R's copies.
- FRITCHII_ONEF_CARRY_OPS (live, default 0; fable_b_one_follower 22.3 step 2, "query reset and
  report inside the draw packet"; with QUERY_OPS on, step 3+): 1 = the samples counter resets
  and the posted samples reports (ZPassPixelCount64, about 75,000 of each a second in the
  market, 1.15 a draw together) are not an op packet each: E keeps them (PushOp's `carry`,
  payload copied, no spans packed) and the next packet E pushes, of any kind, carries them
  after its body (PacketCarried, a CarryTrailer last). R runs them first at that packet, in
  order, before its spans, its draw, its op or its lent call, so the order against every other
  packet is today's. They read nothing of R's view (the samples streamer reads no register;
  the transform feedback ones do and stay op packets). E sends what it keeps as a packet of its
  own (a Delta with no spans) before every `consumed` store (GpuSplit::StoreConsumed, so a
  kept report's GuestWriteOpMark is set before the front can see E past it), at a drain, and
  when 1 KiB is kept; a carrying packet's floor (`played`'s clamp) is its oldest kept op's
  position. Line "One follower carried ops" every 10 s: ops carried a second, the packets that
  carried them (draw, lent, op, alone), and why alone ones went. Expected (Fable 22.3): 20 to
  30 ms/s off E, and R's per-packet cost for the same number of packets.
- FRITCHII_ONEF_UNION_FILTER (live, default 0; fable_b_one_follower 14.4 cut 5;
  FRITCHII_ONEF_INVAL_E): 1 = E's invalidation test (OneFollowerInvalidationOnEngine, about
  157,000 a second, each range against the texture, shader, query and report page filters,
  twice) tests one union filter per range first and the four only when it has a bit there.
  The union is always kept: each of the four marks it too (PageFilter::AlsoMark, attached at
  the rasterizer's construction, the report filter when E makes it), inside the same Mark, so
  R's fence after its mark covers the union as it covers its own filter. Bits never clear, so
  a clear union bit proves all four clear: no behaviour change. Expected 5 to 8 ms/s on E.
- FRITCHII_ONEF_CB_LEAN (live, default 0, bits; fable_b_one_follower 23.1 cut 3, on E): the
  constant buffer words. 1 = a single word (275,000 calls a second in the of4 market, "1 words"
  in "Price constant buffer word calls") goes through the block write,
  Maxwell3D::CallConstBufferWords, as runs of two or more already do, instead of CallMethod,
  DispatchMethod, CallMethodFast and ProcessCBData; the block write takes every check of that
  path (fast mode, no macro collecting, a bound address, the buffer's size, one page) and falls
  back to it where it cannot write the run in one. 2 = the stream-write guard
  (GuardStreamWrite, one per constant buffer write) tests the union page filter first
  (FRITCHII_ONEF_UNION_FILTER's, always kept) and the texture and shader filters only on a hit.
  Exact: the union holds every bit of both. Small: about 2 to 5 ms/s unprofiled together. The
  bind's translation memos already exist as switches (EXP18 & 1, FRITCHII_BIND_ADDR_MEMO).
- FRITCHII_ONEF_KEY_KEEP (live, default 0; fable_b_one_follower 23.1 cut 2; with
  FRITCHII_ONEF_E_PIPELINE and the write tracking on, step 2+, LEAN): the clean pipeline lookup
  (EXP19 / EDEN_NX2_PIPELINE_CLEAN) compares the key's watched register words with its snapshot
  at every lookup (CleanInputsUnchanged, and the blend words with EXP50), about 45% of the
  lookup. NoteRegWrite now counts every write of those words on E (or R in a lent call), before
  its per-packet dedupe, into OneFollower::g_key_writes (the words: OneFollower::g_key_words,
  set by BuildCleanWatch; the count also moves when the tracking stops). The lookup keeps the
  count each time its snapshots equal the registers (a hit, or a new snapshot). 1 = count the
  lookups that reach the compare with the count unmoved, and still compare: "Lookup key words
  ... untouched since the snapshot N, compare kept out 0, untouched but the compare failed 0
  (must be 0)" every 10 s next to the EDEN_NX2_PIPELINE_CLEAN line. 2 = those skip the
  compares. Every flag, topology and engine-state test before the compare still runs, so no
  flag leaves E's engine before its reader (section 18). Exact because every register write on
  E goes through ProcessDirtyRegisters or ApplyDeltaWrites, which note it (the "view words"
  must-be-0 checks the same notes). Expected 20 to 25 ms/s on E where most lookups find the
  words untouched; run =1 first and read the count.
- FRITCHII_ONEF_UBO_KEEP (live, default 0; fable_b_one_follower 25.2 cut a): the streamed
  small uniform binds (BindHostGraphicsUniformBuffer's stream path, BindSmallUniformReusing).
  Each slot keeps its last streamed bind's device range, its pages' write generations
  (Common::PriceGenSum, switched on from the first test) and whether the buffer cache's tracker
  held them clean. A bind is provable when it is the same range, the generations did not move,
  and the pages were tracked then and are now: every writer raises a generation (constant
  buffer words and every buffer cache write through g_buffer_write_pages, the guest's trapped
  stores through g_collected_write_pages and again after the bytes land, with the bank marks
  on), a CPU-modified page would let guest stores through unseen, a GPU-modified range never
  streams. Not on native bank binds, behind a run-ahead frontier or with the bank marks off.
  1 = count and verify: the usual bind still runs, and one that then finds other bytes (a new
  copy serial) is counted, must be 0. 2 = a provable bind binds the slot's stream copy again
  (RebindUniformSlotCopy: still the slot's, the same size, the current stream epoch) without
  the guest read, the compare or the reuse function; otherwise the usual bind. Line "Uniform
  keep" every 10 s. Expected 20 to 25 ms/s on E (the price line's cut class, 182,600 binds a
  second in of10).
- FRITCHII_BANK_FAST (live, default 0; fable_b_one_follower 25.2 cut b): the native bank check
  (CheckNativeBankPagesTracked, EXP9=3) skips a settled bank of its own or flagged slots whole
  (no page flag, every page snapshotted and not due, no CPU-modified page), but only after its
  switch reads, the marks gate, the census and the store upkeep. With the switch, a bind of a
  slot the last full call would skip, while nothing that upkeep reads moved (the switch
  generation, the lost-store count, the marks gate ready, the page table's size, the inner
  invalidation setting), runs the same four tests first and returns with the same counts.
  Same result. Expected 8 to 10 ms/s on E (56,000 bank binds a second at 246 ns).
- FRITCHII_S4_COUNT (live, default 0; fable_b2_answers 3.3, fable_b_one_follower 28.5): the S4
  counter, dry (video_core/fritchii_s4_count.cpp). S4 would serve the fill's uniform binds from
  a mirror of their pages brought up to date once per list entry. Each graphics uniform bind of
  the fill (BindHostGraphicsUniformBuffer, before the bank check and the copy) is counted on E:
  its path and bytes, its 4 KiB pages by class (tracked; untrapped = CPU-modified, guest stores
  land unseen; stream-written = E's own uploads moved its write generation while it was bound,
  the driver banks; GPU-written), the list entries E crossed (the front records its ring
  position at the start of each list entry it parses, GpuSplit::WritePositionNow, without
  ending the open block), and whether a range's bytes changed between two of its binds inside
  one entry (ranges up to 16 KiB hashed whole at every bind, larger ones one page in 64 by
  address, scaled), by cause from per-writer page generations (Common::S4Count: E's writes
  through WriteRing::Note and MemoryManager::Write are stream uploads, buffer downloads and
  words written off E are GPU results, other threads' noted writes are emulator writes, no
  writer seen is a guest store, on a page the tracker held clean at the earlier bind or not).
  2 = also a sample: at 1 list entry in 256 the front hashes up to 8 ranges E bound lately as
  a mirror would see them there (guest memory with OverlayQueuedWrites laid over; a range a
  noted queued upload writes, SplitWriteTracker::LastWriteTo, is "queued", not compared) and E
  compares at its bind of the same range in that entry. The front cannot sample per bind: which
  slots a draw reads is the pipeline's, known on E only. Two lines every 10 s, "S4 count
  (FRITCHII_S4_COUNT=N) 10 s:". S4 pays if the mirror upkeep (MB/s) is well under the bytes the
  fill reads and compares today, and is exact only if both "MUST BE 0" figures are 0 (changes
  inside an entry other than stream uploads). With the split off there are no list entries and
  the second line means nothing. Costs E a hash of each bind's bytes while on (not for a
  profiled fps run). Off: one relaxed load and a branch per uniform bind, per list entry on the
  front and per noted write; the switch is read once a draw.
- FRITCHII_BIND_SKIP (live, default 0; S4-lite, after FRITCHII_S4_COUNT's market counts): a
  graphics uniform bind (BindHostGraphicsUniformBuffer) skips the guest read, the compare, the
  stream copy, the synchronize and the native bank check when the same (stage, slot) last bound
  the same range (address, size, shader size, buffer, channel, switch generation, skip size)
  and its bytes provably did not change since. The proof is the range's write generation
  (common/fritchii_bind_skip.h), read before the earlier bind read anything: the price
  generations (every mark of the bank check's two page flags: E's stream writes, the buffer
  cache's writes and downloads, the write ring's Note on any thread, MemoryManager::Write's
  words, guest stores on tracked pages before and after the store; the switch holds the bank
  marks gate open) plus the tracker's own generations (every change of a page's CPU,
  Untracked, cached-write or GPU bit in the word managers, and every new manager). 1 = within
  one list entry (any page state: guest stores on untrapped pages inside an entry are assumed
  absent, as the S4 counter measured), 2 = also across entries when the range's pages are
  tracked now (with no state change since the earlier bind, tracked all along: guest stores
  there trap and mark), 3 = 2 verified, 4 = 1 verified (the old path runs too; stream: the
  slot's copy against guest memory, cached: a hash of the range at the earlier bind; must be
  0), 5 = dry (decide, count and time, never skip). Stream binds skip to the reuse slot's own
  rebind (same copy serial, same stream epoch); cached ones to the bind after the synchronize
  (a hit). Not skipped: OpenGL, keeps (EXP92/EXP91), mapped copies, the root table's bank,
  EXP9=1 or 2 banks, a run-ahead frontier, a lost after-store mark (every memo dropped). Skip
  modes check one would-skip in 1,024 like the verify and stop for the session at a
  difference. One line every 10 s, "Bind skip (FRITCHII_BIND_SKIP=N)": binds, skipped (in
  entry, across), why not, sampled ns (decide, skip, the old path of skippable binds, the memo
  upkeep of the others) and the ms/s of E saved, the verify counts. Off: one member test per
  bind, one relaxed load per tracker state change, the switch read once a draw.
- FRITCHII_ESPLIT_COUNT (live, default 0; the E split's step S0, work/E_SPLIT_DESIGN.md 4.2,
  src/video_core/fritchii_e_split.h): dry, no thread, no value changed. 1 = every ring entry E
  runs is classed E1 (registers, draw manager, pipeline lookup, span packing), E2 (constant
  buffer words, fills, buffer cache, fences, compute, guest-visible work), join (E1 waits for E2
  to drain and runs it: inline uploads, 2D/DMA/inline-to-memory methods, untagged ring calls)
  or lend. Channel calls are E1's and no join: E2 takes the bind in order (at 2 and 3 a record is
  handed over at each), binding the bound channel again changes nothing, and a real switch's
  RasterizerVulkan::BindChannel lends, which parks both stages (counted as a lend; the line
  splits channel calls into switches and rebinds). A draw's fill, ring B and R packet push and TickWork, and a bind's
  buffer cache half, are carved out of their E1 entry as E2, R's span packing and the method
  sink's register writes (also at the head of an E2 entry) back out as E1, every lend as lend.
  1 entry in 64 timed (random pick, scaled by 64). Stall points: joins by
  cause, E1 decisions that read game memory (the indirect reset test), lookups after an E2
  stream write onto shader pages; conditions from memory counted apart (forwarded, decided on
  E2). 2 = 1, and the hand-over rehearsed on E: at each E2 entry and each E draw fill E1's
  record (the W2 register words written since the last record, the constant buffer bindings and
  root table words changed, the draw state by value) is built into a ring and consumed at once
  into E2's view, then the view is compared with the engine (W2, and at fills the bindings, the
  root tables, the draw state, a list draw's indirect parameters and the buffer flags; must be
  0, the first 16 differences logged with the register index, a difference is taken from the
  engine so one miss counts once). A draw record also carries the buffer flag take: the buffer
  flags raised on the engine since the last record by anything but a recorded register word (the
  Vulkan state tracker's invalidations, the cache's marks, a lent call's), which in the split E1
  reads and clears off its engine and E2 raises in its own copy; a line counts them by flag and
  where VertexBuffers rose (the draw's stream flush, its lookup, its TickWork, E2 entries, joins,
  lends) and the state tracker's invalidations on the viewed engine by thread (any thread but E
  or R in a lent call must be 0). Build and consume timed 1 in 64 (the hand-over cost). Needs
  the one follower's write tracking (FRITCHII_ONEF step 2+), else "untracked hand-overs". 3 = 2,
  and E's fill reads the view (SetInputsOverride, and the handle values' bindings) wherever the
  compare before it was clean; every input reachable through CacheInputs compared again after
  the fill (must be 0). Lines every 10 s, "E split count (FRITCHII_ESPLIT_COUNT=N) 10 s:": the
  classes' ms/s and entries/s; the stall points; the net movable work = E1 - hand-over
  (estimated at 1: 0.15 us a filled draw + 0.05 us an E2 entry; measured at 2 and 3) - joins x
  2 us - E1 decisions x 4 us - lends x 1.5 us; at 2 and 3 the records and the view differences;
  at 3 the fills from the view. Read the classes from 1 (2 and 3 add the rehearsal's work, and
  move the sink's writes before E2 entries out of the timed entries). S1 bar: net >= 60 ms/s
  profiled, stall points <= 2,000/s, view differs 0 over a step-in. Off: one relaxed load per
  ring batch, a local test per entry, two plain loads per filled draw (and three tests of a local
  it holds), one per lend, bind, root table load, condition, stream write, non-empty sink and
  Vulkan state tracker invalidation; the register write hook shares the key-watch byte.
  ring batch, a local test per entry, two plain loads per filled draw, one per lend, bind, root
  table load, condition, stream write and non-empty sink; the register write hook shares the key-watch byte.
- FRITCHII_FRONT_SPLIT_COUNT (live, default 0; the front split's step FS0,
  work/FRONT_SPLIT_DESIGN.md 4.3, src/video_core/fritchii_front_split.h): dry, nothing sent
  changes. 1 = one DmaPusher::Step call in 64 (random pick, scaled by counted / timed) timed by
  stage with time stamp ticks: parser (Step, re-reads, entry checks, acquires, header decode),
  3D state (below Maxwell3D::FrontCallMethod / FrontCallMultiMethod, macros included), compute
  (KeplerCompute's front calls), ring sends (FrontWriteSend's entry, the list's Method / Multi
  entries; compute's own sends stay in compute); waits for the back (WaitFor, Drain) are taken
  out of the stage they happened in. Counted: entries, headers, words, suspensions; 2a's joins
  by cause (an acquire check with engine methods decoded since the last join, a list writer
  check with a writer note since, a GPU-written list on the list draw / dispatch path, Step's
  Run) and Step's Defers; the raw ring words 2c would send for 3D and compute (Method 1, Multi
  2 + words / 2) against the ring words sent; macro DMA reads by answer (0xe2c apart: forwarded,
  snapshot hit, drained for a noted overlap / writer / new page / report page / no snapshot
  path, deferred to E); entries copied again by a second refresh before their parse; front time
  per channel. Line every 10 s "Front split dry: ...". Off: one relaxed load and a compare per
  Step, one plain load per hooked call (stage boundaries incl. FrontWriteSend, engine calls,
  writer notes, acquire checks, macro DMA reads, waits).
- FRITCHII_NOTED_SUMMARY (live, default 0; F4 of work/FRONT_SPLIT_DESIGN.md 2d, the same shape as
  FRITCHII_DISPATCH_WALK_SUMMARY): GpuSplit::NotedWriteSince (each 0xe2c read check whose page
  snapshot predates a noted write) answers from page buckets before walking pending_writes. The
  front keeps the newest note position per page bucket (4,096 by page hash) and one for notes
  over more than 16 pages or of unknown range, from the moment the switch is first read non-zero
  (the "floor"); a read since a position at or after the floor whose buckets and wide position
  are all at or before it has no noted write (exact: every later note set its pages' buckets;
  collisions only cause walks; the walk's own "table overflowed" answer is only conservative).
  1 = on (answered checks skip the walk; 1 in 64 checks walk anyway, timed and compared), 2 =
  verify (the walk runs after every check and answers; NotedSummaryDiffer: buckets said no,
  walk found an overlapping note, must be 0, first 16 logged), 3 = dry (as 2, read for counts
  and the timed saving). Line every 10 s "Noted summary (FRITCHII_NOTED_SUMMARY=N)": checks,
  answered, why the others walked, walk notes the answered checks skip, the timed summary and
  walk ns and the saving in ms/s, the differences. Also in the "Front split dry" line. Off: one
  relaxed load per check, one plain load per note.
- FRITCHII_NOTED_FAST (live, default 0; replaces the walk FRITCHII_NOTED_SUMMARY could not
  answer: its one "wide" position was always newer than the read): GpuSplit::NotedWriteSince
  answers from note chains instead of walking pending_writes (NotedWalk, the front's biggest
  self-time item at Kakariko, ~74 ms/s). From the moment the switch is first read non-zero (the
  "floor") every note is linked, newest first, into one chain: its page bucket's (4,096 by page
  hash) when it lies inside one page, else the wide chain (more than one page, or unknown
  range); a note that covers its chain's head (the head's bytes within its own) is linked past
  it, so repeated notes over the same bytes keep the chain short (exact: every read overlapping
  the head overlaps the newer note). A check looks only at the wide chain and the buckets of
  the read's pages, each down to `since`, with the walk's own overlap test; the table's
  overflow ("more noted since than
  remembered") is one compare. Exact both ways: the answer equals the walk's (None against
  Overlap / Overflow); a check whose walk would reach a note older than the floor, or a read over
  16 pages, walks. 1 = on (answered checks skip the walk; 1 in 64 walk anyway, timed and
  compared), 2 = verify (the walk runs after every check and answers; NotedFastDiffer counts
  disagreements, must be 0, first 16 logged), 3 = dry (as 2, read for the counts and the timed
  saving). Line every 10 s "Noted fast (FRITCHII_NOTED_FAST=N)": checks, answered without the
  walk (of them a write, and the table overflow), walk entries skipped, sampled saving ms/s,
  differ (window, session), checks that walked anyway (older than the chains, long reads), chain
  notes looked at, notes linked and the wide chain's share with its top 3 writers by kind. Off:
  one relaxed load per check, one plain load per note.
- FRITCHII_FRONT_TRIM_RANGE (live, default 0; helper Z, front trims): GpuSplit::RangeLastWrite
  (every constant buffer upload word and report, ~2 million a second at the Castle Town market)
  with FRITCHII_STAMP_READ_PAGES: StampRangedRead's per-page answer (the GPU page's translation
  and whether a stamp reader ever asked about its device page) comes from a 64-entry table
  indexed by GPU page instead of the one-entry caches, which miss whenever two pages take turns
  (upload words and report writes do: a GpuToCpuAddress and an unordered_map lookup each time).
  An entry is filled only from a fresh, stable translation (no page table change running or
  started during it) and answers only for the same memory manager, GPU page and queued function
  position (the one-entry cache's own key), with the map serial and the read-pages table
  unchanged since (stamp_read_gen: bumped at every page added, table clear and reset): exactly
  what a fresh lookup gives. Also RangeLastWrite's EDEN_NX2_RANGED_WAITS and
  EDEN_NX2_FORWARD_READS come from a member filled once from the same statics (a thread-safe
  static guard per call, two per word, before). 1 = on, 2 = verify (the old lookup runs and
  answers; a table answer that differs is counted, must be 0, first 16 logged), 3 = dry (the old
  lookup answers; table hits counted, both timed 1 in 64). Line every 10 s "Front trim range
  (FRITCHII_FRONT_TRIM_RANGE=N)": pages looked up, answered by the table, fills, verify checks
  and differences, dry ns per lookup (old, table) and the saving in ms/s (the static guards'
  share is not in it). Off: one relaxed load per ranged write.
- FRITCHII_FRONT_TRIM_QMD (live, default 0; helper Z, front trims): the front's QMD scheduler
  copy (EXP13 2-4, KeplerCompute's FrontQmdMirror) finds its overlays (FindOverlay, AddOverlay's
  slot) and upload shadows (FindShadow) through compact arrays instead of walking the entries
  (256 overlays of 288 bytes, 64 shadows of 4 KB: one cache line each, per QMD read, inline QMD
  and patch; ~1.1% of the front at the market). The arrays hold each overlay's address and
  position and each shadow's address, kept whatever the switch says (overlay addresses are
  written only in AddOverlay, positions only through SetOverlayPosition, shadow addresses only
  in NewShadow); the lookups visit the same slots in the same order with the same tests, a
  shadow whose address is not within 4 KB below the QMD skipped (it fails the walk's tests:
  a shadow's filled bytes are at most 4 KB). 1 = on, 2 = verify (both run, the walk answers,
  differences counted, must be 0, first 16 logged), 3 = dry (the walk answers, both timed 1 in
  64). Line every 10 s "Front trim QMD (FRITCHII_FRONT_TRIM_QMD=N)": lookups per site with the
  timed walk and array ns, verify differences, the dry saving in ms/s. Off: one relaxed load per
  lookup; the arrays cost a store per overlay or shadow change.
- FRITCHII_FRONT_ASYNC_TABLE (live, default 0; helper AA, report table): GpuSplit's async write
  table (NoteAsyncWrite: every counter report and timed release, written when the host query
  completes; HasAsyncWrite: RanReadCheck for the 0xe2c conditional render reads, ~48,000/s at the
  Castle Town market, 3 slots each, and SemaphoreReadableNow) kept per block of 8 neighbouring
  16-byte slots: a block's 8 positions are one 64-byte line, found through a small open-addressed
  index by block number with a one-entry memo for notes and one for reads. The old table puts
  every slot on its own random line (Set 123 + Find 89 samples of v4sample1, ~1.6% of the front).
  HasAsyncWrite returns the same number for every call: the newest position noted to the slots,
  0 when none; a slot never noted reads 0 in a block, which adds nothing to a newest, as a
  missing slot does in the old table. Nothing is erased (as before: only at reset). Switch
  changes hand the slots from one table to the other on the front at its next note or read.
  1 = on (the blocks alone), 2 = verify (both kept, the old table answers, every read compared,
  differences counted, must be 0, first 16 logged), 3 = dry (both kept, the old table answers,
  notes and reads timed 1 in 64 in both, in turns first). Line every 10 s "Front async table
  (FRITCHII_FRONT_ASYNC_TABLE=N)": notes and reads per second, blocks and old table size, verify
  differences, dry ns per note and read in each table and the saving in ms/s. Off: one relaxed
  load and a member load per note or read. Worst case memory (one slot used per block) 96 bytes
  a slot against the old table's 32 to 64.
- FRITCHII_NOTED_OVERFLOW (live, default 0; F7, needs FRITCHII_NOTED_FAST 1 to 3): the table's
  overflow answer of the chains ("more noted since than remembered", ~98% of the chain checks at
  the Castle Town market, the front DMA reads' page snapshots) asked again of the chains. While
  NOTED_FAST is live each link and head also keeps the position of the note it names, so a chain
  knows how far back it reaches after the table forgot that note: walking the wide chain and the
  read's page buckets from their heads (the walk's overlap test on every remembered note), a
  link to a forgotten note at or before `since` ends the chain clean (positions never rise along
  a chain). Exact: "no write" means no note sent after `since`, remembered or forgotten,
  overlaps the read; a forgotten link after `since`, notes from before the chains went live that
  may be after it, or a read over 16 pages stay a write. 1 = on (a proved answer is "no write" at
  FRITCHII_NOTED_FAST=1; at 2 and 3 the walk answers), 2 = verify (every proved answer checked
  against the table, a remembered overlapping note counted, and 1 in 16 against a shadow of the
  last 65,536 notes, a forgotten overlapping note counted: both must be 0, first 16 logged as
  NotedOverflowDiffer; "too short" when the shadow does not reach back to the snapshot), 3 = dry
  (counted, the chain walk timed 1 in 64, and Maxwell3D::FrontDmaRead's ran-reads check after a
  proved write answer counted and timed 1 in 64 per page check: what 1 would spare). Line every
  10 s "Noted overflow (FRITCHII_NOTED_OVERFLOW=N)": checks, table overflow, chains prove no write
  (share), why the rest stay a write (overlap, forgotten note after the snapshot and its wide
  chain part, older than the chains, long reads), chain notes per overflow, sampled chain cost
  ms/s and ns each, front DMA reads after a proved answer (ran-reads check served, drained, no
  check with FRONT_RAN_READS off: a drain), sampled ran-reads check cost ms/s and ns each, verify
  counts. Off: one relaxed load per overflow answer, one plain load per front DMA read write
  answer; the note path stores two more words while NOTED_FAST is live.
- FRITCHII_STEP_LEAN (live, default 0; F6 of work/FRONT_SPLIT_DESIGN.md 2d): 1 = the list writer
  waits line (MaybeLogListWaits) reads the clock on every 256th call instead of every list
  entry (its rates are counts over the clock time between lines: unchanged, a line comes at
  most 255 entries later), and a command list that ran out gives its heap storage (entries past
  the inline 512, submitted words, offsets, deferred entries) back to the submitter through 8
  lock-free slots instead of freeing it on the front; the submit's CommandList takes it
  (emptied, capacity kept). Storage finding no free slot is freed as before; over 16 MiB of
  words is never kept. Off: one relaxed load per list entry check and per list popped, one call
  and a relaxed load per list submitted.
- FRITCHII_REFRESH_LEAN (live, default 0; helper CG, 2026-11-16): the command words a list
  refresh copies again from memory (DmaPusher::RefreshSnapshot after a channel resumes from a
  semaphore acquire, RefreshDeferredEntries after a GPU map). Old: ReadBlockUnsafe per entry,
  which translates the entry's page every time (GPU page table, then device to host); the v4
  sample had RefreshSnapshot at 3.7% of the front, 0.9% of it in that translation. 1 = each
  4 KiB GPU page is translated once per refresh (the entries of a list mostly sit side by side)
  and each entry copied from that host pointer plus its offset, which is what ReadBlockUnsafe's
  own one-page path reads; a page without a host pointer (unmapped, sparse without memory,
  outside the address space) sends the entry the old way. Off while the back read audit runs.
  2 = verify: the cached copy into a scratch buffer, then the old copy into the list (the words
  used stay the old path's), every byte compared. Line every 10 s while on: "Refresh lean
  (FRITCHII_REFRESH_LEAN=n) 10 s: refreshes R/s, entries E/s, K KiB/s; pages translated T/s,
  reused U/s, entries the old way O/s; verify: C entries compared/s, D differ (must be 0, S this
  session)". The saving is about U x 20-30 ns a second. Off: one relaxed load per refresh.
- FRITCHII_ONEF_NULL_FILL (live, default 0; test only, the picture breaks): 1 = E skips
  FlushCaching, FillRing and PushDraw for every draw it would fill (no packet, R draws nothing;
  TickWork as after a push). The front-wall probe of work/FRONT_SPLIT_DESIGN.md 4.3. Line every
  10 s "Null fill (FRITCHII_ONEF_NULL_FILL=1, ...)". Off: one relaxed load per filled draw.
- FRITCHII_ESPLIT (boot, default 0; the E split's thread, steps S1 and S2 of
  work/E_SPLIT_DESIGN.md 5, src/video_core/fritchii_e_split_thread.h): 1 = the engine thread E
  becomes two. E1 (the back thread) reads ring A, applies the register entries on the real
  engine, looks the pipeline up; E2 ("GPUBackE2", new) runs the E2 entries of the dry classifier
  (constant buffer words, reports, sync points, wait for idle, cache barriers, compute, the
  memory stage's cache calls), each draw's flush (before E1's lookup, only when it has work) and
  tail (FillRing, the R packet's push, TickWork) and each bind's buffer cache half (by value).
  E2 keeps its own view of what a fill reads, fed by E1's records (FRITCHII_ESPLIT_COUNT=2's
  records, now across the threads). E1 runs join entries, its memory decisions (the indirect
  reset test), conditions from memory and E's idle work itself (joins). Needs FRITCHII_ONEF=1
  (refused with a log line without it); FRITCHII_ESPLIT_COUNT is ignored while it runs.
  FRITCHII_ESPLIT_DEPTH (live, default 0) = 0 is S1, lockstep: E1 hands each piece over and
  waits for it, so one thread runs at a time in today's order on today's objects (exact by
  construction); E's per-call thread state (one follower's draw values, lend site, query kind;
  the memory stage's fill state) moves with each hand-over and a buffer cache lock E1 holds is
  handed to E2 for the run. Both sides poll (haltpoll rule: E1 up to 200 us, E2 up to 50 us)
  and then sleep. Lines every 10 s, "E split thread (FRITCHII_ESPLIT=1, depth D) 10 s:":
  hand-overs a second by kind, E2 busy (zone "e split job"), E2 waiting ("e split E2 wait"), E1
  in hand-overs ("e split hand-over") = E2's runs + the hand-over cost (ms/s and us each), joins
  by kind; then the checks (must be 0): ESplitView (E2's view against the engine after the
  record, 1 hand-over in 64), ESplitInputs (on those draws, view equal, the fill reads the view
  and every CacheInputs input is compared after it), E1Touch (E1 released the buffer cache's
  lock, read game memory through the audited funnel or wrote it, outside a join; the first 16
  logged with the site), E2Order (E2 ran a hand-over out of order); seeds and untracked
  hand-overs. Gate 2 shape for S1: E2 busy <= today's E occupied - 0.7 x the dry net. Off:
  nothing starts; a hand-over site pays one load and a branch.
- FRITCHII_ESPLIT_DEPTH (live, default 16 = the queued mode; with FRITCHII_ESPLIT=1, step S2;
  0 = S1's lockstep, kept for the checks; capped at 48): E1 never waits per piece of work. It
  appends records to E2's queue and goes on, up to N draw tails ahead: constant buffer writes'
  memory halves (S4, FRITCHII_ESPLIT_CB_ASYNC, on by default here: several writes a record, the
  register half on E1), each bind's buffer cache half by value, each draw tail with its record and
  R's packed packet, the `consumed` stores. E2 drains the queue on its own and sleeps only when it
  is empty (E1 wakes it only then). E1 waits only at a real join: another E2 entry (reports, sync
  points, wait for idle, barriers, compute, cache calls: E1 runs it itself once E2 drained), a
  constant buffer write S4 keeps on E1 (texture, shader or pool pages and its other causes), the
  draw's flush when E2 is idle or the flush is owed (S4's FlushPlace; E1 runs it, no round trip),
  a draw tail that cannot go async (E1 runs it), a seed of E2's view (first draw, channel switch;
  E1 seeds it), a lend or push to R, a memory decision, a condition from memory, E's idle work.
  On E2 the buffer cache's flag marks outside a fill (a deleted buffer's scrub through
  CacheInputs::Of) go to E2's view, never to E1's engine (CacheInputs' constructor). An async tail needs rule 3's
  pipeline, no texel buffers, ring B by value, no memory stage marks ahead, no inline index bytes
  (the GPU profile's price timers are skipped in an async fill, the handle program keep reads the
  view's bindings). E1's lookup reads shader code from game memory: allowed (counted) unless a
  write onto shader pages is pending, then E2 drains and the flush runs first (a lookup join).
  Rules: the buffer flags are E2's (between transfers E1's engine holds only the flags its work
  raised since the last one; an async draw record takes them and clears them, E2 ORs them into
  its view; wherever E2's work runs on the engine the view's flags are ORed into the view's own
  engine first and taken back after, the engine's then cleared); an
  async fill reads only E2's view; R's draw packet is packed by E1 at the record and pushed by E2
  after the fill (R's queue keeps one producer); a lend E2 makes holds E1 at its next safe point
  (each ring entry; every wait for E2 counts) and packs none of E1's registers; the repack
  layout's VertexInput raise goes to E1's engine at its next draw, join or push; `consumed` is
  stored by an async job at E2's place, and at a batch's end after the drain; GpuSplit::
  BackEntryPosition returns the job's position on E2. When the flush has work and E2 is behind,
  it runs on E2 after E1's lookup (counted: "flushes after E1's lookup"; S0's "lookups after a
  shader page write" says how often that order could matter). Waits sleep after a 20 us poll.
  Extra checks in the thread's lines (must be 0): E1Visible (a fence or query op from E1 outside
  a join), FrontAheadOfE2 (E1 stored `consumed` with E2 behind), ESplitHash (a hash of what the
  fill reads, from the engine on E1 and from the view on E2, 1 async draw in 64), E2Refused (an
  async fill refused after E1 moved on: the draw is dropped, R gets the spans as a delta). Also:
  async draws a second and how far ahead they were posted, E2 lends that held E1 (E2's wait for
  E1 to park, E1's held time), ring B room waits. Step in only after S1's checks read 0. Gate 2:
  DEPTH 0 against N, 2 cycles: max(E1, E2 busy) must drop >= 70% of the dry net.
- FRITCHII_ESPLIT_CB_ASYNC (live, default 1 = on in the queued mode, 0 = every write a join; with
  FRITCHII_ESPLIT=1 and FRITCHII_ESPLIT_DEPTH > 0,
  step S4): 1 = constant buffer words without a wait. E1 runs a 3D constant buffer entry (single
  words, a words block, a multi write) itself: the register half (CB_DATA through the shadow RAM,
  the dirty flags, a deferred draw the first word issues, the offset advance). The memory half
  (EXP107's compare, the bound buffer or CB_DIRECT write, the invalidation, the stream-write
  guard, the memory stage's stats; Maxwell3D::WriteConstantWords) goes into a record by value
  (where the words land: CB_ADDRESS, CB_SIZE, the offset, the texture and sampler pools; the
  words after the shadow RAM), several writes to a record, posted to E2 in ring order before any
  other hand-over, drain, join, push or wait of E1's, at a batch's end or when 4 KiB are full.
  A write goes to a join on E1 instead when it lands on texture or shader pages or E's pools (the
  stream-write guard must act at today's place), crosses a GPU page, is on Switch 2 sparse
  memory, is unmapped, does not fit a record, or the fast draw count or the price timers are on;
  also with the switch off (and in S1) for E1's own constant buffer writes outside a join (a back
  macro's). The words-block verify (FRITCHII_CB_WORDS_BLOCK=2) and EDEN_NX2_PERF_VERIFY's
  read-back run in a join. E2's view takes the offset from E1's next record. The ordering
  argument is in fritchii_e_split_thread.cpp ("S4"). A draw's flush is owed (before E1's lookup,
  E2 drained first when it is behind) when anything but a queued constant buffer write left
  cached writes since the last one (a lockstep hand-over, a join, E1's push or lend, an E2 lend
  that held E1: counted with MemoryManager::CachedWriteCount); a flush in an async tail marks E2's
  buffer flags (the view), not E1's engine. Line every 10 s, "E split thread 10 s: cb words
  (FRITCHII_ESPLIT_CB_ASYNC=N)": queued writes and records a second, KiB a second, writes in joins
  by cause; checks (must be 0): ESplitCbOrder (E2 applied a cb write out of E1's order),
  ESplitCbBytes (1 queued write in 64 read back after E2 wrote it: differs from the words),
  ESplitCbGuard (E2's stream-write guard found texture or shader pages E1 found clear; R is
  drained, the next draw's lend is not asked for), ESplitCbRead (E1 read game memory outside a
  join with cb writes queued; also counted in E1Touch), ESplitAsyncWrite (an async draw tail, bind
  or store left cached writes: the not-owed flush counts on none); then the draw flushes a second
  before the lookup, owed and in the tail. Step in after DEPTH N reads its checks 0: CB_ASYNC 0
  against 1 with DEPTH N, 2 cycles; E1's thread metric (E2 busy and E1 busy) must move by the cb
  words' share (S0's "constant buffer words" ms/s).
- FRITCHII_ESPLIT_FLUSH_ASYNC (live, default 0; with FRITCHII_ESPLIT_DEPTH > 0, step S4): 1 = a
  draw's flush that is not owed (above) goes to the head of the draw's tail on E2 even when E2 is
  idle, so E1 does not wait for it before the lookup. Exact for the same reason the tail flush
  with E2 behind is: what it invalidates is on no shader page, and the lookup reads shader code
  and the pipeline cache only. Counted in the cb words line ("draw flushes").
- FRITCHII_ESPLIT_ASYNC_KINDS (live, default 15; with FRITCHII_ESPLIT=1 in the queued mode): the
  E2 entries that go to E2's queue instead of a join (E2 drained, E1 runs it). Bit 0, memory stage
  cache calls (Call entries tagged StageTag::Cache: fence releases, GPU cache invalidations,
  reference signals, the puller's wait for idle and semaphore releases, the compute list words;
  E2 runs the call, its payload stays in ring A until `consumed` passes it at E2's place). Bit 1,
  raster ops: wait_for_idle, fragment_barrier, invalidate_texture_data_cache, tiled_cache_barrier,
  report_semaphore.query, clear_report_value, sync_info run on E1 (register write, dirty flags,
  the values the handler reads) and the one rasterizer call each ends in goes to E2 by value
  (also when a macro or a pairs block on E1 writes them). Bit 2, compute engine entries: every
  Method, Multi and Pairs entry of the compute engine, its sink included, on E2 with the engine by
  value (GpuSplit::ExecuteOnEngine); KeplerCompute and DispatchCompute read no Maxwell3D register
  (the OoT network bypass reads the tonemap target under its lock, recorded on R as today). 3 =
  compute entries back to joins if an image differs. Bit 3, posted ops: OneFollower::PushOp from
  E1 packs its spans (E1's registers at the op's place) and payload into a packet that E2 commits
  to R's queue in ring order, as an async draw's packet (no drain; carried ops still drain). A
  change drains E2 first. The thread's lines name the entries that still
  join, by kind ("entries that join ... top kinds": engine, ring kind, method, or a call's tag and
  function), top 8 with rates. E2Order covers the queue's order. Also: the S4 constant buffer
  path no longer joins on the GPU profile's price timers (they time the memory half on E2).
- FRITCHII_ESPLIT_BIND_BATCH (live, default 0; the queued mode, FRITCHII_ESPLIT_DEPTH > 0): constant
  buffer binds (Maxwell3D::ProcessCBBind's buffer cache half) without a job each. 1 = a bind is a
  32-byte segment in E1's open record (the S4 constant buffer words' record), applied on E2 in
  record order between the words around it; a record a bind opens takes the engine of its first
  cb write. A bind of the same value (enabled, address, size; or both disabled) as the slot's last
  bind earlier in the same open record is dropped: everything between the two on E2 is that
  record's own segments (other binds, cb writes, which never read the bindings), so the second
  bind meets the state the first left. 2 = verify: the same packing, nothing dropped; E2 hashes
  the slot's buffer cache state (binding, bind memo, dirty bit) around each would-drop bind
  (ESplitBindDrop, must be 0) and counts packed binds with cached writes (a count, not a check:
  R's concurrent writes show too, as in the async jobs' count). 3 = dry:
  binds stay one job each; the line counts what 1 would drop and how many records it would make.
  Must be 0 in 1 and 2: ESplitBindOrder (a packed bind applied out of E1's order). A change posts
  the open record; E2 never reads the mode, so no drain. Line "binds (FRITCHII_ESPLIT_BIND_BATCH=N)":
  binds/s, own jobs, packed, records holding binds (bind-only), dropped, same value in the run,
  same value as the slot's last bind at any distance (a ceiling, never dropped), dry runs (bind
  runs with binds a run, bind and cb record runs = the records 1 makes), E2's cost of a bind job,
  its body (1 in 64) and a packed bind (1 in 64), the checks. Line "E2 jobs by kind": E2's RunJob
  time per job kind (jobs/s, ms/s, us each). Step in: 3, then 2 (checks 0), then 0 against 1, two
  cycles; judge on E2 busy.
- FRITCHII_ESPLIT_IDLE (live, default 1; the queued mode): E1 never drains E2 because its ring
  ran out. A batch's end (the fence quiet check and the `consumed` store at that place) goes to E2
  as a record; with no new ring words E1 waits for E2 to go idle or for new words, whichever comes
  first, and E's idle work then finds E2 idle. 0 = the joins as before (a drain at every batch end
  and before the idle hook). Line "E1 waiting for E2 by cause (ms/s)": each join class, queue
  room, draw depth, time held for E2's lends, push joins by kind (lends, posted ops, pushes,
  drains, carried ops) with counts and ms/s, batch ends posted, idle waits and those ended by new
  ring words.
- FRITCHII_ESPLIT_OWED (live, default 1; the queued mode): a draw's flush is owed by shader page
  writes and E1's own work, not by any cached write during an async job (S4's ESplitAsyncWrite
  counted R's concurrent writes too; it is a count now). Writes whose destination E1 knows are
  tested against the shader page filter at the queue (report queries, puller semaphore releases,
  S4's constant buffer writes); compute uploads count as writes anywhere; any E thread's write on
  shader pages sets the pending flag: the next lookup drains E2 and flushes first (a lookup join).
  Must be 0: ESplitLateShader, 1 late flush in 16 tests the ranges it invalidates (after E1's
  lookup) against shader pages. 0 = S4's rule.
  In the queued mode a MemoryManager::FlushCaching E1 asks for outside a join (a posted clear's,
  AccelerateConditionalRendering's skip) goes to E2's queue at its place (the of27a crash: E1
  cleared the cached-write state E2 was appending to); must be 0: ESplitCachingOwner (that state
  touched by anything but E2, E1 in a join or R in a lent call). The one follower's samples
  mirror is set through E2's queue too (a draw on E1 set it before an earlier queued reset ran and
  stopped FRITCHII_ONEF_POST_FENCE for the session).
- FRITCHII_ESPLIT_PIN (boot, default 0; with FRITCHII_ESPLIT=1, step S3): where E1 and E2 run.
  0 = E1 keeps E's pinned core (PinnedRole::GpuBack), E2 on the performance cores; 1 = E2 on E's
  old core, E1 unpinned on the performance cores; 2 = E's core split by SMT sibling (E2 one
  hardware thread, E1 the other); 3 = E2 on E's core, E1 on R's core (PinnedRole::GpuMem, where
  FRITCHII_ONEF_PIN put R). A line at the start names the masks; every 10 s "placement
  FRITCHII_ESPLIT_PIN=N": E1's and E2's thread CPU time per second against their busy and wait
  times (a spinning wait reads as CPU time: E1 must stay under its busy share + 5%). Gate 2 on
  fps with rest < 2.3 CPUs.
- FRITCHII_ONEF_TRACE (boot, default 0; fable_b_one_follower 15.5): 1 = the lend trace. One
  72-byte record per lend and per posted op (packet number, site, op kind, posted, E's arrival,
  R's start and end, E's resume, E's work since its previous record, R's busy time on other
  packets since its previous record), written every second to <log>/onef_lend_trace.bin, the
  site names to onef_lend_trace_sites.txt. work/lend_replay.py replays it with any kinds marked
  posted. A 72 MiB ring; at about 100,000 records a second the file grows 7 MiB a second.
- One follower step 4, R's FlushRegion outside a lent call (an unlent draw's image upload reads
  game memory): the gather and the buffer cache's download are E's, so R skips them when no
  page of the range was ever GPU-written (nothing to download, exactly). A GPU-written page (or
  a texture download R cannot order after the gather) counts must-be-0 "R needs E's buffer
  flush" and stops step 4 for the session.
- With FRITCHII_ONEF=1 (or LEAN), every 10 s "One follower lends by site" lists the 16 lent
  call sites that parked E longest (the rasterizer function's name and its op, rate and parked
  ms/s; "direct" = Lend called outside the rasterizer's entry points: lent draws, verify
  compares, the memory seam's syncs): the pricing of fable_b 11.1 items 3 to 7.
- With FRITCHII_ONEF=1 (or LEAN), every 10 s "One follower transient lends" (fable_b 19 step 2's
  dry counter): the draws lent with reason "transient", per second, by the test that made them
  lent (view not seeded, channel, engine hint, native mode, bank readers, buffer paths, vertex
  limit, indirect reset, and E's fill refusing: texel buffers, no ring room, other), each split
  by draw kind. FRITCHII_ONEF_UNLENT_RESET, _UNLENT_VALUES and _BIND_SEED are checked against it.
- FRITCHII_ONEF_WAITS_E (live, default 0; fable_b_one_follower 11.2): 1 = the front's waits at
  the macro dma read/writes, qmd read, semaphore flush, list writers/sync/copy writers/flush and
  macro dma dispatch sites end when E ran the target and R played the newest report render op
  before it (reports are the one thing R writes to game memory without a lend); acquires, fence
  releases, the lagged check, fence (word) writers, report flushes (macro dma flush), maps and
  the rest still wait for R. The "Front waits for the back by site" line marks the E sites
  "(E)", and a second line counts the waits that ended before R got there.
- FRITCHII_ONEF_LEAN (boot, default 0): 1 = one follower on (no FRITCHII_ONEF=1 needed) at
  step 4 from boot with the verified configuration, for measuring and later shipping. Defaults
  it sets unless the environment sets them: FRITCHII_ONEF_UNLEND=3, FRITCHII_ONEF_E_PIPELINE=1,
  FRITCHII_ONEF_QUERY_OPS=1, FRITCHII_ONEF_WAITS_E=1, FRITCHII_ONEF_INVAL_E=1,
  FRITCHII_ONEF_VERIFY_EVERY=0, EDEN_NX2_EXP12=5. VIEW and BUILD are not needed (asked for by
  the lean mode); the view compare (PacketVerify), the build compares and the forced verify are
  off, the facts table is not written (E_PIPELINE). The must-be-0 checks that cost nothing stay
  on (owner asserts, stream-write guard, the pre-record hash check and its safety stop,
  descriptor counts, texture read ahead of R). One log line at boot names the configuration.
- FRITCHII_JIT_UNSAFE_EXTRA (boot, bits, default 0): Unsafe accuracy's float relaxations one at a
  time on top of the chosen accuracy: 1 = reduced-error FP (estimate instructions), 2 = inaccurate
  NaNs. Auto already unfuses FMA and ignores the global monitor. They change float results: keep
  only what shows no visual difference.
- FRITCHII_CPU_ACCURACY (boot env; auto, accurate, unsafe, paranoid, debugging): the CPU accuracy
  the JIT uses this session, for sizing runs (e.g. how much exact float handling costs). The
  stored cpu_accuracy setting is not changed, so nothing of it can be saved to a config.
- FRITCHII_ONEF_R_SPIN / FRITCHII_ONEF_E_SPIN (live, pauses, default 8192 = as before): how long
  R (idle) and E (parked or waiting) spin before they sleep on their wake word. Lower values
  hand the core back sooner (with PIN=2 R's hardware thread is a game core's sibling, fable_b
  23.3) at the cost of a wake on the next packet.
- FRITCHII_ONEF_PIN (boot, default 1; 3 = R on the front's idle SMT sibling, fable_b 24; 2 = one hardware thread of that core, its SMT sibling
  stays with the guest cores, fable_b 20.7): R gets PinnedRole::GpuMem's physical core (as
  FRITCHII_PIN_GPUMEM=1 gives GPUMem), taken from the guest set. 0 leaves R unpinned.
- FRITCHII_ONEF_VIEW (live, default 0): 1 = step 2, E packs the register delta, the R-owned dirty
  flags it raised, the draw state, the program hashes and the pools into each packet; R applies
  them to its register view and compares it with the engine at every lent draw ("view ..." checks,
  must be 0). No fps change expected.
- FRITCHII_ONEF_BUILD (live, default 0): 1 = step 3, R also builds each lent draw from the packet
  and its view (pipeline, image views, dynamic state, descriptors, render targets) and compares
  it with what the lent path used ("build ..." checks, must be 0).
- FRITCHII_ONEF_UNLEND (live, default 0): step 4, a mask of draw kinds that run on R from the
  packet alone while E goes on: 1 plain indexed, 2 plain non-indexed, 4 instance loops,
  8 indirect, 16 list, 32 mesh, 64 GPU-counted, 128 inline index. Start with 3. Every lent draw
  counts its reason (step below 4, kind not in mask, pipeline miss, verify, stream write,
  diagnostic, transient). Clears, dispatches, reports, fences and read-backs stay lent (step 5).
- FRITCHII_ONEF_VERIFY_EVERY (live, default 64): with UNLEND on, every Nth unlent draw is forced
  lent and runs step 3's compare: step 4's own must-be-0. 0 = never.
- FRITCHII_ONEF_AHEAD (live, default 1): packets E may push that R has not played: 1, 8, or 0
  for the ring's room. 1 is step 1 without the wait for the result (ordering bugs apart from
  run-ahead bugs).
- The stream-write guard (section 7 of the design): a game memory write from the stream onto a
  page the texture cache tracks while R has unplayed packets is counted ("stream write", must be
  0) and made lent.
- How it is built (parts a and b, one_follower.cpp and one_follower_view.cpp):
  - Lent: every RasterizerVulkan entry E calls that touches R's objects (draws, clears,
    dispatches, queries, fences and sync operations, flushes and read-backs, invalidations, DMA
    and inline uploads, surface copies, the display, channels, the frame end, the back's idle hook
    and fence-quiet check) parks E and runs on R. Calls from other threads (the front, guest
    cores) run where they ran, as today. R counts as the back for every OnBackThread check.
  - The delta: Maxwell3D's register writes on E (ProcessDirtyRegisters, ApplyDeltaWrites) note
    their word while VIEW is on; each packet carries the watched words noted since the last one
    (every word but the constant buffer data words and offset, falcon and the shadow scratch).
  - The flags: forwarded = every dirty flag but IndexBuffer and VertexBuffer0..31 (the buffer
    cache's alone); VertexBuffers is on both sides. A packet forwards what is raised now and was
    not after the last packet or lent call; no raise site is hooked. The program hashes are not
    sent (RefreshStages is the pipeline cache's, R's); R computes them from its view.
  - The guard drains R at the write itself (MemoryManager's block, word and constant buffer
    writes on E), which is that write run lent, so no draw is lent for it ("stream write" on the
    verify line counts the hits; "stream write not lent" must be 0).
  - Owner asserts: the scheduler (its existing owner check, on with FRITCHII_ONEF=1), the
    pipeline lookup, the texture cache's render targets and the query cache's reports count
    "owner on E" when E uses them with R not parked or drained; the buffer cache's graphics
    update counts "owner on R" outside a lent call. Both must be 0; below step 4 they cannot fire
    (R is never behind outside a lend), so they gate step 4.
  - Two positions (design 8.2): GpuSplit's `consumed` stays E's (what the back ran) and a new
    `played` is R's (E's position clamped to R's oldest unplayed packet, raised by E and by R as
    it plays). Every front wait and ring-space check reads `played`, "R has played P", today's
    meaning; step 5 moves single wait kinds to E's position (EngineConsumedNow). Without
    FRITCHII_ONEF the front reads `consumed` as before.
  - Busy per frame for the step 4 gate (8.2): E = the back's busy less "E parked in lends"
    (both ms/s, divided by fps); R = "R busy" on the "One follower threads" line (GPU profile).
- Must-be-0 line, every 10 s: "One follower checks (must be 0; this window / session): view words
  a / b, ...". Ownership: "owner on E" (an R-owned object used on E while R ran) and "owner on R".
- Steps 3 and 4 (part c, vk_one_follower_draw.cpp): from step 3 E's draws go through
  OneFollowerDraw::RouteDraw (lent with the build compare; at step 4 the playable kinds, plain
  indexed, plain arrays and instance loops, are filled into ring B on E and pushed). R keeps a view
  engine (a Maxwell3D no method reaches) fed by the same spans, and plays a pushed draw with the
  R-owned caches pointed at it. Steps 1 and 2 lend every draw from RasterizerVulkan itself.
  Indirect and list draws stay lent at every step. Ring B with one follower: E is the fill side
  (BeginDraw, ClaimHeld and the packet ticks treat it as GPUMem at THREAD=2).
- The facts table (E's fills look up a pipeline's buffer layout by its program registers) is
  keyed by code addresses: a game that rewrites shader code at the same addresses left entries
  for the old code (step 4's first unlent draw got 7 descriptors for a 5-descriptor pipeline).
  Every shader cache drop of a cached program bumps a program code epoch; an entry published
  before the latest bump is a miss (the draw is lent and republishes it). "facts stale" (must be
  0) compares, at every verified lent draw from step 3, the entry E would use with the programs
  the draw ran with.

### FRITCHII_MEMSTAGE_AHEAD

FRITCHII_MEMSTAGE_AHEAD (live, read at a frame end, default 0; the memory stage's step 7, "the
window"; work\fable_memstage_step6_spec.md 1.4 and 6.4): with FRITCHII_MEMSTAGE_THREAD=2, how many
draw packets GPUMem may fill ahead of the back's take. 0 is 6b's level mode, unchanged. Any value
of 65,536 or more is unlimited (the test harness sends 2147483647); the frame end is a level run,
so the lead never passes one frame. Ignored (0) while THREAD is not 2; "Memory stage window
(FRITCHII_MEMSTAGE_AHEAD=n)" is logged when it changes. It runs 0 while EDEN_NX2_RUNAHEAD is on
(the back applies due CPU-write chunks before each entry; logged once), while the delta path
(EXP118, or EDEN_NX2_FASTDRAW_COUNT) was on at boot (a delta draw holds the buffer cache's lock
through Configure, where a read flush is a served call; logged once), and for the rest of the
session after a catch-up could not finish (below; logged).

- GPUMem waits only while that many packets are in ring B that the back has not taken, while
  ring B has no room for another (both after publishing up to the entry), and at level runs (it
  lends and waits for the back to get through the run, as in level mode).
- It comes level inside the window (waits for the back to reach the entry) where running ahead
  would change an answer the back gets from the cache in a served call: an entry that takes a full
  copy of the back's engine (a new or stale copy), a draw whose fill marks buffers GPU-written
  (written storage buffers, transform feedback), and collected constant buffer ranges over
  GPU-written buffer data at the next fill. Counted by reason. Constant buffer words onto
  GPU-written buffer data are tested at the write, before the bytes land: the entry turns hard
  (the back writes them in a level run, in stream order), counted as "entries made hard by
  constant buffer words onto GPU-written data".
- GPUMem never waits on a staging or stream region stamped with a packet not played yet: the
  staging pool never waits for a region (a busy stream takes a new buffer, counted as "new
  staging buffers for GPUMem, the stream busy"). The one wait a draw's fill can reach is the
  multi-range storage bind's (a sync, with its catch-up).
- So what the back reads in a served draw is its position's: texture handles (EXP123=3), uniforms
  and storage addresses come in the packet; with the OoT trackers on, the bytes they read (the
  camera block, the actor's origin and bones) are copied at the fill and come in the packet too.
- GPUMem's loop with the window open (the step 7 fps fixes): the window and the room are tested
  before a draw's fill, not before every entry; GPUMem's busy time is timed per publish batch; a
  run publishes its end once, after GPUMem followed it, and stays lent when it reaches the end of
  what the front published (it waits for more input and ends at the first entry that needs
  GPUMem; "runs kept lent at the end of the input" on the window line); a served draw sends the
  vertex input state only when it may have changed (the back's VertexBuffers flag, not the
  cache's in served mode, is cleared at a packet's draw as the cache's vertex buffer update
  clears it with one thread, and with each send). With EDEN_NX2_GPU_PROFILE=2 GPUMem's
  profile splits its entry loop into "stage entries" and, inside it, "stage cb words", "stage
  pairs" (Pairs and StateDelta blocks' writes), "stage methods" (Method and Multi entries'
  handlers, their words and draws excluded), "stage packets" (Writes-tagged draw packets and
  DrawBatch entries) and "stage draw setup" (a draw the copy issued, served or made level, its
  fill excluded); what stays in "stage entries" is the per-entry bookkeeping (classify, the
  copy's lookup, runs, counters) and the closures, and GPUMem's "other" is the loop around the
  entries (the input's and the back's positions, served calls, publishes); the back's
  "draw" splits off "ring take" (the packet's take from ring B) with "ring take copy" inside it
  (its bytes copied out), "draw locks" and "ring facts" (inside "draw setup": the caches' locks,
  and the facts published under the stage's program key), "draw after configure" (the
  trackers, traces and dumps between Configure and dynamic state) and "rest tail" (PlayRest after
  its play: the held flags, the ticks mark, the watch count).
- work\fable_stage_levelpoints.md 15.7's zones and counters (EDEN_NX2_GPU_PROFILE=2 unless said):
  - Inside "stage draw setup" (StageFillDraw): "setup window" (StageBeforeFill), "setup program"
    (the program id and the facts lookup, or FRITCHII_MEMSTAGE_SETUP_KEEP's keep), "setup level
    tests" (window open: a writing draw, the ranges over GPU-written data; StageLevel's waits are
    "stage wait"), "setup lock" (the buffer cache's lock taken), "setup inputs" (the copy's cache
    inputs, the repack layout, the draw's number) and "setup handle values" (SEAM 3's raw
    handles, read before the fill). What stays in "stage draw setup" is StageDraw's own tests.
  - Inside "memory fill" (FillDrawHere, on GPUMem and on the back): "fill ranges" (the stage's
    collected constant buffer ranges applied), "fill bind setup" (the uniform state, the storage
    binds, the texture buffer unbinds), "fill geometry" (vertex and index buffers), "fill stage
    buffers" (BindHostStageBuffers; its "uniform buffers" and "stage storage buffers" zones stay
    their own) and "fill packet" (the trackers' record and the push). "memory fill" keeps the
    brackets (the arena, the packet number, the counters).
  - GPUMem's fine counters "stage writes: <group>": the 3D register writes its copy follows, by
    register group (program, vertex streams, vertex attributes, index buffer, draw parameters,
    constant buffer binds, constant buffer data, textures and samplers, render targets,
    viewports and scissors, blend, depth and stencil, raster, transform feedback, shadow scratch,
    other), and "stage writes: of them executable". For 15.3 row 4's census: the groups the fill
    never reads are what following only the read set would skip. Counted where they arrive
    (StateWrite, PairsFast, CallMethod's executable writes, CallMultiMethod's constant buffer
    runs); inline index runs of CallMultiMethod are not counted.
  - "GPU memory thread waits for the front by what the front did before its publish, per second"
    (profile 1 or 2): GPUMem's waits for the front's next publish (the "the front" wait), by
    what the front did meanwhile: "waited for the back at <site>" (it finished a drain or a wait
    for writers at that site: its own wait held GPUMem's), "idle (no command list)", "pending
    sleep" (a suspended channel's poll gap), "working, published as it began a wait for the back"
    (no wait finished; the publish that ended GPUMem's wait was a drain's), "working". A wait in
    which the front finished several waits counts under the last one ("spanned several front
    waits"). Ring-full waits (WaitForSpace) are not marked.
  - The front's "push" (15.5 row 5): "front methods" (a GP entry's methods, ProcessCommands, out of
    "push" and out of "resume pending" for a resumed channel's; macros stay "macro"), "publish"
    (GpuSplit::Publish when it publishes; its calls are the publishes a second) and "publish wake"
    (the publish woke the sleeping back or GPUMem: a notify). What stays in "push" is the entry's
    fetch and checks (list writers' waits are "writer wait").
- The second fps fixes (window open only): GPUMem's wait for the front spins about 200 us before
  it sleeps (level mode keeps about 2,000 pauses; the link line's "slept n/s" adds how many of
  those sleeps waited for the front); the back prefetches the first 1 KiB of the next packet in
  ring B when it takes one; the back's loop calls the ring's per-entry work (BackEntry, the run's
  edges, BackAfter) only at a batch's first entry and where a run starts or ends. A take wakes
  GPUMem only when it sleeps on the window or on room in ring B (not on the back's position, a
  run or the front, which a take does not end); ring B's control fields and the link's counters
  sit on cache lines per thread.
- Other changes it makes: packet ticks keep one unplayed mark per thread (the close takes the
  lowest, and ring B's oldest packet; the ring's start and end clear GPUMem's); the lend's flag
  hand-over happens at the run's first entry; a sync in GPUMem's fill, and a join in it over
  GPU-written data, first let the back play up to the published position (a catch-up); a back
  waiting for the buffer cache's lock GPUMem holds serves GPUMem's requests from that wait (each
  wait counted, the first 16 logged with the caller's frames: read each site once and declare it
  safe for a sync there, or make it a served call); a GPUMem waiting for the lock the back holds
  (EXP118's delta draws) serves the back's call from its clean lock sites; GPUMem answers the
  back's served calls between entries too; the facts table has a cache on each side; a waiter's
  idle hook request runs even while GPUMem is busy.
- "Memory stage window" every 10 s, only while the window is open: packets in ring B at each push
  (average, maximum, and per second by 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64-255, 256+ ahead), the
  back's waits for the stage (times and ms/s: should drop toward 0; spec 8.4's target is under
  20 ms/s, and 6b's run rate makes about 58 ms/s the floor until runs drop), GPUMem's waits for the
  window, for room in ring B (and how often the back's takes woke it from them) and to come
  level (ms/s, comings level a second by reason: full copy,
  writing draw, ranges over GPU-written data), entries made hard at a constant buffer write, new
  staging buffers for GPUMem with the stream busy, served calls
  made while packets were ahead, sync and join catch-ups, lends handed over at the run's start, the
  back's waits for the lock GPUMem held and GPUMem's served calls while waiting for the back's,
  served downloads past the join ring's reach (4,096 joins; not a failure), idle hook runs with
  GPUMem busy, and GPUMem's idle work (EXP104's doomed buffers; it runs only when the back is
  parked as GPUMem runs out of input).
- Must be 0 (on the "Memory stage link" line, the first 16 logged): "a sync of GPUMem's fill sent
  to the back before it played the packets ahead of it" (a catch-up that could not finish: the back
  waited in a served call or for the lock; the window is then refused for the session); "a served
  download over GPU-written data in a buffer a packet ahead of the back joined"; "an image
  registered over a constant buffer write GPUMem made for a later entry" (spec 9.1); "a served
  download over a constant buffer write GPUMem made for a later entry" (on GPU-written bytes). The
  last two need GPUMem's own-write ring, which runs only with the verify on (READ_LOG or
  RING_VERIFY), so the fps gate measures play without it. Also "facts key collisions" (any ring
  mode): the facts table's key is a hash of the program registers (the six pipeline slots and the
  program region), and every entry keeps those registers and compares them on a key match, so
  two programs under one key are told apart (a miss, the draw is level), never mixed up.
- EXP104 (the buffer pool, spec 7.5): with the window open GPUMem creates and destroys buffers while
  the back records, so the gate's area load should run with EXP104 on and off (or watch its hitch
  lines). GPUMem has no role in thread_pinning.h: the gate runs it unpinned, and the pinning choice
  of spec 5.8 stays open.
- Verify (spec 6.4) with FRITCHII_MEMSTAGE_READ_LOG=N: every Nth ring packet's fill logs its reads
  of game memory on GPUMem, and the back reads them again at the take and compares, leaving out
  the bytes GPUMem itself wrote after the fill (its constant buffer words for later entries, from
  its own ring; while the window is open they are kept out of the emulator's write ring, which they
  would wrap). The rest is classed as every read log difference; the read log gate line is the
  gate (unexplained 0, ring wrapped 0: GPUMem's own ring and the mark ring count too). Bound ranges
  have no tracker on the back: they differ when an emulator write noted since the fill, a mark the
  cache took since (a served gather or invalidation, a game core's trapped store: a small ring
  while the compare runs) or a pending game-side mark covers them; a difference over a mark the
  cache took is "game write, marked". A value is left out only when every changed byte is
  GPUMem's. The read log line adds "ring packets compared at the take", "left out as GPUMem's own
  later writes", ring packets "with a CPU-written mark since the fill", "async GPU completions
  over-read" (a query result GPUFencingThread wrote when the GPU finished, on uploaded bytes the
  packet does not bind: reported, not a failure; inside a range the packet binds it fails as an
  emulator write), "GPUMem's writes kept out
  of the write ring" and "CPU-written marks noted" (marks taken anywhere: served and inline gathers
  and invalidations, InnerInvalidation, game cores' trapped stores). Keep
  FRITCHII_MEMSTAGE_RING_VERIFY (compare b's hash) on with it.
- FRITCHII_MEMSTAGE_VERIFY & 1 (5.4, handle values) with the window open: the back's own read is
  at play, after GPUMem wrote constant buffer words for later entries, so a handle value whose
  words GPUMem wrote since the packet's fill (from its own-write ring; for a handle program, any
  enabled constant buffer of the stage) is left out and counted apart on the by-value line
  ("handle values left out as GPUMem's own later writes"); any other difference is a mismatch.
  The QMD and null pointer slot checks read twice at once inside level runs and are unchanged.
- Order of the verify runs: 1, 8, 64, unlimited (work\run_step7_verify.sh), then the fps gate
  against THREAD 0 (work\run_step7_gate.sh).

### FRITCHII_MEMSTAGE_LIST_DRAWS

FRITCHII_MEMSTAGE_LIST_DRAWS (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 1;
fix 6 of work\fable_memstage_step7_fps.md, work\fable_memstage_6b_plan.md 7.2): with
FRITCHII_MEMSTAGE_THREAD=2 and the window open (AHEAD > 0), GPUMem serves list draws (OoT's
culled object draws, nx2_list_draw.h) instead of making each one a level run. 0, AHEAD 0 and
THREAD 0/1 keep them as before (level draws on THREAD 2, the back's own fill otherwise).

- Which: a list draw GPUMem's copy issues outside a hard entry, when the back would draw it
  indirectly from the two words. The ones the back reads back on the CPU (EDEN_NX2_LIST_DRAW other
  than 1, quads, a base instance, transform feedback, EDEN_NX2_MESH_FIRST; one test,
  ListDrawReadsBack, on the copy and on the back) stay level, and so do a refused fill (facts
  miss, texel buffers, no room in ring B) and a frame dump or trace.
- How: GPUMem fills it as any served draw, non-indexed, with the copy's indirect parameters in the
  cache's inputs, so the fill's geometry section binds and synchronizes the two words exactly as
  the back's own fill does after SetDrawIndirect; the packet then carries their buffer and offset
  (what the back's Configure asked the cache for, GetDrawIndirectBuffer). The back takes the
  packet in PrepareDraw and records the copy into the draw command from that buffer: it does not
  enter the cache. The copy is recorded at the draw's place in the stream, so the GPU reads the
  words the dispatches before it wrote; the buffer outlives the packet's play (packet ticks), as
  every vertex buffer a packet binds does.
- "Memory stage list draws (FRITCHII_MEMSTAGE_LIST_DRAWS=n, window open/closed)" every 10 s on
  THREAD 2: list draws a second served, and level by why (the window closed or the switch off,
  read back on the CPU, the fill refused it, a frame dump or trace); runs started by a list draw
  against all runs (each level one starts a run); the back's draws from the packet's buffer
  (equals served). List draws in an entry that is hard already are the ring line's "late" level
  draws. The ring line's level draw kind 0 is "indirect" only now, and 5c's "runs by first hard
  kind" names the level draw kinds ("level draw (list read back)", ...).
- Must be 0 (the link line): "a served list draw the back would read back" (the two sides decided
  differently: their state differs), "a served list draw whose packet has no list words buffer"
  (the back asked the cache) and "a list draw packet at a back draw that asked for no list
  arguments" (the other direction). A served list draw whose packet is missing (PacketMissing)
  is filled by the back as before, with the cache's indirect parameters set for it.
- At the market: about 1,600 runs a second start at a list draw (step 7 run, s7g); with the
  switch on they should go, and with them their lends, returns and the back's wait for GPUMem in
  each (step 7 fps list: about -30 ms/s for GPUMem parked, -23 ms/s of the back's wait).
  Compare 0 and 1 with profile_toggle.py in one session.
- Not the second starter at the same rate, "cache call +0xc744a0" (s7g; +0xc8c700 in the
  2026-11-14 build's stage-again run, symbolized against that build's PDB): it is the puller's
  semaphore release (WriteLong, Puller::ProcessSemaphoreTriggerMethod: the query cache's
  payload report, a fence when RELEASE_SIZE is set on Switch 2), not a list-words closure. The
  kept list words closure (KeplerCompute's list dispatch) runs about 850 a second and sits in
  front of its dispatch, inside the dispatch's run. Left as they are here.

### FRITCHII_MEMSTAGE_FAST_LOOP

FRITCHII_MEMSTAGE_FAST_LOOP (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 1;
lever A of work\fable_stage_drains.md): with FRITCHII_MEMSTAGE_THREAD=2 and the window open
(AHEAD > 0), GPUMem's entry loop takes two shortcuts that leave its copy in exactly the state the
loop leaves it in without them. 0, AHEAD 0 and THREAD 0/1 run the loop as before (the shadow on
the back never takes them). "Memory stage fast entry loop (FRITCHII_MEMSTAGE_FAST_LOOP=n)" is
logged when it changes.

- Pairs and StateDelta blocks (3.7 million writes a second in 1.45 million blocks at the market)
  are applied in one loop (memstage_shadow.cpp, PairsFast) instead of StateWrite per write. While
  a deferred draw waits the writes still go one by one through StateWrite (the first stored write
  that is not an instance loop method draws it, reading the copy as it is then); after it no
  write in the block can issue a draw, so the loop reads the shadow RAM's mode once (again after a
  tracked write to shadow_ram_control's own word, the one write that changes it), keeps the
  same-value skip, gathers each stored register's two dirty flags and raises them on the copy
  (and in the flags raised since its last draw) at the block's end, and advances the copy's
  generation by the number of stores. Registers, shadow RAM, every dirty flag, the generation and
  the draws end the same; nothing reads the copy inside a block.
- The input's end (the front's `published`, written at every publish) is read again only where
  GPUMem reaches the last value it read, not at every entry. Entries before it stay published and
  that read's acquire made them visible, so GPUMem reads the same entries in the same order and
  stops for input at the same positions.
- The verify: the memstage gate (work\memstage_gate.py) with FRITCHII_MEMSTAGE_RING_VERIFY (compare
  b's hash at every served draw) and READ_LOG, once with 0 and once with 1; every must-be-0 line
  stays 0. Then 0 against 1 with profile_toggle.py at the market (GPUMem's "stage pairs" and "other"
  at EDEN_NX2_GPU_PROFILE=2, its busy time, fps).

### FRITCHII_MEMSTAGE_ENTRY_FAST

FRITCHII_MEMSTAGE_ENTRY_FAST (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0;
work\fable_stage_levelpoints.md 15.3 row 1): with FRITCHII_MEMSTAGE_THREAD=2 and the window open
(AHEAD > 0), GPUMem takes 3D Method entries and CbWords blocks through a short path
(memstage_shadow.cpp, EntryFast) instead of Process's switch. 0, AHEAD 0 and THREAD 0/1 run
every entry through Process as before. "Memory stage entry path (FRITCHII_MEMSTAGE_ENTRY_FAST=n)"
is logged when it changes.

- Each copy keeps one byte a 3D method (ClassOfMethod, from the engine's execution mask, fixed at
  its construction): store (not executable), work (executable, MethodKindOf's default), a constant
  buffer data word, or slow (every method MethodKindOf names: its kind can depend on the state).
- Store: StateWrite, as Process's sink path (processing_sunk set) or CallMethod (Direct); class
  pass. Work and constant buffer words (EXP107 off): ShadowRam, DirtyStore and MethodCall, as
  CallMethod; class work. A CbWords block: the same per word (CallMethod for a word of another
  class, as Process). The copy is the engine's copy, as Use returns it; a stale copy, a Sink
  entry, a macro method, a slow method and everything else go through Process. The entry's end
  (class, runs, counters, stale copies) is one function both paths call (EndEntry).
- Left out per entry: the per-entry lambdas and launch count setup (other engines only),
  MethodKindOf up to three times, Use, and CallMethod's execution mask and kind tests.
- The "Memory stage GPUMem entry path" line (every 10 s, GPUMem) gives the hit rate: fast entries a
  second, their share of the 3D Method and CbWords entries and of all entries. It counts with the
  switch off too (0 fast entries then).
- The verify: the memstage gate with FRITCHII_MEMSTAGE_RING_VERIFY (compare b's hash at every
  served draw) and READ_LOG with the switch on; every must-be-0 line stays 0. Then 0 against 1 with
  profile_toggle.py, stage on, at the market: GPUMem's busy time and fps (the fine zones of
  EDEN_NX2_GPU_PROFILE=2 time each entry and hide what is cut).

### FRITCHII_MEMSTAGE_CB_RUNS

FRITCHII_MEMSTAGE_CB_RUNS (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0;
work\fable_stage_levelpoints.md 15.3 row 2): with FRITCHII_MEMSTAGE_THREAD=2 and the window open
(AHEAD > 0), GPUMem handles constant buffer words that follow each other as one run
(memstage_shadow.cpp, CbWords) instead of a page test, a backend call, a game memory write and a
collected range per call (1.2 million calls a second at the market, 10.6 a draw). 0, AHEAD 0 and
THREAD 0/1 write each call's words as before. "Memory stage constant buffer word runs
(FRITCHII_MEMSTAGE_CB_RUNS=n)" is logged when it changes.

- A run: calls of served entries, each within one 4 KiB GPU page and off Switch 2 sparse memory,
  whose bytes start where the run's end, in the run's page, on the same copy, under the same page
  lookup (CbTarget). Other words (several pages, sparse memory, EXP107, inside a run or a hard
  entry) take today's path after the run ends.
- It goes on over the entries that read no game memory: other constant buffer word entries (their
  calls decide), plain 3D stores (not executable Method entries, Pairs and StateDelta blocks) and
  wraps. It ends, its bytes written first, at any other entry, at every draw of the copies (before
  the fill, or before the draw makes its entry hard), when the entry turns hard (before its run is
  lent), and whenever GPUMem publishes past the entries it followed (PublishStaged) and at the
  ring's end. So the back never runs past a word whose bytes are not in game memory (the front's
  waits for writers wait for the back's position), and no fill, run or served release reads them
  before they are there.
- Tests, as exact as each call's: the watch test (Watched) stays per call. The GPU-written test
  is a page test that can only turn from clean to written through a fill or a run, and both end
  the run: the run's first call tests its whole page (the backend's ranges_gpu_written on the
  page), and a clean page answers for each of its calls. On a page that may hold GPU-written data
  every call tests its own 32-byte range, as today. A hit makes the entry hard after the run's
  earlier bytes are written; the back writes the entry's words itself, as today.
- The write: one WriteBlockStageAt through the run's page lookup (each call's write is that lookup
  at its offset), and one collected range: the union of the calls' 32-byte ranges, which for bytes
  that follow each other in one page is the run's own 32-byte range. With the window's verify on
  (StageWritesKept) each call still gets its own-write note with its own entry, made just before
  the write.
- The "Memory stage GPUMem CB word runs" line (every 10 s, GPUMem, counted with the switch off too,
  from the same run bookkeeping): calls a second by words (1, 2, 3-4, 5-8, 9-16, 17+), the calls
  that extend the run before them, runs by words, what ended them (an entry, a publish, a draw, a
  hard entry, a word apart, a word off the path), and runs on a page that may be GPU-written.
  With the switch off it sizes the cut: calls minus runs is the writes, tests and ranges saved.
- The verify: the memstage gate with FRITCHII_MEMSTAGE_RING_VERIFY and READ_LOG with the switch
  on (the read compare and the image check see each call's note); every must-be-0 line stays 0.
  Then 0 against 1 with profile_toggle.py, stage on, at the market: GPUMem's busy time and fps.

### FRITCHII_MEMSTAGE_GATHER

FRITCHII_MEMSTAGE_GATHER (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0;
lever B of work\fable_stage_drains.md): with FRITCHII_MEMSTAGE_THREAD=2 and the window open
(AHEAD > 0), a gather (3D invalidate_texture_data_cache, 0x3dd: the game's collected CPU writes
into the caches, then a wait for idle) no longer starts a level run. GPUMem comes level at its
entry (StageLevel, reason "gather") and the back, parked there, runs the method's whole handler as
a request GPUMem waits for; the back's own run of the entry then skips it. 0, AHEAD 0 and THREAD
0/1 keep every gather as before (a run on THREAD 2, the back's own code otherwise).

- Why not GPUMem's own work (the plan's first form: GPUMem takes the game's writes, applies the
  buffer half, sends the ranges to the back for the other caches): the method's wait for idle
  syncs query results into buffers (QueryCache::NotifyWFI, SyncValues: buffer lookups and uploads
  in the buffer cache) and groups the async flushes (SignalOrdering), both in the buffer cache at
  the gather's place in the stream, with the back's query state there. At the market that sync is
  due at nearly every gather (about 68,000 reports a second, each leaves a pending sync until the
  next wait for idle). GPUMem ahead of the back can't give the back that cache state, and a served
  call would answer from GPUMem's later state (joins and uploads ahead). The gather line counts how
  often the sync was due ("the wait for idle synced query results") and how often a game write
  range lay by GPU-written buffer data, so the cost of that first form can be judged.
- Which: a gather that is the whole entry (a lone Direct method entry) of the copy, outside a run
  and outside a hard entry, with no draw before it in the entry. Level instead (a run, as before):
  a gather inside a multi-word or packet entry (the back would run what precedes it in the entry
  after the request), one the back's pending deferred draw would come before (its sink or the
  gather's own store flushes it at the entry: tested by the request, on the back), one right
  before a hard closure the front has published (a Cache or untagged Call, a semaphore release or
  a fence: the gather's run takes it in, so coming level first would only add a wait), no room
  for its record (256), GPUMem stopping.
- Exact: once level, the back has run every entry before the gather's and played every packet;
  GPUMem waits outside any fill, so the back serving the request is the buffer cache's only user,
  at that place in the stream, and it runs InvalidateTextureDataCache's default, the same code
  THREAD 0 runs there. The cache's flags 15-48 go to the back's engines before the gather and
  back to the copies after it, inside the request on the back (GPUMem waits; the back's idle hook
  flush, which may run while it is parked, is on the same thread), as a run of that one entry
  hands them over (the request may mark bindings dirty). The record
  (the entry's position and the back's draw count) makes the back skip the method when it runs
  the entry: nothing of the entry comes before it there (a lone method, no deferred draw), and its
  register store and dirty flags are the same either way.
- What it saves against the run: the lend and the return (flag hand-overs, the run's start and end
  on the back, FlushCaching), GPUMem parked through the back's run of the entry and of the entries
  the run was extended over (GPUMem follows those itself now). What it keeps: GPUMem waits until the back reaches the gather (its lead is gone there, as
  at a run), then for the request.
- "Memory stage gathers (FRITCHII_MEMSTAGE_GATHER=n, window open/closed)" every 10 s on THREAD 2:
  gathers a second run at their entry and level by why (switch off or window closed, not alone in
  their entry, after a deferred draw, before a hard closure, no ring backend, no record room,
  GPUMem stopping), inside a level run, in an entry hard already; runs started by a gather against all runs; GPUMem's ms a second in the
  served ones (the wait to be level and the request, and us each); of those, how often the wait
  for idle synced query results and a game write range lay by GPU-written buffer data, and the
  game write ranges a second; the gathers the back found run at their entry (equals served; the
  ones inside a level run should be 0, a lone gather's entry has nothing after it to turn hard). The window line's "level
  inside the window" has a "gather" reason, and the link line's "served syncs" include the
  back's runs of the served gathers. 5c's "runs by first hard kind" shows the gathers that
  are left.
- Must be 0 (the link line): "a served gather with no record of GPUMem's" (the back ran it in a
  served entry: the two sides decided differently; it then runs it as before, which is not in
  order), "a gather GPUMem had the back run at an entry the back passed without it" (a record
  dropped at a later gather, a lend, the ring's end or start) and "a back draw between a gather
  GPUMem had run and its entry" (the back's draw count moved between the request and its entry).
  The first market run (build 44a2974be6) counted about one "served gather with no record" every
  10 s with the switch off: not the gathers but the window itself. The back read lent_from and
  lent_until with two loads, and a lend between them gave it the last run's start with the next
  run's end: it took the entries between the two runs as lent, stayed "in a run" and at the next
  run's start skipped that start (no flag hand-over, no lend ranges, its first entry run with the
  back served; a gather is the usual first entry). BackLoad now reads one run's pair (lent_until
  stored with release after lent_from, lent_from read again), and a run start that finds the back
  still in another run is a must-be-0 of its own ("a run start while the back was still in the
  last run"), whose end is then taken there. "Level after a deferred draw" is not one of them: the shadow applies sunk writes at once and
  may issue that draw at an earlier entry, while the back issues it when it consumes its sink at
  the gather's entry.
- The record is pushed before the request and GPUMem publishes past the entry right after it (as
  an entry with a packet): the back, parked there, goes on at once. If the request throws, the
  ring ends at the entry and the record stays: the back's run of the entry without the ring
  still skips the gather (a record at or past the ring's end is honoured), so it never runs
  twice; the cache's flags come back to the copies in a scope guard either way. "The window open"
  is GPUMem's own read of it at its loop top (what StageLevel goes by), not the live value.
- The constant buffer ranges GPUMem collected and has not applied yet stay pending through the
  request, as on THREAD 0, where the memory manager's cached ranges wait for FlushCaching at the
  next draw (nothing in the gather, the wait for idle, the query sync or a fence calls it); a lent
  run applies them at its start, earlier.
- At the market (ms46, 2026-11-14): 4,579 runs a second start at a gather (43% of runs, 15 us
  parked each). With the switch on they should go from the run starters; the gain is the run
  overhead per gather, not the whole 15 us (GPUMem still waits to be level). Compare 0 and 1 with
  profile_toggle.py in one session; the gather line has GPUMem's ms a second in the served ones.
- "cache call +0xc8d340" (ms46, the second starter at 1,705 a second; symbolized against the
  2026-11-14 03:09 release PDB): the closure at puller.cpp:1751, the puller's semaphore release
  (WriteLong, Puller::ProcessSemaphoreTriggerMethod: QueryCache::CounterReport with the payload, a
  fence when RELEASE_SIZE is set on Switch 2, so SignalFence and the fence manager), the same
  closure as the earlier "+0xc744a0" and "+0xc8c700". Tagged Cache already, which is right: it
  reaches the fence manager (its releases commit the buffer cache's async flushes) and the query
  cache, and leaves a pending query sync. Not Pass (it writes the semaphore word into game memory
  at the back's place, which uploads GPUMem made ahead could miss, and a fence commits cache
  state). It could be run at its entry the way gathers are here (GPUMem level, the back runs the
  closure as a request), with a skip in the back's entry loop for Call entries:
  FRITCHII_MEMSTAGE_RELEASE below.

### FRITCHII_MEMSTAGE_RELEASE

FRITCHII_MEMSTAGE_RELEASE (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0): with
FRITCHII_MEMSTAGE_THREAD=2 and the window open (AHEAD > 0), the puller's semaphore release closure
(Puller::ProcessSemaphoreTriggerMethod's WriteLong: QueryCache::CounterReport with the payload, a
fence when RELEASE_SIZE is set on Switch 2; "cache call +0xc8d340" in ms46, the second run starter
at 1,705 a second) no longer starts a level run, the same way FRITCHII_MEMSTAGE_GATHER handles
gathers: GPUMem comes level at its entry (StageLevel, reason "semaphore release"), the back,
parked there, runs the closure as a request GPUMem waits for, and GpuSplit::Execute skips the
entry when it gets there. 0, AHEAD 0 and THREAD 0/1: as before.

- Which: the closure carries StageCacheCall::SemaphoreRelease in its header (bits 40-47 of a Cache
  tagged Call entry; Execute and every other reader ignore them). Outside a level run, with the
  switch on and the window open. Kept as a run: a Cache or untagged closure right after it that
  the front has published (the run takes it in, so coming level first would only add a wait and
  a second run), no room for its record (256), GPUMem stopping.
- Exact: once level, the back has run every entry before the release's and played every packet,
  and GPUMem waits outside any fill: the back serving the request is the buffer cache's, the query
  cache's and the fence manager's only user, at the release's place in the stream, and it makes
  the call Execute makes (the entry's thunk on its payload, which stays in ring A until the back
  passes the entry): the report, the fence and what it commits, the semaphore word written to
  game memory, all where THREAD 0 does them. On the back a Call entry is only its call (no method
  sink is consumed, no deferred draw is issued there), so nothing of the entry comes before it,
  and there is no lone-method or deferred-draw exception as for the gathers. The cache's flags
  15-48 go to the back's engines around the call and back to the copies (the fence's commits and
  the query sync may delete buffers and mark bindings dirty), inside the request, as a run of
  that entry hands them over. The front's bookkeeping of the release (NoteRelease, its waiters)
  reads positions and the word: the word is written while the back is at the entry, as before;
  `consumed` passes the entry later than the write, as before.
- "Memory stage semaphore releases (FRITCHII_MEMSTAGE_RELEASE=n, window open/closed)" every 10 s
  on THREAD 2: per second run at their entry and level by why (switch off or window closed,
  before a hard closure, no record room, GPUMem stopping), inside a level run; runs started by a
  release against all runs; GPUMem's ms a second in the served ones (and us each); the releases
  the back found run at their entry (equals served). The window line's "level inside the window"
  has a "semaphore release" reason; the link line's "served syncs" include the back's runs of
  them.
- Must be 0 (the link line): "a served semaphore release with no record of GPUMem's", "a
  semaphore release GPUMem had the back run at an entry the back passed without it" and "a back
  draw between a semaphore release GPUMem had run and its entry", as the gathers'. The record
  first, the publish right after and the record honoured past the ring's end as for the
  gathers: a release is never made twice (a second report, fence and semaphore word write).
- With FRITCHII_MEMSTAGE_GATHER on too, a gather right before a release keeps its run, which
  takes the release in (one run instead of two level waits).
- Compare 0 and 1 with profile_toggle.py in one session (5c's run starters lose "cache call"
  at the release's address).
- Since fable_stage_p3 section 3 all three puller releases are one closure with a fixed layout
  (puller.cpp's ReleaseCall: the rasterizer, the GPU address, the payload and the
  QueryPropertiesFlags, the channel's memory manager; memstage_shadow.h's StageReleaseWord) and
  carry StageCacheCall::SemaphoreRelease: WriteLong as before, and ProcessSemaphoreRelease and
  SemaphoreExecute's release (both fence-flagged), which were untagged closures before: each one
  made GPUMem's copies stale (a full copy at the next 3D entry; very likely the "call +0xc99160"
  behind most of the full copies). They are cache calls now (hard, no copy after) and candidates
  for this switch's request path too.

### FRITCHII_MEMSTAGE_PASS

FRITCHII_MEMSTAGE_PASS (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0;
work\fable_stage_levelpoints.md sections 4 to 6, with Fable's amendment and fable_stage_p3.md
section 1): with FRITCHII_MEMSTAGE_THREAD=2 and the window open (AHEAD > 0), bit 1 (=1) makes a
lone gather (3D invalidate_texture_data_cache, 0x3dd) or wait for idle (0x44) a passed entry
instead of a level run: GPUMem does what the entry does to the buffer cache itself, at its
position, and goes on; the back runs the entry later, served, with its own code, and finds
nothing to do in the buffer cache there. 0, AHEAD 0 and THREAD 0/1: as before (the passes line
is then dry). Bit 2 (=2, or =3 with bit 1) is P3, below: the same for sync points where a query
result sync is pending, and for the puller's release closures and lone 3D fence reports.
- Which (tried before FRITCHII_MEMSTAGE_GATHER; one that does not pass goes to what that switch
  says, a run otherwise): a lone method entry outside a run, no draw before it in the entry,
  - rule Q: no query result sync can be pending at the entry (the mirror in the passes line);
  - rule C: no uncommitted buffer flush (the note, else the sets under the lock): only a fill
    that marks GPU-written data adds one, and those come level first or run lent, so a set empty
    at GPUMem's position is empty when the back gets there. With FRITCHII_MEMSTAGE_MARKS_AHEAD
    on, rule C is not tested (fable_stage_levelpoints 14.7): a writing draw queues its download
    when the back promotes its marks, after GPUMem's place, so the note says nothing there; the
    back groups and commits under the lock at its own place instead (below);
  - room for its record (256).
  There is no read tracking test any more (fable_stage_levelpoints 13: EXP49 is on by default,
  and that test refused every pass); rule T below closes the window it guarded.
- A passed gather (rule G): GPUMem takes the game's collected writes (nothing when every
  collector says empty) and, under the buffer cache's lock, tests their pages (page-aligned, as
  WriteMemory's recording branch reads whole pages: the page bits, then GPUMem's own question,
  GpuWrittenHere: the real set or a mark a fill ahead made, FRITCHII_MEMSTAGE_MARKS_AHEAD). No
  GPU-written data there: the served gather's buffer half at once (claims, WriteMemory per range:
  on such pages only marks, no command; the vertex-stale marks reach GPUMem's copy through the
  mark hook). GPU-written data on a page: nothing applied, the record goes whole and the entry is
  a level run (its gather applies the ranges with the full path before it takes more).
- Rule T (13.3): every take on GPUMem (a passed gather's, GatherOnStage's) is listed on
  GPU::Impl's applying_writes, whatever EXP49 says, until the buffer cache has the writes, so a
  landing of downloaded GPU bytes in guest memory (read downloads, fence downloads: they hold
  the cache's lock and ask ForEachPendingCpuWrite) leaves those bytes out meanwhile, as for a
  tracked gather on THREAD 0. A passed gather unlists under the cache's lock right after its
  buffer half; a whole record stays listed until the back's apply at the entry (unlisted right
  after it, also when it throws); GatherOnStage unlists inside its served call. Lock order: the
  cache's lock, then applying_writes_mutex, then the managers' locks. A game write is always in
  a collector, on that list, or in the buffer cache: THREAD 0's invariant.
- Every passed entry pushes a record (its position; a gather's ranges). The back asks for it
  first at the entry (RasterizerVulkan::InvalidateTextureDataCache, GpuSplit::Execute for a
  release, Maxwell3D's wait_for_idle), so FRITCHII_MEMSTAGE_GATHER's / _RELEASE's "served ...
  with no record" stay real must-be-0s. At the gather, RasterizerVulkan::InvalidateGPUCache
  applies the record's other half (the texture and pipeline caches), then the back's own gather
  runs. A record lives until the back asks for a later entry's (one it never asked for is late,
  applied there), or is dropped at a lend (before the run's first entry), the ring's start and
  end (one at or past the end stays for the back's run of that entry without the ring).
- The back in a served entry: its own gathers (GPU::Impl::InvalidateGPUCache, read tracking on
  or off) return at once when every collector is empty; otherwise the take and the buffer half run on
  GPUMem in one served call (RasterizerVulkan::GatherOnStage), the other caches' half on the back: only
  GPUMem takes while the back is served. Its wait for idle and flush grouping (SignalOrdering)
  find nothing to do: a pending sync there, or (MARKS_AHEAD off) a set uncommitted-flush note, is
  a must-be-0 (then they run as today). With MARKS_AHEAD on, a set note or a played packet's
  marks still due (AheadDue) make the served flush grouping take the lock and group as today,
  on the back at its place (exact: the real sets are as of the back), and that is not counted.
- Must be 0 (the link line), each turning the switch to 0 for the session from the next frame
  end (logged once): "a query result sync pending at a passed entry's wait for idle", "uncommitted
  buffer flushes at a passed entry's flush grouping" (MARKS_AHEAD off only), "a pass record of an entry the back passed
  without asking for it", "a passed gather's buffer half recorded a command" (the seam arena grew
  in its bracket), "a served wait for idle with no pass record", "a pass record's slot freed while
  its taken writes were listed and not applied" (rule T), "taken writes left listed at the ring's
  end beyond the records kept".
- Exactness, in short: the back's query state at the entry has no pending sync (every caller of
  Query is an entry the mirror's table counts, below); the uncommitted set is empty there (above);
  the game writes taken at the gather were collected before GPUMem read the list, and both halves
  land at the entry, the buffer half before GPUMem's next fill (THREAD 0's order). A write made
  while the list runs has no position the game can count on and is taken at the next gather.
- The passes line counts passed ones and the first failed test per method, records and ranges a
  second, the records the back applied, and the back's own served gathers taken on GPUMem.
- Compare 0 and 1 with profile_toggle.py in one session; expected (Fable, an estimate): runs
  from about 10,500 to about 7,000 a second, GPUMem's run wait down 30 to 45 ms/s, +1 to +2 fps
  at the market.

Bit 2, P3 (work\fable_stage_p3.md sections 2 to 7, fable_stage_levelpoints 13.6 and 14.3 to
14.5): the entries where rule Q fails because a query result sync is pending, and the fence
entries. GPUMem resolves the sync's buffers itself, at the entry, when that is a pure lookup, and
puts them in the record; the back's QueryCacheRuntime::SyncValues copies the values into them and
never enters the buffer cache.
- The pure lookup: the report pages P3's mirror holds (every report since the last sync point;
  unknown when there may be others) plus the entry's own, as maximal runs of adjacent 4 KiB
  device pages, at most 16 runs (Fable's 8 raised, levelpoints 14.5). Under the cache's lock
  (BufferCache::ProbeSyncTarget for each run first): an existing buffer holds the run, and no page
  of it is CPU-modified or untracked (an untracked page is a cached write's: the sync's walk would
  re-track it). Then the sync's own ObtainCPUBuffer(FullSynchronize) on each run, at the entry's
  place in the cache's order: it finds that buffer, uploads nothing and marks the usage, as on
  THREAD 0 (the mark keeps a later upload of those bytes after the query's copy). Accurate fences
  never pass (the lookup may wait there). Anything else (no buffer yet, CPU-modified bytes,
  reports unknown, more than 16 runs) is not passed: today's path.
- Which: a lone gather or wait for idle with a sync pending (after P1's tests, the targets before
  the take); a release closure outside a run (StageCacheCall::SemaphoreRelease): unmapped and not
  fence-flagged ones pass outright, a fence-flagged one with the targets, rule C and EXP28 & 1 off
  (deferred payload syncs: its fence may not sync); a 3D fence report alone in its Method entry
  outside a run, as a fence-flagged release (its address from GPUMem's copy). Each needs room for
  its record and pushes one marked `sync`, then is published at once.
- The back: SyncValues while served takes the buffers from the entry's record (a range none of
  its targets holds: must be 0, then today's block); OutdateReadDownloads only with a read
  download pending (g_read_downloads), under the lock. A sync while served at an entry without a
  P3 record is today's block (a read flush's own fence; counted, not a must-be-0). The fence's
  CommitCacheFlushes at a P3-passed entry skips the buffer half on its note whatever
  FRITCHII_FENCE_COMMIT_FAST says; a set note there is a must-be-0 (not with
  FRITCHII_MEMSTAGE_MARKS_AHEAD, where the back commits there under the lock). With bits 0 and 1
  every other path runs as before. The fence's own
  gather while served goes to GPUMem as P1's does (GatherOnStage), for either bit. WaitForIdle's
  "a query result sync pending at a passed entry" counts only when the entry has no P3 record.
- The back asks for the record at every passed entry, at the entry: a 3D fence report in
  Maxwell3D's report_semaphore.query case (an unmapped one reaches nothing else that asks; the
  first gate run counted those as late records).
- Must be 0, added: "a query result sync range at a P3-passed entry that none of the record's
  targets holds", "a served 3D fence report with no pass record". "Uncommitted buffer flushes at a passed entry's flush grouping" is now also
  counted at a P3-passed fence's commit (FenceManager::CommitCacheFlushes). Both turn the switch
  to 0 for the session, as the others.
- Why it is exact (fable_stage_p3 7): GPUMem's lookup at the entry's place returns what the back's
  ObtainCPUBuffer would return there and changes what it changes (the usage mark); a later fill
  that joins that buffer has its copy in its own packet, played after the back's query copy; the
  pages were clean at the entry, so THREAD 0's sync would upload nothing either; and each range
  the back's sync groups lies in one of GPUMem's runs (a sync covers exactly the reports since
  the last one).
- "Memory stage passes P3 (FRITCHII_MEMSTAGE_PASS=n)" (after the dry P3 line, while bit 2 is on):
  passed per kind, sync targets a second, and the back's syncs while served from a record and
  without one. The dry P3 line then counts the live decisions (with "no record room").
- Test: a gate run with FRITCHII_MEMSTAGE_PASS=3 (memstage_gate.py, read log on, flash watch
  route, an area load, and a fight), then a PASS 1/3 toggle with profile_toggle.py. Expected
  (Fable, an estimate): 48 to 59 ms/s off GPUMem's waits for the back, +2 to +3.7 fps at 100%.

### Memory stage passes (the "Memory stage passes" line)

Every 10 s on THREAD 2 (fable_stage_levelpoints 5.2), "Memory stage passes (dry, window
open/closed)": the counts P1 (passed gathers and waits for idle) and P3 (fence entries) are
judged by. Dry means decided and counted, the entries behave as before.
- Per method (gathers 0x3dd, waits for idle 0x44), each lone one GPUMem reaches: passed (dry: it
  would pass P1's tests up to the flush test), or the first test it failed: window closed, in a
  run, late (its entry hard already), not first (not a lone method entry, or a draw before it),
  sync may be pending (the mirror below), read tracking (no longer asked since rule T: 0),
  flushes uncommitted (the buffer cache's uncommitted set, its note first, then under the lock),
  no record room, GPU-written page (both P1 only).
- The mirror of the back's query_cache.HasPendingSync() at GPUMem's place (fable_stage_levelpoints
  6.1 and 12): a function of the entries before that place, each applied in stream order where
  GPUMem's Process reaches it, runs included (their entries go through Process too, so a run's
  return changes nothing). Unknown at the ring's start. A lone 0x3dd or 0x44 (a Method entry)
  clears it whatever becomes of it (passed, run at its entry, or hard in a run: the back runs its
  wait for idle there, which syncs every streamer). A 3D report that is not a fence adds one. A
  3D fence report at a mapped address clears it (its fence is made and syncs), or adds one with
  EXP28 & 1; unmapped, no change. A puller release (hard or run at its entry by
  FRITCHII_MEMSTAGE_RELEASE): unmapped no change, not fence-flagged a report, fence-flagged as
  the fence report. The compute engine (13.5), with EXP28 & 1 off: no change (its dispatches call
  no Query, its releases are fences, which then can only clear), except a 0x6c3 report semaphore
  of the timestamped form (operation 0 or 2, neither the reduction bit 3 nor the one-word bit
  28): one more report, P3's pages unknown; with EXP28 & 1 on, any executable compute method
  but 0x6c and 0x6d: unknown. A DMA engine method, an engine of no known type, a macro run on
  another engine, and an unfollowed entry make it unknown. Any other hard entry, any other cache
  call (none calls Query: the rule beside StageCacheCall), the upload and 2D engines: no change.
- "The mirror at the gathers the back ran at their entry": with FRITCHII_MEMSTAGE_GATHER on, each
  served gather checks the mirror against the back's own answer (it had query results to sync):
  "clear but the back synced" must be 0 (fable_stage_levelpoints 12.3: the proof of the rule).
- Fence entries outside a run (the puller's release closures and the 3D fence reports): whether
  rule C holds there (no read tracking, no uncommitted flush).
- Puller releases on the front (WriteLong): timestamped and fence-flagged (RELEASE_SIZE), each
  with its unmapped ones.
- GPUMem from a run's lend to its return: waiting for the back (the run's end), for the front
  while the run is kept lent, following (the rest); then by compute launches a run (0, 1, 2-7,
  8-63, 64-511, 512+; launches are counted with the GPU profile or the delay test on), runs a
  second with back/front/following ms a second.
- The back's waits for the stage between a run's end and its next packet (its true idle after a
  level point), count and ms a second.
- The delay test FRITCHII_DELAY_RUN_US's spins.
- Then pass records and their game write ranges a second, the records the back applied, and the
  back's own gathers in served entries taken on GPUMem (FRITCHII_MEMSTAGE_PASS on).

"Memory stage passes P3 (dry)" (fable_stage_p3 section 6, after the passes line): per kind
(gathers, waits for idle, the puller's releases, 3D fence reports), whether P3 would pass it and
otherwise the first reason, in this order: not lone (a run, late, not a lone method entry; a
fence report inside a packet or batch); for a release, unmapped (passes: the back's Query returns
at once) or not fence-flagged (passes: no fence, no cache; its page joins the mirror); EXP28 & 1
(fence kinds: the fence may not sync); flushes uncommitted (no read tracking test since rule T);
for a sync point with no report pending, pass (P1's); reports unknown (after a timestamped
compute report, a DMA method, an unfollowed
entry, or a report across a page end); the fence behaviour Accurate; then the targets: the report pages GPUMem saw since the
last sync point (and the entry's own) as maximal runs of adjacent 4 KiB device pages, more than 16
runs, a run no existing buffer holds on its first page ("no buffer": the sync would create one;
should fall to near 0 after the first seconds, else pages are being collected or re-made), a run
past that buffer, a run with CPU-modified or untracked bytes (the sync would upload or re-track:
the count that decides P3's worth). Then the report pages per sync point that reached the target
test (average and maximum). The target test is BufferCache::ProbeSyncTarget under the cache's
lock: it changes nothing (no usage mark in dry). P3's mirror (its report pages) follows the same
table as the mirror above, so it does not depend on what is passed. With bit 2 on, the same line
holds P3's live decisions (plus "no record room").

### FRITCHII_MEMSTAGE_MARKS_AHEAD

FRITCHII_MEMSTAGE_MARKS_AHEAD (live, read at a frame end with FRITCHII_MEMSTAGE_AHEAD, default 0;
fable_stage_compute.md part 1 with its section 10): with FRITCHII_MEMSTAGE_THREAD=2 and the window
open (AHEAD > 0), a draw whose fill writes buffers (written storage buffers, transform feedback)
no longer makes GPUMem come level first (the window line's "writing draw" reason goes to 0, about
310 waits a second at the market). 0, AHEAD 0 and THREAD 0/1: as before.

- How: every set the buffer cache had keeps meaning "as of the back" (gpu_modified_ranges, the
  uncommitted and committed fence downloads, the taken downloads, the read downloads, the read
  traps). A write mark GPUMem's own fill makes runs at once only what its later fills need (the
  dispatch write record, the buffer's write tick, the tracker's GPU-modified bit, the vertex
  buffers' stale flags, the page bits that never clear) and puts the rest on a list in the
  buffer cache, keyed by the push index of the ring B packet the fill makes (ring B's packet
  order: the back's issue order, deferred draws included). The packet's header carries a flag;
  right after the back played such a packet it takes the cache's lock and the rest runs, with
  today's code, in packet order ("promotion"). Every reader of the real sets promotes first.
  GPUMem's own work asks both the real sets and the list (upload skips, the index fast path, the
  bank page copies, join tests, the trackers' record, the level tests).
- The back never blocks inside the lock for a promotion: while GPUMem holds it (its fill) the
  back retries, serving GPUMem's requests, and GPUMem promotes for it when it waits for the back
  meanwhile (a sync's or a join's catch-up), instead of giving up.
- Still level, the test on both sets: claims (collected constant buffer ranges over GPU-written
  data; the list on the ranges' pages), joins over GPU-written data, constant buffer words onto
  GPU-written data. After a claim's wait, a mark of a draw the back still holds deferred on those
  pages refuses the fill (the entry is a run). The list's bound, 4,096 marks, waits as a writing
  draw did.
- The fence commit (FenceManager::CommitCacheFlushes, SignalOrdering) promotes first and runs on
  the back under the lock, also while served (the link's user checks accept it there).
- Exact: a mark enters the real sets after the back recorded its draw and before anything else
  can read or change them (all promote first, under the same lock): against every other event
  (a landing, a game thread's read, a later fence) it falls where THREAD 0 puts it, give or take
  that one packet's play, and nothing after the draw is signalled to the game in that time. The
  tracker's bit is set again at the promotion (a served write at the back's position may have
  dropped it since the fill, and THREAD 0 has it set after such a write); a landing keeps the bit
  on pages with a mark on the list. A fill whose packet never went into ring B, or a packet the
  back drops unplayed, takes its marks back: the back fills that draw itself.
- "Memory stage marks ahead" every 10 s while on: marks put on the list, promoted by the back at
  the play and by others, taken back; the list's length (average, maximum); the back's waits for
  the lock at a promotion (count, ms); level waits for claims (and refused after the wait) and at
  the bound; the must-be-0 totals: a level function on GPUMem's own work over a page with a mark
  on the list, a promotion out of order, the list not empty at a ring's start or end, a mark due
  at a run's first entry, a due mark a reader could not promote, a mark in a call GPUMem serves
  for the back, a mark on GPUMem's own work outside a draw fill, a pushed packet that is not the
  fill's. Each is logged as "Memory stage marks ahead (must be 0)" (the first 16).
- Gate: memstage_gate.py with the read log at market and fight; the fight with kills (the
  read-back counters and the hitch log must not move); the flash watch route; an area load.
  Compare 0 and 1 with profile_toggle.py in one session.

### FRITCHII_MEMSTAGE_FULL_BINDS

FRITCHII_MEMSTAGE_FULL_BINDS (live, read at a frame end, default 0; spec 4.2): every draw binds
every enabled vertex binding, not only the dirty ones. The ones that were not dirty go in runs of
their own, without a usage mark, and the memory player drops a run still bound in the current
command buffer, so the recorded commands stay the same; repacked bindings are left out (binding
one again runs its conversion). It is what the stage will need instead of the scheduler's
command buffer flags (step 6b). With EXP123 >= 1 the seam's stats line adds "memory seam full
vertex bindings": vertex bind runs recorded and dropped as still bound a second. Independent of
FRITCHII_MEMSTAGE_THREAD 0 and 1; THREAD = 2 turns it on by itself.

### FRITCHII_JIT_DISPATCH_BITS

FRITCHII_JIT_DISPATCH_BITS (boot, default 20; fable52_answer_guest 2.1): the A64 JIT's fast
dispatch table holds 2^bits entries of 16 bytes, 12..20 (default 20 = 16 MiB per core, the old
fixed size; 16 = 1 MiB, 12 = 64 KiB). A profile of the game's CPU threads put the read of the
table entry (after `cmp rbx, [r12]` in GenTerminalHandlers) as the hottest instruction in
translated code, 3.2% of JIT time: hot targets spread over 4,096 pages and miss the TLB. The
table only caches block lookups (a miss takes the full lookup and refills the slot), so the
size changes nothing the guest sees, only the hit rate and the cache/TLB footprint. Read once
per process when the first A64 JIT is made (a64_emit_x64.cpp), logged as "JIT fast dispatch
table: 2^N entries"; values outside 12..20 log a warning and keep 20. A32 is unchanged (2^16).

### FRITCHII_BACK_READ_AUDIT

FRITCHII_BACK_READ_AUDIT=1 (boot, default off; Fable's ideal pipeline design 2.5, memory stage
step 2 spec 7.3): counts every read of game memory the GPU back thread makes, by call site, so
the memory stage knows every crossing. The rule for step 7 is that memory the stream writes is
read by the stage only: the list must come down to texture contents, the texture and sampler
tables and shader code; any other site is a crossing to move into the packet. Needs the GPU
split on (only the back counts). Read once when the first GPU address space is made, after
nx2_switches.txt is applied.

Hooked, in the read functions themselves (video_core/back_read_audit.h/.cpp): Tegra::
MemoryManager's Read<T>, GetPointer, ReadHandleWord, GetPointerForDescriptor, ReadBlock,
ReadBlockUnsafe, ReadBlockGuarded, ReadWordsUnsafe, GetSpan and CopyBlock, and the device
memory's (MaxwellDeviceMemoryManager) Read<T>, GetPointer<T>, GetSpan, ReadBlock and
ReadBlockUnsafe. GpuGuestMemory and DeviceGuestMemory read through those, as does a
GpuToCpuAddress followed by a device read. A MemoryManager read counts once, at its caller (its
own device reads are hidden); the managers' writes through the same getters are hidden. Pointer
getters called from outside may still be writes: their kinds end in Mut. Bytes are counted
where the size is known (not for the pointer getters).

Off: one predictable branch on a static bool per read call. On: a thread-local check, then a
lock-free table of 512 call sites (open addressing on the return address). Every 10 s the back
logs the top 40 by reads, as offsets from the exe's base (logged once as "Back read audit: on,
image base ..."):

    Back read audit: 10.0 s, 87 sites, 123456 reads/s, 4567890 bytes/s, 0 reads dropped (table full)
    Back read audit: #01 +0x1a2b3c4 MmReadHandleWord 51234.5 reads/s 204938 bytes/s

`python -I E:\OotRemakeEmulation\work\back_read_audit.py [log] [--exe EXE] [--windows N]
[--chain]` reads those lines (default: the live log), averages the last session's windows and
symbolizes the sites with llvm-symbolizer against the exe that wrote the log (its .pdb next to
it), marking each site CROSSING, allowed (texture cache, descriptor tables, shader code; a guess
from the file name) or helper (a GuestMemory helper that was not inlined: the caller is one
level out). A read function the compiler inlined into its caller (ThinLTO can, across files)
reports the caller's caller: read such a row as "a read somewhere under this call".

### FRITCHII_VERTEX_INPUT_COMPACT

FRITCHII_VERTEX_INPUT_COMPACT (live, default 0 = exactly as before; fable_perdraw_b 3.2).
RasterizerVulkan::UpdateVertexInput sent all 32 vertex bindings, used or not, about 59,000 times
a second (only about 1,500 of them equal to the one before, so EXP83's memo cannot help), in a
command that captured two 32-entry arrays: about 2 KB of chunk space copied for every send, and
a full 32 KB chunk (one worker dispatch) every 16 sends.
- 1: only the bindings that an attribute in the list names are described, and the command keeps
  12 bytes a binding or attribute in one of three size classes (up to 4 bindings and 8
  attributes, up to 8 and 16, else 32 and 32); the worker builds the Vulkan structs. The
  attribute list is the same as at 0: every non-constant location the registers define.
- Why it is exact: vkCmdSetVertexInputEXT replaces the whole vertex input state. A draw reads
  vertex data only through its shader's input locations; each location reads the binding its
  attribute description names. Every location the registers define is described as before, and
  so is the binding it names, with the same stride, input rate and divisor. A binding that no
  attribute names has no effect on any draw: a buffer bound to it (vkCmdBindVertexBuffers2) is
  ignored, and leaving it out also drops its stride and divisor checks (an unused stream's
  divisor of 0, a big unused stride). The repack path (stride 72) only changes bindings that
  attributes name; mesh emulation does not use this state.
- A later pipeline and a left-out binding: the described set depends on the registers only,
  not on the pipeline, so it cannot happen that a later pipeline reads a binding an earlier
  compact command left out. A pipeline reads locations, every defined location is described
  with its binding, and a register write that moves an attribute to another binding raises
  VertexInput and sends the state again. That is also why the attribute list is not cut to the
  locations the bound pipeline's vertex program reads: the state outlives that pipeline,
  UpdateVertexInput returns at once when no input register changed whatever pipeline is bound,
  and delta draws skip it, so such a cut would need a send whenever a pipeline reads a location
  the last send left out.
- EXP83's memo compares the lists that were built, so it works the same in both modes (a switch
  flip mid-game just makes the next compare differ and sends).
- "Vertex input compact (FRITCHII_VERTEX_INPUT_COMPACT=n) last 10 s: N recorded, bindings
  described B a send (referenced R), attributes A a send, command C bytes a send (full F)"
  (Render_Vulkan, Info, next to the EXP83 line). At 0 it says what 1 would save: "referenced"
  is what 1 describes.
- Expected: about half of the 24 ms/s UpdateVertexInput takes on the back, fewer chunk
  dispatches (and worker wakes), and a cheaper SetVertexInputEXT on the worker. Picture check
  across areas before it becomes a default (vertex layouts differ by scene).

### FRITCHII_WORKER_SPIN

FRITCHII_WORKER_SPIN (live, value = microseconds, default 0 = exactly as before; fable_perdraw_b
3.6). Scheduler::DispatchWork wakes the Vulkan worker with condition_variable::notify_all when it
sleeps, a system call: about 12 ms/s on the back, since the worker waits about half its time.
- n > 0: with an empty queue the worker first spins outside the queue lock for up to n us,
  watching dispatch_serial (raised under the lock with every push), before it waits as before.
  While it spins worker_waiting stays false, so a dispatch skips the notify. Try 50 to 100.
- Why no wakeup is lost: worker_waiting is set and read only under queue_mutex. After the spin
  (work seen or time up) the worker takes the lock again and the condition wait tests the queue
  under it before sleeping, so a chunk pushed during the spin is popped there. A chunk pushed
  after that test finds worker_waiting true and notifies, which the waiter, registered before it
  released the lock, receives. The spin only changes when the worker sleeps, never what it runs
  or in which order. WaitWorker's waits are untouched (the worker still notifies on each pop),
  and the spin holds no lock, so they are not held up by it.
- Cost: worker CPU while it spins (the worker runs at critical priority on a performance core);
  the process already uses about 7.5 CPUs, so judge it on a toggle with the 1% lows.
- "Worker wake (FRITCHII_WORKER_SPIN=n) 10 s: dispatches D/s, notified N/s, skipped B/s busy +
  S/s spinning; worker spins W/s, H/s found work, spinning M ms/s" (Render_Vulkan, Info; counted
  in both modes, at 0 the spin numbers are 0). "busy" is the old skip (the worker was running a
  chunk), "spinning" is the new one.
- Expected: 6 to 11 ms/s on the back (most of the notifies), at the price of the worker's spin
  time shown on the line.

### FRITCHII_BIND_ADDR_MEMO

FRITCHII_BIND_ADDR_MEMO (live, default 0 = exactly as before; fable_perdraw_b 3.7).
GpuToCpuAddress costs the back about 30 ms/s, 16 of it in the vertex, index and uniform binds
(UpdateVertexBuffer 8.4, BindGraphicsUniformBuffer 4.1, UpdateIndexBuffer 3.5), which translate
the same few pages again and again.
- 1: each vertex stream, the index buffer and each graphics uniform slot (stage, slot) keeps its
  last translation: the manager (unique_identifier), the small GPU page (page_bits), its device
  page and the manager's map_serial when it was looked up (Tegra::MemoryManager::
  GpuToCpuAddressMemo). A hit needs all three equal and costs one load of map_serial. A disabled
  vertex stream is not translated at all (it becomes a null binding whatever the translation
  gives).
- Why it is exact: a translation is a function of the address and this manager's page tables
  only, and those change only in PageTableOp and BigPageTableOp (Map, MapSparse, Unmap and the
  remaps through Map), which bump map_serial before their first table write and after their
  last (MapChange). So an unmoved serial means the tables are the ones the remembered lookup
  read. A lookup is kept only when no change was running when it started and none started
  before it ended (BeginMapRead, MapReadStable, the sequence lock the EXP63 page cache uses), so
  a half-written table is never remembered. Within a small page the translation is linear (both
  tables map whole small pages), so base plus offset is what the walk returns. Another address
  space (a channel switch) has another unique_identifier and misses. Unmapped results are not
  kept. A change running at the moment of a hit is ordered after it, as for a walk that races it.
- "Bind address memo (FRITCHII_BIND_ADDR_MEMO=n) 10 s: hits H/s, translations T/s (U/s not kept:
  a map change overlapped), disabled vertex streams not translated D/s" (HW_GPU, Info; only
  while the switch is on).
- Expected: 8 to 12 ms/s on the back. The memos sit beside EXP18's uniform bind memo, which
  skips the whole bind; this one only shortens the translation when EXP18's memo misses or is
  off.

### FRITCHII_STORE_MEMO

FRITCHII_STORE_MEMO (boot to arm, then live between 1 and 2; default 0 = exactly as before;
fable_a_gameceiling_answer 12.1). Non-zero when the game starts arms the memo epoch; set later
live without that, it stays off for the session (one warning line).
Guest stores to GPU-tracked pages were 3.6% of the guest cores' busy time at the market: each
one walks the page table twice and then sets the watch flag, records the store, asks or skips
OnCPUWrite, collects the dirty bit, fences for a download landing and sets the collected flag.
- Always (no switch, exact): the store's handler takes the host pointer the store's own walk
  found instead of walking the page table a second time (Memory::WriteToRasterizer).
- 1 (dry): each core keeps a one-entry memo of its last full store path and counts the stores it
  would serve; the full path still runs for every store.
- 2: a memo hit only records the store for the uniform keeps (thread-local) and returns.
- A hit needs: the store inside one 64-byte device block, the same block as the core's last full
  path, the memo epoch (Common::g_store_memo_epoch) unchanged since that path began, the core's
  write area still on the page, and no run-ahead write tracking armed.
- Why it is exact: the epoch moves around everything that takes such a mark away (a dirty manager
  Gather, a Take of the collected or watch page flags): a count of clears running in its low 16
  bits and a generation raised as each ends, so a memo is only kept when no clear ran at the read,
  and any clear since changes the value. With it unchanged, every mark the last full path set is
  still there, and the full path would have found each set (no-ops; last_address on the page
  means no OnCPUWrite either). The landing wait: the memo is only kept when that path saw no
  landing published, so every landing published later found the block's collected bit (still
  collected: no gather) and keeps those bytes. The uniform keeps' record still runs on a hit.
  Once armed the epoch moves whatever the live value says, so a change between 0, 1 and 2 never
  trusts an old memo. Unarmed (the default) a clear costs one relaxed load; armed, two atomic
  adds per flag take that clears a set bit and per gather that takes something (an empty gather
  returns before its exchange and moves nothing).
- "Store memo (FRITCHII_STORE_MEMO=n) 10000 ms: trapped stores S/s, would serve (=2: served) H/s
  (P%), same block but a mark taken since M/s; epoch moves: gathers G/s, flag takes T/s; since
  boot hits/stores by core ..." (HW_Memory, Info; at most every 10 s while the switch is on).
- Expected: 1.5 to 2% of the guest cores' busy time if most trapped stores hit (Fable A's guess:
  stores come in runs over the same block); the dry line says how many do. If the flag takes or
  gathers move the epoch too often the hit share stays low and the switch is not worth it.

### FRITCHII_JIT_INDIRECT_CACHE

FRITCHII_JIT_INDIRECT_CACHE (boot, read once per process, default 0 = exactly as before;
fable_a_gameceiling_answer 12.2). Every indirect branch (BR, BLR) and return of the game went
through two shared routines; the indirect one waits on a load from the 16 MiB fast dispatch table
(2.56% of the guest cores' busy time) and has one host jump for every target.
- 1 (dry): each indirect branch and return site gets a 16-byte slot (4 MiB pool per JIT) and
  jumps to dry copies of the shared handlers, which count how often the target equals the site's
  last one, and for returns how often the return stack buffer served it. Execution is the same.
- 2: each indirect branch site computes the target's location descriptor itself, compares it
  with its slot and jumps straight to the slot's block on a match. A miss goes to a shared routine
  that looks in the fast dispatch table; on a table hit it fills the slot, else the table's own
  miss path (lookup or compile, table refill) runs as before and the slot fills at the next miss.
  Each site has its own host jump, so the host's predictor works per site.
- 3: 2, and each return site pops the return stack buffer itself (the shared handler's
  instructions at the site, a miss continuing in the fast dispatch handler as before).
- Why it is exact: a slot only ever holds a descriptor and the entrypoint the fast dispatch
  table held for it at that moment; every slot is emptied whenever any block is removed (a range
  invalidation) or the cache is cleared, the two places the table's entries go too. A match
  therefore jumps where the table would have. Halt checks are unchanged (the shared handler made
  none either). Sites past the pool's 262,144 slots keep the shared handler.
- "JIT indirect sites (FRITCHII_JIT_INDIRECT_CACHE=1) core N 10.0 s: indirect branches B/s, same
  target as the site's last P%; returns R/s, same target as the site's last Q%, return buffer
  hits H%; sites S" (Core_ARM, Info, per core, every 10 s from Jit::Run). With 2 or 3: "slot
  misses M/s; sites S".
- Expected: 1.5 to 2% of the guest cores' busy time if the dry line shows most indirect branches
  hit their site's last target (Fable A 12). Code grows by about 40 bytes per indirect branch site
  (3: also per return site).

### FRITCHII_JIT_NO_FRAME_POINTER

FRITCHII_JIT_NO_FRAME_POINTER (boot, read once per process, default 0 = as before;
fable_a_gameceiling_answer 12.6). Since Eden 8e14f07a69 every A64 block stores rbp, points it at
its stack frame and restores it before its terminal: three instructions and 24 bytes per block
run, about 8% of an average block's bytes.
- 1: blocks leave rbp as the dispatcher set it. rbp stays out of the register allocator either
  way (reg_alloc.cpp), so no block reads or writes it.
- Exact for execution: rbp was only ever a frame pointer for stack walkers. Ours stop at the
  first JIT frame (nx2_sampler); the Windows unwind data covers the dispatcher's prologue, not the
  blocks. What changes: external profilers and crash dumps that follow rbp lose the JIT frames.
- No log line beyond the boot line "FRITCHII_JIT_NO_FRAME_POINTER=1: blocks do not set up rbp".
- Expected: 0.5 to 1% of the guest cores' busy time (Fable A's estimate). Needs one fresh-launch
  A/B (boot switch).

### FRITCHII_JIT_RSQRT_ZERO

FRITCHII_JIT_RSQRT_ZERO (boot, read once per process, default 0 = as before;
fable_a_gameceiling_answer 12.5). The vector reciprocal square root estimate (FRSQRTE, 32 and
64-bit lanes, AVX hosts) has an inline path, but one lane that is NaN, negative, zero, denormal
or +infinity sends the whole vector to C++ (0.62% of the guest cores' busy time with the other
soft float calls; 190 samples of 322).
- 1: the C++ fallback counts its calls by lane kind (code generation otherwise unchanged).
- 2: also a vector whose only such lanes are exact +-0 stays inline: the other lanes are computed
  by the inline path's own instructions (the zero lanes from 1.0), the host's exception flags are
  saved and put back around that (the C++ estimate raises no host flags), the zero lanes become
  infinity with the zero's sign, and FPSR.DZC is set, as FPRSqrtEstimate does for a zero. Any
  other bad lane still takes the C++ call. Not used when FPCR.DZE (the trap) is set.
- Exact: per lane the same values as FPRSqrtEstimate (the inline path is already trusted to match
  it for the other lanes), and the same FPSR bits (DZC; nothing from the host's sqrt and divide).
- "JIT vector rsqrt estimate fallbacks (FRITCHII_JIT_RSQRT_ZERO=n) 10000 ms: calls C/s, only zero
  lanes bad Z/s (P%); lanes/s: zero, denormal, negative, NaN, +infinity; zero lanes by lane
  a/b/c/d" (Core_ARM, Info, at most every 10 s, integer math only). At 2 the calls left are the
  ones it could not serve.
- Expected: 0.3 to 0.4% of the guest cores' busy time if most calls are "only zero lanes bad"
  (Fable A's guess: a zero in the unused fourth lane of a three-component vector).

### FRITCHII_JIT_HOT_PACK

FRITCHII_JIT_HOT_PACK (live test hook, default 0; fable_a_gameceiling_answer 12.3). Prices the
13.2% of the guest cores' busy time spent arriving at blocks: is it code layout (instruction
fetch, instruction TLB)? Needs FRITCHII_JIT_HOT_LIST (boot env) = a text file of "pc fpcr samples"
lines, e.g. E:\OotRemakeEmulation\work\fable_a_gc\hot-blocks-market.txt (19,332 blocks).
- The game loads at a random address every launch (Fable A 8), so a list's absolute pcs only
  fit the launch that made them. A pc below this launch's image base (rtld's load address) is
  read as an offset from it: use the `-rel` lists (work\fable_a_gc\hotlist-300*-rel.txt).
  Larger pcs are taken as absolute. "{core}" in the path is replaced by the core
  (hotlist-300-core{core}-rel.txt). FRITCHII_JIT_HOT_LIST_ORDER=1 keeps the file's order (the
  per-core lists are hottest tier first), else pc order. The log line names the file, how many
  pcs were offsets and the image base.
- Each new non-zero value (set it through the debug server's `switch`, e.g. 1, later 2 for a
  second round) makes every guest core, at its next idle, clear its own code cache and compile the
  hot list first, in guest pc order (PrecompileBlock through the warm-compile path, which now does
  a pending whole-cache clear first instead of compiling into the code it throws away). Everything
  else compiles again as it runs, after the hot code.
- "JIT hot pack (FRITCHII_JIT_HOT_PACK=n) core c: cache cleared, K hot blocks gone through in M ms"
  (Core_ARM, Info). An interrupt stops the compile early (the line says so); the rest compiles
  lazily.
- Exact: a cache clear and recompile of the same guest code (as after any code invalidation).
  Expect a compile stall of a second or so at the moment it runs.
- The test: market heavy phase, fps before, set the switch, wait for the six lines and about 30 s
  for the cold code to come back, fps after. Fable A: 1.5% or more means layout matters (then the
  real work: slow paths out of line, hot code together); nothing means drop that line of work.

### FRITCHII_JIT_FAR_CODE

FRITCHII_JIT_FAR_CODE (boot, default 0; Fable A 2.1). 1 = each block's slow paths (the deferred
emits: NaN handling, fallbacks, page-boundary paths) go to a far region, the last 3/8 of the code
cache, instead of right behind the block, so the hot bytes of blocks sit together (fewer cache
lines and pages per core). Only with a code cache of 64 MiB or more (512 MiB here).
- Exact: the same instructions, other addresses. A full region clears the cache like a full cache
  does; PrecompileBlock (warm compile, hot pack) keeps half the smaller region free.
- "JIT cache clear (FRITCHII_JIT_FAR_CODE) core c: near N MiB used, far F MiB used" (Core_ARM,
  Info) at each full clear: checks the 5/8 : 3/8 split.
- Expected 1 to 2% of game code time (Fable A, estimate).

### FRITCHII_JIT_CHEAP_BARRIERS

FRITCHII_JIT_CHEAP_BARRIERS (boot, default 0; helper AT, 2026-11-15, port of zortos293/Lichii
#1327, ProphetWild's commit from #161 by 999sian). x86-64 hosts only. 1 = all of #1327: ordered
and exclusive loads of up to 64 bits (LDAR, LDAPR, LDAXR, A32 LDA/LDAEX) are a plain mov instead of
`xor reg,reg; lock xadd [addr],reg`, and DMB *LD / DMB *ST (CRm<1:0> = 01 or 10) emit nothing
instead of mfence. 2 = only the plain loads, 3 = only the DMB change (to split a problem).
- Exact on x86-64 (TSO): loads are never reordered with later loads or stores (so every load is
  an acquire load), stores never with later stores. The one reordering x86 does, a store passing a
  later load, is still covered: ordered stores (STLR) stay xchg, a full barrier, so STLR then LDAR
  keeps its order; a full DMB and every DSB keep mfence; 128-bit ordered loads keep lock
  cmpxchg16b; exclusive stores still lock cmpxchg against the value the exclusive load saved.
- Side effect: an ordered load no longer faults on a write-protected (GPU-cached) page, so it
  reads it inline like a plain LDR does instead of taking the slow path. With read traps or
  reactive flushing on, a page that needs a GPU download has no read access at all and still
  faults, so the data seen is the same.
- Upstream (zortos, since 11-11) and Blitchii run it by default. Their numbers: LDAR on a
  word shared by 4 threads 20.9 -> 1.6 ns; OoT Kokiri 36.5-36.8 -> 38.1-38.8 fps on the source
  commit author's PC. No log line beyond "FRITCHII_JIT_CHEAP_BARRIERS=N: ..." (Core_ARM, Info) the
  first time a block with an ordered load or a DMB compiles.
- Our earlier revert (50d9c09292, the Kokiri slab) came with the rest of #161; the slab's likely
  cause is #161's GPU parts, see helper_at_answer.md. Check the Kokiri Forest spot with it on.

### FRITCHII_JIT_INLINE_CNTPCT

FRITCHII_JIT_INLINE_CNTPCT (boot, default 0; helper BR, 2026-11-16, idea from bakemono's handoff
and Blitchii). x86-64 hosts with the TSC wall clock only (multicore). 1 = a guest CNTPCT_EL0 read
is computed in the block (`lfence; rdtsc; lfence`, times the wall clock's factor, high 64 bits)
instead of a host call that saves every live register and goes through CoreTiming::GetClockTicks.
- Same value, bit for bit: it is WallClock::GetCNTPCT's own sum. The factor is read from memory
  at every read, and it is 0 while GetClockTicks would differ (CPU clock not Normal, core speed
  sync, EDEN_NX2_GUEST_THREAD_STATS), so those reads take the host call through a thunk that
  keeps every register. System::ApplySettings recomputes it, so a CPU clock changed in game goes
  back to the host call at once.
- The polling pause is kept: two reads closer than ~1.7 us pause twice, using the same "last read"
  as the host callback. EDEN_NX2_CNTPCT_PAUSE=0 turns it off inline too.
- Off by itself when the wall clock is not the TSC (Coffee Lake and older Intel without a crystal
  frequency: "FRITCHII_JIT_INLINE_CNTPCT: off, the wall clock is not the TSC"), and with guest
  thread stats on (they count reads in the host callback).
- Log: "FRITCHII_JIT_INLINE_CNTPCT: guest counter reads computed in the JIT ..." (Core_Timing,
  Info) once.
- Expected: less guest-core CPU time per counter read (OoT's waits poll the counter millions of
  times a second); fps only where the guest cores limit. Blitchii measured it only bundled with
  the cheap barriers (guest core 4: 750-828 -> 703 ms/s, same fps, GPU-bound laptop).
- Test: toggle needs a fresh launch (boot switch): market, 2 launches each, compare the guest
  cores' ms/s and fps; check game speed (cutscene audio sync, Dynamic FPS step) stays the same.

### FRITCHII_HWWAIT

FRITCHII_HWWAIT (boot, default 0; helper BR, 2026-11-16, idea from bakemono's handoff). x86-64
only, and only where CPUID says the CPU has the instruction and the wall clock is the TSC.
1 = UMWAIT on Intel CPUs with WAITPKG (Alder Lake and newer client parts, Tremont/Gracemont);
2 = also MWAITX on AMD (MONITORX). Anything else, or a CPU without it, is today's code.
- Site: the GPU front's wait for the back (GpuSplit::Backoff, every split drain). Its first phase,
  16384 PAUSEs, becomes 256 hardware waits of 64 PAUSEs' time each (PAUSE length measured at boot),
  so the phase lasts as long as before; a store to the position the wait reads first ends it at
  once. The yield / FrontSleep phase after it is unchanged. UMWAIT uses C0.1 (the fast wake).
- Not a CPU-time cut in Task Manager (the OS counts the wait as running, like PAUSE); the gain, if
  any, is power headroom for the busy cores on power-limited laptops, and the SMT sibling gets the
  core while it waits. Unmeasured: keep it off unless a fixed-power A/B shows no fps or 1% low loss
  (NoteSiteWait ms/s must not rise).
- The guest cores' idle poll already has a hardware wait: EXP79 (Common::WaitForFlag, C0.2, MWAITX
  on AMD, adaptive window). HostTiming's pre-event spin and the Vulkan worker spin are not covered.
- Log: "FRITCHII_HWWAIT=N: spin waits use UMWAIT (C0.1) | MWAITX | off, this CPU has no usable
  hardware wait (CPU: WAITPKG yes/no, MONITORX yes/no, TSC clock yes/no)" (Common, Info) once.

### FRITCHII_JIT_CHAIN

FRITCHII_JIT_CHAIN (boot, bits, default 0; Fable A 2.2). Off with cycle counting (single core).
- 4 = compact links (needs FAR_CODE): a direct block link is one 5-byte jump (today 22 to 53
  bytes); when the target is not compiled it jumps to a far stub that stores the pc and goes to
  the dispatcher. A link with a halt check is `cmp halt, 0; jne <far stub>; <jump>`.
- 2 = halt check only on links back (target pc <= the block's start pc). Forward links lose it,
  B/BL links back gain it (they have none today), indirect branches and returns gain one. Between
  two checks only forward links run, so the code stops for a halt within a bounded number of
  blocks. A halt raised inside a block without the debugger is noticed a few blocks later.
- 1 = chain (needs FAR_CODE, implies 4): when a block ends in a link to a block not compiled yet,
  that block is compiled right behind it and the link becomes a 5-byte nop; up to
  FRITCHII_JIT_CHAIN_DEPTH blocks in a row (default 4, 0..64). Conditional branches put the
  fall-through side last so it is the one chained. Code is compiled before it runs.
- Useful values: 6 (compact + backward checks), 7 (all). Bits 1 and 4 without FAR_CODE log a
  warning and stay off.
- Expected 1 to 2% (Fable A, estimate).

### FRITCHII_JIT_DUMP

FRITCHII_JIT_DUMP (boot, default 0; Fable A 4.1 and 8.5). Writes the host code of each compiled
block with its guest words, for offline reading. 1 = written every 4096 blocks, 2 = written only
at full cache clears and shutdown (host bytes are read when written, so links show as patched).
- FRITCHII_JIT_DUMP_CORE: the core that dumps (default 0, -1 = all).
- FRITCHII_JIT_DUMP_FILE: the file (default jit-dump-core{core}.bin in the working directory;
  put it under work\measure). "{core}" becomes the core; with -1 and no "{core}", ".coreN" is
  appended.
- FRITCHII_JIT_DUMP_LIST: only these blocks, one hex pc per line (first word of the line, so the
  hot lists work). Pcs below the image base are offsets from it, others absolute.
- Format: 64-byte header ("FJITDMP2", version 2, core, code base, code size, image base, far
  region address or 0, flags bit 0 = far code, list entries). Then 64-byte records: kind (1 block,
  2 prelude), flags (bit 0 far part, bit 1 chained), guest word count, location descriptor,
  absolute guest pc, guest pc minus image base, near address, far address, near size, far size,
  sequence number; each followed by its guest words, near bytes, far bytes. The image base
  changes every launch: match blocks across launches by the offset.
- "FRITCHII_JIT_DUMP: core c writes <file> (image base 0x...)" (Core_ARM, Info).

### Texture cache churn ports (helper AG)

- FRITCHII_TC_COLLECT_OVER (live, default 0; port of zortos #1306 by adamuremek): past the
  critical memory the collector's aggressive pass may delete one more image per 2 MiB over it
  (up to 1,024 on top of 40), and from 1 GiB over it also takes images unused for 3 frames
  instead of 10. Breaking pots in OoT's Hyrule Castle makes ~55 images a frame (about 25 new
  384x384 D32 shadow maps at moving addresses); at 40 deletes a frame the cache grew until
  device memory ran out and the AMD driver crashed. Does nothing below the critical memory.
  Line every 600 frames "Collector ports (...)": passes past critical, deletes allowed, passes
  1 GiB+ over, peak MiB over, images deleted past critical.
- FRITCHII_TC_COLLECT_EVICTABLE (live, default 0; port of zortos #1731 by moonzzeyy): the
  collector checks whether it may delete an image in this pass (decoding, costly outside the
  aggressive or old pass, needs a download outside high priority, already queued by EXP121)
  before it counts it against the pass's budget, and its candidate walk leaves such images out.
  Without it, 10 or 20 old costly (ASTC) images at the old end of the list used the whole budget
  and the pass deleted nothing. Also: the old costly pass (EDEN_NX2_TC_OLD_AGE) goes through the
  block-fill sort, and every delete takes its bytes off its block's fill. Same line as above:
  images skipped.
- FRITCHII_SWEEP_SUPERSEDED (live, frames, default 0 = off; port of Skitchii 16fb54111d by nup):
  a GPU-written image untouched for N frames whose memory a newer image of another shape (used
  since) covers is deleted without a download, checked every min(N, 30) frames, as EXP58 does
  for the cold sweep's chain images after 60 frames. Not bound images, ones with a download
  queued, sparse or decoding ones. Skitchii runs 3 (Castle Town guard house at 3440x1440: ~20
  targets moved a frame while the camera turns, driver usage 13-15 GB on a 12 GB card; orbit
  16-24 -> 42-51 fps; 2 lost fps standing). Line every 600 frames "Superseded sweep
  (FRITCHII_SWEEP_SUPERSEDED=N)": images and MiB deleted, images looked at, ms spent.
  Skitchii's EDEN_NX2_IMAGE_POOL_MIB is EXP66 here (live, MiB).
- FRITCHII_COLD_SWEEP_MAX (live, default 0 = 16): the cold sweep's per-frame delete cap
  (EDEN_NX2_TC_COLD_FRAMES). zortos #1571 (jpkto) and #1285 (Ghostwolf13) delete dead
  overwritten render targets early, 10 or 8 unused frames, up to 512 or 64 a frame: the guard
  house built up 7,858 dead targets and 27 GB within seconds there, the market lost the device on
  8 GB cards. The cold sweep (4 frames, on by default) and EXP64 already do that here, at most 16
  a frame; this raises the cap. Line every 600 frames "Cold sweep cap (...)" whenever the cap was
  reached (also with the switch at 0): frames that stopped at the cap, images watched.

### Pot room VRAM (helper CC)

- FRITCHII_COVERED_RETIRE (live, frames, default 0 = off): a GPU-written render target whose whole
  memory newer GPU writes cover (images written after its last write and touched no earlier than
  it last was) is deleted without a download once untouched for N frames. Checked once after each
  new image comes to overlap it: by the cold sweep (its own caps, 64 checks and 64 deletes a
  frame, guarded shapes too), by the collector (no download for such an image), and when an
  EXP121 download it queued is dropped for a newer overlap. Owner's pot room log (build 9): ~30
  targets a frame made at bump-allocated addresses (6,964 384x384 D32 in 600 frames), 20-300 dead
  ones under each new clear, kept until EXP58's 60 frames; device-local 9.2 -> 12.4 GB. Line every
  600 frames "Covered retire (...)": checks and ms, deletes by sweep / collector / landing, MiB,
  kept as busy, cap frames, and textures sampled again at a retired address (must stay near 0;
  the first 20 get a "Covered retire: ... sampled N frames after" warning).
- FRITCHII_COLLECT_LAND_UNDER (live, default 0): an EXP121 collector download still lands when the
  overlap the GPU wrote since is still GPU-only (its bytes are not in guest memory; its own
  download lands over ours later, and one that lands first drops ours). Before, such downloads
  were dropped and the image kept, so the next pass queued it again: 5,028 queued and 4,956
  "written again" in 600 frames in the pot room. The collector line now splits "written again"
  into newer overlap / CPU write and counts landings under a GPU-only overlap.
- Bug fix (always on): an EXP121 download no longer lands over a CPU write made to its image after
  it was queued (counted in the collector line as "by the CPU").

### FRITCHII_JIT_BLOCK_LIST (the "JIT block list" lines)

Boot, default 0 (helper AG, port of the idea of zortos #1225 by ne1i). Every block the CPU
recompiler compiles because the game ran it (not warm compiles), inside the application's code
image, is recorded relative to the image in `<cache dir>/jit/<program id>-<build id>.blocks`
(64-byte header naming the image, then append-only chunks of entries, each with a checksum; a
torn tail is cut off at the next launch; at most 2M entries, 8 bytes each). A writer thread
appends new blocks every 10 s and at shutdown. With 1 the next launch hands the list, in the
order it was first compiled, to EDEN_NX2_JIT_WARM's idle compiles once nothing another core
compiled is waiting, so every core compiles it ahead within the same budget (20 ms of every
100 ms per idle core; precompiles stop with half the code space left, 256 MiB, or half the
smaller region with FRITCHII_JIT_FAR_CODE, so the cache is never thrown away for them). 2 = record only (build the list; the A side of an A/B). Warm-up needs
EDEN_NX2_JIT_WARM=1 (on by default), several cores, a 64-bit game and no debugger. OoT: after a
save load one 721 ms frame then ~20 frames of 40-450 ms on zortos' build, a first-time code path
compiling 700-7,763 blocks in one frame. Lines: "JIT block list (FRITCHII_JIT_BLOCK_LIST=N): K
blocks loaded from ..." at boot; every 10 s with new blocks "... new blocks written, listed;
warm-up handed out a/b/c/d of K loaded" (per core). The hitch line's warm compile time shows
what the warm-up costs.

### FRITCHII_OOT_RENDER_STEPS

TEST, off by default (boot). 1 = the picture menu's Resolution (and oot_render_height) also takes
360p, 540p, 810p, 900p and 1260p: built-in patches of OoT's render scale (0.25, 0.375, 0.5625,
0.625, 0.875 of 2560x1440), docked, Max Resolution on, DLSS or FSR picked. Off, those heights are
ignored and the game draws 1440p. They stay behind the switch until a run shows the game draws
them exactly: a line "no scene camera this frame: the camera's extent isn't the target's" means
the game padded the size and its own upscale draws instead. To release a height to everyone, add
it to the list in `OcarinaRenderHeightOffered` (settings.cpp): the menu follows.

Below 720p the output is at most 3x the scene: Auto is 1080p (not 4K), 540p can go to 1440p with
FSR, 360p makes 1080p only. DLSS gets 1080p only, its exact modes (540p Performance, 360p Ultra
Performance). Check per height: `Applying built-in patch "540p render (built-in)"`, `the game draws
its scene at 960x540`, `NVIDIA DLSS (Streamline): 960x540 -> 1920x1080`, no repeating `the game's
upscale draws frame N`.

### FRITCHII_OOT_ACTOR_PARTS, _TOLERANCE, _TRACE, FRITCHII_OOT_MOTION_VIEW

TEST, all boot switches, off by default (16 for the tolerance). OoT has no motion vectors, so
Fritchii makes them: the camera's from depth, and for Link a replay of his one recognized skinned
draw (9702 indices) with this frame's and the last frame's bones. Everything else gets the
camera's motion only, which is none while the camera stands still.

- FRITCHII_OOT_ACTOR_PARTS=1: the replay also takes the other draws of the actor's vertex program
  with Link's bones and origin. A draw that doesn't belong is dropped by the replay's depth check.
- FRITCHII_OOT_ACTOR_TOLERANCE=N: how far the replay's depth may be from the scene's, in steps of
  the depth format. Try 64 or 256 if the motion view shows Link speckled.
- FRITCHII_OOT_ACTOR_TRACE=1: every 600th frame logs `Ocarina actor trace:` lines: the indexed
  draws of that program or with 48-byte vertices (shaders, index count, target, depth, blend,
  alpha test, discard), and which of them share Link's skeleton.
- FRITCHII_OOT_MOTION_VIEW=1: the game's target shows the motion vectors: black = none, blue =
  some (more red and green = more pixels of motion). With a still camera only the replay's pixels
  light up, so a screenshot shows which parts of Link have motion. Outputs up to 1440p only.

### Low-end PC simulation: FRITCHII_SIM_GPU_SLOW, FRITCHII_SIM_CPU, FRITCHII_SIM_CPU_SLOW, FRITCHII_SIM_RAM_LIMIT_MB

TEST, all off by default (helper EY, 2026-11-19). They make a fast PC behave more like a weak one,
so performance work can be judged for low-end hardware. Nothing on the PC changes: no admin rights,
no driver, power, registry or Windows setting, no qt-config.ini. Everything is set by this process
on itself and ends with it. work\lowend_sim.py sets them from presets. VRAM uses the existing
EDEN_NX2_VRAM_LIMIT_MIB + FRITCHII_VRAM_SIM_OTHERS=1.

- FRITCHII_SIM_GPU_SLOW=<factor> (live, 1 to 10; 3.5 or 350; live values are whole numbers, 350 or
  3; 0 = off). GPU timestamps around every main-queue submission; for each ms of GPU work,
  (factor - 1) ms of a one-invocation compute dispatch at the end of later submissions, between
  full barriers, so nothing runs beside it. The dispatch is measured too and the difference carried
  over. Line every 10 s: "Low-end simulation, GPU x3.50: work W ms/s, added A ms/s, xR reached, GPU
  busy B%; S submissions/s, D dispatches/s". R should sit at the factor; a lower R with "dropped"
  above 0 means the load came faster than it can be added (loading screens). Dispatches take 1 to
  16 ms each and there can be several a frame, so 16 ms does not limit a frame. The upscaler runs
  inside the measured submissions and is scaled. Not slowed: the present pass and the side queue.
- FRITCHII_SIM_CPU=<cores>/<threads> (boot; 6/12, 4/8, 6/6). The process keeps that many of the
  PC's last cores (always fewer than it has), thread pools size themselves by it and thread
  pinning plans for it, so a 4-core simulation runs unpinned like a real 4-core PC.
- FRITCHII_SIM_CPU_SLOW=<factor> (live, 1 to 4). The guest cores, GPU front, GPU back (E, E2),
  render thread R and Vulkan worker take `factor` times as long over their real work: after each
  stretch of it they spin for (factor - 1) x its length. A stretch is timed with the clock and
  capped by the thread's own cycle counter, so being descheduled or blocked does not count; the
  same time can't count twice. Left out: guest cores idling or polling, spin loops (every pause),
  waits for the other GPU threads, the worker and the host GPU. Guest code that spins on memory
  without a kernel call can't be told from work. Not slowed: the memory stage, shader compile
  workers, HLE service threads, audio. Line every ~10 s, ms per second summed over each class's
  threads: "Low-end simulation, CPU x1.50, ...: game W + A (x1.50, left out L), front ...". The
  x must match the factor; a frame can't get slower than the factor from this switch alone.
- There is no CPU cap switch on purpose. A Windows job object's hard CPU cap limits the process's
  total only, and it stops every thread until the next interval: measured on the owner's PC
  (lowend_sim.py --measure-cap, 2026-11-19), one thread capped at half a processor ran in turns with
  stalls of about 307 ms. A slower CPU does nothing like that.
- FRITCHII_SIM_RAM_LIMIT_MB=<MiB> (boot, 4096 or more): a job object's limit on committed memory.
  Past it this process's allocations fail and the game may crash; the PC is not affected. It is a
  failure test, not a 16 GB PC. Not used by the presets; lowend_sim.py reports the game's peak
  memory instead.

What these do not reproduce: cache size (the 7800X3D's 96 MB L3 helps emulators more than clocks
do, so a CPU factor has to stand in for it), memory and VRAM bandwidth, PCIe lanes, another GPU's
balance of geometry, fill and compute, other drivers' behaviour and costs, and upscaler cost on
other hardware. Use them to find what becomes the limit and to compare builds on the same setting,
not to promise a frame rate for a named card.

### Slow links and small BAR: FRITCHII_LINK_STATS, FRITCHII_SIM_LINK*, FRITCHII_SIM_BAR_MIB, FRITCHII_DESCRIPTOR_SETS, FRITCHII_LIST_ARGS_LOCAL

Helper FC, 2026-11-19. For eGPUs and PCIe x4 or x8 slots, with or without resizable BAR. All off by
default. Nothing on the PC changes. work\lowend_sim.py has presets (pcie3-x8, pcie3-x4, oculink-x4,
thunderbolt) and --link, --link-load, --link-us, --hog, --hog-mode, --bar, --link-stats.

- Always on, boot log: "Stream buffer for uniforms and buffer uploads: N MiB in memory type T (heap
  H, S MiB), device memory" and the same for texture and query uploads. "host memory" there on a
  discrete card means the CPU-visible window was full and the GPU reads that stream over the link.
  "Descriptors: ..." says which descriptor path the device allows and how much CPU-visible device
  memory it has.
- FRITCHII_LINK_STATS=1 (live). Every 10 s: "Link traffic per second": MB and requests by who asked
  for CPU writes into device memory, GPU reads of host memory and GPU writes to host memory; stream
  buffer full; real waits for the GPU; occlusion results read; list draws; what that traffic alone
  takes at 3200 and 2200 MB/s. It counts what the renderer asks for. The driver's own traffic is
  not in it (command buffers, push descriptors, query pools, present). Off: one relaxed load per
  staging request and per real GPU wait, and one per frame.
- FRITCHII_SIM_LINK=<MB/s> (live, 0 off), FRITCHII_SIM_LINK_LOAD=<MB/s> (live, taken from it, at most
  nine tenths), FRITCHII_SIM_LINK_US=<us> (live). A model: each counted transfer is charged its time.
  CPU writes into device memory make the writing thread spin. GPU transfers become GPU time through
  FRITCHII_SIM_GPU_SLOW's dispatch, which runs alone, so a copy that would overlap other GPU work
  counts in full. The latency is added to each real wait for the GPU and each list draw whose
  arguments are in host memory.
- FRITCHII_SIM_LINK_HOG=<MiB/s> (live, 0 pauses it; it or FRITCHII_SIM_LINK_HOG_MODE must be set at
  boot, for its queue).
  Real 16 MiB copies on a transfer queue, as the eGPU report's link_hog.py did, so everything on the
  link competes with them, the driver's traffic included. This is the closest thing to a slow link
  on a fast PC. FRITCHII_SIM_LINK_HOG_MODE (boot): 0 host to VRAM, 1 VRAM to VRAM (the control: same
  engine and video memory, nothing on the link), 2 VRAM to host. Line every 10 s: "Link load": MiB/s
  reached, MiB/s while copying (a rough measure of what the link gives), busy %. On a x16 link the
  same MiB/s takes a much smaller share: compare at a rate that leaves what an x4 link would have,
  and remember it also takes host memory bandwidth, which a real slow link does not.
- FRITCHII_SIM_BAR_MIB=<MiB> (boot; 0 = none): room the stream buffers get in CPU-visible device
  memory. The first pool made (texture and query uploads) asks first. What doesn't fit goes to host
  memory. It models the stream buffers only, not the driver's own use of the window.
- FRITCHII_LINK_STATS=2 also hashes every buffer upload in 256-byte chunks against that chunk's last
  upload and adds to the line how much was the same bytes again. Dry and costly (a hash table and a
  second read of guest memory), and an upper bound: the GPU may have changed its copy meanwhile.
- FRITCHII_PCIE_LINK_LOG=1 (boot; FRITCHII_LINK_STATS does it too): "PCIe link to the GPU: narrowest
  reported ...", from Windows' PCI link properties of the GPU and of each PCI device above it (top
  speed times the width in use). Read-only. A root port may report nothing (it does on the owner's
  PC). A Thunderbolt or USB4 port, found by its name, is capped at 2750 MB/s. Nothing acts on it yet.
- With FRITCHII_LINK_STATS each pool also logs, every 10 s, "Stream use for ...": MB/s and requests
  written, the worst frame, wraps, the most regions busy at a frame end, how often it was full and
  what went to host staging for it, how many of those were free at a fresh look at the GPU's
  progress, and requests over a region (always host staging).
- A stream buffer that lands in host memory on a discrete card with CPU-visible device memory logs
  a warning ("fell out of device memory").
- FRITCHII_SMALL_BAR_STREAMS (boot, default 0; 1 on). Only on discrete cards with split allocators
  whose CPU-visible device memory is 1 GiB or less. Sizes the two stream buffers from the room free
  in that window less 16 MiB, uniforms and buffer uploads first (up to 64 MiB), texture and query
  uploads the rest (up to 128 MiB), at least 16 MiB each; on a free 256 MiB window that is today's
  128 + 64. A buffer the allocator's budget check sent to host memory is tried again past the
  budget. Before a request spills to host staging because the ring looks busy, the GPU's progress
  is read again. FRITCHII_SMALL_BAR_STREAM_MIB=<transfers>,<buffers> (boot, 16 to 256 each) sets the
  sizes for tests and turns the rest on. Resizable BAR and integrated GPUs are not touched.
- FRITCHII_DESCRIPTOR_SETS (boot, default 0; 1 on, 2 AMD drivers on a discrete card with 7 GiB or
  less of CPU-visible device memory). Draws and dispatches take descriptor sets from pools instead
  of push descriptors. In AMD's open driver source, push descriptors are command buffer embedded
  data, kept with the command buffers in host memory unless the CPU-visible window is over 7 GiB;
  descriptor pools try device memory first. So without resizable BAR each draw's shaders would fetch
  their descriptors over the link. More CPU work per draw on the Vulkan worker. The pipeline layouts
  change, so the driver's pipeline cache misses on the first launch with it (and back).
- FRITCHII_LIST_ARGS_LOCAL (boot, default 0): list draw arguments in device memory, so the GPU's
  copy of the two words and the indirect fetch stay on the card.

Not reproduced by any of these: the latency of a real slow link for fetches the renderer can't see,
how a given driver places its memory, and an eGPU's present path when the display is on the host.

## Numbered experiments (EDEN_NX2_EXPn)

Numbered experiment switches EDEN_NX2_EXP0 .. EDEN_NX2_EXP79, live through the debug server:
slots for batches of fixes built together and A/B-tested in one session
(E:\OotRemakeEmulation\work\EXPERIMENTS.md lists which fix uses which slot). 0 = off.

### EXP8

EXP8 (texture upload hitches), bits: 1 = keep idle upload staging buffers up to 32 MiB
(StagingBufferPool::ReleaseLevel), 2 = no guest upload for a new image that GPU copies
overwrite whole (TextureCache::JoinImages), 4 = large 2D levels untiled on the transcode
workers too (UnswizzleImage), 8 = guest memory of uploaded textures not hinted cold
(TextureCache::TickFrame).

### EXP10

EXP10 = 1: macro DMA read snapshots (EDEN_NX2_DMA_SNAPSHOT) skip the drain for compute and
copies sent since whose write ranges miss the pages read (Maxwell3D::FrontDmaRead): copies,
inline uploads and releases as the front notes them, native dispatches as the back records
them once it ran them (the read waits for that much); 2 = the same, but a dispatch still
queued drains as before. +4 (5, 6): compute report semaphores noted as before, 16 bytes or
anything when unmapped (GpuSplit::NoteReleaseWriter). +8 (9, 10, 13, 14; live): the read tests
the bytes it reads, not the whole page: the page scans list the writes since the snapshot that
overlap the page (8 a page, then the two closest merge; GpuSplit::ListedWrites) and the read
drains only when its own bytes overlap one. Verify with EDEN_NX2_DMA_SNAPSHOT=10: "by byte
ranges (N differed)" in the "DMA snapshot EXP10" line must stay 0.

### EXP12

EXP12 (DEFAULT 4 since 2026-11-14, with EDEN_NX2_GPU_WAKE=1 built in; EDEN_NX2_SEM_FAST
acquires read current but unmet, puller.cpp ReleaseFencesForReadable):
1 = their fence release waits only for the queued writers the read can't account for, full
drain only if still unmet; 2 = suspend then too, without the full drain; 3 = measure only;
4 = as 2 without the wait for those writers (the "fence writers" site): the fence release is
queued, the word read once more, and an unmet acquire suspends at once. Exact for the reason 2
is: the waiter is registered before the fence release is queued, the back latches it at any
release that meets the value, and a suspended channel resumes only on that latch or a met read
of the word (CanResumeAcquire). Live. The risk is speed only: an acquire the latch does not
resume within 16 polls or 1 ms gets a poll drain (the "acquire" site), which costs more than
the wait did.

Lines, every 5 s:
- "Fence writer waits (EXP12=N)": as before. At 4, "writer waits" is 0 and "met there" counts
  acquires met at the read without the wait.
- "Fence writer resumes (EXP12=N)", every mode: writer waits skipped (4) and how many of them
  had the back short of the target (overstates: the back stores its position only every few
  thousand words of a batch and at its end); the
  fence path acquires resumed from a suspension, per route (the first answer of the two
  CanResumeAcquire calls a resume makes; latch: the back latched the
  waiter; word read: a read of the word met it; front answer: a queued release, the QMD
  prediction or the GEQUAL bound; poll drain: met after a poll drained the back), as a
  histogram per second of the time from acquire to resume (under 100 us / 500 us / 1 ms /
  5 ms / longer) with the mean; poll drains of these acquires and their time, split by
  pending under 2 ms or longer, and those of the other acquires; at 4, the reduction bounds of
  resumed acquires checked and not; then per channel: fence path acquires, met there,
  suspended, resumed by route, and the mean time to resume.
- At 4 the QMD prediction's "met by" for an acquire suspended there is counted at its resume
  instead of at once (there would be no "unmet"). Its reduction bound is checked there only
  when a word read met it within 1 ms of the acquire, as 2's read after its wait; by a latch or
  later the word may already be a later write's. Compare "safety: N checked" in the QMD
  prediction's reduction bound line between 2 and 4.
- With EDEN_NX2_GPU_WAKE=0 (the default before 2026-11-14) a latch does not wake the GPU
  thread: once nothing moved for 500 us it sleeps 1 ms at a time (EXP6=0), so a latched resume
  can wait out one sleep. 4 leans on latches for the acquires 2 met after its wait.

Must stay 0 at 4: the 2 s "waits on semaphore" warning. Judge 2 against 4 with a live toggle:
"fence writers" in the front waits by site to 0, the "acquire" site under 20 ms/s, "from
acquire to resume" not higher.

5 = 4, and the poll of a suspended acquire whose word reads current (CanResumeAcquire's
readable branch) no longer drains the back in full (fable_fence_writers_2 section 3). Once 1 ms
passed since the last check (the 16-poll count no longer applies there), a lagged check
publishes what was sent, waits only for what was sent by the previous check (by the suspension
for the first: the "lagged check" drain site), then asks the latch and the word again and the
queued release, QMD prediction and GEQUAL tests, without ReadSemaphoreWord. Exact: the drain
added only the latch, asked again, and ReadSemaphoreWord, which differs from the plain read
only when the buffer cache holds GPU-written bytes of the word; those mark its page GPU-written
(never cleared), which makes the word unreadable, so that poll takes the drain as before. No
hang: everything sent is published, and what was sent by one check has run by the next, where
it latched, shows in the word, or marked the page. Only with EDEN_NX2_GPU_WAKE on (read live)
and default fences; otherwise the drain as for 4 (without the wake an idle front sleeps 1 ms
at a time, which the drain hid). Unreadable words keep the drain path.

Lines at 5: the "Fence writer resumes" line adds lagged checks/s, how many waited and their
time (expect near 0), and how many found the acquire met; the "lagged check" route in the
histogram and per channel; "pending over 100 ms" (every mode, counted once per acquire, fence
path and others; not a hard zero: acquires the guest CPU releases can wait that long during
loads and pauses, so compare with EXP12=2 in the same scene). Judge 4 against 5 (GPU_WAKE=1 on
both): poll drains of these
acquires pending under 2 ms to near 0, the split drain zone down, latch and lagged resumes
under 1 ms up, channel 2's time to resume down.

6 = 5, but the acquire waits when something queued can write its word itself
(fable_fence_writers_2 section 4): the target is the newest of the QMD prediction's record for
the word that has not landed, the block and page stamps over the word alone
(GpuSplit::BytesStamp: WriteStamp without its floors, i.e. writers of anything, 3D method
writes, unknown writes, functions, the overflow) and a queued puller release to it, taken
before the fence release is queued. Past the back's position: wait for it (drain site "fence
word writers"), then the latch and the word, as 2. Otherwise suspend at once, as 4. While the
write stamps are off (before the front's QMD copy started them) it waits for 2's target. Exact
as 4: it only chooses when to look, and a wait skipped is never a wrong answer. The polls check
lagged as 5. With EDEN_NX2_GPU_WAKE=0, 6 keeps its wait at the acquire and only the polls fall
back to 4's drain. The "Fence writer resumes" line adds word writer waits/s, which term set
their target (unlanded prediction record, block/page stamp, queued release), how many had the
stamps off, how many acquires had none queued, and (counting only) the term the full
WriteStamp would have waited for, by StampTerm, which tells real writers (block, page stamp)
from floors. Judge 5 against 6 (GPU_WAKE=1).

### EXP17

EXP17 = 1: a small streamed uniform buffer binds again any stage or slot's copy of the same
guest range made in the current command buffer, when the bytes still match
(BindSmallUniformReusing; the original's EDEN_NX2_UBO_RANGE_REUSE).

### EXP18

EXP18 (buffer binding memos), bits: 1 = a constant buffer bind of the slot's current
GPU range keeps its translation and resolved buffer (UBO_REBIND), 2 = storage bindings reuse
their last FindBuffer result (STORAGE_MEMO), 4 = buffers touch the LRU once per frame
(default 4: exact, see LoadExperiments). All while the mappings and the set of buffers are
unchanged (BufferCache::binding_epoch).

### EXP19

EXP19 (graphics pipeline lookup without Refresh, PipelineCache::CurrentGraphicsPipelineClean):
1 = as EDEN_NX2_PIPELINE_CLEAN=1, 2 = also the original's patches (blend registers left to the
dual-source bit and Dirty::Blending, attribute types redone alone under Dirty::VertexInput).

### EXP50

EXP50 (live; with EXP19 / EDEN_NX2_PIPELINE_CLEAN on; after a ported fix):
1 = the blend registers leave the clean lookup's word compare and
Dirty::Blending is not looked at; when they moved since the key was made, the attachments
RefreshBlending would build from them (FixedPipelineState::BlendingMatches) and the
dual-source bit are compared with the key's: equal keeps the pipeline without a Refresh,
different patches only the blend part of the key. Reads the registers, so the ported change's writes
without the flag are still caught. Logs "saved by the blend value check" and why the other
lookups refreshed in "EDEN_NX2_PIPELINE_CLEAN per second". Off by default, not measured yet.

### FRITCHII_INTEL_PCACHE_FETCH

Boot, default 1 on Intel's Windows driver (no effect elsewhere); 0 = saves during play as on other
drivers. That driver can fault inside vkGetPipelineCacheData while other threads build pipelines
with the same cache, and the game closes with no message. Our VkPipelineCache file is then only
read back from the driver with no build running: at exit and between titles. A crash loses what
this session added to our file only; Intel's own disk cache still has it. No effect on speed or
picture. Check on an Intel GPU: play 10 minutes with new pipelines, no "Vulkan driver pipelines
cached at" in play, one at exit, and the next boot logs "Loaded Vulkan driver pipeline cache".

### FRITCHII_DLSS_LOW_VRAM_CNN

Boot, default 0. DLSS's default model is now "Automatic": K, as before. With this at 1 Automatic
draws with NVIDIA's CNN preset E on cards under 7 GiB of video memory (reported on an RTX 3050 6 GB
at 1080p Performance: 1.55 ms less GPU time, 27 MB less memory than K). A player who picks K, or any
model, keeps it. The status bar and the log name the model that draws. Off by default because E has
not been judged in OoT, and because Streamline 2.14 marks it deprecated: a DLSS file without it
falls back to NVIDIA's default, M or L, the slower models that drew thin lines in Fire Emblem. E
can also ghost more on grass and thin objects than K, mostly at Performance and Ultra Performance.
Test on any RTX card: `EDEN_NX2_VRAM_LIMIT_MIB=6144` + this at 1 (the log says "the Automatic model
is the CNN preset E"; the status bar says E), Performance and Ultra Performance, against K at the
market, in tall grass and at thin fences; compare GPU busy time. `EDEN_NX2_DLSS_PRESET=E` forces it.

### FRITCHII_PCACHE_SAVE

Boot, default 1; 0 = every save as before. Only where our VkPipelineCache file is in use (not
NVIDIA's or AMD's Windows drivers by default). The file is written only when the driver's data
changed size since it was read or last written: a boot whose pipelines were all in the cache
counted as many builds as one that made them and wrote the whole file again. A save during play
also waits until free RAM is at least twice the cache plus 4 GiB, because it copies the whole
cache into memory and the file cache and pushes the game's pages out; the exit save always runs.
Check: boot twice with no new pipelines; the second boot logs no "Vulkan driver pipelines cached
at" before play and the file's date stays.

### FRITCHII_PIPELINE_KEY_HASH

FRITCHII_PIPELINE_KEY_HASH (live, default 0): the graphics pipeline lookup after a key change
(GraphicsPipeline::Next: the current pipeline's key, then each transition's, a full key compare
each) compares a 64-bit fingerprint of the keys first (GraphicsPipelineCacheKey::Fingerprint,
over exactly the bytes the key compare reads) and runs the full compare only where the
fingerprints match.

- 0: Next as before.
- 1: fingerprints first. The pipelines' fingerprints are made once (their keys never change),
  the current key's once per lookup, from the key as the lookup has it.
- 2: both, Next's answer kept; the answers that differ are counted (must be 0) and the first 16
  logged.

Exact: equal keys have equal fingerprints, so a candidate skipped by its fingerprint is one the
full compare rejects, and the candidates are tried in Next's order. Works with or without
EDEN_NX2_PIPELINE_CLEAN, EXP52 and EXP55 (they decide when the lookup runs, this how it compares).

Counts, one line every 10 s while on: "Pipeline key hash": lookups/s, keys looked at (and per
lookup), full compares, compares skipped by the fingerprint, fingerprint matches with another key,
and the verify's checked / differ.

### EXP22

EXP22 (split ring traffic; gpu_split.h DeltaWrite and DrawBatch), bits:
1 = 3D state writes that went as Direct macro writes or (EDEN_NX2_RING_PAIRS) as pairs go in
StateDelta blocks, applied in one loop (FRONT_STATE, without its per-register merge),
2 = repeated instance-loop groups go as one DrawBatch entry that counts the repeats
(DRAW_BATCH), 4 = same-value index_buffer.first writes stay on the front
(SKIP_SAME_DRAWREG, as EXP2), 8 = the back checks each delta and batch against the single
writes and keeps theirs (logs "EXP22 check"). 7 = all three. The original's FRONT_CB_ENCODE is not
ported (it holds constant buffer writes off the ring).

### EXP23

EXP23 = 1: trim unused pipeline libraries every 600 frames past 4096 (ported, stage B;
PipelineCache::TickFrame, GraphicsPipelineLibraryCache::TrimUnused). Off by default, not
measured yet.

### EXP24

EXP24 = 1: Ocarina of Time's present poll (`while (nvnEventGetValue(e) != 1) SleepThread(1 ms)`,
from a recomp's research) sleeps until the GPU's next write of a 1 instead, at most 1 ms
(core/hle/kernel/svc/svc_nx2_oot_present.cpp; the site per build id is in its PresentSites
table, or EDEN_NX2_OOT_PRESENT_SITE=\<ret>[:\<blr>] in hex main offsets). Logs "OoT present
poll (EXP24)" counts every 10 s. EXP24 = 2: locate only, sleeps unchanged: short sleeps
(<= 2 ms) are counted by call chain and the top 8 logged every 10 s ("OoT present poll
locate"). EXP24 = 3: no GPU wake, the poll's 1 ms sleep is EDEN_NX2_OOT_PRESENT_SLEEP_US
instead (default 250 us, at most the time asked for). EXP24 = 4 (the default, LoadExperiments)
(fable52_answer_pipeline section 2): as 3, but returns at once when the event's value word
(event object field EDEN_NX2_OOT_PRESENT_FIELD, default 0) already reads 1, and the sleep ends
as soon as it does (Nx2OotPresentPollCheck from KThread::BeginWait: the game's MainThread
writes it, not the GPU). Logs "OoT present poll (EXP24=4" every 10 s.

### EXP25

EXP25 (Ocarina of Time resource poll backoff, ported; needs the built-in patch
placed at launch by EDEN_NX2_OOT_POLL_BACKOFF=1, core/hle/kernel/svc/svc_nx2_oot_poll.cpp):
the resource request thread sleeps before rescanning requests that are not ready.
0 = no sleep (as without the patch, plus one SVC per rescan), 1 = 500 us (the original default),
above 1 = that many us, at most 16000. Off by default, not measured yet.

### EXP26

EXP26 (native uniform bank check trims, ported; BufferCache::
BindHostGraphicsUniformBuffer and CheckNativeBankPagesTracked), bits: 1 = a bind that streams
the buffer (at most 4 KiB, not GPU-modified) skips the check, the stream copies guest memory
itself (BUFCACHE_BANK), 2 = pages compared with an inline SSE2 loop instead of memcmp
(PAGE_CMP), 4 = changed pages copied to the host buffer from their snapshots instead of being
marked CPU-modified (two VirtualProtect calls each), for pages whole in the bound buffer with
no GPU-written data (UploadNativeBank's upload, without its once-a-frame bank shadows: the
check stays per bind, d79572d28a). Off by default; measure 1 and 2 first, then 4. Verify
with EXP9=2 ("missed" stays 0) and the flash watch.

### EXP35

EXP35 (guest writes of never-GPU-written cached pages, Core::Memory::HandleRasterizerWrite):
1 = collected for the split's back (applied at its next FlushCaching) instead of OnCPUWrite
under both caches' mutexes. On by default (2026-11-08): Castle Town guest-core lock spinning
11.6% -> 7.6% of busy (writes 6.1% -> 0.7%), standing 43.0 -> 45.3 fps, route 45.0 fps with
175 flash reports (camera-motion noise ~270 either way).

### EXP36

EXP36 (live): 1 = guest reads of cached memory always take GetFlushArea's locked path (turns
off its lock-free answer for image-free, never-GPU-written pages). For the washed-out hunt.

### EXP37

EXP37 (live): 1 = nx2:gsv answers the driver's armed syncpoint waits right after each guest
syncpoint increment (SyncpointManager guest increment hook), not only at its 500 us poll.

### EXP35 = 2 and EXP60

EXP35=2 (live): every guest write of cached memory is collected for the back, not only writes to
never-GPU-written pages. EXP60 (live): 1 = a guest read whose cache mutexes stay taken after a
short spin is answered with a flush request served by the back (GetFlushArea).

### EXP49

EXP49 (live; async read-back): 1 = a guest read
of GPU-written buffer memory (GPU::OnCPURead) has the GPU thread record and submit the
downloads without waiting for the host GPU (RasterizerVulkan::FlushRegionForRead). The reading
thread waits for them and writes them to guest memory (BufferCache::FinishReadDownloads); a
read whose bytes are already on their way only waits. Until they land the bytes stay
GPU-modified; guest CPU writes keep their bytes (ClearReplacedDownload, pending writes at
landing); writes from the GPU's side (MemoryManager, query results, DMA clears and copies) land
them first. 0 = the synchronous flush (Scheduler::Finish on the GPU thread under the buffer
cache's lock). Both log "Read-back (EXP49) per second" every 10 s (reads, GPU thread time in
read flushes, reader waits). Not ported: the original's early downloads and EDEN_NX2_READ_CHECK.

### EXP27

EXP27 (keep the split's back fed, fewer submits), bits: 1 = the front
publishes before it yields or sleeps polling suspended channels (PUBLISH_WAITS,
GpuSplit::PublishBeforeWait), 2 = retired (was PUBLISH_IDLE, each entry published at once
while the back found the ring empty; EXP39 & 1 replaces it), 4 = the back prefetches the ring 128 words
ahead (RING_PREFETCH), 8 = command list copies after a dispatch are submitted when the front
waits for them, when the back goes idle or with the next submission, instead of at once
(DISPATCH_BATCH, RasterizerVulkan::SubmitNx2ListCopies). Off by default; 1, 2 and 4 change
only when work becomes visible, 8 changes submission timing (the boulder-flash family: flash watch).

### EXP28

EXP28 (fewer fences; FenceManager::SignalFence), bits: 1 = a fence whose
only work is syncing payload releases into host buffers is skipped like an empty one (the
guest word was written at once; the copy goes with the next fence made or wait for idle,
values kept per release since the query slot may be reused: GuestStreamer::
SyncDeferredWrites) (FENCE_TRIM), 2 = a made fence with nothing to wait for on the host GPU
joins the newest queued fence, or with none queued and the fencing thread idle runs its work
on the split's back at once (FENCE_FOLD, FoldStubbedFence). Off by default; timing family
(boulder flashes): flash watch after each. "Fence decisions/s" (GPU profile) counts
both.

### EXP29

EXP29 (a ported micro bundle), bits: 1 = Device::GetDeviceMemoryUsage reuses an
answer younger than 50 ms (MEMSTAT_MS; the buffer and texture caches ask twice per frame on
the back; zone "memory query" times the driver query), 2 = GPU::TickWork checks an atomic
before taking the sync request mutex (OPT_TICKWORK), 4 = native compute engines no longer
record inline upload execs for the old launch method they never run (DROP_NATIVE_UPLOADS).
Default 6: 2 and 4 are exact (2 only allows interleavings the mutex already allowed, 4 drops
a list nothing in native mode reads); 1 is off, since the caches' GC then sees usage up to
50 ms old. EDEN_NX2_EXP29=0 is the behaviour before.

### EXP30

EXP30 = 1 (no env var of its own): a macro DMA read of report memory
with pending query results (the ported white flash fix, RasterizerVulkan::FlushQueryResults) waits
only until the counter reports in its range are written (Tegra::Nx2WaitAsyncReportsSettled,
slots from 12 bytes before the read), not for an operation queued behind every fence, which
also waited for all GPU work sent after those reports (8-15% of the back while walking in
Ocarina, ~11 waits/s of ~3.7 ms, and the front's FrontDmaRead drain with it). Vulkan only
(OpenGL keeps its FlushRegion). 0 = the operation wait as before. Off by default, not measured
yet: compare the back's "query DMA wait" profile zone (ms and waits per second) and the white
flash watch.

### EXP31

EXP31 (render target memo, RT_FAST, ported; TextureCache::
UpdateRenderTargets), bits: 1 = a draw whose render targets are clean (no Dirty::RenderTargets)
skips preparing them again while the same views were prepared for a draw in the same frame,
no image was marked modified, created, deleted, rescaled or CPU written since (modification
tick and bindings_epoch unchanged), and every target image is GPU-modified with no CPU
modification, alias or sparse state, 2 = with 1: such draws run the loop anyway and check it
changed nothing but the targets' own modification ticks (flags, bindings_epoch; logs "RT_FAST
(EXP31) mismatch" and drops the memo on a difference). Logs skips, verified and mismatches
every 600 frames while on. Off by default; measure the back's "render targets" zone (3 = verify
first, then 1).

### EXP32

EXP32 (texture binding memo, the core of a ported BINDMEMO for "the same textures
as the pipeline's previous draw"; Vulkan GraphicsPipeline::TextureMemo, graphics pipelines whose
image bindings are all sampled textures), bits: 1 = a draw that reads the same TIC and TSC
handles as its pipeline's previous draw, with the same pools, channel, GPU mappings and texture
cache epoch (no view removed: image deleted, joined or rescaled), the same guest TIC/TSC
entries and images without CPU writes to upload or aliases, takes that draw's view and sampler
ids and descriptor entries instead of GetSamplerId, FillImageViews and PushImageDescriptors
(handles are still read every draw: the EXP21 storage pointer rule and the per-bind uniform bank
check of d79572d28a are untouched), 2 = verify: every hit for 120 frames after the switch
changes, then one hit in 64, runs the full path instead and compares view ids, sampler ids,
descriptor entries and rescaling bits ("Texture memo mismatch (EXP32)" log, first 32), 4 = a
mismatch turns the memo off until the switch changes. "Texture memo (EXP32=n)" logs hits, misses
by cause, records and verify mismatches every 10 s while set. Off by default; 7 = memo, verify
and auto-off. Measure the back's "read handles", "fill image views" and "push descriptors"
zones (the sampler lookups move from the first to the second with the memo on).

### EXP33

EXP33 = 1 (ported; QueryCacheBase::CounterReport and
FenceManager::SignalFence): at fence behaviour Default without GPU accuracy High (or
EDEN_NX2_ACCURATE_PARTS bit 1), a fence-flagged query report is no longer treated as
synchronized: the query stays registered until its fence runs, and that fence is made even
when empty and runs the report's operation once the host GPU passed the work before it
(VideoCore::g_signaling_query_report_fence), as with delayed fences. Other fences are skipped
as before. Read once per report, live. The ported fix for Ocarina's white flashes on leaves and
walls at low accuracy; ours already has the ported FlushQueryResults wait (EXP30 trims it), so
off by default, not measured yet: white flash watch, and the front's semaphore waits (the
reports land later than with the fence skipped).

### EXP34

EXP34 (EDEN_NX2_ACQUIRE_PRECHECK=2's reset hole, fable_answer_6 6i (b); puller.cpp
PrecheckAudit): where level 2 would answer a GEQ acquire met from guest memory alone,
1 = it takes the normal path instead when the front knows a write to the word is still queued
for the back: a plain release (puller, SEM_EXECUTE, 3D or compute 0x6c3) with any payload, a
noted write over the word (puller semaphores, 3D reports, compute 0x6c3 releases and
reductions, copies, uploads) or, with EXP13 on, a QMD prediction record, and when
SemaphoreReadableNow read the word current but below the value (the Castle Town route's only
cases, 5-13 per 5 s; most likely work it doesn't track ran between that read and level 2's,
the log's first 8 say); 2 = only when that
queued plain release writes less than the word read (a reset); 0 = count only. The counts
are logged either way ("Acquire precheck (level 2)", every 5 s). Default 1: it only turns met
answers into the path level 2 takes for a word below the value, never the other way, so it is
a strict tightening; its cost is the drains it puts back (compare 0, 1 and 2 with
profile_toggle and the AcquiresFast count).

### EXP39

EXP39 (live; keep the split's back fed), bits: 1 = the back
stores its position when it runs out of work (GpuSplit::back_idle_at) and, while that is the
position last published, the front publishes at its next Commit once it holds 8 words, not
after 256 (outside open Pairs and StateDelta blocks; replaces EXP27 & 2), 2 = when an acquire
resumes, the snapshot refresh (DmaPusher::RefreshSnapshot) skips the current list's entries
that ran already and the entry the acquire suspended inside, which reads memory anyway
(CanUseSubmittedCommands); the lists behind it are copied as before. Default 3: both only
change when the back sees work or which words nobody parses again are copied. Measure with
profile_toggle (EXP39 0 vs 3): back "split wait", front "resume pending".

### EXP40

EXP40 (live; command lists on GPU-written pages), bits:
1 = each command list page keeps the 4-byte words GPU work wrote that guest memory lacks
(BufferCache::Nx2ListPage::gpu_words: set by every GPU write noted to the page, the whole range
a dispatch or copy may write, cleared for words the host writes whole and for all of them when
the front flushes the page); once the page was flushed, a list on none of those words reads
guest memory and waits for no copy, and a list on some reads those from the copy and the rest
from guest memory (the "list GPU wait" zone falls; a storage write mark that
EDEN_NX2_WRITTEN_SSBO_MARK_KIB capped counts as written to the end of its last page, since
the shader may write past the cap); 2 = a list on GPU-written pages waits
only for the compute and copy work sent before the last host sync ("list copy writers"
site), not for every one sent so far ("list writers"). Off by default: both read words
earlier than before. Verify with EDEN_NX2_LIST_VERIFY=1 ("list verify ... 0 differ"). Always
on (bug fixes): copy engine copies and clears tell the list pages what they write to guest
memory (their copies went stale), DMAClear checks its whole range (it passed words as bytes),
and a front waiting for a list copy publishes first.

### EXP41

EXP41 (live; command entries on pages no GPU work wrote yet, DmaPusher::Step): 1 = the writer
waits before the port: EDEN_NX2_LIST_WAIT_SKIP=1
(built in) skips every entry's wait, =0 waits for every queued dispatch and copy. 0 (default)
= the new shape: an entry waits only for the compute and copy work sent before the last host
sync (WaitForSyncedWriters), and with LIST_WAIT_SKIP=1 an entry that begins between methods
(a command buffer the CPU writes) doesn't wait unless copy engine work is still queued;
entries inside a method's data (the parameters GPU work writes) still wait. Against the
built-in skip-everything this only adds waits. Entries that didn't wait for every writer are
checked on the back ("EDEN_NX2_LIST_WAIT_SKIP: command list at ... was GPU-written": stays 0).

### EXP42

EXP42 (live; VkImage pool, fable_answer_12 2B; vk_texture_cache.cpp): 1 = a deleted
single-level, single-layer render target's VkImage (and its rescaled copy), once the GPU is done
with it, is kept with its memory for the next image made with the same create info and view
formats, instead of vkDestroyImage + vkCreateImage. Only device-local, non-dedicated images of
256 KiB or more; the pool holds at most 512 MiB, 180 frames each, and empties when the switch
goes off or an image does not fit in device memory. A taken image starts uninitialized
(UNDEFINED layout, contents discarded) as a new one does. Its bytes are left out of the
collector's device memory figure, as deleted images waiting for the GPU are. Logs "Image pool
(EXP42) last 600 frames" and counts pooled creates in "Slow clear texture cache". 1 is the default
(LoadExperiments); 0 = off.

### EXP79

EXP79 (live, 0 = off; ported; Kernel::PhysicalCore::Idle):
a guest core going idle polls its interrupt flag in a hardware wait (MONITORX/MWAITX on AMD,
UMONITOR/UMWAIT on Intel, Common::WaitForFlag; yields elsewhere) instead of a spin of yields
(or EDEN_NX2_IDLE_PAUSE's pauses), so Interrupt's store ends the poll at once and the core
issues nothing meanwhile. The poll length adapts per core (Common::NextPollWindow, the
cpuidle-haltpoll rule): a period a longer poll would have caught doubles it, one past
EDEN_NX2_IDLE_SPIN_US (the limit, 100 us for Switch 2) halves it, down to an eighth of the
limit. EDEN_NX2_IDLE_STATS=1 logs each core's idle periods every 10 s (polled, blocked, wake
latency). Blocking still uses EDEN_NX2_CORE_LOCK's address wait when that is on. In OoT the
guest threads' handoffs sit on the frame's critical path. Off by default.

### EXP87

EXP87 (live, read per draw, 0 = off; ported; GraphicsPipeline::
ConfigureImpl): draw templates. A pipeline whose bindings are all sampled textures with handles
read from constant buffers keeps each draw's views, samplers, texture descriptors and rescaling
words in a shared table (8,192 slots, 4-way, keyed on the pipeline, the address space, the
texture and sampler pools and the handle words). A later draw with the same key, also in a
later frame, takes them instead of resolving its views and samplers and pushing descriptors,
while the template's TIC and TSC bytes are still in memory, no mapping changed
(HostPointerSerial), no image changed its scale (ScaleEpoch) and none of its views was deleted
or moved (ImageViewGeneration). Not with EXP32 or EDEN_NX2_TEX_BANK_GEN on, for mesh pipelines,
or with the OoT FSR LOD bias on a sampler. 1 = on; 2 = verify: draws a template holds for are
resolved in full and compared with it, nothing taken from templates ("Draw template verify in
10 s: ... mismatched" must stay 0). With EXP87 != 0, "Draw templates in 10 s" logs hits,
misses, kept templates and rejections every 10 s.

### EXP86

EXP86 (live, 0 = off; fable52_answer_gpuwritten; RasterizerVulkan::GetFlushArea): a guest
read of pages the GPU ever wrote (no image there, EXP36 off) skips the caches' locks
when the buffer cache's page mirrors (gpu_written_mirror.h) say the locked path would answer
"preemptive" and change nothing: no GPU-modified byte on the pages now, and a Preflushable
mark or no armed read trap there. The mirrors change only under the buffer cache's mutex; a
sequence counter is odd from a hold's first change to its release, and a busy counter sends
the read to the locks. 1 = the fast path; 2 = verify: locked reads compare the mirrors' answer
with what the locked path decides ("mirror clean but cache not" must stay 0), no fast answers.
"GPU-written page mirror" logs the answers every 5 s, also while off.

### EXP88

EXP88 (live, bits, 0 = off; ported, parts a to
c, fable52_answer_next70 3; nx2_cb_path.h): 1 = part c: RasterizerVulkan::InnerInvalidation
(FlushCaching's ranges) and, on the back, OnCacheInvalidations (collected guest writes) skip
BufferCache::WriteMemory and its lock where it would change nothing: no page ever left the
CPU-modified state (MemoryTrackerBase::IsRegionEverSynchronized), no GPU write ever marked one
and no enabled vertex binding overlaps (BufferCache::WriteReachesBuffers); its three page marks
stay (NoteWriteUnlocked: the bank check reads g_buffer_write_pages). The collected writes also
skip the texture lock and the pipeline cache where their page filters are clear, and the
ClaimCpuWrites lock where no GPU-modified mark ever reached. 2 = parts a and b: constant buffer
data (CallConstBufferWords in place of CB_DIRECT, ProcessCBMultiData in place of
WriteBlockCached) goes to the bound buffer's device address, translated once per CB_ADDRESS,
CB_SIZE and memory map (Maxwell3D::WriteBoundConstantBuffer), its device ranges logged for
FlushCaching (MemoryManager::WriteConstantBufferData); buffers that aren't one mapped device
range and Switch 2 sparse memory take the old path. 4 = verify: skipped ranges take the locked
WriteMemory anyway after checking it changes nothing; direct writes are read back and their
translation redone at FlushCaching. Both mismatch counts must stay 0. Same family as EXP85
(its inline writes reach FlushCaching with & 1) and CB_DIRECT (& 2 replaces it). "Constant
buffer path (EXP88" logs the counters every 5 s while not 0.

### EXP90

EXP90 (live, 0 = off; Fable, fable52_answer_after88 2; TextureCache::TouchImageLru): an image's
LRU node is touched once a frame, decided from a frame tick kept on the image
(ImageBase::lru_touched_tick) instead of reading the node (a cache miss) on every
PrepareImage, the way EXP18 & 4 does it for buffers. Exact: the node already ignores a second
touch with the same tick, so the LRU order and the ticks eviction and the sweeps read stay the
same. "Image LRU touch (EXP90" logs touches made and skipped every 5 s while not 0.

### EXP89

EXP89 (live, 0 = off; N > 0 = frames; Fable's lingering protection, fable52_answer_after88
section 1; core DeviceMemoryManager::UpdatePagesCachedCountNoLock): when a page's cached
count falls to 0 it stays protected and is noted with the frame; when a cache counts it again
within N frames nothing is called (no unprotect, no protect). RasterizerVulkan::TickFrame's
sweep (DeviceMemoryManager::ReleaseLingering) really uncaches the pages noted more than N
frames ago; Map, Unmap and UnregisterProcess uncache their pages at once. Exact: a lingering
page is a protected page no cache tracks, whose collected writes reach the caches as today.
Switching to 0 live releases every noted page at the next sweep. Start with 4. Measure first
with EDEN_NX2_PROTECT_STATS=1 and EXP89=0: "Page protection frames since the last unprotect"
gives the share of protects each N would cancel; "Lingering protection (EXP89" logs the
cancelled calls, releases, pages held and, as the verify, pages left lingering after an
Unmap (must stay 0). Watch "writes collected" in the Guest access line: if it rises much,
lower N.

### EXP91

EXP91 (live, 0 = off; fable52_answer_uniformkeeps stage 0; counting only, nothing bound
changes): RasterizerVulkan::InnerInvalidation notes every cached write range it gets
(FlushCaching's, EXP88's constant buffer data among them) in a ring of 128 with a sequence
number, before any early return (BufferCache::NoteCachedWrites). Each of today's slot rebinds
in BindSmallUniformReusing (same bytes, same tick) is then judged as a keep would be: K when
the slot's last bind was the same range and no range noted since overlaps it, with nothing
pending in MemoryManager (accumulator, EXP88's ranges); else not kept, by reason (range
written, ring overflow, pending cached writes, other range, no keep yet). "Uniform keeps
count (EXP91" logs small binds, rebinds and those counts per second every 5 s while not 0.
Gain of the later stages is about K x 55 ns; under 100,000 a second, stop there.

### EXP102

EXP102 (live, pauses, 0 = none; fable52_answer_yield 7): with EXP98=1, an early-returned
yield pauses this many times, keeping the old cadence (try 6, 16, 64).

### EXP103

EXP103 (live, pauses, 0 = off): common WaitForFlag's fallback (no MONITORX/WAITPKG, as on
this owner's PC; EXP79's idle wait uses it) polls with N pauses between checks instead of
SwitchToThread (try 64).

### EXP104

EXP104 (live, 0 = off, 1 = on with entries kept 180 frames, N > 1 = kept N frames; Fable's
ideal pipeline design 8.3, step 3b; renderer_vulkan/vk_buffer_recycler.h): a pool of VkBuffers
by size class for the buffer cache's buffers and the staging buffers. Buffer cache buffers
are made at their size class (powers of two from 64 KiB, up to 32 MiB; sparse-compatible ones
never) and, when the delayed destruction ring lets a Buffer go, its VkBuffer is pooled instead
of destroyed; staging buffers the staging pool releases are pooled by memory usage and size.
Creates take a pooled one of their class first. Exact: an entry carries the scheduler tick
current when it was returned and is handed out only once the GPU passed it (IsFree); the
cache clears a new Buffer's range as before. 256 MiB held at most (oldest go first); entries
past their age, over the cap or left when the switch goes off are destroyed by the back in its
split idle hook (EXP99's way), at the frame's end without the split or past 256 MiB waiting.
Costs up to 2x memory per buffer cache buffer (the class rounding). Gate (Fable): real creates
("missed") near 0 a second in steady play, device memory flat after 10 minutes. With
EDEN_NX2_BUFFER_CHURN_STATS=1 the "Buffer churn (5 s" line ends with the pool's counters.

### EXP106

EXP106 (live, 0 = off; Fable's delta draw port plan, fable_answer_backthread_threads.md 10,
step D2): every graphics pipeline keeps its last draw (GraphicsPipeline::KeptLastDraw: raw
handle words, computed handle values, views, samplers, view generations, image descriptor
entries per stage, rescaling words, ScaleEpoch), through a second instantiation of
ConfigureImpl. Records only, no draw changes; for the delta draw steps after D2.
EDEN_NX2_FASTDRAW_COUNT=1 keeps them too. Off, PrepareDraw reads this switch once a draw.

### EXP107

EXP107 (live, 0 = off; ported): inline
constant buffer data (Maxwell3D::CallConstBufferWords, ProcessCBMultiData) is compared with
the words memory holds, on the back, in order (Maxwell3D::CompareConstantData; one GPU page,
not sparse). Equal and safe: not written at all, so no invalidation, no buffer cache mark and
no delta-draw break (OoT rewrites ~258k blocks a second with the same words, often over
texture handles). Safe means the original conditions: no GPU-written data may be on the pages
(VideoCore::MayHoldGpuWrites, always true outside Switch 2 titles) and no guest read download
may land there; plus ours: not the texture header or sampler pools (also through an alias)
and no run-ahead used this session. Equal but not safe: written as before. With
EDEN_NX2_FASTDRAW_COUNT=1 equal writes are neutral for the count, and the 5 s line ends with
how many changed nothing and how many were not written.

### EXP108

EXP108 (live, 0 = off; ported):
the GPU's own cached writes (MemoryManager::FlushCaching: inline constant buffer data, its
accumulated ranges) mark their pages CPU-modified but leave them tracked
(MemoryTracker::MarkRegionAsWrittenTracked) instead of untracking them as a guest CPU write
does; the upload clears the bits and has nothing to track again. Saves a VirtualProtect pair
per page per write cycle (~4% of the GPU thread at OoT's Kokiri water, +1.4 fps on the
author's PC). Only on pages with no GPU-modified range and no GPU-modified bit (a tracked
page's GPU bits count); never in Nx2Perf verify. A guest CPU access to such a page takes the
slow path, as for any tracked page; a guest write there untracks it as before.

### EXP109

EXP109 (live, 0 = off; ported): a guest write
that untracks buffer cache pages (RasterizerVulkan::OnCPUWrite) changes their host protection
after dropping buffer_cache.mutex instead of under it (the back waited for those system calls
before draws). The word state changes under the lock as before; the page count decrements are
collected per thread (VideoCommon::DeferredPageCaching) and applied in order before OnCPUWrite
returns, so the guest's write still finds the page unprotected. Ours defers only decrements
(increments, e.g. an upload inside the write's slow path, apply at once): late decrements only
keep a page protected a little longer; a late increment could let a page the texture cache
tracks lose its protection in between. Never in Nx2Perf verify.

### EXP110

EXP110 (live, 0 = off, 1 = on at 512 MiB, N > 1 = on at N MiB; fable52_answer_stall3 3, the
ghost pools' "unproven first + takes"): the EXP42 pool's eviction rule. Over the cap, oldest
first, the first entry whose shape (exact create info) was never taken from the pool goes;
failing that the one whose shape has the fewest takes, then the least recent take, then the
oldest. Takes and the last take are kept per shape in a map that outlives entries, evictions
and the emptying on memory pressure (bumped on a take only, never aged; at 4,096 shapes the
3,072 most recently taken stay). Ages as before (180 frames, EXP82 for proven shapes, proof
looked up per shape when tested); EXP65 is not applied. The cap replaces EXP66's and is shared
with EXP99's idle backlog: the pool holds the cap less the backlog, the backlog is bounded at
128 MiB instead of 512 and, past 64 MiB, loses two images a frame end too. Memory pressure,
EXP42 off and a lowered cap still trim by the plain oldest-first loop. The "Image pool (EXP42)"
line ends with the rule, the backlog in MiB and the frame-end destroys. Ghost run (orbit,
182-212 s): 66-68% of lookups taken at 512 MiB against 60-64% for today's rule at 1,024 MiB.

### EXP111

EXP111 (live; 0 = Windows' default, 1 = above normal, 2 = high; vk_rasterizer.cpp TickFrame):
the process priority class, applied when the value changes. The owner gets more fps with the
window focused (Windows' foreground boost); this keeps the game's threads ahead of other
normal-priority programs when it isn't. Watch for a stream encoder starving at 2.

### EXP112

EXP112 (live, 0 = off; fable52_answer_fountain 4; BufferCache::DownloadMemoryForRead, with
EXP49 on): a guest read that must flush GPU-written buffer memory also downloads the
GPU-written run around its pages in the same round trip: the pages after it, then before it,
that hold GPU-modified bytes in a row, up to 256 KiB with the read's own pages, inside the
read's buffer, ending at a neighbouring Preflushable page (the read's own pages may be marked:
with preflushing on, the read marks them first). Exact: only GPU-modified bytes not already on
their way, the same read download, fence and landing. The run's read trap marks go at once,
so once it lands EXP86's mirrors answer the neighbouring pages' reads without the lock (a GPU
write there arms them again). At the great fairy fountain one game job read ~16 such pages a
frame, each a round trip (~0.4 ms). Check FRITCHII_READ_RUNS=1's "Read runs" line first: the
run histogram says whether the reads have neighbours to take.

### FRITCHII_READ_RUNS

FRITCHII_READ_RUNS=1 (boot, diagnostic, default off; read_run_stats.h): the guest reads of
GPU-written memory that go to the back or wait for a download (GPU::OnCPURead past its
preemptive answer). Every 600 frames these lines:

- "Read runs": each read's kind (flushed, waited for a read download on its way, waited for a
  fence download on its way), whether it is on the page after the previous one, the
  GPU-written run around its page, the wait, distinct pages a frame, and EXP112's run flushes.
- "Read overlap" (fable52_answer_kills 2b): whether the read's own bytes [address, address +
  size) overlap the GPU-written bytes inside its pages that it waits for, or only share the
  page(s) with them. Checked in BufferCache::GetFlushArea under the buffer cache's mutex it
  already holds, against: for a fence download wait (EXP72), pending_download_ranges up to the
  newest download the read needs; for a read download wait (EXP49 in_flight),
  read_download_ranges; for a flush, gpu_modified_ranges at the read (what the flush
  downloads). Per kind as overlap/page only/unknown (unknown: the buffer cache did not answer,
  e.g. image memory, or EXP60 handed the read off). For page only, the gap to the nearest
  GPU-written byte (<64 B, <256 B, <1 KiB, <4 KiB; "more" must stay 0). The read size
  (<=16 B, <=64, <=256, <=4096, more). The wait split by overlap/page only/unknown, in ms a
  frame and ms a read. "gpu_modified_ranges answers otherwise" counts waits where the download
  ranges and gpu_modified_ranges disagree (should be near 0).
- "Read writers", up to 8 "Read writer k/n" lines and "Read runs for B1"
  (fable52_answer_readback 3): which write made the bytes each read waits for. Every GPU write
  mark (BufferCache::MarkWrittenBuffer, on the thread that binds: the back, or the memory stage)
  goes into a ring of 16,384 marks with its range, host time, the dispatch count at the time
  and its writer: a dispatch's storage or texture buffer (the compute program's hash, the same
  hash as the write extent checks use, and the binding index), a draw's (stage and binding), or
  other (copies, queries, transform feedback; no bind named it). The binds set the writer in a
  thread-local around their marks. At a read whose pages hold GPU-written bytes, GetFlushArea
  (reading thread, cache mutex held, before anything lands) takes the newest mark over the
  read's bytes and copies up to 64 of the read's bytes from guest memory; OnCPURead, once the
  read is answered, compares them with guest memory again.
  - "Read writers": reads with a mark over their bytes, with one only on their page, with none
    in the ring, and not GPU-written in the buffer cache; the write's age at the read (from the
    mark to the read's ask) as <1 ms / 1-1.5 / >=1.5 per kind; reads whose bytes differed at
    landing out of those compared; marks a frame (the ring covers about 16,384 / that frames).
  - "Read writer k/n", the 8 writers with the most wait: reads by kind; the age (<0.5, <1, <2,
    <4 ms, more); the wait in ms a frame and a read; the read's offset in the marked range (the
    bound range, or a multi-range segment) as lowest to highest, distinct offsets, and the most
    common step from the writer's previous read; how many reads' bytes differed at landing; the
    dispatches bound between the write and the read.
  - "Read runs for B1": the GPU-written runs around the reads (GetFlushArea's run, up to 65
    pages) kept 16 frames after their last read and merged where they touch: how many and how
    big a frame, and the bytes marked GPU-written inside them a frame (all writers, and by
    dispatches). That is about what fence downloads would add if B1 marked those runs
    Preflushable (an upper bound: a range marked twice before a fence downloads once).

  Reading it for fable52_answer_readback 2: flushed reads whose writes are 1.5 ms or more old at
  the read are the ones B1 turns into landed reads (the bytes were written long before the read;
  only the missing mark sent them to the back); flushed reads under 1 ms only become fence
  waits with B1, and the fence download kind only gains from B2 (reaching the dispatch run
  sooner). A writer whose bytes never differ at landing is the one to read the code of. A steady
  step in the offsets means the read walks an array: mark ahead by a page instead of the run.
  Costs with it on: a lock and a ring slot per write mark ("marks a frame"), a scan of up
  to 16,384 marks and a 64-byte copy per read; off: one test of a constant at each site.

FRITCHII_READ_RUNS=2 (helper_cu_answer.md): all of the above, plus each write mark carries the
runtime's stamp (CurrentTick) and one "Read writer ticks" line per read kind every 600 frames:
the newest mark over the read's pages (the bytes its download takes) and its writer kind; where
that write's submission was when the read asked (not submitted, submitted and not run, run, from
BufferCacheRuntime::ReadWriterTicks: TickOf the stamp, the current tick, the refreshed known
tick); how many submissions the read's tick is after the writer's and how many of those had not
run; and every reader host wait (FinishReadDownloads for EXP49, WaitFenceDownload for EXP72) cut
at the writer's tick: the reader first waits for the writer's submission alone (only if it is
submitted; no flush is asked for), then the rest as before. Same timeline, so the wait's total is
unchanged; "then until the download was there" is what a copy ordered right after the writer
(async kind) or a faster landing (fence kind) could save. The EXP49 "on the host GPU" figure
keeps both parts. One semaphore refresh more per read. FRITCHII_READ_RUNS (and the two switches
below) are read at their first test, so nx2_switches.txt sets them (before, only a real
environment variable did: the static initializer ran before main() applied the file).

### FRITCHII_EARLY_COMPUTE

FRITCHII_EARLY_COMPUTE=1 (read at first use, dry count, default 0; early_compute.h,
helper_cu_answer.md Round 2): turns FRITCHII_READ_RUNS=2 on and asks, for every dispatch of a
program whose writes the guest reads back (learned from the reads' writers), whether it could run
at once on a second queue: its bound buffer ranges (uniform, storage with multi-range segments,
texture buffers; BufferCache::JudgeEarlyCompute before its own binds) have no unrun GPU write
mark over them (RAW, WAW) and the ranges it writes have no unrun use (WAR; every
Buffer::MarkUsage goes into a 65,536-entry use ring). Its write marks keep the verdict; each
read of those bytes adds its "until the writer ran" time to the saving of its writer's verdict.
Images are judged in ComputePipeline::Configure (the back or R): a sampled or storage image whose
last GPU write (TextureCache::MarkModification's scheduler tick) is not run. Lines every 600
frames: "Early compute" (verdicts, images, reads by verdict, predicted saving in ms a frame) and
up to 6 "Early compute feeder" lines. 2 (verify) and 3 (on) are designed, not built: they count
as 1. Off: a relaxed load at each bind and buffer use.

### FRITCHII_READ_SIDE_COPY

FRITCHII_READ_SIDE_COPY=1 (read at first use, dry count, default 0): turns FRITCHII_READ_RUNS=2
on; at each new read download (EXP49 RecordReadDownload) the newest write mark over all the
copied bytes gives that write's scheduler tick: eligible when it is already submitted (a copy on
a second queue could wait for it alone), else in the chunk the download closes (today's path is
as good). An eligible download's reader wait is cut at that tick; the part after it is the
saving. "Read side copy" line every 600 frames.

2 (verify) and 3 (on) need the value at boot: the device is then made with a second queue of the
graphics family (Device::WantsSideQueue; a family of one queue logs it and counts dry). An
eligible download's copy is recorded on that queue (vk_side_queue.h), after the master timeline
reached the writer's tick, with a host-read barrier, signalling its own timeline value; every main
submission with a tick after the writer's waits for that value until the side semaphore reports
it (MasterSemaphore::RequireSide: never one up to the writer's, which the copy itself waits for).
The download's main tick (its SubmitForHostRead) is such a submission, so a landing that waited
for it finds the side copy done too. Two rules keep the queues from waiting on each other (build
26 hung without the first, helper_cu_answer.md Round 4): the main ticks the side submissions wait
for never go down (a later copy whose writer is older waits for the previous copy's tick: the side
queue runs in order), and a side copy is made only once its tick was given to vkQueueSubmit, not
only to the worker (else the main queue copies it). "Read side queue" line: main submissions that
waited, waits for a side value never submitted (must be 0, dropped and logged), refusals, raised
waits, host waits over 2 s (must be 0; each logged every 2 s with both queues' ticks).
- 2: both copies, the main one into the download's staging and the side one into a second staging
  buffer. The reader waits for the side copy, then the main one; the landing compares the two
  byte for byte ("copies differ", must be 0, each one an error line) and lands the main bytes.
  The line gives when each was there.
- 3: the side copy alone. The reader waits for the side value only and lands that download at
  once, out of order (read downloads never overlap: RecordReadDownload copies only the gaps),
  when no other download covers the read's range; any other landing waits for the main tick as
  before. Off (0 or 1): nothing changes, one load per read download.

### FRITCHII_SUBMIT_TIMES

FRITCHII_SUBMIT_TIMES=1 (read at first use, diagnostic, default 0; vk_submit_times.h): turns
FRITCHII_READ_RUNS=2 on; a GPU timestamp at the top of every main submission's upload command
buffer and at the bottom of its command buffer (4,096 slots). After each read's whole wait, the
submissions from the one after the tick known done at the read to the writer's (at most 64, the
newest) are looked up: their GPU busy time, the gaps between one's end and the next one's start,
and overlaps. "Read writer run-up" line per read kind every 600 frames, next to "Read writer
ticks", with the same reads' "until the writer ran". Costs two timestamps and a query reset per
submission and up to 64 query reads per cut read.

The GPU-written ranges are what the cache marks: for a dispatch or draw that is the bound
writable buffer range (cut only by the native store bound, or by EDEN_NX2_WRITTEN_SSBO_MARK_KIB,
which must be unset for this), not only the bytes the shader stored. So "page only" is as exact
as the cache's own tracking (no written binding covers the read's bytes), while "overlap" means
a written binding covers them, not that the shader changed them. Reads after the first one in an area are answered from it (memory.cpp's per-core area)
and are not seen. Off: one test of a constant at each site.

### FRITCHII_SLOW_GPU

FRITCHII_SLOW_GPU (live, default 1; 0 = off, 2 = also the 10 s summary when nothing was slow;
helper EB, 2026-11-18, vk_submit_times.h SlowGpu) and FRITCHII_SLOW_GPU_MS (live, default 50,
0 = no lines). Why: an owner playtest had the guest wait 298 and 205 ms on two GPU read-backs
with the CPU idle, no pipeline builds and VRAM fine, so the GPU itself took 0.3 s, and nothing
said on what; and the rare GPU hangs/TDRs (about 1 in 200 launches) end in a device loss whose
NVIDIA fault info is empty.
- Every main-queue submission gets a GPU timestamp at its start (top of the upload command
  buffer) and end (bottom of the command buffer), in a 256-slot pool reset from the host
  (hostQueryReset; a reset command per submission without it). The Vulkan worker reads them back
  when it begins the next command buffers, batched (once 8 are pending or 50 ms passed), only
  for ticks the master semaphore already reported, so it never waits for the GPU. When the GPU
  is 256 stamped submissions behind, new ones go untimed (counted in the summary).
- What a submission holds is counted by the vk::CommandBuffer wrappers on the worker
  (vulkan_cmd_stats.h, a thread_local pointer the worker sets per chunk; null on every other
  thread): draws, dispatches, render passes, clears, fills, buffer copies and bytes, image
  uploads/downloads/copies, query ops, barriers, pipeline binds, the draws or dispatches per bound
  pipeline (8 per bind point; graphics and compute pipelines name their program hashes at the
  bind, ASTC decode and BC encode passes name themselves), and BlitScreen's presentation passes
  (window adapt, upscalers, anti-aliasing) bracketed apart.
- "Slow GPU submission: tick T ran G ms on the GPU (over N ms; queue wait ~Q ms, started X ms
  after the previous one ended / started X ms before the previous one ended / previous submission
  not timed, vkQueueSubmit U us); from THREAD at FILE:LINE[, a Finish (the thread waits for it)]
  [, signals a semaphore][, waits on a semaphore]; draws D (I indirect), dispatches ..., top
  graphics: vs HASH fs HASH xN, ...; top compute: cs HASH xN / astc xN / unnamed xN" (Info), at
  most 20 a minute.
  - G is end stamp minus start stamp: GPU time, including any wait on a semaphore inside the
    submission (", waits on a semaphore") and the tail of the previous submission it overlapped
    ("started before the previous one ended").
  - Queue wait is an estimate: GPU start minus the CPU time just before vkQueueSubmit, less the
    smallest such difference seen in the last 10 to 20 s (a submission the GPU took at once), so
    it is the time the submission sat behind earlier work (or the OS scheduler) before starting.
    Good to a fraction of a millisecond; without calibrated timestamps that is the best there is.
  - THREAD is the recording thread's name: GPUBack (E, or the whole back without one follower),
    GPURender (R), VulkanPresent, ...; FILE:LINE is the Flush or Finish call site.
  - Reading it: a big G with a small queue wait and a short gap is the GPU working on that
    submission: look at its draw count and its top programs (a single program with a huge count
    or a texture decode pass). A small G with a big queue wait means earlier submissions or
    another process held the GPU: look at the lines before it. "unnamed" programs are helper
    passes (blits, conversions, mesh emulation) that do not name themselves.
- "Slow GPU 10 s: S submissions timed, U not (the GPU 256 or more behind), K over
  FRITCHII_SLOW_GPU_MS=N ms (H lines held back, 20 a minute), longest M ms (tick T), submission
  times sum B ms/s" (Info) after a window with a slow submission, a held line or an untimed one
  (every window with FRITCHII_SLOW_GPU=2). B counts overlaps twice: a GPU busyness hint only.
- Breadcrumbs: the last 8 submissions' contents are kept. The hang report (FRITCHII_HANG_REPORT_S)
  logs them after its stall snapshot, and a device loss (Device::ReportLoss, after the fault info):
  "PREFIX: GPU breadcrumbs: the main queue's last 8 submissions, oldest first; the GPU reported
  tick R done; newest submitted S", then per submission "PREFIX: GPU breadcrumb tick T: done /
  NOT done, started on the GPU / NOT done, not started on the GPU / NOT done; submitted X ms ago
  (vkQueueSubmit U us) / not through vkQueueSubmit yet; from ...; <contents>" (Critical). The
  first "NOT done, started on the GPU" is the one the GPU was running when it stopped. Only
  submissions recorded while the switch is on are kept ("none" otherwise).
- Not covered: the present thread's swapchain copy (it waits on the acquire semaphore, so its GPU
  time would be the present engine's), the side copy queue (FRITCHII_READ_SIDE_COPY >= 2, copies
  only, off by default), VK_NV_device_diagnostic_checkpoints / VK_AMD_buffer_marker (not
  enabled: they need device extensions and per-checkpoint driver work, and the breadcrumbs' start
  stamp already says which submission was running).
- Cost on: per submission two timestamps on the GPU (well under a microsecond), on the worker
  about 1 us (a clock read, two ~0.8 KiB copies, a lock, a share of one query read and host
  reset per 8 submissions); per counted command a thread_local load and an add. At the market's
  300 to 1,100 submissions a second that is about 1 ms/s on the Vulkan worker, which is not the
  bottleneck. Off: one relaxed load per submission, one compare after vkQueueSubmit, and the
  wrappers' thread_local load (always there, the pointer is null). Check: profile_toggle on
  FRITCHII_SLOW_GPU, judged on the worker's busy time.

### EXP113

EXP113 (live; 0 = off, 1 = no poll, N > 1 = poll at most N us; PhysicalCore::Idle): OoT's
guest cores block at once when idle instead of polling for up to 100 us (EXP79's hardware wait
or, without MONITORX/WAITPKG as on the owner's PC, a SwitchToThread loop; ~795 ms/s over six
cores). With SMT those polls share physical cores with the GPU threads. Timing only. An
explicit EDEN_NX2_IDLE_SPIN_US still wins.

### EXP114

EXP114 (live; 0 = off, N > 0 = N us; gpu_thread.cpp): while channels wait on semaphores, the
GPU front polls them with a SwitchToThread between polls for 500 us after the last progress.
With N it waits N us with PAUSE instead, polling less often and leaving its SMT sibling free.
Measured elsewhere at the market: 2 us best (1, 5, 10, 20 less). Not under EDEN_NX2_GPU_WAKE,
whose wait is in hardware already. Timing only.

### EXP115

EXP115 (live; 0 = off, 1 = after 8 misses, N > 1 = after N; uniform_stream_reuse.h): a
(stage, constant buffer) slot whose streamed bytes differed from its copy that many binds in a
row copies guest memory straight into the stream buffer for its next 64 binds: no compares,
no extra copy into the reuse store. Exact (a stream copy of guest memory, as with reuse off);
it only gives up rebinds those 64 binds could have found.

### EXP116

EXP116 (live, read each frame; 0 = 4096 bytes as before, N = N bytes from 256 to 16384;
Vulkan only): the size up to which uniform buffers are streamed and reused instead of going
through the cached buffers. OoT's skinned models bind 7-13 KiB bone palettes about 8 times a
frame with the same bytes. Try 16384. The reuse store is sized for 16 KiB copies either way.

### EXP117

EXP117 (live, 0 = off; TextureCache::RescaleRenderTargets; only matters above 1X): a pass
with a square depth target and no colour target (a shadow map) is drawn unscaled. Scaling a
shadow map does not sharpen the picture and costs the square of the scale in memory (OoT's
12288x12288 sun map: 648 MiB at 1.5x); the per-object maps stay square under Shadow Size
Steps and EXP68, so they are left unscaled too. Switching it on scales such images down once.

### EXP118

EXP118 (delta path for draws that sample nothing; live, but only if it is on at boot: that
raises the breaks EDEN_NX2_FASTDRAW_COUNT counts with, which then stay on for the session; off
by default; src/video_core/fritchii_delta_path.h, fable52_answer_potroom3 sections 2 and 4).
A draw whose previous draw used the same pipeline, with only light state changed in between (no
break: the count's rules), whose pipeline has no texture, image, texture buffer or image buffer
descriptor, keeps the pipeline instead of looking it up and configures it without the
descriptor pools' sync, handle reads, sampler lookups and image views (nothing to bind).
Uniform, storage, vertex and index binds, the descriptor push, render targets (EXP31 still
decides whether they are prepared again), dynamic state, the host query, the draw record and
the reports run as for every draw. With the vertex input moved, the pipeline must take it
dynamically and the attribute types its programs read must be its key's; the cache's types are
then left as a lookup would leave them. Never with EXP32, EXP87 or EDEN_NX2_TEX_BANK_GEN on.
Values: 1 = on; 2 = verify only (decide, draw the full way, the full lookup must give the pipeline
the delta draw would have kept); 3 = on, and every delta draw also looks the pipeline up (another
pipeline: drawn the full way) and synchronizes the descriptor pools after its configure (nothing
may change). "Delta path (EXP118=n) per frame over 600 frames" logs draws decided, delta draws
(or would-be ones), those with the vertex input moved, the full draws by reason, and with 2 or 3
the lookups compared and the differences (must be 0; the first 16 are logged with the shaders).
FRITCHII_DRAW_KINDS also counts, per class, the draws whose pipeline samples nothing and, with
EDEN_NX2_FASTDRAW_COUNT=1, the would-be delta draws. Measure with breaks on at boot on both
sides (EXP118=2 at boot, then toggle 0/1); the breaks' own cost needs a fresh-launch A/B.

### EXP119

EXP119 (report runs: the report side of the occlusion tests; live, off by default;
src/video_core/fritchii_report_runs.h, fable52_answer_potroom3 sections 2 and 4, step 2). A
sample count report (type 21) that is not a fence, and the amend reset of a sample counter
reset, no longer queue a std::function each in the fence manager's list: they become entries
of a run, one operation that writes them in one loop when the fence's results are in. An entry
joins a run only while the run's operation is still the last one queued and no fence took the
list since (the fence manager's operation stamp, checked under its list lock), so each entry
runs exactly where its own operation would have: same fence, same neighbours, same order
against the counter's other operations (presync amend, replicated query, slot-limit resets).
The report is still listed in the query cache's map from the moment it is made until its fence
ran (the split's front, guest reads and render enable rely on it), with one hash fewer; it is
unlisted and freed as before. Render enable: the Conditional mode skips its map lookup (the
runtime only uses the address), IfEqual / IfNotEqual do both lookups under one lock. Today's
path for fences, other report types and traced reports (EDEN_NX2_QUERY_TRACE,
EDEN_NX2_SEM_WATCH). Values: 1 = on; 2 = verify: today's operations still write (queued keeping
the run's stamp, so the runs take the shape they have with 1), the runs only compute outcome,
address and value, and each report's own operation compares (the run must have run before it,
with the same result); render enable does both forms and compares the lookups. Mismatches must
be 0 (a guest write to a report between its run and its operation would count one; the first
16 are logged). "Report runs (EXP119=n) per frame over 600 frames" logs reports, those in runs,
runs started and ran, counter resets (in runs), today's path by reason, render enable calls
(without lookup, with one lock) and with 2 the compares and mismatches. Toggle live 0/1 with
profile_toggle.py; zones "query report", "counter reset", "render enable" (GPU profile 2).

### EXP120

EXP120 (live, 0 = off, DEFAULT 2 since 2026-11-13: market 1.89 M reads skipped, 0 differed, 1%
low 37.7 -> 39.2; DmaPusher's NoteEngineWriter, fable_answer_long_drains 8.4): the
KeplerMemory and Fermi2D methods the GPU thread split sends are noted for the macro DMA read
snapshots (EDEN_NX2_DMA_SNAPSHOT) by what the back really writes, instead of every one as an
unranged writer, which ends every snapshot ("unranged" misses in the "DMA snapshot" line). With
1: a method that only stores a register (every KeplerMemory method but data, every Fermi2D
method but the blit launch, pixels_from_memory.src_y0 + 1) and a single data word that is not
its method's last (the upload is written with the last one) are not noted; the data of a linear
upload notes the bytes it may write (from the upload registers as sent: line_length_in,
line_count, address, pitch, and the exec's copy size). Blits (with 1), block linear uploads
and anything else stay unranged. With 2, also a blit launch notes its whole destination surface
(the registers as sent: address, layout, pitch or block linear size from width, height, depth
and block sizes, the format's bytes per pixel; as SoftwareBlitEngine sizes it, whose covered
rows stay inside it; the texture cache's copy writes an image, not guest memory). A range is
noted as the device ranges its mapped pages make (unmapped pages are written by nothing), when
it is up to 1 GiB and makes at most 16 of them; otherwise it stays unranged. "DMA snapshot
unranged notes (EXP120=n; single/run)" logs, every 5 s with the "DMA snapshot" line, each kind
of method by call (single method / non-incrementing run): noted unranged, ranged, not noted
(with 2, why blits stayed unranged: a destination register not sent yet, format, empty, over
1 GiB, unmapped, over 16 device ranges);
and the reads EXP120 let skip the drain (an unranged note it spared since the page's snapshot)
with their own "differed" count, which EDEN_NX2_DMA_SNAPSHOT=10 checks and must stay 0. With 0
the counts show who notes the unranged writers. Toggle live 0/1 (or 0/2) with profile_toggle.py.

### EXP122

EXP122 (live, 0 = off, DEFAULT 1 since 2026-11-13: 0 mismatches in 4.39 M verified answers;
GpuSplit's page watch, fable_answer_long_drains 8.2 (a'),
fable52_answer_kills 3.4; needs EXP10 on): the device pages the macro DMA read snapshots
(EDEN_NX2_DMA_SNAPSHOT) are served from are watched. Each compute or copy range the fronts note
(NoteWriterRange, NoteReleaseWriter) is tested against those pages when it is noted, and the
bytes it covers in a watched page are kept with its position (8 a page, the rest folded into one
union). A read of a watched page then answers "may a writer since the snapshot write these
bytes" from that list, with no scan of the writer range ring and no scan limit: before, every
"past the scan" drain of the session was the scan giving up (4,096 ranges at ~11,500 a second),
never an overlap. Pages that are not watched (2,048 at most, 256 sets of 8, least recently used
replaced; the first table held 256 and kept evicting the rarely read pages, exactly the ones
that need it, fable52_answer_exp122), or
whose watch started after the snapshot asked about (another channel's snapshot), take the ring
scan as before; the ring's other readers are unchanged. Unbounded writers still count for every
page. 1 = on. 2 = on, and every "no write" answer is checked by the old scan without its limit;
an overlap it finds that the watch did not report is a verify mismatch (the first 16 logged as
"Writer page watch (EXP122=2)"), which must stay 0. EXP10's "DMA snapshot EXP10" line logs reads
answered by the page watch (and how many said "may write"), watched pages, byte-list overflows,
verified answers and verify mismatches, then the checks left to the ring (page not watched,
watched only after the check's snapshot, over two pages) and the pages replaced; with the
watch working, "page not watched", "pages replaced" and "past the scan" stay near flat. Off by default, not measured yet: run 2 first (0
mismatches in a kill fight and at the pot room), then toggle 0/1 with profile_toggle.py and
compare the "past the scan" count and the macro dma read drains.

### EXP121

EXP121 (live, 0 = off, DEFAULT 1 since 2026-11-13: verified 0 mismatches, fight +1.7 fps twice;
fable52_answer_kills 3, fix 1): the texture cache's collector, once device
memory is at its expected threshold, downloads each GPU-written image it evicts and waits for
the GPU every time (2 to 3 ms, then ten more short waits, in fight frames). With 1 it records
the download into a staging buffer without waiting and deletes the image at a later frame end,
once the GPU is past the copy: guest memory written, untracked, unregistered, deleted, as the
waiting path does. Until then the image stays registered (a guest read takes the usual read
path) and counts as freed for the collector. The queued download is dropped and the image kept
if it is used again (its LRU tick moved), written again (by the GPU, the CPU, or a guest read
download), deleted by another path, or if newer GPU data reaches its guest memory (another
image's or a DMA buffer download, or an overlapping image written since). Still waiting: an
allocation that failed (the memory is needed now), sparse images, and past 64 queued downloads
or 256 MiB of staging (EXP57 still limits those). With 2, each landing is also compared with a
fresh waited download (mismatches must be 0; the first are logged with the image). "Texture
cache last 600 frames: collector downloads" logs, with EXP121 on or EDEN_NX2_TC_LOG, the waits
left, queued, landed (and frames from record to landing), dropped by reason, fallbacks and
mismatches. Toggle live 0/1 (or 0/2) with profile_toggle.py in a scene at the memory threshold
(8 GB cards, long fights).

### EXP123

EXP123 (SEAM, live, 0 = off; the memory stage's step 2a, fable_memstage_step2_spec.md 6.1):
every Vulkan command the buffer cache runtime records (uploads, copies and their barriers,
clears, the quad index tables, the null buffer's fill, index, vertex and transform feedback
binds, the uint8, quad and vertex repack compute passes, and its uniform, storage and texel
buffer descriptors) is pushed as an entry; MemPlayer::Exec (vk_mem_stage.cpp) is the only code
that turns an entry into scheduler and descriptor queue calls. With 0 each entry runs the
moment it is pushed: today's calls at today's moments, in today's order. With 1 the entries of
a fill bracket wait in the arena and are played where the call site plays them: after the
geometry binds (UpdateGraphicsBuffers, BindHostGeometryBuffers), after each stage's
BindHostStageBuffers, after BindHostComputeBuffers, after each call that returns a buffer
(ObtainBuffer for a GPU count, an indirect dispatch, kept list words, a DMA image copy;
GetDrawIndirectBuffer and Count; the query cache's ObtainCPUBuffer) and at the end of each
single operation (downloads, write and invalidation ranges, runahead chunks, unmaps, DMA
copies and clears, inline uploads, list page copies and flushes, the frame tick, the fence
commit). The picture must stay identical. Read once a frame; a change makes the player forget
the remembered index and vertex buffers (a few rebinds). The rebind checks against the
recording epoch and the list draw facility (list draw arguments, kept list words) are the
player's; finishes, waits, submits for host reads, the multi-range bind and the kept words wait
go through SeamClock, which plays what is pending first.

Verify, always counted: "around the packet" (5.1, something recorded or added a descriptor
inside a fill without going through the player or a sync: a runtime path that was not
converted) and "late plays" (5.2, entries still pending when something else recorded: a call
site misses its play). Both must stay 0; the first 16 are logged as "Memory seam: ..." with the
use and its source location. "Scheduler uses not through the clock" counts flushes, finishes
and state resets inside a fill that did not go through SeamClock. With EXP123 = 1 (or
FRITCHII_MEMSTAGE_SEAM_STATS=1) "Memory seam (EXP123=n)" logs every 10 s: draws, dispatches and
fence commits a second with entries each, deferred entries and plays a second, the counters
above, entries a second by kind (which kinds OoT uses at all: convert, multi-range, texel
buffers), fills by site with entries each, syncs by kind, and entries run outside any fill by
kind. Compare 0 and 1 with profile_toggle.py ("back us a draw", heavy-phase market, 4 cycles).

EXP123 = 2 (sub-step 2b, spec 4 and 6.2): as 1, and each draw and dispatch is filled once,
before the texture work. Needs FRITCHII_MEMSTAGE_TICKS=1 or 2 (boot): without packet ticks 2 and
3 run as 1, logged once ("SEAM 2/3 need FRITCHII_MEMSTAGE_TICKS=1 (or 2); running SEAM 1") and
counted in frames on the "Memory seam" line. The draw's fill (MemStage::FillDraw, at the rasterizer's draw site after
the vertex repack layout) runs SetUniformBuffersState, every prepared stage's storage binds, the
texture buffer unbinds, UpdateGraphicsBuffers, BindHostGeometryBuffers and every stage's
BindHostStageBuffers; the dispatch's (MemStage::FillDispatch, before ComputePipeline::Configure)
the uniform state, the storage binds and shader hash, the texture buffer unbinds,
UpdateComputeBuffers and BindHostComputeBuffers. Configure no longer calls the buffer cache: it
plays the packet's sections where the calls were (the geometry after the texture buffer binds,
each stage after Acquire and before its image descriptors, the dispatch's section before its
image descriptors). A sync inside the fill (the multi-range bind, the kept words wait) plays only
what it needs (copies, uploads, barriers, conversions); the index, vertex and xfb binds and the
descriptors wait for the packet's plays. When Configure ends, true, false or by an exception (a
scope guard at the call site, MemStage::RestAtEnd), whatever is left is played (the cache
believes those uploads are on their way); descriptors left then are dropped with the draw.
Descriptor entries take the pipeline's form (descriptor buffer or not), as the fill runs before
Acquire. Pipelines with texture or image buffers keep their fill in Configure as with 1
("late fill": the binds need the texture cache's views). The indirect dispatch's ObtainBuffer, the
extent probes, list page copies and draw indirect buffers stay where they are (fill and play at
the call, as with 1). There is no tick guard: packet ticks keep a held packet's numbers open.
"Flushed while held" (5.5) counts held packets whose fill and PlayRest are in different command
buffers (a flush or finish in between, syncs included; the first 16 logged with the caller):
expected near 0, as a vertex binding the fill did not see dirty is not bound again in the new
command buffer. Texture writes while held: the texture cache writes game memory inside Configure
(GC downloads, EXP121's landed downloads, a flushed download), after the early fill read. Each
held fill keeps the device ranges it read (uniform copies, uploads, bound ranges, storage and
table addresses); every such write is counted, and a write over one of those ranges is counted
apart and the first 16 logged as "Memory seam: texture write over fill read". That count must
read 0 for 2 to be exact. The stats line gets a "memory seam 2b" line: held and
late fill draws and dispatches a second, packets not held (arena busy on another thread), flushed
while held, texture writes while held (over fill reads, must be 0), rest plays (and descriptors dropped), descriptor form mismatches (must be 0), and fill
and play time with EDEN_NX2_GPU_PROFILE. The GPU profile gets two zones, "memory fill" (every fill
at 2, the buffer cache's own work included: what leaves the back in step 6) and "memory play".
FRITCHII_MEMSTAGE_READ_LOG adds the read log (5.3). Compare 1 and 2 with profile_toggle.py
("back us a draw", heavy-phase market, 4 cycles).

EXP123 = 3 (sub-step 2c, spec 6.3 and 9.1-9.3; as 2: the recorder, the player and the early
fill): what the back reads from game memory that the stream writes comes in the packet by value. Handle values: the
raw handle of every image element a draw or dispatch binds, in the order the handle loops read
them (per stage: texture buffers, image buffers, textures with handle tables expanded, images;
one value per element, EXP21's buffer address words keep their place unread), made by one
function for both sides (texture_handle_values.h: RawTextureHandle, FillStageHandleValues), so
handle programs' constant buffer words, storage buffer addresses and storage words are read by
the fill. The fill always runs: draws whose handle loop the back skips (TEX_BANK_GEN hits) now
pay for those reads and are counted. The handle values are not part of 2b's early fill yet: the
pipeline's Configure fills them at the top of its handle work ("filled in Configure"). The QMD: the
native QMD scheduler's 256-byte read goes through the dispatch packet (MemStage::Qmd), which
opens there; the error path's 512-byte read stays direct. Null pointer slots: the compute
pipeline lookup's pointer read (StaleUnboundPointerSlot) goes through the open dispatch packet too
(MemStage::ReadPointerSlot). With 3 the stats line adds "memory seam by value": draws and
dispatches with values a second (values each, how many filled in Configure, how many the back
skipped and their values), QMDs a second (and reads that ran no dispatch), null pointer slots,
and the verify counters below. Values below 3 change nothing here.

### Compute counters (fable_memstage_compute_brief 3)

Counting only, no switch; for one market run with FRITCHII_MEMSTAGE_THREAD=2, AHEAD unlimited
and EDEN_NX2_GPU_PROFILE=2. Nothing is counted without the GPU profile.
- "Memory stage runs by channel n s" (THREAD 2 with EDEN_NX2_GPU_PROFILE, every 10 s after the
  "Memory stage ring" line):
  GPUMem's level runs by the channel the stage had bound at the run's first entry (0 to 6, then
  "other"): runs a second, entries and compute launches each (0xaf with bit 1, 0xb0 above 1,
  0x107, as the compute front notes dispatch writers), and GPUMem's run time in ms/s (from the
  lend to the return). Then runs a second and ms/s by entries a run (1, 2-7, 8-63, 64-511, 512+)
  and by compute launches a run (0, 1, 2-7, ...): the ms/s in compute lists against small runs.
- "Memory stage run waits n s" (THREAD 2 with EDEN_NX2_GPU_PROFILE, right after it;
  work\fable_stage_levelpoints.md 14.9): GPUMem's wait for the back at a run's end, split where
  the back reached the run's first entry (BackEntry's mark): the part before is the back's
  backlog (ring B and the entries before the run), the part after the run itself, ms/s and us a
  run (a back that was there before the wait began: all of it is the run; "not split" counts
  runs whose mark was another run's, should be about 0). Then GPUMem's lend to return by the
  run's first hard kind (the 5c line's key, top 12): runs a second, back/front/following ms/s,
  as the passes line's buckets by compute launches. Nothing is counted without the profile.
- On the link line (always): GPUMem's sleeps in its waits by reason, each with how many a wake
  call ended (one that found GPUMem asleep: the back's take, its position, the front's publish)
  and the time from that call to GPUMem running, in us (the wake latency, 14.4 row 4's size).
- Fine zones (GPU profile 2): "inline upload" (AccelerateInlineToMemoryImpl, inside "method"),
  "dispatch fill" (MemStage::FillDispatchHere, on whichever thread fills) and "qmd signal" (the
  QMD scheduler's signal with its QMD reads and releases; the dispatches it runs stay
  "compute"). Zones are exclusive, so "method" and "compute" lose what they cover.
- Fine counters (GPU profile 2, each thread's "fine counters per second" line). The front
  thread: compute upload data words by path, "upload words (macro emit)", "(method)"
  (FrontCallMethod) and "(multi-method)" (FrontCallMultiMethod). The back: inline uploads by
  size ("inline uploads 4 B", "5-64 B", "65-256 B", "over 256 B") and by branch ("to never
  GPU-written pages", EXP85's branch; "uploaded in place"; "not uploaded"; "unmapped");
  dispatches by what their fill marks ("marking a buffer GPU-written" with "dispatch marked
  bytes", "writing images only", "marking nothing": the buffer cache's GPU-written marks made
  while DispatchCompute holds its lock), "indirect dispatches", "dispatches with texel buffers"
  (late fill), "dispatches skipped" (DispatchCompute ended before the record: unconfigured, grid
  over the device's limit, an exception), "dispatches replaced" (the upscaler), "dispatches
  copying list pages", "dispatches with extent probes"; and served calls made inside a dispatch
  by cause ("served in a dispatch: FlushRegion", ... in the "Memory stage ring" line's order).

### EXP124

EXP124 (upload words as one entry; live, 0 = off, DEFAULT 1 since 2026-11-13: market 56.2 -> 58.4
fps, 1% low 38.8 -> 40.0; fable_memstage_compute_brief 4).
Today the compute macro's emit sends every inline upload data word (KeplerCompute 0x6d) as its
own Direct MacroWrite: about 457,000 ring entries a second at the Castle Town market, each
followed by GPUMem as a hard entry, and on the back each a last call that Upload::State flushes
alone (AccelerateInlineToMemory with 4 bytes). With 1, the words a front macro call emits
collect and go as one Multi entry marked Direct (GpuSplit::MacroWords; pieces of 8,192 words;
never for the 3D engine, whose Multi entries the memory stage applies without the Direct bit),
sent before any other write the macro emits and at its end (or when the macro call ends in an
exception, so none waits for the next call); after the entry, in this order, the
upload's writer note (FrontNoteWriter: its whole range, as each word noted it) and with EXP13=2
the copy's batch at the entry's position (FrontPatchOverlays). FrontApply still runs at each
word (a later register read of the macro sees it). FrontCallMethod's and FrontCallMultiMethod's
words are unchanged. The memory stage follows the entry as one hard entry (an executable method
of another engine), as it followed each word. On the back the entry calls
KeplerCompute::CallMacroWords: every word goes through CallMethod as before (registers end the
same), but only each entry's last word is a last call (a batch over 8,192 words is several
entries, each ending with one), and where the flush writes in place (native
partial transfers, linear, one line) it goes to AccelerateInlineWords, which writes the words as
their own calls would have: word by word while a grid placeholder is live (the scan sees x, y
and z in one write, x alone in a word's) or when the destination is off a 4-byte boundary (the
list note clears only words a write covers whole); otherwise split at 4 KiB pages of the GPU
address, a piece in one call where InlineSkipsCaches (EXP85 & 1, deferral on, no GPU-written
mark on its page) holds and it is mapped and not on sparse memory, word by word otherwise. Words
an earlier non-last call left unflushed are flushed with the first word's call, as before;
block linear, multi-line and non-native uploads keep a last call per word.
Why one call per piece is the same as its words' calls: the write lands the same bytes (guest
memory between two words is touched by nothing that reads the destination on a never
GPU-written page); the list note bumps a page's write count once instead of once a word, and
every user of that count compares it for equality only; the query filter is per page and the
invalidation drops queries by overlap; the invalidation accumulator is fed word by word (it
keeps a range per call where one does not extend the last, so a single range could differ).
Values: 1 = on; 2 = on, and before each one-call piece every word is asked as its own call
would ask (its translation and the page test); a piece whose words disagree goes word by word.
GPU profile 2 fine counters: front "upload word batches (EXP124)" (one per batch sent; a ring
entry per 8,192 words of it) and "upload batch words"; back
"upload pieces in one call", "upload pieces word by word (page may be GPU-written)", "(unmapped
or sparse)", "upload flushes word by word (grid placeholder live)", "(not word-aligned)",
"upload pieces checked (EXP124=2)" and "upload pieces whose words disagree (must be 0)" (the
first 16 also logged as "Upload words (EXP124=2)"). Toggle live 0/1 with profile_toggle.py (a
macro call reads it once). Fields that should move at the market: the ring's level entries
(about 500,000 to 50,000 a second), entries each run, the shadow's KeplerCompute 0x6d rows
(about 9,000 a second, as multi entries), GPUMem's stage entries, the back's "method" and new
"inline upload" counts, the front's "macro" zone. Must stay 0 with THREAD 0 and THREAD 2 (AHEAD
unlimited), at the market and the fight: the memstage gate, "DMA snapshot ... 0 differed", the
QMD release prediction's payload differences and unpredicted counts, the compute front verify,
FRITCHII_MEMSTAGE_VERIFY=2's QMD mismatches, the flash-watch route.

### EXP126

EXP126 (live, 0 = off, DEFAULT 1 since 2026-11-14: trial 0 differed, market 57.7 -> 58.7 fps;
fable_qmd_wait_brief piece 2): the ranged ring's overflow
(below) out of GpuSplit::WriteStamp. With 0 a dropped ranged write counts as a write to every
address for the write stamps, so with the back more than about 5,000 ring words behind (the
Castle Town market) every QMD read the front's copy cannot serve waits for the whole backlog
("qmd read", about 95 waits and 125 to 150 ms a second), and small patches cannot become
overlays. With 1 it does not: RangeLastWrite stamps each ranged write's bytes before it touches
the ring, so a dropped write is still found over its own bytes, and stamps only err newer (a
collision makes one newer, never older). Unranged 3D writes still count for every address;
QueuedGuestWriters and the macro DMA reads (MemoryWriterTarget) scan the ring and keep the
overflow as before. An overlay from memory made while the overflow is queued is kept out of the
EXP13 feed (its unbounded-writer check sees the overflow; EXP127 not 0 changes that, see
EXP127). Aliases: with EXP126 not 0 every
ranged write is stamped by device address as well as by GPU address (the page translated once
and cached, as NoteWrite does), so a QMD or semaphore read of the same memory through another
GPU address finds it in WriteStamp (the QMD sites, PatchHolders and the semaphore feed check all
ask it with the device address). With 0 they are stamped by GPU address only, as before (the
overflow floor covered the dropped ones; the newest 256 had that hole already). While a queued
function may remap the range, or for sparse pages, the write counts for every address instead
(the stamp floor) until the back ran it. When EXP126 turns on, the ranged writes sent before were
not stamped by device address: with 1 they count for every address until the back ran them.
The ring scans (QueuedGuestWriters, ExactWriterCheck) still compare GPU addresses only; they
keep the overflow, so their answers are as before.
With 2 WriteStamp keeps the overflow as with 0, but the device stamps are on as with 1, so its
answers are the same as 0's or newer (never served earlier than 0; a block collision can add a
wait). The table below is cleared whenever EXP126 changes, so a switch into 2 starts from 1's copy
and this one being the same. The QMD copy follows what 1's copy would hold where it
differs, in a trial table (never served): the overlay 1 makes from memory at a patch nothing but
the overflow refused, and an overlay or upload shadow block that takes a patch at 1 only (a
holder still_holds refused for the overflow alone), each patched further as 1's PatchHolders
would. At every read it works out what 1 would serve (an overlay, a shadow block, guest memory
at once, or a wait, with 1's target and those holders) and, where that is something else than
this copy serves or the same without the wait, compares the 256 bytes with what this copy
reads (after its wait, if it waits). "differed" on the line below must be 0 (the first 16 are
logged as errors with the word and the source). A change 1 would make that the table does not
follow (a holder 1 drops as newer than a patch, an overlay this copy makes where 1 keeps its
own) marks the block: its reads are counted as not compared until the back passes it (every read
of a marked block, also those 1 would have waited for, so the count overstates); entries
dropped at the table's limit (4,096 blocks, only after those the back passed) count as lost.
Not followed at all: the overlay table's capacity at 1 (its evictions and evicted floor). To
judge 1: profile_toggle.py on EXP126,
0 against 1, market heavy phase uncapped with EDEN_NX2_GPU_PROFILE, and watch "qmd read" in the
front waits by site, the EXP13 lines' "after a wait" and "ranged overflow" waits, overlays from
memory, the fence writers wait and fps; with 1 the EXP13 checks (payload differs, unpredicted,
resyncs, auto-off, bound violated) and "DMA snapshot ... differed" must stay 0.

### QMD wait floor line

"QMD wait floor (EXP126=n)" (HW_GPU, every 5 s with EDEN_NX2_GPU_PROFILE or EXP126 on;
fable_qmd_wait_brief piece 1,
counting only): why the front's QMD scheduler copy (EXP13 = 2 to 4) waits at "qmd read". The
front keeps at most 256 ranged 3D writes (constant buffer words, reports; EDEN_NX2_RANGED_MAX);
at the next one the oldest is dropped and its position, `ranged_overflow_position`, counts for
every address in the ring scans. It used to raise the "3D method write" floor; it is a term of
its own now (the EXP13=2 wait cause line says "ranged overflow" for it), with the same effect.
The line gives:
- ranged ring overflows a second; QMD reads a second, how many with the overflow still queued,
  and the ring words from it to the write position then (average, most);
- small QMD patches a second by outcome: a holder (overlay or shadow) took it, an overlay from
  memory made (and of them, made with the overflow queued: 0 unless EXP126 = 1), refused as
  unmapped or GPU-written, an overlay queued, an evicted inline QMD, or by the stamps, by the
  term that refused it (block stamp, page stamp, stamps on since/over 1 MiB, unknown write,
  unbounded writer, unranged writer, queued function, 3D method write, ranged overflow), and how
  many of those nothing but the overflow refused;
- QMD read waits a second by the term that set the target (the same terms, and evicted inline
  QMD) with the ring words each waited, and how many nothing but the overflow made wait;
- the stamp terms, the "overflow alone" counts and the exact scan below are worked out only with
  the line on (EDEN_NX2_GPU_PROFILE or EXP126 not 0; WriteStampWhy and the scan cost the front
  otherwise); without it refusals count under "by the stamp" with no terms;
- for refusals and waits set by a block or page stamp: whether the exact scan over the same bytes
  (GpuSplit::QueuedGuestWriters) finds the writer at the stamp, only an older or unbounded one, or
  nothing (a hash collision, or a page stamp of a write elsewhere in the page), as counts for
  the window: patches a/b/c, reads a/b/c;
- macro DMA read waits a second (WaitForMemoryWriters with a range) and those whose target is the
  ranged overflow, with the ring words each;
- EXP126 not 0: ranged writes stamped by device address a second, and those counted for every
  address instead (a queued function, sparse pages, no memory manager given);
- EXP126 = 2: reads compared, by what 1 serves (guest memory, an overlay, an upload shadow
  block), how many differed, trial overlays and blocks built, reads not compared and trial
  entries lost (counts for the window).

### EXP127

EXP127 (live, 0 = off, default 0; fable_fence_writers_brief piece 2): QMD sources the EXP13=3/4
feed refuses today although the copy read them as safely as one it takes. 1 = audit, 2 = on.
With 0 nothing changes. With 1 and 2 the copy tags the records with the sources below and the
"QMD feed sources" line (next section) is written; with 1 the feed takes nothing new and counts
what 2 would have met; with 2 it takes them (EDEN_NX2_FEED_MEMORY_OVERLAY on, as for overlays
from memory). The value is read at each feed decision, so a record tagged while 1 or 2 was on
is refused again at 0.
- Overlays from memory built while the ranged overflow was queued (2a; needs EXP126 = 1). The
  belt that keeps an overlay from memory out of the feed when an unbounded writer is queued over
  the QMD (GpuSplit::QueuedGuestWriters, EDEN_NX2_FEED_MEMORY_OVERLAY) counted the ranged ring's
  overflow as one, so with the back far behind (the market) every such overlay was "not checked"
  (220 to 233 a second). With EXP127 not 0 and EXP126 = 1 the belt counts the overflow only for
  the dropped writes no device stamp covers (those sent before the device stamps started; its
  other unbounded writers count as before), and an overlay that only the overflow kept out gets
  the source "overlay from memory (the stamped overflow queued)"
  (QmdPrediction::Source::OverlayFromMemoryOverflow). Why it is exact: the belt is for writers
  the write stamps cannot see, and the overflow is not one. RangeLastWrite stamps a ranged write
  by GPU address and, with EXP126 not 0, by device address before it enters the ring, and the
  overlay is built only when WriteStamp over the QMD (both addresses, with EXP126 = 1 the writes
  sent before the device stamps as a floor) is not newer than the back's position. So no dropped
  write still queued writes the QMD's bytes; EXP126's trial compared about 560 reads a window, 0
  differed. With EXP126 at 0 or 2 the belt is as before.
- Plain memory reads (2b): a QMD read from guest memory with nothing queued over it
  (Source::Memory, 174 to 202 refusals a second at the market with EXP126 = 1) passes the guards
  an overlay from memory passes: mapped, not GPU-written, no inline QMD of it queued or evicted
  (a queued overlay of it would serve the read or set its target past the back), no upload
  shadow block of it queued (it would serve the read), no write stamp over it newer than the
  back. With EXP127 not 0 (EXP13 3 or 4, EDEN_NX2_FEED_MEMORY_OVERLAY on) and EXP126 not 0 (at 0
  ranged writes are stamped and scanned by GPU address only, so a write through another GPU
  address of the same memory would be missed) the read asks the GPU-written check again (the
  first one ran before the back's position was loaded: a GPU writer the back ran in between
  would be at or below it, past the stamps) and the overlays' belt (QueuedGuestWriters: no
  unbounded writer queued; the overflow as above with EXP126 = 1); with both clean its records
  get the source "memory, checked"
  (QmdPrediction::Source::MemoryChecked). Exact on the same ground as the overlays from memory:
  the copy reads the QMD at the same signal the back reads it at, so the back reads the same
  bytes unless something outside the stream (the guest CPU, a read-back landing) writes the QMD
  in between, and the overlays have that exposure too (their bytes come from memory at the
  patch, earlier still). The back checks every release against its record, and the feed turns
  itself off at the first one that differs.
To judge: one market and one fight window at 1 (the "QMD feed sources" line: the new sources'
back checks must show 0 payload differs and 0 unpredicted, and their would-be decisions 0
wrongly met), then profile_toggle.py on EXP127 1 against 2 at the market, heavy phase,
uncapped, GPU_PROFILE on. At 2 these must stay 0 / on: the EXP13 feed line's "feed on", "0
violated", "0 wrongly met", "0 not fed (check table full)"; the EXP13 line's "back QMD: ... 0
payload differs, 0 unpredicted" and "0 resyncs/s". Should move at 2: the feed line's "not fed
for their QMD source" (from memory, overlays from memory; with 1 these count the new sources
too, as at 0) and "built/s with an unbounded writer queued" to
near 0, "met by a queued plain release" up by 80 to 100 a second, the fence writer line's
readable acquires unmet down by 105 to 120 a second, the EXP13 line's "would be met by a
predicted queued release" to near 0.

### QMD feed sources line

"QMD feed sources (EXP127=n, EXP126=m)" (HW_GPU, every 5 s with EXP127 not 0 and EXP13 2 to 4):
- the back's check of the copy's QMD records by the source their QMD was read from, per second
  as matched/payload differs/unpredicted (an unpredicted release has no record: it counts under
  the source of the copy's last QMD read), for every source: inline QMD, patched inline QMD,
  overlay from memory, upload shadow, memory, memory after a wait, overlay from memory (not
  checked for the feed), overlay from memory (the stamped overflow queued), memory, checked;
  releases and reductions together;
- overlays from memory built a second with the stamped overflow as the only unbounded writer
  queued (with EXP127 not 0 these no longer count in the feed line's "built/s with an unbounded
  writer queued");
- QMD reads a second from memory with nothing queued over them (the EXP13 line's "from memory")
  whose belt was clean ("memory, checked"), those with an unbounded writer queued, and those
  whose page was GPU-written by the time the back's position was loaded (EXP13 3 or 4 with
  EDEN_NX2_FEED_MEMORY_OVERLAY on and EXP126 not 0; 0 otherwise);
- EXP127 = 1: acquires a second (resume polls included) the feed would have met through the new
  sources, by source, as queued plain/run but unlanded/reduction bound (the plain and narrow
  gates as at 2; the bound's check table is not asked), and those decisions checked once their
  record landed as the feed's own audit checks its decisions: right, wrongly met (must be 0; the
  first 8 are logged as errors "QMD feed sources (EXP127=1) audit"), unknown (left the table, or
  not landed in 2 s), dropped unchecked (the 4,096-decision ring, checked every 256 decisions
  and at each report, overwrote one not judged yet; should be 0), and session totals;
- EXP127 = 2: acquires a second fed through the new sources, by source.

### EXP101

EXP101 (live, 0 = off, 1 = runs, 2 = cached page pointer + prefetch; fable52_answer_presenter): nvhost_gpu::SubmitGPFIFOImpl's pushbuffer
snapshot reads runs of entries whose GPU addresses follow each other with one ReadBlockUnsafe
instead of one per entry (OoT's presenting thread submits ~6,500 entries a frame; the loop is
on the frame's critical path). Exact: same words at the same offsets. "Pushbuffer snapshot
(EXP101" logs submits, entries, reads and loop time every 5 s while on.

### EXP105

EXP105 (live, 0 = every entry; N > 1 = 1 in N): DmaPusher::Step's check that a command list
the front read early was not GPU-written by queued work (EDEN_NX2_LIST_WAIT_SKIP log). It only
reads and logs, never fired in OoT on 2026-11-12, and cost ~8 ms/s of the back (try 64).

### EXP99

EXP99 (live, 0 = off; fable52_answer_chicken 1): evicted EXP42 pool images (OoT's per-NPC
depth targets, ~3 a frame while a chicken passes or an enemy dies) are destroyed on the image
free thread (2; needs EDEN_NX2_ASYNC_IMAGE_FREE=1) or, with 1, by the back itself in its idle hook
(up to 2 each time it runs out of work), instead of at the frame's end. The pool line every 600
frames counts where they went; the timeline shows "back: image pool tick".

### EXP100

EXP100 (live, 0 = off; fable52_answer_imagereads, Fable's F1; RasterizerVulkan::GetFlushArea):
a guest read of pages that hold an image skips the caches' locks when no image there was
GPU-modified. TextureCache::gpu_image_filter counts per 64 KiB page every registered,
non-sparse image that was GPU-modified since it was registered (CountGpuModified at
MarkModification, SynchronizeAliases, JoinImages and RegisterImage; removed at
UnregisterImage; clears of the flag are ignored, so it is conservative), under the texture
cache's mutex, with a sequence counter (image_seq, odd while a hold changes the count, closed
by the mutex's release hook). Nothing counted on the pages, no sparse image ever there, the
counter even and unmoved, and the buffer side clean as today (no GPU write ever marked the
pages, or EXP86 = 1's mirrors say clean): the locked path would answer the same preemptive
area and change nothing. Not with EXP36 or EXP81 on or EXP43 = 4. 1 = the fast path; 2 =
verify: locked reads compare the count with the texture cache's locked look, no fast answers
(mismatches must stay 0, the first 16 are logged with the image's range, flags and
nx2_gpu_counted). "Guest reads locked because" logs the answers and why reads stayed locked.

### EXP96

EXP96 (live; fable52_answer_guest 4): the HLE reply hold (k_server_session.cpp HoldHleReply,
written for Mario Tennis Fever, active in every title). 0 = EDEN_NX2_IPC_MIN_US or 50 us;
1 = off; N > 1 = N us. In OoT the service thread that presents waits in it ~14 ms/s.

### EXP97

EXP97 (live; fable52_answer_guest 3a): spins of a contended KSpinLock (the scheduler lock)
before it sleeps. 0 = 128 as before; N > 0 = N. EDEN_NX2_KLOCK_STATS=1 logs its locks every
5 s ("Kernel spin lock (5 s").

### EXP98

EXP98 (live; fable52_answer_yield): guest yields (svcSleepThread 0 and -1) skip the scheduler
lock when it would find nothing to do (k_scheduler.cpp YieldIsKnownNoOp). A change counter
per core, raised under the lock for the cores of every thread whose scheduling changes; a
yield that did nothing stamps its core and that core's counter and raises nothing; the early
return reads the lock word, then the counter. 0 = the code as before (per-process count);
1 = early return; 2 = verify: always take the lock, check the stamp's prediction under it
and count mismatches (must be 0, the first 16 are logged). While not 0, "Guest yields
(EXP98" logs yields by form, early returns, lock-held checks and locked outcomes every 5 s.
-2 (YieldToAnyThread) is only counted.

### EXP94

EXP94 (live, 0 = off; fable52_answer_front 1): RasterizerVulkan::HasCachedReports answers
"none" from the query cache's page filter before taking its mutex (the front asks for every
macro DMA read, ~5 ms/s of the front in the market heavy phase). Exact: the filter marks every
page a query was ever registered on.

### EXP95

EXP95 (live, 0 = EDEN_NX2_OOT_PRESENT_SLEEP_US or 250; N > 0 = N us; fable52_answer_front 2):
the short sleep of OoT's present poll with EXP24 on. The event word is written by the game's
main thread, so a sleep only ends at its timeout; shorter sleeps end the poll sooner after
the write (try 100).

### EXP93

EXP93 (live, 0 = as EDEN_NX2_UBO_INPLACE says, 1 = on; fable52_answer_writer 3): small
uniform buffers are compared with their slot straight in guest memory instead of through a
scratch copy (VideoCommon::UniformInPlaceCompareEnabled). Judge on "bind stage buffers".

### EXP92

EXP92 (live, 0 = off; fable52_answer_uniformkeeps 3 and 4, idea from a ported change, part d;
BufferCache::BindSmallUniformKeeping): a small uniform slot bound again with the same range,
in the same command buffer, with nothing written to that range since, binds the slot's stream
copy again without reading or comparing guest memory (a "keep"). Written-since is known two
ways: tier 1, the back's cached writes by exact range (EXP91's ring, filled while EXP91 or
EXP92 is on; nothing pending in MemoryManager); tier 2, a generation per device page
(Common::UniformKeepPages) raised after the bytes land by every other writer (MemoryManager
writes, device memory writes, the GPU write listener, in-place spans, run-ahead words, and
guest CPU stores, which trap because a kept range's pages are watched: one
UpdatePagesCachedCount +1 each, given back at RasterizerVulkan::UnmapMemory and at
DeviceMemoryManager Map/Unmap; a page stored to more than 64 times a frame is denied). Plus
the device translation generation. Not on native bank binds or behind a run-ahead frontier.
1 = keeps on, every 256th kept bind verified (the tripwire): one stale bind logs an error and
turns keeps off for the session. 2 = verify: a keepable bind runs the normal path anyway, which
must rebind the same copy ("same"); stale ones are counted, the first 32 logged, and bound
from their fresh bytes. "Uniform keeps (EXP92" logs binds, kept, not kept by reason, watched
pages, guest stores on them, denied pages, verified and stale every 5 s while not 0. Stale
must stay 0. 1 -> 0 at any time drops every keep (watched pages stay watched until unmapped).

### EXP85

EXP85 (live, bits, 0 = off; fable52_answer_microcuts 1 and 2b): 1 = a small inline upload to
pages no GPU write ever marked skips the buffer cache's lock (InlineMemory would find nothing
there; the list note and the query cache as before), 2 = BufferCache::WriteMemory answers
"no GPU-written data on these pages" from the GPU-written bit before the range set. 3 = both.

### EXP84

EXP84 (live, 0 = off; fable52_answer_microcuts 3.4; RasterizerVulkan::GetFlushArea): a guest
read skips the caches' locks when no image is registered on its pages now (TextureCache::
image_count_filter, counted at register and unregister, plus the pages sparse images ever
covered) instead of never (page_filter, whose bits never clear while the game's targets move
every frame) and no GPU write ever marked them. Daytime market: guest cores waited 22-54 ms/s
for those locks, ~3,500 locked reads a second, 65 of them needing a flush. "Guest reads
locked because" logs the reasons every 5 s, also while off.

### EXP83

EXP83 (live, 0 = off; fable52_answer_microcuts 3.2; RasterizerVulkan::UpdateVertexInput): a
vertex input state identical to the last one recorded in this command buffer is not sent
again (any vertex buffer address or stride write resent all 32 bindings). The memo drops on
StateTracker invalidation (new command buffer, input or whole state; helper passes call
InvalidateState) and when a pipeline without dynamic vertex input is drawn. "Vertex input
(EXP83" logs sends and repeats every 10 s, also while off.

### EXP82

EXP82 (live, frames, 0 = off; fable52_answer_after7778 1; TextureCacheRuntime::TickFrame):
proven EXP42 pool entries (a shape taken from the pool before) stay this many frames instead
of 180, so per-NPC depth targets whose size comes back later find their image. Unproven
entries and the byte cap (EXP66) are unchanged. Try 1800. Judge on the pool's taken/missed
line and the clear zone.

### EXP81

EXP81 (live, frames, 0 = off; fable52_answer_readback_card 3; BufferCache TickFrame and
GetFlushArea): with EXP69's Preflushable reset, unmark only the pages the CPU has not read for
this many frames. Every CPU read, locked (GetFlushArea) or EXP86's lock-free clean answer,
sets its pages in a page bitmap that TickFrame folds into per-page stamps every frame
(fable52_answer_fences2 2), so it works with EXP86 on. Unmarking every page made each page
the game still reads need one async read (~40 a second at the market) to be marked again; a
mark only decides whether a GPU write is copied at the fence or at the read, so it is exact.
"Preflush reset" logs the KiB kept. Try EXP69=4 with EXP81=4, then 2 and 8.

### EXP78

EXP78 (live, 0 = off; fable52_answer_killuploads 3-4; TextureCache::TcSkipStale): what
EDEN_NX2_TC_SKIP_STALE=1 does at boot, switchable: a deleted GPU-written image marks its whole
pages stale (CPU writes clear them) and a new image whose range is all stale (and not
buffer-written) skips its upload from guest memory nobody wrote. Fight kill bursts: 30-105
such uploads inside draws, 3-10 ms. Marks start when it turns on. "stale uploads skipped" in
the texture cache's 600-frame line.

### EXP77

EXP77 (live, 0 = off; fable52_answer_keeptex_2 2b; TextureCache): a GPU-written chain target
the cold sweep deletes leaves its shape at its address; a texture header that asks for that
address again marks the shape (and address) GuardSampled, so the sweep keeps that shape from
then on instead of deleting it and remaking it from stale guest bytes. Fight: 197 GPU-written
images a window remade after a sweep. Only keeps longer. Counted in the "Cold sweep" line.

### EXP76

EXP76 (live, bits; fable52_answer_keeptex 2a; TextureCache): kill frames remade textures the
game already had (fight: 767 of 833 new sampled images in 10 s had bytes made before; a BC5
2448x3056 one 129 times). 1 = FindImage keeps an image the game's remap flagged when the same
texture is asked for at the same GPU and device address with the same shape (never
GPU-written, no alias or sparse state): no delete, create or new views; it is uploaded as a new
one would be. 2 = whole uploads of 64 KiB or more store a CityHash128 of the guest bytes (read
after the upload, dropped if the image was CPU-written meanwhile); the next whole upload whose
bytes hash the same is skipped. Any other refresh path clears the hash. The
EDEN_NX2_IMAGE_HASH_LOG=1 summary logs "Remaps (EXP76=n)": images flagged, kept, hash checks,
uploads skipped. 4 (with 2; measure only, fable52_answer_keeptex_2 step 1): hashed images are
indexed by (shape, content hash); each new sampled image hashes its guest bytes and the
"Content reuse" line counts which reuse would serve it (live idle twin, live busy twin,
kept deleted image, nothing) and the hash cost in GB/s. GPU writes clear the hash
(MarkModification, SynchronizeAliases). Off by default.

### EXP75

EXP75 (live; us, 0 = off; fable52_answer_pipeline section 3; Core::Timing::CoreTiming's timer
thread): precise guest timers. The thread spins the last this-many us before a precise event
(the kernel's hardware timer: guest sleeps and timeouts, EventType::precise) instead of waking
on the OS timer, whose 0.5 ms tick made the game's 100 us sleeps last ~420 us and its 1 ms
present poll 1.45 ms. Other events stay on the timer. Costs a share of one logical CPU (kept
off the GPU threads' cores). Default 600 (LoadExperiments).

### EXP74

EXP74 (live; us, 0 = off; fable52_answer_perdraw 1; FenceManager::ReleaseThreadFunc): the fencing
thread lets the texture/buffer cache lock go between two pops after holding it this long, so a
batch of 20-60 fences no longer keeps the back waiting 1-13 ms.

### EXP73

EXP73 (live; 0 = off; fable52_answer_bosswave_2 3b; Maxwell3D macro DMA snapshot miss path): the
report flush after a drained read runs only when reports are listed over the bytes read; else it
was an empty round trip to the back (~2,850 a second in the Stalchild fight).

### EXP72

EXP72 (live; 0 = off; fable52_answer_bosswave 3; BufferCache::GetFlushArea, WaitFenceDownload):
a preflushed guest read whose GPU-written bytes are all in a submitted fence download waits (up
to 20 ms) for that download to land instead of a new copy behind everything recorded since.

### EXP70

EXP70 (live; ms, 0 = off; fable52_answer_acquire 4; PresentManager::GetRenderFrame): after this
long without a free render frame, the renderer drops the oldest queued frame that is not at
the present thread's front and reuses it. With EDEN_NX2_PRESENT_EXTRA_FRAMES (boot) giving more
render frames than swapchain images. Display stalls (acquire blocked 76-140 ms) stalled emulation.

### EXP69

EXP69 (live; frames, 0 = off; fable52_answer_stalls 5.1; BufferCache::TickFrame): every N frames
the Preflushable marks GetFlushArea set go; a range the CPU still reads is trapped on its next
read and marked again. Marks never cleared before: ~100 MB of fence downloads a second.

### EXP68

EXP68 (live; bits, 0 = off; fable52_answer_npcdips 6): bit 1 = a depth clear whose scissor is
the real width (zeta width rounded up to 16, same 64-byte column) looks up the scissor-wide image
the draws then use, so OoT's per-NPC depth targets are one image a frame, not two plus an upload
and alias copies. The next depth bind at the address checks the size; a miss turns it off there.
Bit 4 = images at an address whose target keeps changing size skip the EXP42 pool (their dims
never come back; the pool sat at 512 MiB during NPC passes).

### EXP67

EXP67 (live; bucket in px, 0 = off): depth render targets at an address whose target changes
dims get a host image rounded up to the bucket (ImageFlagBits::HostPadded), so the EXP42 pool
reuses the VkImage for the next frames' nearby dims instead of a create + 0.2-1.3 ms depth bind
(OoT per-NPC depth targets). A texture lookup that finds a padded image stops padding there.

### EXP66

EXP66 (live; MiB, 0 = built in): the EXP42 pool's cap (512 MiB; 1,536 with EXP42=2), trimmed
at once when lowered. NPC passes held it at its cap (~0.5 of the owner's +0.8 GB).

### EXP65

EXP65 (live; 0 = off; vk_texture_cache.h): EXP42 pool entries whose exact create info was never
taken back age out after this many frames (proven shapes keep the 180). Per-NPC targets sized by
screen bounds filled the pool to 512 MiB with dims that never repeat.

### EXP64

EXP64 (live; N = min frames untouched, default 16, 0 = off; fable_answer_npcdips 3, NPC dips): a new render target that a clear
overwrites whole retires the images at its exact address that the GPU wrote in an earlier
frame (TextureCache::RetireSlotPredecessors) instead of leaving them as bad overlaps for EXP58's
sweep 60 frames later. Ocarina draws each passing NPC into a D32 target sized by its screen
bounds at a fixed slot, so each frame made a new image and the old ones piled up (80-95 bad
overlaps walked per clear, ~2,000 live images, the pool full at 512 MiB, VMA growing: device
memory +0.8 GB while the chicken passes). Not for shapes the CPU read or that were sampled in a
later frame (the relocation guard's GuardCpuRead / GuardSampled), images bound, queued for
download, aliased, scaled or of the excluded kinds. Logs "Slot retire (EXP64) last 600 frames".

### EXP45

EXP45 (live; 1 = off): the native compute write extent self-check (BufferCache::ExtentTrust,
fable_answer_25): a program is marked in full until a dispatch at its dims showed its proof
holds, and for good once one wrote past it (Ocarina of Time's glowing triangles, 2026-11-09).

### EXP43

EXP43 (live; render target relocation, fable_answer_12 2A; TextureCache::JoinImages): a render
target a clear overwrites whole, with no copies, aliases or remapped images among its overlaps
and exactly one predecessor among its bad overlaps (an image of the same single-level,
single-layer 2D shape it drifted off by less than its size, not written in this frame: Ocarina
of Time's bump-allocated per-frame targets while an NPC moves), takes that image's place;
bad overlaps of other shapes stay as for a new image. 2 = relocate: the image moves to the new
address (stale at the old one, untracked, unregistered, its overlap links dropped, registered
again; its VkImage, memory, views and framebuffers stay). 3 = as 2, and with several
predecessors the most recently modified one moves (lets a chain made before the switch went
on stop growing and age out). 1 = retire early: the predecessor is deleted at once and a new
image made, as with 0. 0 = off: candidates are only counted. 4 = verify (fable_answer_16 1c): as
3 under the render-frame clock (as EDEN_NX2_RENDER_CLOCK=1), and each old range is kept as a
tombstone for 2 render frames; reads that land on one are counted ("Relocation tombstones
(EXP43=4) last 600 frames: N hits", first 8 logged), which must stay 0. Default 3 (LoadExperiments). Never for a shape
whose GPU-written contents were sampled or rendered to without a clear in a later frame, or
read by the CPU (sticky per shape for the session: "Render target relocation guard (EXP43)"),
nor for rescaled, sparse, converted or CPU-read images, ones with a download queued, bound as
another target of the clear or in another channel. Logs each shape's first candidate and
relocation and "Render target relocation (EXP43=n) last 600 frames".

Compute dispatch waits (no switch; helper AK, 2026-11-15, the log line of zortos293/Lichii #995 by
VirusEx): a dispatch that waits more than 50 ms for its compute pipeline's driver build logs
"Compute dispatch waited X ms for pipeline <hash>" (every GPU channel waits with it). #995's main
change, the pointer-slot rebuild compiling on a worker while the old translation keeps running, was
already in (f253d02aa5, port of #969: ComputePipelineCacheEntry::pending).

### EXP51

EXP51 (live; RAM, fable_answer_26 C2c; Vulkan PipelineCache::EvictUnusedComputePipelines):
compute pipelines unused for 9000 frames are destroyed, least recently used first, while more
than 500 are held (checked every 600 frames). Their next dispatch translates them again on the
GPU thread, binding the pointers they bound, and builds them from the driver's pipeline cache
(no new disk cache record). 1 = only programs dispatched this session, 2 = also boot builds no
dispatch used yet. Never one still building or with a rebuild pending; destroyed through
retired_compute_pipelines once the GPU and their worker build are done. Off by default, not
measured yet: compare the 10 s memory line's compute count and the working set over a long
session, and the "Evicted N compute pipelines" log's "dispatched again" count (each is a
GPU-thread rebuild, a possible hitch).

### EXP52

EXP52 (live; with EXP19 / EDEN_NX2_PIPELINE_CLEAN on; fable52_answer_1 section 1; Vulkan
PipelineCache::CurrentGraphicsPipelineClean): 1 = when only the bound programs changed since
the last key (every other Refresh input stands), the key's state is kept and only what the
programs feed is redone (with dynamic vertex input, the attribute types masked by what the
programs read), then Next / the cache map looks the new key up, without
FixedPipelineState::Refresh. 2 = as 1, and each such key is also built by the full Refresh and
compared byte for byte; Refresh's is kept, the first 16 differences are logged ("Programs-only
key (EXP52=2) differs"). The "EDEN_NX2_PIPELINE_CLEAN per second" line counts "programs only"
and, at 2, "verified N, M differ". With EXP52 on its "programs" reason stays 0 (a change of
programs together with other state counts under that state). Off by default, not measured yet:
prove 2 shows 0 differ first (under EXP19=2 without EXP50, blend writes without
Dirty::Blending already leave the clean lookup's attachments stale, and verify reports that
too), then compare the lookup zone with profile_toggle (EXP52 0 vs 1).

## Defaults changed on 2026-11-14 (autopilot)

On by default since build after d5d510bb9c (src/yuzu/main.cpp BuiltinSwitches and
src/common/fritchii_switch_defaults.cpp); each section below still says how it works and that =0 turns it off:
FRITCHII_GATHER_ONE_LOCK=1, FRITCHII_GATHER_EMPTY_TEST=1, FRITCHII_FENCE_COMMIT_FAST=1, FRITCHII_FRONT_RAN_READS=1,
FRITCHII_VERTEX_INPUT_COMPACT=1, FRITCHII_BIND_ADDR_MEMO=1, FRITCHII_WORKER_SPIN=80, FRITCHII_CB_WORDS_BLOCK=1,
EDEN_NX2_EXP52=1, EDEN_NX2_EXP55=1. Combined toggle at the market (one session): 62.7 -> 64.1 fps, 1% low 44.4 -> 45.5.

### EDEN_NX2_SPIRV_INLINE_GLOBALS

Boot (shader translation). How the SPIR-V helper functions that route global memory accesses to a
storage buffer are emitted. 1 (default): inlined at every call site (+2.6 fps walking Hyrule Field on
an RTX 4080, 2026-11). 0: DontInline everywhere (NVIDIA compiles big compute shaders in a fraction of
the time: up to 7.6 s each inlined). 2: DontInline for compute programs only, graphics inlined
(player report 2026-11-18, 5.8.4: a 7.6 s freeze mid-combat on an RTX 3070 when the driver had not cached a
compute shader). 3: compute programs only, graphics as with 1: the search for the storage buffer
that holds the address is one DontInline function per program (same comparisons and order as the
helpers, returns the buffer index and base or not found), and the load/store/atomic helpers stay
inlinable: each access calls the search once and then switches on the returned index to do the
access on that buffer; an address no buffer holds gets the same fallback as before. Aims at the
compile time of =2 with less of its runtime cost (=2 measured ~1.7 fps lower at the market, ~0 in
fights); idea from tasarren/Lichii (compute compile 1.5-1.75 s -> 0.26-0.52 s on their path).
Not measured on Fritchii yet. Changing it changes the SPIR-V, so the driver compiles those shaders
again once.

## OoT built-in patches (not switches)

### Water ripples at any frame rate (built-in)

On by default, in both frame step modes (Dark's Dynamic FPS and the high refresh mode); Add-Ons can
turn it off. A port of Blitchii PR #1180 (480e01394 and its review follow-up), found and verified
live by a tester (2026-11-17: a Hyrule Castle water room at a pinned step of 0.5, ripples jumping on
camera turns with the earlier patch, steady with this one, the per-object rest within +-0.5).

The water renderer (main+0x472E0) runs `iterations * trunc(step)` ripple passes a frame
(iterations = 6, `fcvtzs w9, s0` at main+0x475F8, `mul w8, w8, w9` at main+0x4760C). Below a step
of 1 (above 60 fps; the step is relative to 60 Hz) some or all frames run no pass, and the grid
that follows Link still moves, so the surface jumps. The earlier Fritchii patch (an accumulator at
main+0x3C46E90 returning `trunc(step + rest)`) kept the average rate but at a step of 0.5 still
ran 0 and 6 passes in turn.

Now: main+0x475F8 is `bl 0x3c46f20`, main+0x4760C is `nop`, and the 148-byte routine at
main+0x3C46F20..0x3C46FB4 adds `iterations * step` to a rest kept per water object (x21), returns
w8 = the rounded passes, at least 1 while the step is above 0 (0 while paused), and keeps
`total - passes`, never below -0.5. At a step of 0.5: 3 passes every frame. The rests live in 8
slots of {u64 object, f32 rest} at main+0x4E9FE20, the next slot to take at main+0x4E9FEA0 (the
high refresh state is main+0x4E9FE00..0x4E9FEB0 now; +0x10 held the old accumulator's index and
is unused). The routine clobbers x9-x12, s0-s2, the flags and x30; checked against the game's
code: the function saves x30 in its prologue and none of those is read after the call before it
is written. The old accumulator's spot (main+0x3C46E8C..0x3C46F10) is free again.

Ctrl+U switches only the call word while the ripples are on. With the ripple built-in off,
switching Dark's mode back restores both words and clears the routine; it waits while a guest
thread stands in a routine or between the two water words.

### EDEN_HYBRID_CORES

Boot, Windows, CPUs with performance and efficiency cores. Wins over the "Intel P-cores" setting
(Advanced > CPU). 2 or unset with the setting on Automatic = soft: Windows places the time-critical
threads. 4 = GPU pinned, same as "Pin GPU to P-cores": the GPU front, back and render threads are
held to the P-cores with CPU sets, released when they don't fit or after two 10 s windows with over
50 late guest timers a second. It adds nothing where `Thread pinning:` already gave them cores of
their own. 1 = strict with guards, 3 = strict without, 0 = off. Check the `CPU placement:` and
`Hybrid placement in 10 s:` lines.
