# Diablo II: Resurrected on the AYN Thor: what we built and found

How the setup in [d2r-thor-setup.md](d2r-thor-setup.md) came about: what this fork changes, what we measured, and what
didn't work. State as of 2026-10-30 (FEX build 127, VKD3D fex37, Turnip fex17, frame generation layer 13). Every FEX
build was tested on the device with a per-second stats log; the numbers below come from those logs.

## Device

| | |
|---|---|
| Handheld | AYN Thor, Snapdragon 8 Gen 2 (1x Cortex-X3 on core 7, 4x A715/A710 on cores 3-6, 3x A510 on cores 0-2), Adreno 740 at up to 680 MHz, 120 Hz screen |
| App | GameNative, D2R Steam version, Proton 11.0-1 arm64ec |
| Cores | 3 to 7 for the game; FEX puts its helpers and the game's loader thread on 0 to 2 |

## Where it stands

- 60 FPS shown from a steady 30 real frames (frame generation 2x, flow scale 0.5). In the build 126 run, 178 s of play
  had no frame over 41 ms; the slowest frames left are the ones just before a loading screen (60-70 ms).
- The GPU is the limit in fights. Each D2R draw costs a fixed amount of GPU time (measured after build 97: shadow pass
  ~2.1 µs, depth prepass ~3.0 µs, main pass ~6.0 µs per draw), so a frame's GPU time grows with the draw count, not the
  resolution. With frame generation the game still holds 30 real frames in fights of 6,000 draws per frame.
- 60 real frames are out of reach in fights on this GPU.

## What this fork changes

### FEX (`.wcp`, `Source/Windows/`)

- Built by GitHub Actions with llvm-mingw (`.github/workflows/build-fexcore-wcp.yml`) on top of Dmage22's `d2r/clean`
  D2R fixes. Two variants per build: `thor` (with the stats log) and `thor-clean` (no log, no measuring, a small crash
  note on an error exit). Both target armv8.4-a and tune for Cortex-X3.
- **D2R defaults built in** (`Source/Windows/Common/Switches.cpp`): every fix is on for `D2R.exe` without environment
  variables, and each has a `FEX_*` switch to turn it off ([d2r-switches.md](d2r-switches.md)). The log's `# switches:`
  line lists them.
- **Stats log** (`thor` only): per-second CPU, scheduling and wait times per thread, JIT and disk cache work, frame times
  with a breakdown of every slow frame, memory and storage use, and the VKD3D, Turnip and frame generation layer reports.
- **Hitch fixes, each found in a log first:**
  - Self-modifying protection code: pages written over and over stay writable and their code checks itself instead of
    faulting on every write (build 54).
  - Controller rumble: the game's render workers blocked in `XInputSetState`; rumble is handed off (build 53).
  - Android's low-memory killer closed other apps at area loads, which then restarted on all cores for ~35 s: the game's
    data files are kept out of Android's file cache (builds 59-61).
  - The game's loader/protection thread competed with the game on the big cores: it is found and moved to the little
    cores, while game threads are never moved (builds 62-65, 81).
  - The main thread runs alone on the fastest core (core 7); other programs (steam.exe, the launcher) are kept off it
    (builds 49, 76, 77). Android's own work (memory reclaim, root tasks) still lands there, so the main thread leaves
    core 7 within milliseconds when it has to wait for it and stays on the other big cores (builds 117-123).
  - The game's job workers waited for a CPU in busy scenes: they run at nice -10 (builds 87-88), the wineserver too.
  - The code buffer filled up mid-play and every thread had to reload its code: 128 MB from the start, renewed at
    loading screens, and code that comes back the same is reused (builds 83-85, 94, 95).
  - Game data read from storage during play: the reads are learned and read in at the next loading screen (builds 89-90).
  - FEX's own locks waited through Wine's shared wait lists, whose user-mode spin locks held every waiting thread for
    ~120 ms when one was pushed off its CPU: they now wait in the Linux kernel (build 125).
  - VKD3D's queue thread waited 163 ms for a CPU on core 7 during memory reclaim: VKD3D's frame threads run at nice
    -10 and stay off core 7 (build 126).
- **Disk cache file mapping without the unixlib** (build 24): GameNative ships only the PE DLLs, so every cache hit was a
  separate file read through Wine. FEX now maps the file through a Windows section (block read 11.9 -> 3.6 µs).

### VKD3D (`vkd3d-patches/`, vkd3d-proton 3.0b as native ARM64EC)

Native ARM64EC gave about +5 FPS over GameNative's x86 build. The patches (37 so far, each described in its release):

- Frame pacing: the next frame starts only as late as it can without missing its slot (without frame generation a frame
  took ~43 ms from start to screen instead of ~70); the frame cap's wait no longer holds the only VkQueue; presents run
  on their own thread.
- A frame goes to the window system only once the GPU has drawn it (GameNative's X server shows an image as soon as it
  gets it).
- The game is kept at the frame latency it set (a timed-out wait at startup used to leave a third frame in flight).
- GPU memory is made ahead on a background thread (D2R creates heaps while it is played).
- D2R's 3D lighting table blur (compute, run in bursts when entering an area) is swapped for our version: 30 ms ->
  2.2 ms per run. The ground clutter shader runs its cheap tests first. The empty second shadow map isn't cleared again
  while it is still empty.
- Each frame is also made without the game's UI layer, for frame generation (fex35-37).
- A GPU pass meter, present timeline and draw probe for the stats log (off in the clean FEX).

### Turnip (`turnip-patches/`, Mesa 26.3.0-rc2)

- Early preambles for vkd3d's root constant buffers: D2R's lighting draws ~45% cheaper (fex2).
- The command processor loads each draw's root constant buffer ranges, and indexed ones go through the shader's UBO
  table, so a new root buffer per draw no longer resets the descriptor state: busy frames 1.4-1.8 ms less GPU (fex9-10).
- Depth and color clears at the start of a pass as one 3D draw (fex3, fex8), compute loop unrolling (fex8), a cached
  memory budget query (fex5), shader preamble loads issued first (fex6).
- GPU counters per pass kind for the stats log (off in the clean FEX).

### Frame generation layer (`tools/lsfg/d2r.patch`, GameNative's lsfg-vk-android `abc77c8`)

- The first four fixes (2026-10-13): the crash from tracking only the last VkInstance, a wrong wait stage, one lock for
  the shared queue, and the pacer stall on late frames (15 FPS locks).
- Layer 7-10: the layer presents from its own thread (nice -20) instead of inside the game's present, starts each frame's
  generation at the game's blit, sends a generated frame only once drawn, wakes 1 ms early to hit its slot, and makes no
  generated frame across a freeze.
- Layer 11-13: frame generation runs on VKD3D's frame without the UI; UI pixels come from the real frame, moved halfway
  along their own motion per 16x16 tile. The map overlay and HUD don't smear and item names move with the world, for
  ~2 ms of GPU time per real frame.

### Releases

`bundle-d2r-release.yml` publishes all parts as one release (`d2r-thor-test-N` while in development) with the notes in
`tools/release/d2r-thor-notes.md`. The parts come from their own releases (`fexcore-*`, `vkd3d-proton-*`, `turnip-*`,
`lsfg-d2r-*`), listed in `tools/release/d2r-thor-assets.txt`.

## Tried and dropped

| Idea | Result |
|---|---|
| TSO off for D2R.exe | No gain, and unsafe without volatile metadata |
| PAUSE as ISB instead of YIELD | No measurable change; reset everyone's disk cache |
| Caching the executable-range query (build 27) | D2R crashed within 2 s: its protector flips page protections without telling FEX |
| Frame latency 2 instead of 3 (build 50) | No gain |
| Global "no preamble" in Turnip (build 68), descriptor cache fix (fex1), fast Z clears (fex4) | No gain |
| Read-only depth not written back (VKD3D fex5) | No effect: D2R's depth is always writable |
| Light list read-ahead in shader loops (fex7) | No gain |
| Pipeline switches sending only changes (fex11) | 74% less command processor work, no GPU gain |
| Lighting shaders without alias moves (fex12), without preamble (fex13), other wave setups and 128-pixel waves (fex14) | No gain or slower |
| Clutter shader atomics once per wave (fex16) | 5.9% slower |
| Keeping unchanged constants loaded (fex17) | No effect |
| Batching draws (draw probe, VKD3D fex23-28) | Does not pay on this GPU; the per-draw cost is the lighting shader's run time |
| Lower resolution scale | More FPS, but no fix for hitches, pacing or input lag |
| Flow scale 0.25 | Cheaper, but smears items; 0.5 is the setting |
| All 8 cores for the game | Worse: game threads land on the little cores (15-19 FPS in fights) |
| Precompiling D2R.exe ahead of time | Not possible: D2R.exe is encrypted on disk |

## Things to know

- **Adding any FEX option that affects code generation resets the disk cache**, because the cache is keyed on those
  settings. Builds from this fork avoid that.
- The disk cache is in `AppData\Local\fex-emu\DiskCache` in the container. Back it up once it's well filled.
- GameNative re-copies its own frame generation layer on updates; copy ours back afterwards.
- Android's memory reclaim (kswapd) runs whenever free RAM gets low, mostly on core 7. FEX can move the game's threads
  away from it but can't stop it.

## Still open

| Item | State |
|---|---|
| The frame just before a loading screen (60-70 ms) | The game reads data and runs new code there |
| Very big fights | GPU-bound; 30 real frames held in earlier frame generation tests |
| One game thread waits ~0.12 s per second for a CPU | No visible effect found yet |
| Final release | Remove the measuring code from VKD3D, Turnip and the layer, not just switch it off |
| Send the layer fixes upstream to GameNative | Would survive app updates |
