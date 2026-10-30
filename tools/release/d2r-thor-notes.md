> **TEST VERSION, still in development.** Made on 2026-10-30 for longer test sessions. It is tested on one AYN Thor
> only. Expect changes, and keep a backup of your container.

Everything needed to run **Diablo II Resurrected (Steam)** on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740) in
**GameNative**: our FEX build, our VKD3D build, our Turnip graphics driver and our frame generation layer, all made
for this game.

## Where it stands

- **60 FPS on screen, made from a steady 30 FPS** with frame generation (every second frame is generated).
- **No hitches in normal play.** In the last test run (FEX build 126), 3 minutes of play went by without a single frame
  over 41 ms. The run before had 6 frames over 50 ms, the worst at 161 ms.
- The map overlay, HUD and item names on the ground stay sharp and move smoothly. Frame generation no longer smears
  around them.
- Fights with up to about 4,600 draws per frame ran smoothly in the last run. Very big fights were not part of it (see
  "What is still open").

## Two FEX versions: pick one

Both have exactly the same fixes. They have different names in GameNative, so you can import both and switch the
container's FEXCore setting between them at any time. Nothing else needs to change.

| File | Name in GameNative | What it is |
|---|---|---|
| `fexcore-2610-d2rfix28-thor-clean.wcp` | `2610-d2rfix28-thor-clean` | **Clean version, for playing.** No log file and none of the measuring parts in FEX, VKD3D, Turnip and the frame gen layer. If the game ever crashes, it leaves one small file `D:\fexcrash-D2R.exe-b127-<time>-<pid>.txt` (a few KB) with the last errors. Nothing is written otherwise. |
| `fexcore-2610-d2rfix28s-thor.wcp` | `2610-d2rfix28s-thor` | **Logging version, for reporting problems.** Writes `D:\fexstats-D2R.exe-b127-<time>-<pid>.txt` for every session (several MB per minute) and runs the measuring parts. All testing was done with this one. |

Play with the clean version. If something feels wrong and it happens again, switch to the logging version, play until
it shows up, and send that session's log.

## Files

| File | What it is |
|---|---|
| the two FEX files | **FEX build 127**, see above. The x86 translator with all D2R fixes. |
| `vkd3d-proton-3.0b-arm64ec-fex37.wcp` | **VKD3D fex37.** vkd3d-proton 3.0b (DX12 to Vulkan) as native ARM64EC code, with our fixes for frame pacing, lag, hitches, cheaper GPU work, and the frames without the game's UI that frame generation uses. |
| `turnip-26.3.0-rc2-fex17.zip` | **Turnip fex17.** Mesa 26.3.0-rc2 Adreno driver with our fixes for D2R's lighting, clears and per-draw cost. |
| `liblsfg-vk-layer.so` | **Frame generation layer 13** (release `lsfg-d2r-14`). Replaces GameNative's own layer. Required for the frame generation fixes. |

## What you need first

- GameNative with D2R (Steam version) installed in its own container.
- **GameNative's Proton 11.0-1 (arm64ec)** in that container. GameNative's 11.0-2 crashes D2R.
- Leave `D2R_loader.dll` in the game folder. It belongs to the game, and D2R crashes without it.

## Installation

### 1. Import the parts

1. Download both FEX `.wcp` files, the VKD3D `.wcp`, the Turnip `.zip` and `liblsfg-vk-layer.so` to the Thor.
2. **Delete any older `2610-d2rfix28s-thor` and `2610-d2rfix28-thor-clean` first** in GameNative's component list.
   Every FEX build uses the same names, so GameNative would otherwise keep the old ones.
3. Import the `.wcp` files in GameNative as custom components.
4. Import `turnip-26.3.0-rc2-fex17.zip` as a custom graphics driver.
5. Replace `Z:\home\xuser-STEAM_2536520\.local\lib\liblsfg-vk-layer.so` (inside the container) with the downloaded
   `liblsfg-vk-layer.so`.

### 2. Container settings

- FEXCore: `2610-d2rfix28-thor-clean` (or `2610-d2rfix28s-thor` for a log)
- VKD3D: `3.0b-arm64ec-fex37`
- Graphics driver: `Turnip 26.3.0-rc2 FEX fex17 (build 21)`
- DXVK: 2.7.1
- **CPU cores: 3 to 7.** FEX itself moves its helper threads and the game's loader thread to the small cores.
- Wrapper **Max Device Memory: 2 GB** (prevents out-of-memory crashes).
- Environment variables:
  - `FEX_DISKCACHE=1` (keeps translated code between sessions; important against stutter)
  - `VKD3D_CONFIG=no_upload_hvv,nodxr`
  - `MESA_SHADER_CACHE_MAX_SIZE=4096MB`
  - Nothing else is needed. All D2R fixes in FEX, VKD3D, Turnip and the layer are on by default for `D2R.exe`.
  - Do **not** set `VKD3D_DISABLE_EXTENSIONS` (locks the game at 12 FPS).
  - Do **not** set `VKD3D_FEX_DRAW_PROBE` (a measuring test that ends by crashing the game).

### 3. Frame rate and frame generation

- Set the **Thor's screen to 120 Hz**.
- GameNative's frame generation: **on, multiplier 2, FPS limit 30, flow scale 0.5, performance mode on.** That shows
  60 FPS made from 30. Flow scale 0.5 is what removed the smearing on items.
- In D2R: **VSync off**.

Start the game. The first sessions stutter when new areas load, because FEX is still filling its code cache. That gets
better the more you play.

## What changed since test version 3 (FEX build 97)

**Frame generation (new):** 60 FPS on screen from a 30 FPS game, with our own fixes to GameNative's frame generation
layer and VKD3D:
- The layer presents frames from its own thread instead of holding up the game, starts each generated frame as soon
  as the game's frame is ready, sends a generated frame only once the GPU has drawn it, and times every frame exactly
  to the screen.
- After a freeze it doesn't generate a frame between two pictures far apart in time (that once put a chest's label in
  the middle of the screen).
- VKD3D hands frame generation each frame without the game's UI as well. Frame generation works on that, and the UI is
  taken from the real frame and moved halfway along its own motion. So the map overlay and HUD don't smear, and item
  names on the ground move with the world.

**Cheaper GPU work (Turnip fex9 to fex17, VKD3D fex28 to fex34):**
- The driver loads the game's per-draw constants before each draw instead of each lighting shader fetching them:
  busy frames 1.4 to 1.8 ms less GPU time.
- The game's lighting table blur (run in bursts when entering an area) takes 2.2 ms instead of 30 ms. Entering an area
  no longer causes hitches from it.
- The ground clutter shader does its cheap tests first (up to 42% less work in town). In town, the game's empty
  second shadow map is no longer cleared every frame while it is still empty (0.55 to 0.69 ms less per frame there).

**Hitches from Android (FEX builds 117 to 126):**
- Android's own work (its memory cleanup and other apps) lands on the Thor's fastest core, where the game's main
  thread runs. The main thread now leaves that core within milliseconds when it has to wait there, and stays on the
  other big cores.
- The game's other threads may use that core meanwhile. VKD3D's frame threads stay off it and run with higher
  priority (build 126 fixed a 161 ms freeze from this).

**Lock freeze (FEX build 125):** FEX's internal locks now wait through the Linux kernel instead of Wine's shared wait
lists, whose spin locks could hold every thread for a moment when one was pushed off its core. This fixed a 144 ms
freeze.

**Clean version (FEX build 127):** the clean version now also turns off the frame generation layer's GPU timers, and
leaves a small crash note if the game ever crashes.

## What is still open

- **Loading screens:** the last frame before a loading screen can take 60 to 70 ms while the game reads data and runs
  new code. It happens only when changing areas.
- **Very big fights** (6,000 draws per frame and more) were not part of the last test. The graphics chip is the limit
  there. In earlier tests with frame generation, the game still held 30 FPS (60 shown) in such fights.
- **60 FPS without frame generation** is not possible in fights; the graphics chip is too slow for that.
- **Android can still interrupt:** its memory cleanup and other apps can briefly take CPU time. FEX moves the game's
  important threads away from that work as fast as it can, but it can't stop Android doing it.
- **The clean version is less tested.** An older clean build (FEX 111) crashed once after about 6 minutes. That build
  had no log, so the cause is unknown, and many fixes came after it. If this one crashes, send the `fexcrash` file.
- One of the game's threads waits about 0.12 s per second for a free core. No visible effect has been found so far.
- The measuring code is still inside VKD3D, Turnip and the layer. The clean FEX switches it off. The final release
  removes it completely.

## Keep your code cache

Once you have played through most areas, back up the folder `AppData\Local\fex-emu\DiskCache` from the container's C:
drive. If the cache ever gets reset (for example by a FEX update), put the folder back and the game is smooth again
right away. A D2R patch changes the game code, so after a patch part of the cache is rebuilt while you play.

## Reporting problems

Switch the container to the logging version (`2610-d2rfix28s-thor`) and play until the problem shows up. It writes
`D:\fexstats-D2R.exe-b127-<time>-<pid>.txt` for every session. The `b127` in the name shows that the new FEX is really in
use. Send that file with your report. If the clean version crashed, send `D:\fexcrash-D2R.exe-b127-...txt`.

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| 30 FPS shown instead of 60 | Frame generation is off, or the screen is at 60 Hz. Turn it on (x2, limit 30) and set 120 Hz. |
| Smearing around items | Flow scale is below 0.5. Set it to 0.5. |
| Smearing around the map overlay or HUD | GameNative's own layer is still in place. Replace `liblsfg-vk-layer.so` as in step 1. |
| Locked at 12 FPS | `VKD3D_DISABLE_EXTENSIONS` is set. Remove it. |
| Game crashes about 40 s after the start | `VKD3D_FEX_DRAW_PROBE` is set. Remove it. |
| Log file name doesn't show `b127` | GameNative kept an older FEX. Delete all old FEX components and import again. |
| Game crashes at start | Check GameNative's Proton 11.0-1 (not 11.0-2) and that `D2R_loader.dll` is in place. |
| Stutter in new areas | Normal while the FEX cache fills. Keep `FEX_DISKCACHE=1` and back up the cache once it is full. |

## Built from

| Part | Source |
|---|---|
| FEX build 127 | this repository, commit f34c503 (releases `fexcore-2610-d2rfix28s-thor-127`, `fexcore-2610-d2rfix28-thor-clean-127`) |
| VKD3D fex37 | vkd3d-proton v3.0b plus `vkd3d-patches/` 0001 to 0037 (release `vkd3d-proton-3.0b-arm64ec-fex37-39`) |
| Turnip fex17 | Mesa 26.3.0-rc2 plus `turnip-patches/` 0001 to 0017 (release `turnip-26.3.0-rc2-fex17-21`) |
| Frame gen layer 13 | GameNative's lsfg-vk-android plus `tools/lsfg/d2r.patch` (release `lsfg-d2r-14`) |
