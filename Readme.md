# Diablo II: Resurrected on Android (AYN Thor)

This project makes **Diablo II: Resurrected (Steam version)** run smoothly on **Android** in
[GameNative](https://github.com/utkarshdalal/GameNative), tuned for the **AYN Thor** handheld (Snapdragon 8 Gen 2,
Adreno 740).

It is a fork of [FEX](https://github.com/FEX-Emu/FEX), the x86 emulator GameNative uses for Windows games on ARM, built on
Dmage22's [D2R launcher fixes](https://github.com/Dmage22/FEX-D2). Besides FEX it holds our own builds of everything else
in the game's path: vkd3d-proton (DirectX 12 to Vulkan), the Turnip graphics driver for Adreno GPUs, and GameNative's
frame generation layer. Every change was made for D2R and measured on the device.

> **Right now this is tested on one AYN Thor only.** Other Snapdragon 8 Gen 2 devices may work but are untested.
> Not affiliated with Blizzard, the FEX project, GameNative or Lossless Scaling. No anti-cheat changes; play online at
> your own risk.

## Where it stands (2026-10-30)

- **60 FPS on screen**, made from a steady **30 FPS** with frame generation 2x.
- **No hitches in normal play.** In the last test run, 3 minutes of play went by without a single frame over 41 ms.
- The map overlay, HUD and item names on the ground stay sharp and move smoothly with frame generation.
- **Still open:** the frame just before a loading screen can take 60 to 70 ms; very big fights are limited by the graphics
  chip (30 real frames held in earlier tests); Android's own background work can still take CPU time now and then.

## Download

Get the latest bundle: **[TEST version 4](https://github.com/weltenaichaos/FEX-D2-reforged/releases/tag/d2r-thor-test-4)**.
Its release notes list every change and what is still open.

| File | What it is |
|---|---|
| `fexcore-2610-d2rfix28-thor-clean.wcp` | FEX for playing. No log file. |
| `fexcore-2610-d2rfix28s-thor.wcp` | The same FEX with a per-session log, for reporting problems. Both can be installed side by side. |
| `vkd3d-proton-3.0b-arm64ec-fex37.wcp` | Our vkd3d-proton 3.0b, native ARM64EC. |
| `turnip-26.3.0-rc2-fex17.zip` | Our Turnip driver (Mesa 26.3.0-rc2). |
| `liblsfg-vk-layer.so` | Our frame generation layer, replaces GameNative's own. |

## Setup in short

Step by step: **[docs/d2r-thor-setup.md](docs/d2r-thor-setup.md)**. Common questions (other devices, storage, playing
online, frame generation): **[docs/d2r-faq.md](docs/d2r-faq.md)**.

| | |
|---|---|
| App | GameNative, D2R Steam version in its own container |
| Proton | GameNative's proton-11.0-1-arm64ec (11.0-2 crashes D2R) |
| FEX | `2610-d2rfix28-thor-clean` (or `2610-d2rfix28s-thor` for a log) |
| DX12 / DX11 | VKD3D `3.0b-arm64ec-fex37`, DXVK 2.7.1 |
| Graphics driver | Turnip `26.3.0-rc2 FEX fex17` |
| CPU cores | 3 to 7 |
| Environment | `FEX_DISKCACHE=1`, `VKD3D_CONFIG=no_upload_hvv,nodxr`, `MESA_SHADER_CACHE_MAX_SIZE=4096MB` (nothing else; all D2R fixes are on by default) |
| Frame generation | GameNative's Lossless Scaling: 2x, FPS limit 30, flow scale 0.5, Performance Mode on, our layer file copied over GameNative's |
| Screen / game | Thor at 120 Hz, VSync off in D2R |

Frame generation needs Lossless Scaling owned on Steam. The first sessions stutter in new areas while FEX fills its code
cache; back up `AppData\Local\fex-emu\DiskCache` once it is full.

## What we did

Every build was played on the Thor with a per-second log of all threads, frames, CPU and GPU work. Each fix below came
from a cause found in such a log, and each one can be switched off for comparison
(**[docs/d2r-switches.md](docs/d2r-switches.md)** lists every switch). The details and numbers are in
**[docs/d2r-thor-findings.md](docs/d2r-thor-findings.md)**.

### FEX

- **D2R runs at all:** Dmage22's fixes let Blizzard's protected loader run under Wine ARM64EC.
- **Built for the Thor:** armv8.4-a, tuned for the Cortex-X3. D2R's settings are built in, so no extra variables are needed.
- **Faster code cache:** FEX's disk cache file is mapped through Windows (GameNative has no unixlib), and translated code
  that comes back the same is reused instead of translated again.
- **CPU placement:** the game's main thread gets the fastest core alone; FEX's helpers and the game's loader thread go to
  the small cores; other programs (Steam, the launcher) are kept off the main thread's core. When Android's own work
  (memory cleanup, system tasks) lands on that core, the main thread moves to the other big cores within milliseconds.
- **Priorities:** the game's job workers, the wineserver and VKD3D's frame threads run at higher priority, so a busy
  scene doesn't leave them waiting for a CPU.
- **No more freezes from FEX itself:** self-modifying protection code no longer faults on every write, FEX's locks wait
  in the Linux kernel instead of Wine's spin-locked wait lists, and the code buffer is renewed at loading screens instead
  of overflowing in play.
- **Game data:** zlib and MD5 run as native ARM code, unpacked data is cached, the game's data files stay out of Android's
  file cache (Android's memory killer used to close other apps at every area load), and data the game reads during play
  is learned and read in at the next loading screen.
- **Controller rumble** no longer blocks the game's render threads.
- **Stats log** (logging version only): per-second numbers and an explanation of every slow frame.

### VKD3D (DirectX 12)

vkd3d-proton 3.0b built as native ARM64EC code (about +5 FPS over GameNative's x86 build), with 37 patches in
[`vkd3d-patches/`](vkd3d-patches):

- **Frame pacing for low input lag:** each frame starts as late as it can without missing its slot; the frame cap no
  longer blocks the GPU queue; presents run on their own thread.
- **Frames show only once drawn** (GameNative's display showed frames the GPU hadn't finished).
- **GPU memory made ahead** on a background thread, since D2R keeps creating it during play.
- **Faster shaders:** D2R's lighting table blur, run in bursts when entering an area, takes 2.2 ms instead of 30 ms;
  the ground clutter shader does its cheap tests first; an empty shadow map isn't cleared again.
- **Frames without the game's UI** for frame generation, so the UI doesn't smear.

### Turnip (Adreno driver)

Mesa 26.3.0-rc2 with 17 patches in [`turnip-patches/`](turnip-patches): D2R's lighting draws load their constants early
and through the command processor (about 45% cheaper per draw, 1.4 to 1.8 ms less GPU time per busy frame), and clears
run as one 3D draw.

### Frame generation layer

GameNative's Lossless Scaling layer with our patch [`tools/lsfg/d2r.patch`](tools/lsfg/d2r.patch): no crash with D2R,
no 12 or 15 FPS locks, frames presented from the layer's own thread exactly on time, no generated frame across a freeze,
and the game's UI taken from the real frame and moved along its own motion (no smearing around the map, HUD and item names).

## Reporting problems

**Please open an [issue here on GitHub](https://github.com/weltenaichaos/FEX-D2-reforged/issues/new/choose)** and include:

- your device, GameNative version and which release you use (for example TEST version 4),
- what happened and when (loading, town, fights, after how long), and any switch you changed,
- a log: switch the container's FEX to `2610-d2rfix28s-thor`, play until the problem shows up, and attach
  `D:\fexstats-D2R.exe-b<N>-<time>-<pid>.txt` (zip it if it is large). If the clean version crashed, attach
  `D:\fexcrash-D2R.exe-...txt` instead.

## Repository layout

| Path | What is there |
|---|---|
| `Source/Windows/` | FEX's Windows (ARM64EC) side with the D2R changes: thread placement (`Common/PriorityGuard.cpp`), switches and defaults (`Common/Switches.cpp`), stats log (`Common/StatsLog.cpp`), game data, code buffer and loading screen handling (`ARM64EC/`) |
| `vkd3d-patches/` | Our vkd3d-proton patch series, plus test tools |
| `turnip-patches/` | Our Turnip patch series, plus test tools |
| `tools/lsfg/d2r.patch` | Our frame generation layer patch |
| `tools/release/` | Parts list and notes for the bundle release |
| `.github/workflows/` | Builds of FEX (`build-fexcore-wcp.yml`), vkd3d-proton, Turnip, the layer, and the bundle release |
| `docs/d2r-thor-setup.md`, `docs/d2r-faq.md`, `docs/d2r-switches.md`, `docs/d2r-thor-findings.md` | Setup guide, FAQ, switch list, and what we found |

## Credits and licenses

- [FEX](https://github.com/FEX-Emu/FEX) by the FEX-Emu team: the emulator this fork is based on (MIT). For FEX itself
  (Linux install, general use), see the upstream project.
- [Dmage22/FEX-D2](https://github.com/Dmage22/FEX-D2): the fixes that made Blizzard's protected launchers run under Wine
  ARM64EC, which this work started from.
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton), [Mesa](https://gitlab.freedesktop.org/mesa/mesa)
  (Turnip) and GameNative's [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android): our builds apply patches
  on top of them; each keeps its own license.
- [GameNative](https://github.com/utkarshdalal/GameNative), which runs it all on Android.
