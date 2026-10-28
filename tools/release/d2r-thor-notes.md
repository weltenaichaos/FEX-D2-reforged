> **TEST VERSION, still in development.** This bundle is a snapshot of the current work, made on 2026-10-28 so that
> all parts are in one place. It is tested on one AYN Thor only. Expect changes, and keep a backup of your container.

Everything needed to run **Diablo II Resurrected (Steam)** on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740) in
**GameNative**: our FEX build, our VKD3D build and our Turnip graphics driver, all made for this game.

## Where it stands

- **Calm play and most fights: a steady 40 FPS** with the Thor's screen at 120 Hz and GameNative's limiter at 40.
  In the last test run, more than two minutes of play went by without a single frame over 40 ms.
- **Big fights with many monsters: about 32 to 38 FPS.** The graphics chip is the limit there. That is the part we
  are working on now.
- Rubber banding, the hitches every few seconds and the long freezes from earlier builds are fixed.
- About 43 ms from the game starting a frame to the frame being on screen.

## Files

| File | What it is |
|---|---|
| `fexcore-2610-d2rfix28s-thor.wcp` | **FEX build 97, test build.** The x86 translator with all D2R fixes. Writes a log file per session to `D:\` (see "Reporting problems"). All testing was done with this one. |
| `fexcore-2610-d2rfix28-thor-clean.wcp` | FEX build 97 with the same fixes, but without the log file. Less tested. |
| `vkd3d-proton-3.0b-arm64ec-fex27.wcp` | **VKD3D fex27.** vkd3d-proton 3.0b (DX12 to Vulkan) as native ARM64EC code, with our fixes for frame pacing, lower lag and hitches. |
| `turnip-26.3.0-rc2-fex8.zip` | **Turnip fex8.** Mesa 26.3.0-rc2 Adreno driver with our fixes for D2R's lighting and clears (cheaper GPU work per frame). |
| `liblsfg-vk-layer.so` | Optional. Fixed frame generation layer for GameNative's Lossless Scaling support. Not used or tested with this version. |

## What you need first

- GameNative with D2R (Steam version) installed in its own container.
- **GameNative's Proton 11.0-1 (arm64ec)** in that container. GameNative's 11.0-2 crashes D2R.
- Leave `D2R_loader.dll` in the game folder. It belongs to the game, and D2R crashes without it.

## Installation

### 1. Import the parts

1. Download the two FEX `.wcp` files (or just the test build), the VKD3D `.wcp` and the Turnip `.zip` to the Thor.
2. **Delete any older `2610-d2rfix28s-thor` and `2610-d2rfix28-thor-clean` first** in GameNative's component list.
   Every FEX build uses the same name, so GameNative would otherwise keep the old one.
3. Import the `.wcp` files in GameNative as custom components.
4. Import `turnip-26.3.0-rc2-fex8.zip` as a custom graphics driver.

### 2. Container settings

- FEXCore: `2610-d2rfix28s-thor` (or `2610-d2rfix28-thor-clean`)
- VKD3D: `3.0b-arm64ec-fex27`
- Graphics driver: `Turnip 26.3.0-rc2 FEX fex8 (build 10)`
- DXVK: 2.7.1
- **CPU cores: 3 to 7.** FEX itself moves its helper threads and the game's loader thread to the small cores.
- Wrapper **Max Device Memory: 2 GB** (prevents out-of-memory crashes).
- Environment variables:
  - `FEX_DISKCACHE=1` (keeps translated code between sessions; important against stutter)
  - `VKD3D_CONFIG=no_upload_hvv,nodxr`
  - `MESA_SHADER_CACHE_MAX_SIZE=4096MB`
  - Nothing else is needed. All D2R fixes in FEX, VKD3D and Turnip are on by default for `D2R.exe`.
  - Do **not** set `VKD3D_DISABLE_EXTENSIONS` (locks the game at 12 FPS).
  - Do **not** set `VKD3D_FEX_DRAW_PROBE` (a measuring test that ends by crashing the game).

### 3. Frame rate

- Set the **Thor's screen to 120 Hz**. At 60 Hz, a limit of 40 runs at 30.
- Set GameNative's **FPS limiter to 40 before launching**. Frame generation off.
- In D2R: **VSync off**, frame rate cap **unlimited**.

Start the game. The first sessions stutter when new areas load, because FEX is still filling its code cache. That gets
better the more you play.

## Still in development

- Big fights below 40 FPS, as above. Each draw in the game's main pass costs the graphics chip about twice as much as
  the same kind of draw in our own test, and finding out why is the next step.
- The measuring parts are still in this version (frame timing in VKD3D, the FEX log in the test build). They cost
  little. The final release removes all of them.
- Frame generation has not been tested with this version.

## Keep your code cache

Once you have played through most areas, back up the folder `AppData\Local\fex-emu\DiskCache` from the container's C:
drive. If the cache ever gets reset (for example by a FEX update), put the folder back and the game is smooth again
right away. A D2R patch changes the game code, so after a patch part of the cache is rebuilt while you play.

## Reporting problems

The test build writes `D:\fexstats-D2R.exe-b97-<time>-<pid>.txt` for every session. The `b97` in the name shows that
the new FEX is really in use. Send that file with your report.

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| Runs at 30 FPS with the limiter at 40 | The screen is at 60 Hz. Set it to 120 Hz. |
| Locked at 12 FPS | `VKD3D_DISABLE_EXTENSIONS` is set. Remove it. |
| Game crashes about 40 s after the start | `VKD3D_FEX_DRAW_PROBE` is set. Remove it. |
| Log file name doesn't show `b97` | GameNative kept an older FEX. Delete all old FEX components and import again. |
| Game crashes at start | Check GameNative's Proton 11.0-1 (not 11.0-2) and that `D2R_loader.dll` is in place. |
| Stutter in new areas | Normal while the FEX cache fills. Keep `FEX_DISKCACHE=1` and back up the cache once it is full. |

## Built from

| Part | Source |
|---|---|
| FEX build 97 | this repository, commit 7006d95 (releases `fexcore-2610-d2rfix28s-thor-97`, `fexcore-2610-d2rfix28-thor-clean-97`) |
| VKD3D fex27 | vkd3d-proton v3.0b plus `vkd3d-patches/` 0001 to 0027 (release `vkd3d-proton-3.0b-arm64ec-fex27-29`) |
| Turnip fex8 | Mesa 26.3.0-rc2 plus `turnip-patches/` 0001 to 0008 (release `turnip-26.3.0-rc2-fex8-10`) |
| Frame gen layer | GameNative's lsfg-vk-android plus `tools/lsfg/d2r.patch` (release `lsfg-d2r-6`) |
