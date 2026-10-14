# Diablo II: Resurrected on the AYN Thor: what we built and found

How the setup in [d2r-thor-setup.md](d2r-thor-setup.md) came about: what this fork changes, what we measured, and what didn't work. State as of 2026-10-14.

## Device

| | |
|---|---|
| Handheld | AYN Thor, Snapdragon 8 Gen 2 (1x Cortex-X3, 4x A715/A710, 3x A510), Adreno 740, Turnip driver |
| App | GameNative, D2R Steam version, Proton 11.0-1 arm64ec |
| Cores | 3 to 7 only (the three A510 little cores are switched off for the game) |

## What this fork changes

### FEX builds (`.wcp`)

- Built by GitHub Actions with llvm-mingw (`.github/workflows/build-fexcore-wcp.yml`), on top of Dmage22's `d2r/clean` D2R fixes (d2rfix28).
- Three variants: `thor-clean` (for play), `thor` (with the stats log), `base` (Dmage22's settings unchanged). The Thor builds target armv8.4-a and tune for Cortex-X3. LTO stays off; it breaks ARM64EC builds.
- **Stats log** (`thor` only): `FEX_STATSLOG=<folder>` writes per-second, per-thread numbers: CPU, share of time in emulated code, JIT time, SMC, disk cache hits and a breakdown of disk cache lookup cost. `FEX_STATSLOG_PROFILE=1` adds the hottest translated blocks. It reads JIT block headers instead of suspending threads, because suspending threads crashed D2R. It also avoids FEX's Windows CRT stubs (`fflush`, `printf`), which terminate the process.
- **Disk cache file mapping without the unixlib** (build 24): GameNative ships only the PE DLLs, so FEX's normal way of mapping its code cache file was never available, and every cache hit was a separate file read through Wine. FEX now maps the file through a Windows section. Measured: reading a cached block went from 11.9 to 3.6 µs, and a whole lookup from 43 to 36 µs. `FEX_DISKCACHEFILEMAPPING=0` turns it off.

### vkd3d-proton ARM64EC

vkd3d-proton 3.0b built as native ARM64EC code (`build-vkd3d-arm64ec-wcp.yml`). GameNative's default build is x86-64 and runs through FEX. Measured gain: about +5 FPS.

### Frame generation layer

GameNative's Lossless Scaling layer (`liblsfg-vk-layer.so`, from `GameNative/lsfg-vk-android`) crashed or flickered with D2R. We build it from upstream commit `abc77c8` plus `tools/lsfg/d2r.patch` (`build-lsfg-layer.yml`). The fixes:

1. **Wrong Vulkan instance (the crash).** The layer remembered only the last VkInstance created. D2R creates several, so the layer called into the wrong or a destroyed one. It now tracks which instance owns each GPU.
2. **Wrong wait stage.** An internal submit waited at `TOP_OF_PIPE`, which doesn't really wait. Changed to `ALL_COMMANDS`.
3. **Queue locking.** Turnip exposes only one queue, which the game and the layer share. All submits now go through one lock. Harmless, but it turned out not to be the flicker cause.
4. **Pacer stall on late frames.** When a real frame arrived more than one vsync slot late, the pacer pushed it to the next slot. That snowballed into 15 FPS locks and stutter. Late frames are now shown immediately and the pacer re-anchors on them.

Debug output (GPU timings, pacer stats) is behind `LSFG_DEBUG=1`.

### Bundle release

`bundle-d2r-release.yml` publishes one `d2r-thor-N` release with all four files, the setup guide as release notes, and checksums. The file list is `tools/release/d2r-thor-assets.txt`.

## Measurements

Frame gen 2x, flow scale 0.25:

| Scene | Limiter off (real / shown) | Limiter 30 (real / shown) |
|---|---|---|
| Town | 35-40 / 70-80 | 30 / 60 steady |
| Fights | 28-33 / 56-66 | 28-30 / 56-60, mostly 60 |

Uncapped looks better on paper, but the swings feel stuttery. The 30 cap gives even frame times.

- **Fights are CPU-bound.** Three game threads run at 90-96% load, and 97% of their time is translated x86 code. Little overhead outside the game's own code is left to cut.
- **Quiet zones are GPU-bound.** That's where render settings matter.
- **Frame gen is cheap on the GPU:** about 1.8 ms per generated frame plus 0.2 ms for the copy.
- **D2R.exe has no volatile metadata**, so FEX must keep strict x86 memory ordering for every access. Relaxing it (TSO off) gave no gain anyway.
- **Stutter** comes mostly from first-time translation of code that isn't in FEX's disk cache yet, on the game's main thread. Loading code that is already cached costs about 36 µs per block.

## Why earlier attempts were stuck at low FPS

- **12 FPS:** `VKD3D_DISABLE_EXTENSIONS` hid `present_wait`. vkd3d then waited on an event with an 80 ms timeout every frame.
- **15 FPS, or the game not starting with frame gen:** the layer pacer bug above (fix 4).
- **All 8 cores enabled:** worse (15-19 FPS in fights), because game threads landed on the little cores.

## Tried and dropped

| Idea | Result |
|---|---|
| TSO off for D2R.exe | No gain, and unsafe without volatile metadata |
| Uncapped real-frame spacing in the layer | Less stable than the 30 cap |
| PAUSE as ISB instead of YIELD (D2R spinlocks, main thread ~11% in one lock) | No measurable change (25-45 FPS both ways). Removed again, because adding it reset everyone's disk cache |
| Caching the executable-range query per thread (build 27) | Cut the lookup-key step from ~13 to ~1 µs, but D2R crashed within 2 seconds. D2R's protector flips page protections without telling FEX, so FEX must ask Windows fresh every time. Reverted |
| Flow scale 0.5 | Heavier and looked worse than 0.25 |
| In-game VSync | More latency, no gain; use Mailbox instead |
| Precompiling D2R.exe ahead of time | Not possible: D2R.exe is encrypted on disk and only decrypted in memory while running |

## Things to know

- **Adding any FEX option that affects code generation resets the disk cache**, because the cache is keyed on those settings. Builds from this fork avoid that.
- The disk cache is in `AppData\Local\fex-emu\DiskCache` in the container. Back it up once it's well filled.
- GameNative re-copies its own frame gen layer on updates; copy the fixed one back afterwards.

## Remaining ideas

| Idea | Expected gain | Effort |
|---|---|---|
| Send the layer fixes upstream to GameNative | Fixes survive app updates | Low |
| Raise render scale (GPU has headroom at a 30 cap) | Sharper image | Settings only |
| `VKD3D_SWAPCHAIN_LATENCY_FRAMES=1` | Less input lag (untested) | Settings only |
| Load already-cached code in the background at game start | Removes cache-load bursts, not first-time compiles | Days |
| Translate likely-next code ahead of time on the idle little cores | Could reduce first-time compile stutter | Weeks, risky with D2R's self-modifying protector |
