# Fritchii Intel Fixes

Unofficial fixes that make [Fritchii](https://github.com/Frizti/Fritchii) run well on **Intel GPUs**
(Arc and Xe). Tested with Zelda: Ocarina of Time on an Arc B580. NVIDIA and AMD users don't need
this.

## Download

Get the builds from the **[Releases](../../releases)** page. Five versions are there: IntelFix3,
IntelFix4, IntelFix5, IntelFix6 and IntelFix7.

1. Download `Fritchii-v5-IntelFixN.zip` (the `-symbols.zip` files are only for developers).
2. Extract the whole folder and run `lichii.exe`.
3. It uses the same saves and settings as your normal Fritchii (`%APPDATA%\fritchii`).

**Please try ALL versions, not just the newest.** Every PC is different, so tell us which one
runs and looks best on yours. You'll find what each version improved in
[CHANGELOG-Intel.md](CHANGELOG-Intel.md).

## Please send logs

In the emulator, click **Help → Export Log...** and send the log with:
- the version (e.g. IntelFix6)
- your GPU, CPU and RAM
- where in the game you were, your FPS, and a screenshot if something looks wrong

Open an [issue](../../issues) here, or send it to the person who shared this page.

## For developers

[`for-developers/`](for-developers) has everything needed to merge the fixes into Fritchii:
- an explanation of each fix: [`Fritchii Read.md`](for-developers/Fritchii%20Read.md)
- an agent guide: [`Fritchii agent Intel Fixes Read.md`](for-developers/Fritchii%20agent%20Intel%20Fixes%20Read.md)
- one patch per fix, plus a git bundle

## Source and license

The builds are Fritchii `oot-gather-fix` at commit
[`4f5c53149b`](https://github.com/Frizti/Fritchii/commit/4f5c53149b7e17d1c24445f68d79a83a3b2a17f9)
(v5.8.4) plus the patches in [`for-developers/patches`](for-developers/patches):
- IntelFix3 = patches 0001-0007
- IntelFix4 = 0001-0008
- IntelFix5 = 0001-0009
- IntelFix6 = 0001-0011
- IntelFix7 = 0001-0012

Patches 0013-0014 only touch a developer tool and the MSVC build setup.

Licensed under GPL-3.0, like Fritchii (see [LICENSE.txt](LICENSE.txt)).

Not affiliated with Nintendo. No games, keys or firmware are included. Only use games you own.
