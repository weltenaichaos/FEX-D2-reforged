# Diablo II: Resurrected on the AYN Thor: setup guide

Everything needed to run **Diablo II Resurrected (Steam)** on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740) in
**GameNative**, as of 2026-10-30 (TEST version 4).

Result: **60 FPS shown**, made from a steady 30 real frames with frame generation 2x. In the last test run, 3 minutes
of play went by without a single frame over 41 ms.

## Files

All files are in [TEST version 4](https://github.com/weltenaichaos/FEX-D2-reforged/releases/tag/d2r-thor-test-4).

| File | What it is |
|---|---|
| `fexcore-2610-d2rfix28-thor-clean.wcp` | **FEX for playing** (build 127). The x86 translator with all D2R fixes. No log file and no measuring. If the game ever crashes, it writes one small file `D:\fexcrash-D2R.exe-b127-<time>-<pid>.txt`. |
| `fexcore-2610-d2rfix28s-thor.wcp` | **FEX for reports** (build 127). The same fixes, plus a per-second log `D:\fexstats-D2R.exe-b127-<time>-<pid>.txt` for every session. |
| `vkd3d-proton-3.0b-arm64ec-fex37.wcp` | **VKD3D fex37.** vkd3d-proton 3.0b (DX12 to Vulkan) as native ARM64EC code, with our D2R fixes. |
| `turnip-26.3.0-rc2-fex17.zip` | **Turnip fex17.** Mesa 26.3.0-rc2 Adreno driver with our D2R fixes. |
| `liblsfg-vk-layer.so` | **Frame generation layer** (`lsfg-d2r-14`). Replaces GameNative's own Lossless Scaling layer. |

The two FEX files have different names in GameNative, so both can be imported. Switch the container's FEXCore setting
between them at any time; nothing else changes.

## What you need first

- GameNative with D2R (Steam version) installed in its own container.
- **GameNative's Proton 11.0-1 (arm64ec)** in that container. GameNative's 11.0-2 crashes D2R.
- Lossless Scaling owned on Steam, so GameNative can use its `Lossless.dll` for frame generation.
- Leave `D2R_loader.dll` in the game folder. It belongs to the game, and D2R crashes without it.

## Installation

### 1. Import FEX, VKD3D and Turnip

1. Download both FEX `.wcp` files, the VKD3D `.wcp` and the Turnip `.zip` to the Thor.
2. **Delete any older `2610-d2rfix28s-thor` and `2610-d2rfix28-thor-clean` first** in GameNative's component list.
   Every FEX build uses the same names, so GameNative would otherwise keep the old ones.
3. Import the `.wcp` files in GameNative as custom components.
4. Import `turnip-26.3.0-rc2-fex17.zip` as a custom graphics driver.

### 2. Container settings

- FEXCore: `2610-d2rfix28-thor-clean` (or `2610-d2rfix28s-thor` for a log)
- VKD3D: `3.0b-arm64ec-fex37`
- Graphics driver: `Turnip 26.3.0-rc2 FEX fex17 (build 21)`
- DXVK: 2.7.1
- **CPU cores: 3 to 7 only.** Cores 0 to 2 are the slow little cores. FEX itself moves its helper threads and the
  game's loader thread there.
- Wrapper **Max Device Memory: 2 GB** (prevents out-of-memory crashes).
- Environment variables:
  - `FEX_DISKCACHE=1` (keeps translated code between sessions; important against stutter)
  - `VKD3D_CONFIG=no_upload_hvv,nodxr`
  - `MESA_SHADER_CACHE_MAX_SIZE=4096MB`
  - Nothing else is needed. All D2R fixes in FEX, VKD3D, Turnip and the layer are on by default for `D2R.exe`.
  - Do **not** set `VKD3D_DISABLE_EXTENSIONS` (locks the game at 12 FPS).
  - Do **not** set `VKD3D_FEX_DRAW_PROBE` (a measuring test that ends by crashing the game).

### 3. Frame generation

1. In GameNative, turn on frame generation (Lossless Scaling) for the D2R container with:
   - Multiplier **2x**
   - FPS limit **30**
   - Flow scale **0.5** (lower values smear items)
   - Performance Mode **on**
2. Start the game once and quit. GameNative then installs its own copy of the layer into the container.
3. Replace that copy with this release's `liblsfg-vk-layer.so`. Use the Wine file manager in the container and copy it over
   `Z:\home\xuser-STEAM_2536520\.local\lib\liblsfg-vk-layer.so`
   The correct file is **2,755,712 bytes**.

### 4. Screen and in-game settings

- Set the **Thor's screen to 120 Hz**.
- In D2R: **VSync off**.

Start the game. The first sessions stutter when new areas load, because FEX is still filling its code cache. That gets
better the more you play.

## After a GameNative update

GameNative re-copies its own frame generation layer when it updates. If smearing around the map overlay or HUD, a crash
at start or FPS locks come back, redo step 3.3 and check the file size again.

## Keep your code cache

Once you have played through most areas, back up the folder `AppData\Local\fex-emu\DiskCache` from the container's C:
drive. If the cache ever gets reset (for example by a FEX update), put the folder back and the game is smooth again right
away. A D2R patch changes the game code, so after a patch part of the cache is rebuilt while you play.

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| 30 FPS shown instead of 60 | Frame generation is off, or the screen is at 60 Hz. Turn it on (2x, limit 30) and set 120 Hz. |
| Smearing around items | Flow scale is below 0.5. Set it to 0.5. |
| Smearing around the map overlay or HUD, crash at start, or FPS locks with frame generation | GameNative's own layer is active. Redo step 3.3. |
| Locked at 12 FPS | `VKD3D_DISABLE_EXTENSIONS` is set. Remove it. |
| Game crashes about 40 s after the start | `VKD3D_FEX_DRAW_PROBE` is set. Remove it. |
| Log file name doesn't show `b127` | GameNative kept an older FEX. Delete all old FEX components and import again. |
| Game crashes at start | Check GameNative's Proton 11.0-1 (not 11.0-2) and that `D2R_loader.dll` is in place. |
| Stutter in new areas | Normal while the FEX cache fills. Keep `FEX_DISKCACHE=1` and back up the cache once it is full. |

## Reporting problems

Switch the container to `2610-d2rfix28s-thor` and play until the problem shows up. Send the session's
`D:\fexstats-D2R.exe-b127-<time>-<pid>.txt`. If the clean version crashed, send `D:\fexcrash-D2R.exe-b127-...txt`.
