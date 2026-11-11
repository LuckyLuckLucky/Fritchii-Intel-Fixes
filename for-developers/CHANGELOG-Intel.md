# Fritchii for Intel GPUs: what's new

For Intel Arc and other Intel GPUs. Tested with Zelda: Ocarina of Time on an Arc B580.
NVIDIA and AMD users: nothing changes for you.

## What got better

| Version | Improvements |
|---|---|
| **IntelFix1** | Flickering and missing grass and foliage **fixed**. Random light beams **fixed**. Flashing lights **fixed**. Missing textures and fog glitches **fixed**. |
| **IntelFix2** | "Preparing shaders" **works on Intel now** (before, Intel threw all prepared shaders away). |
| **IntelFix3** | **Big FPS boost:** slow outdoor areas went from **20-30 fps to about 40 fps**. **No more lag spikes** while shaders prepare in the background. |
| **IntelFix4** | Correct lighting math, and a steady **40 fps** in smaller areas. |
| **IntelFix5** | Red lines on the main menu **fixed**. |
| **IntelFix6** | Less GPU work per frame (helps weaker Intel GPUs most). |
| **IntelFix7** | Same as IntelFix6. Your shader preparation progress carries over. |

**Overall:** all known visual glitches are fixed, outdoor areas run much faster, and many areas
hold a steady 60 fps.

## Please try ALL versions

Every PC is different. A version that runs best on one PC may not be the best on yours.
**Please try every version (IntelFix1 to IntelFix7), not just the newest one,** and tell us which
one runs best and looks best on your PC.

## Please send logs

Logs are how every fix above was found. For each version you try:

1. Play for a few minutes, ideally where it runs slowly or looks wrong.
2. Close the game, then in the emulator click **Help → Export Log...**
3. Send the log together with:
   - the **version** you tried (e.g. IntelFix3)
   - your **GPU, CPU and RAM**
   - **where** in the game you were, your **FPS**, and a **screenshot** if something looks wrong

Slow spot? Start **`Profile run (send me the log).cmd`** (IntelFix2 and newer) instead of
`lichii.exe`, stand there for a minute, then export the log the same way.

## Good to know

- **The first start takes long:** let "Preparing shaders" finish once. It's slow, but you only
  need it once and progress is saved.
- **Want a steady frame rate?** Click the **60 FPS / 30 FPS** button in the status bar. The game
  keeps its normal speed.
- **Still being worked on:** Hyrule Field when you look far into the distance (about 25 fps on
  a B580), and slightly flickering grass shadows.
