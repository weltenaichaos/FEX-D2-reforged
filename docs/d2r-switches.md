# FEX switches for D2R

Every fix in this fork has a switch. **For `D2R.exe` all fixes are on by default, so you never need to set any of
these.** They are there to turn one fix off for a comparison, or when you suspect a fix causes a problem. For every
other program FEX behaves like upstream FEX unless a switch is set.

State: FEX build 163, VKD3D fex42, Turnip fex17, frame generation layer 14, our Proton d2r12 (build 14). The build numbers in the
tables say where each fix came from; [d2r-thor-findings.md](d2r-thor-findings.md) has the background.

## How to set a switch

- In GameNative, open the container's settings and add an environment variable, for example `FEX_RUMBLE=keep`.
- A variable you set wins over the built-in default.
- On/off fixes are turned off with `0`.
- Switches that set a priority (a Linux "nice" value, lower = more important, -20 to 19) are turned off with `off`. On
  those, `0` means "nice 0", not off.
- The logging version lists every switch with its value in the `# switches:` line near the top of the log. Built-in
  values are marked `(default)`.
- VKD3D switches need our VKD3D (fex42), Turnip switches our Turnip (fex17), frame generation switches our layer. A
  VKD3D switch does nothing when its matching `VKD3D_FEX_*` variable is already set.
- If you changed any switch, say so when you report a problem.

## Game data and loading screens

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_NATIVE_ZLIB` | `1` | The game's data unpacking (zlib) runs as native ARM code instead of being translated. | `0` |
| `FEX_NATIVE_MD5` | `1` | The same for the game's MD5 checksums. | `0` |
| `FEX_INFLATE_FAST` | `1` | That unpacking uses FEX's own inner loop: input 8 bytes at a time, repeats copied 8-16 bytes at a time instead of byte by byte. In the b157 run the game unpacked 1.52 GB in play in 6.9 s of CPU, and one 93 ms frame was 38 ms of it on the main thread. On a PC this loop was 1.3-2.2x as fast as zlib's, with the same output on 16,000 test streams (build 158). | `0` |
| `FEX_UNPACK_CACHE` | `1` | Unpacked game data is stored in `AppData\Local\fex-emu\UnpackCache` (up to 16 GB), so the next load reads it instead of unpacking again. Needs `FEX_NATIVE_ZLIB`. | `0` |
| `FEX_UNPACK_CACHE_DIRECT` | `1` | That cache is read past Android's file cache, so it doesn't fill the device's memory (build 58). | `0` |
| `FEX_UNPACK_CACHE_PLAY` | `1` | During play, cache data that isn't in memory yet is left to the game to unpack, and read-ahead is shorter (build 62). | `0` |
| `FEX_UNPACK_CACHE_AHEAD` | `ramp` | Read-ahead grows along the game's reads instead of reading 8 MB past every hit (build 83). | `fixed` |
| `FEX_UNPACK_CACHE_MAIN_PLAY` | `1` | The game's main thread never waits for storage for cache data while frames are coming (build 90). | `0` |
| `FEX_UNPACK_CACHE_FRAMES` | `1` | The same for every thread, also while the game streams in a new area with many lookups. In the b157 run the game's threads read 4,150 pieces from storage in play this way, 0.76 ms each, where unpacking one takes about 0.2 ms, and the cache read 1.97 GB ahead, half of it never used. Loading screens keep the full cache (build 158). | `0` |
| `FEX_UNPACK_CACHE_LEARN` | `256` | Cache data the main thread needed during play is remembered across sessions and read in at the start and at loading screens (budget in MB, build 90). | `0` |
| `FEX_UNPACK_CACHE_FLUSH` | `1` | New cache data is written in 8 MB batches instead of piece by piece (build 91). | `0` |
| `FEX_GAME_FILES_UNCACHED` | `1` | The game's own data files are kept out of Android's file cache. Before this, Android's memory killer closed other apps at every area load, and they restarted on all cores (builds 59-61). Needed by the next switch. | `0` |
| `FEX_GAME_FILES_LEARN` | `256` | Game data the main thread waited for during play is remembered across sessions and read in at loading screens (budget in MB, build 89). | `0` |
| `FEX_GAME_FILES_REFILL` | `1` | That learned data is checked every 500 ms in play too; what Android's memory reclaim threw out is read back in the background before the game needs it (build 130). | `0` |
| `FEX_MEM_HEADROOM_MB` | `0` | In a loading screen FEX takes memory until this many MB are unused and gives it back, so Android's memory reclaim runs in the load instead of in play (build 130; off from build 131: it made Android close background apps, which restarted on all cores). `FEX_MEM_HEADROOM_GUARD_MB` (default `3072`) is the available memory it never goes below. | `0` |
| `FEX_GAME_FILES_LOAD_FREE_MB` | `1024` | In a loading screen with less unused memory (MemFree) than this, FEX lets go of its kept game data every 200 ms (all but the last 32 MB read, readahead and learned pieces: ~600 MB), so an area load doesn't make Android close other apps (build 138; in the build 137 run 9 apps were closed at the first area load and restarted for 28 s). | `0` |
| `FEX_GAME_FILES_PLAY_FREE_MB` | `1024` | The same in play with less unused memory than this (every 500 ms), so a new area the game streams in while frames keep coming takes FEX's kept data first instead of making Android close other apps (build 139; in the build 138 run 12 and 5 apps were closed at t=46 and t=189). Build 148 raised it from 384: in the build 147 run unused memory sat at 384-512 MB in most of play, where FEX kept 80-250 MB more game data. | `0` |
| `FEX_GAME_FILES_BURST_MB` | `32` | In the play room, a pass also starts once the game has read this many MB since the last one (at least 100 ms after it began), so a new area's data leaves the file cache as it comes instead of waiting up to 500 ms (build 148; in the build 147 run the game read 191 MB in a second at t=282 and Android closed 12 apps). | `0` |
| `FEX_GAME_FILES_NICE` | `-10` | FEXFileCache (the thread that drops the game data) runs at this priority, so its drops come on time when all cores are busy (build 148; in the build 147 run it waited 457 ms for a CPU at t=283). | `off` |
| `FEX_GAME_FILES_ROOM_AHEAD_READ` | `1` | In the load and play rooms, a piece marked readahead that was read since counts by that read, so the rooms keep only the last 32 MB read as meant (build 147; in the build 146 run the first area load kept 171-385 MB as readahead and Android closed 18 apps). | `0` |
| `FEX_GAME_FILES_LEARN_RANGES` | `1` | A 1 MB piece kept only because it holds learned data keeps only its learned ranges, not the whole piece (build 147; build 146 kept ~195 MB of pieces for 54 MB of learned ranges). | `0` |
| `FEX_LOAD_LIGHT` | `64` | Frames that give the GPU very little work also count as a loading screen, so loading screens drawn at full speed are seen too (build 94). | `0` |

## FEX code and locks

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_CODE_BUFFER_START` | `max` | FEX's buffer for translated code starts at its full 128 MB (build 84). | `grow` |
| `FEX_CODE_BUFFER_RENEW` | `50` | The code buffer is renewed at a loading screen once it is this % full, instead of filling up and dropping all code during play (build 84). | `off` |
| `FEX_CODE_BUFFER_RENEW_EARLY` | `10` | The same, in % for the first three loading screens. | |
| `FEX_CODE_BUFFER_RENEW_GAP` | `10` | A loading screen within this many seconds of the last renewal renews only at `FEX_CODE_BUFFER_RENEW`'s mark, not the early one. In the b159 run entering the game counted as two loading screens 2.2 s apart, and the second renewal dropped the 13,084 blocks of code just loaded for the new area (build 160). | `0` |
| `FEX_CODE_REUSE` | `1` | Code from the disk cache that comes back unchanged is reused from the buffer instead of placed again (build 85). | `0` |
| `FEX_JIT_REUSE` | `1` | The same for freshly translated code. The game's protection rewrites its code every minute or two (build 95). | `0` |
| `FEX_SMC_HOTPAGE` | `16` | A memory page that its own code writes to this often within 100 ms stops being write-protected; its code checks itself instead. This fixed the protection's regular freezes (build 54). | `0` |
| `FEX_KERNEL_FUTEX` | `1` | FEX's internal locks wait in the Linux kernel instead of Wine's wait lists. Fixed a 144 ms freeze (build 125). | `0` |
| `FEX_LOCK_BOOST` | `4` | A thread that holds FEX's code lock or memory lock this many ms while another thread waits for it runs at the main thread's priority (nice -20) until it lets go. In the b154 run the code lock was held 56-111 ms by a thread that waited for a CPU itself (build 156). Needs `FEX_KERNEL_FUTEX`. | `0` |
| `FEX_WINE_WAIT_FIX` | `1` | Wine's wait lists (every lock and condition variable in the process) run with upstream Wine's fix for a race that froze the game for good (build 118 and 139 runs). FEX checks ntdll's code first and leaves it alone if it doesn't match. On our Proton d2r1 (order fix only) FEX still swaps in for its sleeping list lock; from d2r2 on (both fixes, export `__wine_d2r_wait_lists` = 2) FEX stays out. `2` = the same fix with Wine's spin lock (build 142-144). | `0` |

## CPU cores and priorities

FEX finds the fastest core and the little cores by their top clock speeds. On the Thor that is core 7 and cores 0-2.

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_MAINCORE` | `fast` | The game's main thread runs alone on the fastest core (build 49). | `off` |
| `FEX_MAIN_NICE` | `-10` | The main thread's priority (build 83). | `off` |
| `FEX_MAINCORE_WAIT` | `40` | The main thread may also use the other big cores while it waits this many ms a second or more for its own (build 51). | `0` |
| `FEX_MAINCORE_ESCAPE` | `4` | When Android's own work lands on the main thread's core and the main thread waits this many ms for it, it moves to the other big cores within milliseconds (build 117). | `0` |
| `FEX_MAINCORE_ESCAPE_REPEAT` | `3` | Leaving again within this many seconds of coming back makes it stay away longer, up to 10 s (build 120). | `0` |
| `FEX_MAINCORE_ESCAPE_QUIET` | `5` | While away, it only comes back once its core had no other work for this many seconds (build 121). | `0` |
| `FEX_MAINCORE_ESCAPE_STAY` | `1` | Once it has left, it stays away for the rest of the session (build 123). | `0` |
| `FEX_MAINCORE_AWAY_SHARE` | `1` | While the main thread is away, the game's other threads may use its core (build 123). | `0` |
| `FEX_MAINCORE_ESCAPE_NICE` | `-20` | The main thread's priority while it is away. | `off` |
| `FEX_MAINCORE_OTHERS` | `user` | Other programs of the app (steam.exe, the launcher) are kept off the main thread's core (builds 76-77). `process` keeps only the game's own threads off. | `off` |
| `FEX_HELPER_CORES` | `little` | FEX's helper threads (stats log, priority guard, escape watcher, rumble sender, file cache dropper) run on the little cores (build 49). Since GameNative gives the game cores 3-7, Wine refused this and they ran on the big cores; from build 156 FEX moves them at the Linux level. From build 158 every FEX thread sets its own Linux name when it starts, so the search by name finds the stats log too (in b157 it kept the name "D2R.exe" and stayed on the game's cores). | `off` |
| `FEX_LOADER_CORES` | `little` | The game's protection/loader thread is found and moved to the little cores (builds 62-63, 81). | `off` |
| `FEX_LOADER_GAME_EXCEPTIONS` | `400` | A thread with this many exceptions in the game's own code within 2 s is a game thread and is never moved (build 65). | `0` |
| `FEX_PRIO` | `normal` | Every thread at normal Windows priority. Wine on Android turns Windows priorities into extreme Linux values (build 48). | `keep` |
| `FEX_HELPER_PRIO` | `normal` | FEX's helper threads at normal priority instead of idle (build 48). | `idle` |
| `FEX_SERVER_NICE` | `-10` | The wineserver's priority (build 49). | `off` |
| `FEX_WORKER_NICE` | `-10` | The game's job worker threads' priority (builds 87-88). | `off` |
| `FEX_FRAME_THREADS` | `-10` | VKD3D's frame threads, the frame generation layer's present thread and (from build 160) the window system's present queue thread run at this priority or better (build 126). | `off` |
| `FEX_FRAME_THREADS_OFFCORE` | `1` | Those threads stay off the main thread's core while it is shared (build 126). | `0` |
| `FEX_PRESENT_THREADS` | `-20` | VKD3D's present thread and the window system's present queue thread run at this priority or better. With frame generation the generated frame goes out when VKD3D's present thread wakes at its slot; in the b159 run that thread (at -10) waited ~39 ms a second for a CPU, and generated frames came 1-6 ms late in 39 of 169 calm seconds (build 160). | `off` |
| `FEX_INPUT_THREADS` | `-10` | Wine's controller thread runs at this priority or better, so the game's controller reads don't wait for it while it holds the controller's lock (build 137). | `off` |
| `FEX_STEAM_THREADS` | `-10` | steam.exe's thread that answers the game's Steam calls every frame runs at this priority or better, so the game doesn't wait on Steam while the cores are crowded (build 147; in the build 146 run it waited up to 340 ms a second for a CPU). | `off` |

## Other game fixes

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_RUMBLE` | `async` | The game's controller rumble calls return at once, and a FEX thread passes them on. They used to block the game's render threads (build 53). Left to Proton when its xinput exports `__wine_d2r_xinput` (our Proton d2r3+, build 145). | `keep` (Wine's way); `off` = no rumble at all |

## VKD3D (DirectX 12)

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_VKD3D_NOSTAGGER` | `1` | Adds `no_staggered_submit` to `VKD3D_CONFIG`: GPU work goes out without VKD3D's queue thread waiting between batches (build 52). | `0` |
| `FEX_VKD3D_FOLD_CLEARS` | `1` | A full-screen clear between render passes becomes the start of the next pass instead of a pass of its own (build 55). | `0` |
| `FEX_VKD3D_CAP_PRESLEEP` | `1` | The frame cap's wait happens before the present, without blocking the GPU queue the game needs (build 56). | `0` |
| `FEX_VKD3D_FRAME_START` | `1` | Each frame starts as late as it can without missing its slot, so the game reads input closer to when the frame is shown (build 56). | `0` |
| `FEX_VKD3D_CAP_GRID` | `1` | FEX tells VKD3D the exact frame time of GameNative's FPS limit (build 97). | `0` |
| `FEX_VKD3D_LATENCY_RESYNC` | `1` | Keeps the game at the number of frames in flight it asked for, after a startup hiccup used to add one (VKD3D fex16). | `0` |
| `FEX_VKD3D_PRESENT_DRAWN` | `1` | A frame goes to the screen only once the GPU has drawn it (build 66). | `0` |
| `FEX_VKD3D_SKIP_FRESH_CLEARS` | `1` | New GPU memory that is still zero isn't zero-filled again (build 64). | `0` |
| `FEX_VKD3D_MEMORY_AHEAD` | `1` | GPU memory the game will ask for is made ahead on a background thread (build 80). | `0` |
| `FEX_VKD3D_CBV_SIZE` | `1` | Constant buffers keep the size the shader declares, so Turnip can load them before a draw (build 98). Changing it makes VKD3D compile its shaders again once. | `0` |
| `FEX_VKD3D_SHADER_SWAP` | `1` | Two of the game's shaders are swapped for faster versions that draw the same: the lighting table blur (30 ms to 2.2 ms per run) and the ground clutter shader (builds 108-110). `2` swaps only the blur. | `0` |
| `FEX_VKD3D_CLEAR_MEMO` | `1` | The game's empty second shadow map isn't cleared again while it is still empty (build 110). | `0` |
| `FEX_VKD3D_UI_SPLIT` | `1` | Each frame is also made without the game's UI, for frame generation (build 122). | `0` |
| `VKD3D_FEX_START_HITCH_SKIP` | on | Set directly (no FEX switch). Frames that would have come late anyway (loading screens, area changes, hitches) no longer keep frame start pacing at 0 for 8.5 s, and from fex39 neither do the frames around them or the first 10 s after a loading screen, so the lag after loads drops back sooner (VKD3D fex38-39). `1` = fex38 only. | `0` |
| `VKD3D_FEX_START_SLOW_SKIP` | on (fex41) | Set directly (no FEX switch). One slow game frame that came late or nearly late with the hold-back on no longer keeps frame start pacing low for 8.5 s (lag 58-67 ms instead of ~50 for 10-17 s after it); it lowers the hold-back for ~2 s, and only a second one within 8.5 s keeps it low as before (VKD3D fex40). From fex41 also a lone frame that would allow (nearly) no hold-back at all, whatever was held back. | `0` (`1` = fex40) |
| `VKD3D_FEX_START_ROOMY_MS` | `10` | Set directly (no FEX switch). Frames with this much room (ms) or more don't limit frame start pacing outside GPU-bound scenes: the room measured tops out at ~15 ms, so they held the hold-back low in steps for 11-17 s after a late frame (VKD3D fex41). 4-30. | `0` |
| `VKD3D_FEX_START_CAP_MS` | `4` | Set directly (no FEX switch). After a frame comes late or nearly late with 24 ms or more held back (outside GPU-bound scenes), frame start pacing stays at least this many ms below that hold-back (not under 20 ms); the ceiling rises 1 ms per 450 frames (~15 s) without another. Above ~28 ms frames came late 5 times as often, and the hold-back kept climbing back there (VKD3D fex42). 1-15. | `0` (= fex41) |

## Turnip (graphics driver)

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_TU_CHANNEL` | `1` | The link between FEX and our Turnip. With `0` our Turnip behaves like stock Turnip and every switch below is off. | `0` |
| `FEX_TU_EARLY_PREAMBLE` | `1` | The lighting draws' setup runs ahead of the draw: lighting draws about 45% cheaper (Turnip fex2). | `0` |
| `FEX_TU_PREAMBLE_LOADS` | `1` | That setup issues its memory reads first (Turnip fex6). | `0` |
| `FEX_TU_LOOP_PREFETCH` | `1` | Light list loops read the next entry one step ahead (Turnip fex7). | `0` |
| `FEX_TU_CP_UBO` | `1` | The GPU's command processor loads each draw's root constant buffers (Turnip fex9). | `0` |
| `FEX_TU_UBO_TABLE` | `1` | The remaining ones go through the shader's buffer table: busy frames 1.4-1.8 ms less GPU time together with the switch above (Turnip fex10). | `0` |
| `FEX_TU_DEPTH_CLEAR` | `1` | Depth and colour clears at the start of a pass run as one 3D draw (Turnip fex3, fex8). | `0` |
| `FEX_TU_BUDGET_CACHE` | `1` | The GPU memory budget is read at most every 250 ms instead of every frame (Turnip fex5). | `0` |
| `FEX_TU_DRAW_SETUP` | `4` | The lighting shaders run without a setup step at all, their constants loaded by the command processor: lighting about 5% faster (Turnip fex15). | `0` |

## Frame generation layer

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_LSFG_DEFER` | `1` | The layer shows frames from its own thread instead of holding up the game (layer 7). | `0` |
| `FEX_LSFG_GEN_DRAWN` | `1` | A generated frame goes to the screen only once the GPU has drawn it (layer 7). | `0` |
| `FEX_LSFG_EARLY` | `1` | Frame generation starts as soon as the game's frame is ready (layer 8). | `0` |
| `FEX_LSFG_PRESENT_EARLY` | `1000` | The present thread wakes this many microseconds before a frame's slot and waits the rest actively, so no frame is late (layer 9). | `0` |
| `FEX_LSFG_PRESENT_NICE` | `-20` | The present thread's priority (layer 10). | `off` |
| `FEX_LSFG_GEN_GAP` | `50` | A game frame that comes more than this many ms after the one before gets no generated frame, so a freeze doesn't produce a wild guess (layer 10). | `0` |
| `FEX_LSFG_UI` | `1` | Frame generation works on frames without the UI, and the UI is taken from the real frame, so the map and HUD don't smear (layer 11). | `0` |
| `FEX_LSFG_UI_TOL` | `0` | How many 1/255 steps a pixel may differ and still not count as UI. | |
| `FEX_LSFG_UI_MOTION` | `24` | The UI is drawn halfway along its own motion, so item names move with the world. The number is the largest move searched, in pixels (layers 12-13). | `0` |
| `FEX_LSFG_GEN_AHEAD` | `1` | The generated frame goes to the layer's present thread before the game's present waits for the frame's slot, so it doesn't wait for the game's thread to wake up (layer 14; b161: 8% of generated frames 1 ms or more late). | `0` |

## Picture to the screen

| Switch | Default | What it does | Off |
|---|---|---|---|
| `FEX_WSI_DIRECT` | `1` | Takes `sw` out of `MESA_VK_WSI_DEBUG` for the game (GameNative sets it when the container's "Use DRI3" is off). With `sw`, GameNative's Vulkan wrapper copies every picture on the GPU into a CPU buffer behind the game's queued work, waits for that copy inside the present, and the X server copies it again: the frame generation layer's presents took 1-12 ms (18-26 ms in heavy scenes) from 2026-11-01 on, against 0.1 ms in every run before. Without it the wrapper hands the X server the picture's own buffer (build 150). | `0` |

## Logging and measuring

These only run in the **logging version** (`2610-d2rfix28s-thor`). The clean version has them all off.

| Switch | Logging version | What it does |
|---|---|---|
| `FEX_STATSLOG` | `D:\` | The folder for the per-session log `fexstats-D2R.exe-b<N>-<time>-<pid>.txt`. `0` = no log. |
| `FEX_STATSLOG_PROFILE` | `1` | Samples what the busiest threads run, and explains every slow frame. |
| `FEX_STATSLOG_TIMELINE`, `FEX_SCHEDLOG`, `FEX_GPULOG`, `FEX_STALLPROBE` | on | Per-thread timeline, CPU wait times, GPU clock, lock and wineserver probes. `0` turns each off. From build 133 a wait of 15 ms or more for Wine's memory lock gets a `# memory lock wait` line that lists every thread not asleep at 15 and 45 ms (Linux call, state, CPU), to find the thread holding the lock. |
| `FEX_PROBE_VQ_NOTES` | `1` | Build 153: with our Proton's memory lock notes (d2r4 and later) the stall probe reads who holds Wine's memory lock and since when, instead of taking the lock with a VirtualQuery every 4 ms. `vq` on the probe line is then the longest hold it saw, `# memory lock wait` lines are holds seen at 15 ms or more, and the probe line adds `memory lock taken N/s` (every memory call, Wine fault and VirtualQuery of the process). The lock watch thread ends, and the probe's 16 MB test region is refreshed with madvise without Wine's lock. `0` measures with the VirtualQuery again. |
| (no switch) | on | Build 154: with our Proton d2r6 and later, a `server calls` line every second gives the game process's wineserver requests by type (how many, average time from send to reply; a `select` includes its wait) and what Wine's reuse of short-lived events did. Wine's own `WINE_D2R_CALL_COUNTS=0` turns the counting off. |
| (no switch) | on | Build 161: with our Proton d2r10 and later, FEX's priority guard and stats log read the game's threads (ids, names, priorities, cores, TEBs) from Wine's thread table, and their CPU times from Linux, instead of asking the wineserver about every thread (b159: ~66 thread list and ~160 thread info requests a second). This also works in the clean version. Every 5 s a `registry` line names the registry keys and values the game's threads asked for last. Wine's own `WINE_D2R_THREAD_TABLE=0` brings back the old way. The stall probe's wineserver round trip runs every 32 ms instead of 16. |
| (no switch) | on | Build 163: with our Proton d2r12 and later, a `debug output` line in seconds with OutputDebugString calls (calls and time, exceptions raised, DBWIN_BUFFER looks and the ones left out, latest caller, what could see the exceptions) and a `file flushes` line in seconds with FlushFileBuffers calls (per thread, and each call with its start `t=`, time and kind). Wine's `WINE_D2R_ODS_FAST=0` puts back stock OutputDebugString (two exceptions and a DBWIN_BUFFER lookup per call); `WINE_D2R_CALL_COUNTS=0` turns the counting off. |
| `FEX_STATSLOG_APP`, `FEX_STATSLOG_SCAN` | unset | Log only the program named; a per-second scan of the game's code for the log. |
| `FEX_LOADER_WATCH` | `1` | Counts the loader thread's calls. |
| `FEX_PIPE_WATCH` | `1` | Build 146: the main thread's pipe reads and writes (Steam's round trip every frame), with each message's size and first 8 bytes; the Windows process that made the pipe, from the list of Windows processes; and that process's threads' time on a CPU and waiting for one (`pipe watch` and `pipe peer` lines). |
| `FEX_VKD3D_PASSES`, `FEX_VKD3D_TIMELINE` | `1` | GPU time per render pass; each frame's way to the screen. |
| `FEX_TU_COUNTERS`, `FEX_TU_SAMPLE_DRAWS` | `1`, `100` | GPU counters for passes with 100 draws or more. |
| `FEX_LSFG_GPU_TIMES` | `1` | GPU time of frame generation. |

The **clean version** sets two switches of its own instead:

| Switch | Clean version | What it does |
|---|---|---|
| `FEX_VKD3D_LOG_OFF` | `1` | VKD3D's own text output off. |
| `FEX_CRASH_NOTE` | `D:\` | If the game ends with an error, a small file `fexcrash-D2R.exe-b<N>-<time>-<pid>.txt` with the last errors. Nothing otherwise. `0` = none. |
| `FEX_START_WATCH` | `D:\` | Both versions, build 128 (Android log from build 132): for every start a small file `fexstart-D2R.exe-b<N>-<time>-<pid>.txt` with the container's processes and GameNative's own app process starting and ending in the first 2 minutes, and GameNative's warnings and errors from Android's log (a failed start's included), to find what ends a start that stays black. Build 136: written through Linux (keeps going while Wine hangs) with `## threads` blocks (every thread's state and wait) at 1 s and after any second of near-zero CPU in the first 30 s. Build 140: every exception FEX passes on outside the game's exe (first 400), with the memory region and file at the code and the faulting address, code bytes and stack top. `0` = none. |
| `FEX_START_WATCH_S` | `120` | How many seconds the start watch follows processes (5-3600). | |
| `FEX_SYSDLL_REDIRECT` | `1` | Build 151: a call into free memory where kernel32, kernelbase or ntdll usually are (code built by another process using its own addresses) continues at the same offset of where Wine loaded them in this process. Fixes the failed starts where Wine loaded kernel32 elsewhere and D2R crashed ~1.5 s in. `0` = off. | |

## Tried and left off

These experiments stay switchable for comparisons. None of them helped (details in
[d2r-thor-findings.md](d2r-thor-findings.md)).

| Switch | Default | What it was |
|---|---|---|
| `FEX_MAINCORE_ESCAPE_LIVE` | `0` | **Don't turn on.** A faster check for the main thread's move. Build 118 crashed and froze with it. |
| `FEX_FRAMELATENCY` | `off` | One more frame in flight (build 50): no gain, more input lag. |
| `FEX_GAME_FILES_AHEAD` | `0` | Big game data reads requested whole (builds 87-88): no gain. |
| `FEX_VKD3D_SHADER_COPY` | `0` | Copies of the lighting shaders into the log, for study (builds 67-75). |
| `FEX_TU_NOPREAMBLE`, `FEX_TU_DESC`, `FEX_TU_SWITCH`, `FEX_TU_NOALIAS`, `FEX_TU_WAVE_PAIR`, `FEX_TU_CS_ATOMICS`, `FEX_TU_CONST_KEEP` | `0` | Turnip experiments (Turnip fex1, fex11-17): no gain or slower. |
| `FEX_THREADPIN` | unset | An early thread placer, replaced by the core switches above. |

## Fine tuning

Built-in values that have no default entry. There is normally no reason to change them.

| Switch | Value | What it sets |
|---|---|---|
| `FEX_UNPACK_CACHE_MAX_GB` | `16` | Largest size of the unpack cache. |
| `FEX_UNPACK_CACHE_RESET` | unset | `1` empties the unpack cache at the start. |
| `FEX_UNPACK_CACHE_READAHEAD_MB` | `64` | Unpack cache read-ahead. |
| `FEX_NATIVE_MD5_CACHE`, `FEX_UNPACK_CACHE_SKIP_ADLER`, `FEX_NATIVE_ADLER_NEON` | on | Further shortcuts for checksums of cached data. `0` turns each off. |
| `FEX_GAME_FILES_MS` | `500` | How often the game data files are dropped from Android's file cache. |
| `FEX_GAME_FILES_KEEP_MB` | `512` | Game data the main thread reads is kept in memory up to this size. |
| `FEX_GAME_FILES_GUARD_MB` | `2048` | Below this much free memory, kept game data is let go. |
| `FEX_GAME_FILES_READ_WATCH` | on | Watches the game's reads (needed for keeping and learning). |
| `FEX_CODE_BUFFER_LOAD_MS`, `FEX_LOAD_LIGHT_MS` | `250`, `500` | How long without frames, or with light frames, counts as a loading screen. |
| `FEX_DISKCACHE_PREFETCH`, `FEX_DISKCACHE_PREFETCH_MB` | on, `2048` | FEX's code cache file is read into memory at the start, up to this many MB. |
| `FEX_RUMBLE_MS` | `50` | How often rumble changes are passed on. |
| `FEX_JOBWAIT`, `FEX_JOBWAIT_BACKOFF` | on, `0` | The game's job wait loop leaves the job queue's lock free for the workers. |
| `FEX_CODELOCK_POLITE` | `0` | Upstream behaviour of FEX's code lock. |
| `FEX_FRAMETIMES` | on | FEX's frame clock. **Leave it on:** loading screen detection and code buffer renewal need it. |
