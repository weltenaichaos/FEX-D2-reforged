Everything needed to run **Diablo II Resurrected (Steam)** on the **AYN Thor** (Snapdragon 8 Gen 2) in **GameNative**, as of 2026-10-13.

Result: a steady **60 FPS shown** (30 real frames + frame generation 2x), also in fights.

## Files

| File | What it is |
|---|---|
| `fexcore-2610-d2rfix28-thor-clean.wcp` | **FEX for normal play** (build 24). The x86 translator with the D2R fixes, tuned for the Thor, with faster disk cache loading. |
| `fexcore-2610-d2rfix28s-thor.wcp` | Same FEX with an optional stats log. Only for diagnosing problems. |
| `vkd3d-proton-3.0b-arm64ec.wcp` | vkd3d-proton 3.0b (DX12 to Vulkan) built as native ARM64EC code, about +5 FPS over the x86 build. |
| `liblsfg-vk-layer.so` | Fixed **frame generation layer** for GameNative's Lossless Scaling support. Fixes the D2R crash, the 12 and 15 FPS locks, and the late-frame stutter. |

## What you need first

- GameNative with D2R (Steam version) installed in its own container.
- **Proton 11.0-1 (arm64ec)** in that container. 11.0-2 crashes D2R.
- A Turnip Adreno driver, and DXVK 2.7.1.
- For frame gen: Lossless Scaling owned on Steam, so GameNative can use its `Lossless.dll`.
- Leave `D2R_loader.dll` in the game folder. It belongs to the game, and D2R crashes without it.

## Installation

### 1. FEX and VKD3D

1. Download `fexcore-2610-d2rfix28-thor-clean.wcp` and `vkd3d-proton-3.0b-arm64ec.wcp` to the phone.
2. **If you already have an older `2610-d2rfix28-thor-clean`, delete it first** in GameNative's component list. Every build uses the same version name, so GameNative would otherwise keep the old one.
3. Import both `.wcp` files in GameNative as custom components.
4. In the D2R container settings, select:
   - FEXCore: `2610-d2rfix28-thor-clean`
   - VKD3D: `vkd3d-proton-3.0b-arm64ec`
   - DXVK: 2.7.1

### 2. Container settings

- **CPU cores: 3 to 7 only.** Turn off cores 0 to 2. Those are the slow little cores, and using them makes fights drop to 15 to 19 FPS.
- Wrapper **Max Device Memory: 2 GB** (prevents out-of-memory crashes).
- Environment variables:
  - `FEX_DISKCACHE=1` (keeps translated code between sessions; important against stutter)
  - `VKD3D_CONFIG=no_upload_hvv,nodxr`
  - `MESA_SHADER_CACHE_MAX_SIZE=4096MB`
  - Do **not** set `VKD3D_DISABLE_EXTENSIONS`. It locks the game at 12 FPS.

### 3. Frame generation

1. In GameNative, turn on frame generation (Lossless Scaling) for the D2R container with:
   - Multiplier **2x**
   - Flow scale **0.25**
   - Performance Mode **on**
   - Present mode **Mailbox**
2. Start the game once and quit. GameNative then installs its own copy of the layer into the container.
3. Replace that copy with this release's `liblsfg-vk-layer.so`. Use the Wine file manager in the container and copy it over
   `Z:\home\xuser-<your container>\.local\lib\liblsfg-vk-layer.so`
   The correct file is **2,641,968 bytes** (shown as about 2.50 MB).

### 4. Frame rate and in-game settings

- Set GameNative's **FPS limiter to 30 before launching.** With frame gen on, this caps the real frames and the layer doubles them to 60.
- In D2R: **VSync off**, frame rate cap **unlimited**.

Start the game. The first sessions stutter when new areas load, because FEX is still filling its code cache. That gets better the more you play.

## After a GameNative update

GameNative re-copies its own frame gen layer when it updates. If the flicker, crash or FPS locks come back, redo step 3.3 and check the file size again.

## Keep your code cache

Once you have played through most areas, back up the folder `AppData\Local\fex-emu\DiskCache` from the container's C: drive. If the cache ever gets reset (for example by a FEX update), put the folder back and the game is smooth again right away. A D2R patch changes the game code, so after a patch part of the cache is rebuilt while you play.

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| Locked at 12 FPS | `VKD3D_DISABLE_EXTENSIONS` is set. Remove it. |
| Locked at 15 FPS, or the game won't start with frame gen | The old layer is active. Redo step 3.3. |
| Heavy flicker or crash at start with frame gen | The old layer is active. Redo step 3.3. |
| Fights drop below 20 FPS | Check that only cores 3 to 7 are enabled. |
| Stutter in new areas | Normal while the FEX cache fills. Keep `FEX_DISKCACHE=1` and back up the cache once it is full. |
| Game crashes at start | Check Proton 11.0-1 (not 11.0-2) and that `D2R_loader.dll` is in place. |

## Diagnosis (optional)

Use `fexcore-2610-d2rfix28s-thor.wcp` instead of thor-clean and set `FEX_STATSLOG=D:\` and `FEX_STATSLOG_APP=D2R.exe`. FEX then writes a per-second log `fexstats-D2R.exe-<time>-<pid>.txt` to that folder.
