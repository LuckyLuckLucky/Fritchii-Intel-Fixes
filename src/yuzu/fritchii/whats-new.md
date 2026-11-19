## ⬇️ Download this file: **`Fritchii-v6.2-Windows-x64.zip`**

Linux: **`Fritchii-v6.2-Linux-x86_64.AppImage`** (or the `.tar.gz` of the same build).

On Fritchii v4 to v6.1 you don't need to download anything: it offers this update when you open it. Otherwise unzip it anywhere and run `Fritchii-v6.2\fritchii.exe` (a small `lichii.exe` stays next to it, so old shortcuts keep working). Don't download "Source code (zip)" or "Source code (tar.gz)": they contain no emulator.

---

Fritchii v6.2: smaller graphics cards, slower PCs and above 60 fps.

### Good to know
- **Intel 12th to 14th gen (P-cores and E-cores): you shouldn't need `EDEN_HYBRID_CORES=0` any more.** v6.1 tied the GPU threads tightly to the P-cores, which halved fps on some of these CPUs. v6.2 lets Windows place them by default. If you added it, Fritchii turns it off for you on the first start (the line stays in Tools > Switches as a comment). If fps is still about half of what you expect, remove the # in front of it and tell us on the Discord.
- **First start asks once: "Use the recommended settings?"** Use recommended applies the settings tuned for OoT and saves your old ones first. Keep my settings changes nothing. You can switch later in Settings > General.
- No new shader preparation: v6.2 keeps v6.1's prepared shaders.

### Graphics cards with 4 to 6 GB
- **No more texture churn when video memory runs short.** On small cards Fritchii kept deleting textures the game was about to use again and making them over, dozens of times a frame. Now it keeps the ones the game keeps coming back to. On a simulated 4 GB card in Castle Town: **43 → 75 fps**, 1% lows **20 → 57**. A real 4 GB card also has to swap memory over PCIe, so expect less than that, but a big step either way.
- Less video memory lost to half-empty buffer blocks on small cards.

### Picture
- **The OoT Picture menu sets the render size and the upscaled output separately**, so you can draw at 720p and let DLSS or FSR bring it up to 1080p or more.
- DLSS has an **Automatic** model, now the default. It uses K, the tested one, and the status bar shows the model actually in use.

### Loading and busy areas
- **Area loads unpack the game's files natively** instead of through the emulated CPU, so loading screens should be a bit shorter.
- The game no longer starts tidying its texture memory every frame, only once a second. Every texture it moved used to be set up again by Fritchii.

### AMD
- **New setting: OoT occlusion speed-up** (Graphics > Advanced). On AMD cards OoT's visibility checks are answered without the slow GPU queries: about **+9% fps** in the market on an RX 9070 XT. Standard is on by default. AMD players get a one-time offer of Full, which goes further. NVIDIA and Intel already use another fast path, so nothing changes there.

### Intel CPUs
- **CPU settings > Intel P-cores:** on Intel CPUs with P-cores and E-cores, try the pinned choice and tell us on the Discord whether it helps your PC.

### Above 60 fps (High refresh rate)
- The time of day no longer slows down or freezes above 60 fps.
- Epona jumps fences instead of galloping through them.
- A song's notes show one at a time with the music.
- The Deku Tree's web reacts when Link lands on it, and moving platforms carry Link.

### Below 60 fps
- Link's cap and other cloth settle as they should when the game runs slow, instead of flaring out for a second.

### Stability
- When Windows runs out of memory for a moment, Fritchii waits and tries again instead of closing.
- A crash report now names the GPU work that hung, and a few rare GPU limits no longer end the game.

Some fixes in this release are ports from the Longshot and HeyListen projects, with thanks.

---

## What came in v6.1

Fritchii v6.1: the pot room fix.

### Pot room
- **Walking around the pot room (Castle Town guard house) no longer eats VRAM.** Moving there made thousands of new shadow images every few seconds and video memory climbed by about 4 GB. A fix for it existed before v6.0 but didn't make it into the release. It's built in now.
- On our test PC (strafing in the pot room, uncapped): **67 → 71 fps**, 1% lows 46 → 48, video memory **10.5 → 7.4 GB**.

### Linux
- The v6.0 Linux files were built a few hours before the final Windows build, so they missed the last v6.0 fixes and still said "v6.0 Beta". v6.1 on Linux has everything. Fritchii's tuned settings are applied once on Linux with this update.

Nothing else changes: your settings stay as they are on Windows, and no new shader preparation is needed.

---

## Everything new in v6.0

If you're coming from v5.x, this is what v6.0 brought (and what you get now too).

The biggest Fritchii update so far: 840 changes since v5.8.4. It had one goal, **OoT holding 60 fps**, and it ships because it does: Castle Town market now runs at or above that goal on our test PC, the long hitches are mostly gone, the pot room no longer crashes, and Linux builds are here.

**Heads up, three one-time things after updating:**
- **"Preparing shaders" runs once more, on every GPU, and this time it takes a while** (about 15 minutes on a fast 8-core CPU, longer on 6-core CPUs and laptops). Two reasons: the way shaders are translated changed, so the prepared ones from v5.x don't match anymore, and **the program is now called `fritchii.exe` instead of `lichii.exe`**. Your graphics driver keeps its own shader cache per program name, so for `fritchii.exe` it starts empty and has to compile everything once. Let it finish; the next starts are quick again.
- **Fritchii asks once to clear old shader caches** ("Frees X GB"). Clear is safe: those are files Fritchii rebuilds by itself or no longer reads, and the shaders get prepared again anyway. Your graphics driver's own cache is never touched.
- **OoT's settings now live in one place.** Values set in OoT's own Properties (or left there by older Lichii/Eden builds) used to silently win over what you set in Emulation > Configure. They move into the main settings once, as you last saw them in game, and a short message lists what moved. The old file is backed up first (`custom\0400486024DEC000.ini.before-v6`). OoT's Properties now only shows Add-Ons.

Fritchii's tuned settings are applied once more with this update; you can change anything afterwards.

#### Performance

Uncapped on our test PC (Ryzen 7 7800X3D + RTX 4080, 1440p). With the 60 fps cap on you see a flat 60; the uncapped number is the headroom you have left. Around 70 uncapped is what holds a smooth 60.
<!-- Measured on the release candidate's code, 2026-11-18: market = PGO v10 A/B (fresh launches, last 60 s of 90), Zora's River = build 70 regression, pot room = builds 66-70. -->

| Spot | v6.0 uncapped |
|---|---|
| Castle Town market (day, heaviest view) | ~74 fps |
| Zora's River | ~91 fps |
| Pot room (guard house) | ~92 fps |

For comparison: on v5.8 the market ran 50 to 60 and rarely hit 60.

- **A third GPU thread.** Your CPU does a lot of work before your graphics card draws anything: it reads the game's GPU commands, sets up each draw and records it for Vulkan. In the market that is about 2,650 draws every frame. Until v5.8 two threads shared that job and the one setting up draws was the wall. v6 adds a third thread that records the draws, so the work spreads over more CPU cores.
- **The game's own code runs faster.** OoT's code is recompiled for your CPU while it plays. That recompiler got a round of improvements: about 11% less CPU time for the game's own threads (69.4 to 61.1 ms of core time a frame in the market). Reading the game clock no longer leaves the compiled code (about +2 fps).
- **One code cache for all 6 emulated CPU cores.** Each core used to compile and keep its own copy of the same game code. Now they share one: **about 1 GB less RAM** and about a quarter less stutter when areas load.
- **The build is optimized from real play again.** Fritchii is compiled with a profile of how the code actually runs in OoT. Retrained on the new code it gave +6.5 fps in the market by itself (60.4 to 67.4, same code).
- **Fewer waits between the game and the GPU.** When the game reads back something the GPU made, the emulator used to wait for more than it had to. Several of those waits are cut or shortened. Biggest single one: the Great Fairy fountain went from 52.3 to 71.0 fps.
- **GPU threads get their own CPU cores and a higher priority,** so the rest of the emulator and your PC don't push them aside (turned off by itself on CPUs with fewer cores).
- **Smooth 60 on 120/144 Hz screens.** With the 60 cap on a high refresh screen about 1 frame in 60 stayed on screen an extra refresh (a 25 ms stutter). Now 99.7 to 100% of frames show evenly, and the capped 1% low went from 40.8 to 48.8 or more.
- **Long sessions stay fast.** A 40 minute Castle Town session: median 81.3 fps, 0 crashes, no hitch over 50 ms after the first minute, and RAM flat at about 10.7 GB the whole time.

#### Stutter and freezes

- **Kill hitches about half as frequent.** When you kill an enemy the game reads GPU results back, and the copy waited behind the whole frame's work. It now runs on its own GPU queue right after the job that wrote it (checked byte for byte). In a Stalchild fight with kills, frames slower than 20 ms went from 2 a second to 1, and the read-back time in them from 10.6 to 5.1 ms. They're not fully gone yet (see Known issues).
- **Pop-in instead of shader freezes, on every GPU.** A shader that's still being built now makes its object show up a moment late instead of freezing the game. Before, this was AMD and Intel only. Compute shaders your last session used are built first at boot, before the game asks for them.
- **Fewer first-time compute shader freezes** (zortos293/Lichii#2104): a compute pass whose shader is still building is skipped for that frame when nothing reads its result. The biggest compute shaders feed game logic and can't be skipped safely, so after a new install or a driver update a few freezes remain the first time you meet them; from the next session on they're built at launch.
- **Game code you played before is compiled at launch.** Fritchii remembers which game code your sessions ran and compiles it ahead next time, so fights and new areas don't compile it mid-play. Using a code mod (like the Hyrule Cheat Menu) no longer wipes that list.
- **Shorter freezes when areas load** (the shared code cache above, plus fewer textures uploaded twice).
- **Night market:** dogs entering from behind no longer hitch, and chickens much less.
- **Pot room:** each object's shadow used to need a new depth image almost every frame (4,000 to 10,000 per 600 frames while strafing at the wall). Shadow sizes are now rounded to steps: 30 to 90, frame times 20.5-26 ms down to 18.4-18.8 ms, and the shadows look the same.
- **A display stall no longer stops the game.** When Windows or the driver held a frame back, emulation waited with it.
- **16 GB cards at 4K:** VRAM limits now follow your driver's real budget, so the game no longer spills into system RAM and freezes for seconds.
- **8 GB and 6 GB cards** get a small-card mode by themselves that keeps the game inside your VRAM (on an 8 GB test: 66.0 to 67.1 fps, 0.1% low 38.5 to 42.6). On these cards a resolution above 1x runs at 1x.

#### Fixes

##### Visual
- **No more black flashes when switching items fast:** a picture whose GPU memory was handed to another one came back empty for a frame. The pictures that need to stay are now remembered between sessions (and shipped with a list from our testing), so they're never moved away.
- **Text garbage fixed.** Dialogue boxes, choice buttons and numbers could get random texture junk over them, worst on shooting gallery retries and after hiding and showing the UI with the left stick. The texture cache threw text images away without saving them first; now it saves them (zortos293/Lichii#1310 has the reports).
- **Grass, pebbles and far trees no longer vanish while Link swings his sword** (zortos293/Lichii#1628).
- **Square tiles in the mist and foam around Link** (waterfalls, rupees): a GPU buffer the game reads was bound too short. Port from CheezerWheezer's CheezyEmu.
- **Swim wake and water ripples no longer jump when Link turns at high frame rates** (port of Blitchii #1180, found and checked by a tester).
- **High refresh (Ctrl+U) fixes:** sound and music no longer go silent after a few loading zones above 60 fps, moving platforms work above 100 fps, Gohma steers right above 60, cutscene voices stay in sync, the Scarecrow's Song keeps its length, hair and cloth sway at real speed, and the rupee counter above Link counts up and fades again (zortos293/Lichii#1660, #1663, #1679, #1743, #1745, #1793). They no longer overlap the flags cheat codes use (#1981).
- **Black picture on some handheld PCs on Linux** (when one of OoT's anti-aliasing passes can't be built, the game now draws without it). From CheezyEmu.

##### Crashes and freezes
- **Pot room (guard house) crashes and freezes are gone.** The game's texture pool ran out there; Fritchii now doubles it with a built-in patch (zortos293/Lichii#1313), placed so the Hyrule Cheat Menu mod still works.
- **A rare GPU crash at boot ("invalid write at 0x0")** can't happen anymore: the emulator no longer hands the driver an empty GPU address.
- **Running low on memory** no longer ends the emulator during the background memory cleanup (zortos293/Lichii#1451), and stopping a game no longer ends in a "Userspace PANIC" (#1450, #1952).
- **The ocarina warp freeze** (zortos293/Lichii#1218) and a movie crash with error 2162-0001 (#1512).
- **The shader cache file can't be broken by a crash while it's being saved** anymore.
- **Damaged or unusual game files** are refused with a log line instead of crashing (a batch of loader fixes from Lichii).

##### Audio and input
- **Bluetooth headsets stay in stereo:** a "none" microphone choice keeps Windows from switching them to the low-quality headset mode (Fritchii #60).
- **Booting with the UI volume at 0** no longer leaves the game silent until a restart (zortos293/Lichii#1847).
- **Gyro keeps working after a controller reconnects** (zortos293/Lichii#1805).

#### AMD

- **Fritchii keeps your prepared shaders itself, like NVIDIA's driver does.** The 0.2-1 s stalls the first time something showed up are gone, and the preparation survives Fritchii updates.
- **Red flashes over wet ground and water** are fixed. AMD's driver interpolates one kind of screen-space value wrong when a triangle is cut near the camera; the fix now covers mesh shaders too, which is how OoT draws most things.
- **Zora's Domain effects that were skipped are drawn again** (one session dropped 305,000 of those draws).
- **No more white sky in Hyrule Field** (from CheezyEmu, through Lichii).
- **RX 470 to 590:** shaders are built on 2 threads, so the PC no longer runs out of RAM after the intro (from CheezyEmu).
- **FSR 4 on RX 9000 cards** is set up like CheezyEmu's.
- **Safe settings are applied by themselves** on AMD's driver, no file editing needed.
- **Bigger textures can no longer crash AMD and Intel cards at 2160p and 2880p** in Castle Town (zortos293/Lichii#1890).

#### NVIDIA (DLSS)

- **DLSS gets camera jitter by default,** for sharper detail. If the picture shakes for you, untick "DLSS jitter".
- **New "Texture sharpness" setting** (Auto, Sharper, Custom). Auto now works from DLSS's real render size.
- **DLSS and FSR always take the game's own picture,** since they pick their own render size. Before, picking a higher Resolution quietly turned them off ("DLSS only works at 1x"). The quality names show OoT's real sizes.
- **DLSS fixes from Lichii:** clamped colour input, the right camera, render scale and texture detail (zortos293/Lichii#1672, #1659, #1589, #1690).
- NVIDIA DLSS wins over "Use the game's own DLSS" when both are on. DLSS options are hidden on other GPUs.

#### Intel

- **Intel Arc fixes from a community fork:** shaders get prepared on Arc, plus fixes for how the GPU runs some of OoT's shaders.
- **Fritchii picks your Arc over the CPU's built-in graphics** by itself (it now picks the graphics card with the most video memory, and remembers your choice by name).
- **A shader compile can no longer hold a frame for 10 s:** draws wait at most 50 ms a frame, then show up once their shader is built.
- Arc runs, but not perfectly yet (see Known issues).

#### Linux (new)

- **Linux builds:** an AppImage and a tar.gz (x86_64). `chmod +x Fritchii-v6.1-Linux-x86_64.AppImage` and run it, or unpack the tar.gz and run `bin/lichii`. Your data lives in `~/.local/share/fritchii`. The AppImage updates itself like the Windows build.
- **Shader preparation on Mesa drivers** (RADV, ANV, NVK) as well as NVIDIA's.
- **NVIDIA DLSS on Linux** through the NVIDIA driver (the DLSS panel says if your driver is too old).
- **Steam Deck:** starts in handheld mode on the first launch, and Steam's shader cache limit is raised so the preparation sticks. It runs, but at about 10-12 fps it's below what OoT needs; a lower Resolution (810p) helps.
- Crashes on Linux now leave a report in the log, and there's an issue form for Linux problems.

#### Settings and UI

- **More DLSS and FSR outputs:** with an upscaler on, pick 1080p, 1440p, 1800p, 4K, 2880p or 8K (2880p needs 6 GB of video memory, 8K needs 12 GB; the menu hides what your card can't hold). 4K and 1440p work exactly as before. Outputs above 1440p build on Max Resolution's 1440p picture, so picking one turns Max Resolution on (the menu says so). Upscalers always start from the game's own picture (up to 1080p, or 1440p with Max Resolution), so with one on, 1080p is the lowest. To go below 1080p, set Upscaler to Off and pick a Resolution.
- **With DLSS or FSR on, the row says "Upscale to"** and a line says the upscaler starts from the game's own 1080p (1440p with Max Resolution). To go below 1080p, set Upscaler to Off and pick a lower Resolution.
- **The status bar shows the real picture,** like "1440p → 4K · DLSS" or "up to 1080p · DLSS Native", and follows quality changes made in game (it used to say "Scale: 1x").
- **Max Resolution asks once:** v6 turns the Max Resolution add-on off and asks on the first start, with what it needs (about an RTX 3070 / RX 6800, 8 GB) and whether your card handles it. It's needed for DLSS/FSR outputs above 1440p.
- **"Good to know" on the first start:** the known issues you may run into, in one short window (Help > Known Issues shows it again).
- **Upscaler right under Resolution:** one row with Game, FSR 3.1, FSR 4 and DLSS (only the ones your GPU can run).
- **Window filter Auto (new default):** a picture bigger than your screen (1440p or 4K on a 1080p screen) is shrunk with Area, which averages every pixel and doesn't shimmer; a smaller one is stretched with ScaleForce as before.
- **Resolution shows real resolutions,** like **1440p (Handheld 1080p)**, from 360p all the way to 8K, so weaker PCs can pick 810p or 720p. Changing it prepares shaders again once, so the next session doesn't compile them in play.
- **Tools > Switches:** see and edit the test switches (`nx2_switches.txt`) inside Fritchii, with Save and Restart.
- **High refresh "Unlimited" cap:** with VSync on (Mailbox) high refresh can go past your monitor's rate. Don't combine Unlimited with VSync off: the game can crash right after a loading screen (the list says so).
- **Performance overlay:** GPU and CPU time per frame, each emulator thread's time and what is holding the frame rate back ("Limited by").
- **The build is named in the title bar,** so a screenshot says which version you run.
- **New Fritchii icon:** the pear, by a community artist.
- The Settings window fits your screen, the Controls page fits the Fritchii look, and borderless fullscreen works from a maximized window (zortos293/Lichii#1895).
- In fullscreen, screenshots are saved straight to the folder (the Save as window could open behind the game and look like a freeze).
- If the update can't find `lichii.exe` after unpacking, Fritchii says it was probably your antivirus. The Donate button opens Buy Me a Coffee.

#### For bug reports

- **Help > Export Log says much more:** what held up each slow frame, a full report when the game freezes for 5 s or more, GPU fault details after a GPU crash, every pause with its cause (focus loss, sleep, a dialog), a run summary with 1% and 0.1% lows, and the build. If something goes wrong, export the log right after and post it in the Issues channel with your GPU model.
- **Help > Profiling Mode:** turn it on, play the spot with the problem, export the log and send the zip. A red bar on the game reminds you it's on; turn it off after.

#### Known issues

<!-- Short form of known-issues.md (the master list): keep both in step. -->
- **Short dips when you kill enemies:** about half as frequent and much shorter than before, not gone yet.
- **First launch after the big shader preparation:** if the game crashes during the first area load, start it again once.
- **Intel Arc:** runs, but shader preparation is slow and new things can still freeze the game for a while. Intel CPUs with P- and E-cores: if fps is about half of what you expect, add `EDEN_HYBRID_CORES=0` in Tools > Switches.
- **Steam Deck:** about 10-12 fps, below what OoT needs; a lower Resolution (810p) helps.
- **Rare GPU hang,** about once in 200 launches here (older versions too). Restart and send the log if it repeats.
- **DLSS shakes for you?** Untick DLSS jitter.

#### Credits

Built on **zortos293's Lichii**: the base Fritchii grows from, and a long list of its pull requests are ported in this release. Thanks to zortos and every Lichii contributor whose work is in here.

**Dark (Eglantine Dev)** for Dynamic FPS (built-in). **DahBard's Blitchii** for the swim wake and ripple fix and several ideas (thread stacks, the power mode log line, load hitch work). **CheezerWheezer's CheezyEmu** for FSR 4, the mist and foam tiles fix and a batch of AMD fixes. The **Hookshot** OoT recomp team for ideas behind faster GPU data checks. Intel Arc fixes from a community fork, Linux and Steam Deck ideas from a community Linux fork, memory ideas from two more Lichii forks, and community pull requests on Fritchii (#36, #60, #64).

Everyone in the v6 test group for the logs, screenshots and long sessions, and the whole **Ocarina of Forks** Discord for the moral support, the novel ideas and the debugging help.
