// SPDX-License-Identifier: MIT
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>

#include <cstdlib>
#include <cstring>

namespace FEX::Windows {
namespace {
  struct Default {
    const char* Name;
    const char* Value;
  };

  // The build without the stats log (thor-clean) also turns off what only measures for the log: the GPU pass meter and
  // present timeline in FEX's VKD3D build, Turnip's pass sampling and GPU counters, and VKD3D's own text log. Every fix
  // keeps its default.
#ifdef FEX_DISABLE_STATSLOG
  constexpr bool Measuring = false;
#else
  constexpr bool Measuring = true;
#endif

  // D2R.exe runs with what its tests have used since build 40, so a test needs no environment variables. Every other
  // program keeps upstream behaviour unless a variable is set. A variable set to 0 still turns a default off.
  constexpr Default D2RDefaults[] = {
#ifndef FEX_DISABLE_STATSLOG
    {"FEX_STATSLOG", "D:\\"}, // where the tests' logs have gone so far
    {"FEX_STATSLOG_PROFILE", "1"},
#else
    {"FEX_VKD3D_LOG_OFF", "1"},
#endif
    {"FEX_NATIVE_ZLIB", "1"},
    {"FEX_NATIVE_MD5", "1"},
    {"FEX_UNPACK_CACHE", "1"},
    // Build 58: the unpack cache's data file stays out of Android's file cache (see ARM64EC/NativeUnpackCache.cpp). Its
    // 3-4 GB of reads per area load came right before the device's other cores went busy in the build 56 and 57 logs.
    {"FEX_UNPACK_CACHE_DIRECT", "1"},
    // Build 59: the game's own data files (CASC archives) are dropped from Android's file cache every 500 ms (see
    // ARM64EC/GameFiles.h). In the build 58 log the second area load still filled memory and Android's memory killer
    // closed 20 apps, which then restarted for ~35 s on 5-6 cores. Build 60 finds them in the game's folder (build 59
    // looked for them among the game's open files and found none).
    {"FEX_GAME_FILES_UNCACHED", "1"},
    // Build 48: no thread at extreme Linux nice values (see PriorityGuard.h, Threads.cpp).
    {"FEX_PRIO", "normal"},
    {"FEX_HELPER_PRIO", "normal"},
    // Build 49: the main thread alone on the fastest core, FEX's helpers on the slowest, the wineserver above the game
    // (see PriorityGuard.h).
    {"FEX_MAINCORE", "fast"},
    {"FEX_HELPER_CORES", "little"},
    {"FEX_SERVER_NICE", "-10"},
    // Build 83: the main thread at nice -10 too. In the build 82 log other programs took most of its core for 6 s and 17 of
    // the run's 58 frames of 40 ms or more came then (see PriorityGuard.h).
    {"FEX_MAIN_NICE", "-10"},
    // Build 87: the game's job workers at nice -10 in every other 10 s window, at nice 0 in the others (see
    // PriorityGuard.h). In the build 86 log busy scenes had the main thread polling for their jobs ~12 ms a frame while
    // each of the 4 workers waited 85-150 ms a second for a CPU. D2R asks for HIGHEST for them (nice -5); FEX_PRIO=normal
    // put them at 0.
    // Build 88: at -10 all the time. Over 23 windows of each half in the build 87 run the raised workers waited 158 ms a
    // second for a CPU against 271, lost their CPU 498 times a second against 2,438, the main thread polled for their jobs
    // 35 ms a second against 44 (38 against 82 in the busiest stretch), and play had 98 slow game frames against 130.
    {"FEX_WORKER_NICE", "-10"},
    // Build 51: the main thread may leave its core while it waits 40 ms a second or more for it (see PriorityGuard.h).
    {"FEX_MAINCORE_WAIT", "40"},
    // Build 117: in the build 116 run Android's memory reclaim (kswapd, a kernel thread) and once a root process ran on the
    // main thread's core 7 in play; the main thread got about half the core (Android's CPU groups put system work beside
    // the app's group, so its nice -10 doesn't count there), waited 30-115 ms within single frames and made 48-156 ms frames.
    // FEX_MAINCORE_WAIT's widening (checked once a second) came too late and didn't help: allowed the other big cores too,
    // the kernel kept it on core 7 (98%) waiting. A watcher now checks every 4 ms: when the main thread waited N ms or more
    // for core 7 within the last 8 ms it moves to the game's other big cores (not core 7) at nice -20 for 100 ms (doubling up
    // to 1.6 s while it keeps having to leave), then goes back. 0 or "off" turns it off (FEX_MAINCORE_WAIT then works as
    // before).
    // Build 118: in the build 117 run the watcher saw each wait only once it had ended (run_delay grows when the thread
    // gets its CPU back), so the 23-93 ms waits in play (6 slow frames in ~10 min, during memory reclaim) ran their full
    // length before it moved, and its 60 moves after short waits came when nothing was in the way any more. It now checks
    // every 2 ms whether the main thread is ready to run without a CPU right now (its state and its exact CPU time) and
    // moves it once that has lasted N ms, while it still waits.
    {"FEX_MAINCORE_ESCAPE", "4"},
    // Build 119: build 118's check while the main thread waits is off again. Both build 118 runs failed (a crash after the
    // character screen, a freeze at a loading screen with the game's threads spinning in Wine's wait and wake calls, seen in
    // no earlier log), and that check was build 118's only change. "1" turns it on.
    {"FEX_MAINCORE_ESCAPE_LIVE", "0"},
    // Build 120: in the build 119 run something outside the app took the main thread's core every 1.49 s for 20-95 ms
    // (a 40-120 ms game frame every 1.5 s, the map overlay's judder). The thread left after each one for 200 ms only and
    // came back 1.3 s before the next. Leaving again within 3 s of coming back now doubles the hold up to 10 s (from 100 ms
    // again after 30 s without leaving). "off" = build 119 (1 s, up to 1.6 s).
    {"FEX_MAINCORE_ESCAPE_REPEAT", "3"},
    // Build 121: in the build 120 run that outside work went on all session; the main thread stayed away 97% of play but
    // came back each time its 10 s hold ended, into the next burst (a 32-88 ms wait every 10.4 s). While away it now comes
    // back only once its core has had no burst of other work for 5 s (read every 20 ms from /proc/schedstat). "off" = 120.
    {"FEX_MAINCORE_ESCAPE_QUIET", "5"},
    // Build 123: in the build 121 run the main thread left core 7 once at the start and stayed away, except once: after 5 s
    // without outside work there it came back, and memory reclaim (kswapd) hit it 2.5 s later (a 46 ms frame). It now
    // stays away for the rest of the run once it has left. "0" = build 121 (back after 5 s quiet).
    {"FEX_MAINCORE_ESCAPE_STAY", "1"},
    // Build 123: in the same run's heaviest fight the game's job workers waited 328 ms a second for a CPU on cores 3-6
    // (now shared with the main thread) while core 7 was 82% idle. While the main thread is away, this process's threads
    // and the wineserver's may use core 7 too; they lose it again before the main thread comes back. "0" keeps them off.
    {"FEX_MAINCORE_AWAY_SHARE", "1"},
    {"FEX_MAINCORE_ESCAPE_NICE", "-20"},
    // Build 76: every other thread of the app's Linux user off the main thread's core too, steam.exe's above all (see
    // PriorityGuard.h). In the build 75 log the main thread waited for its core 7 ms per slow frame, nearly always inside
    // SetEvent, and steam.exe's CPU use tracked those waits in four runs.
    {"FEX_MAINCORE_OTHERS", "user"},
    // Build 50 tried one more frame in flight on the swapchain's frame latency handle (FEX_FRAMELATENCY=2). Heavy scenes
    // were no faster: the wait moved into vkd3d-proton's Present, the GPU or present path being the limit there. It only
    // added input lag, so build 51 leaves the handle as the game set it (see ARM64EC/FrameLatency.h).
    {"FEX_FRAMELATENCY", "off"},
    // Build 52: vkd3d-proton submits without its queue thread waiting for the GPU between batches (see
    // EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_NOSTAGGER", "1"},
    // Build 53: the game's rumble calls return at once and a FEX thread passes the changes on (see ARM64EC/Rumble.h).
    {"FEX_RUMBLE", "async"},
    // Build 54: a page whose own code keeps writing to it (a protector's decryption loop, 6,000-10,000 write faults a
    // second for 5-20 s every ~45 s in the build 48-52 logs) stops being write-protected; its code checks itself instead
    // (see Common/InvalidationTracker.h).
    {"FEX_SMC_HOTPAGE", "16"},
    // Build 55: FEX's vkd3d-proton build folds full clears into the next render pass (the build 54 heavy scenes were GPU
    // bound at its top clock), times every render pass on the GPU and each frame's way from Present() to the window
    // system (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_FOLD_CLEARS", "1"},
    {"FEX_VKD3D_PASSES", Measuring ? "1" : "0"},
    {"FEX_VKD3D_TIMELINE", Measuring ? "1" : "0"},
    // Build 56: the build 55 log showed the frame cap sleeping ~32 ms in every vkQueuePresentKHR on vkd3d's queue thread,
    // with the VkQueue the game's compute and copy queues share held all that time, and each frame waiting ~23 ms for the
    // queue thread after the game was done with it. FEX's vkd3d-proton build sleeps the cap's wait out before the present
    // without the VkQueue, and holds back the game's frame start by the waiting it measured, as far as the game's own
    // slow frames allow (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_CAP_PRESLEEP", "1"},
    {"FEX_VKD3D_FRAME_START", "1"},
    // Build 97: FEX hands the cap's exact frame time (fps_limit rounded to whole screen refreshes, from the cap layer's
    // conf.toml and GameNative's vsync.txt) to FEX's vkd3d-proton build (fex22), whose pre-sleep learned it from held
    // presents up to fex21. In the fex21 log it learned 30.4 ms instead of 25 ms while loading in and kept it all
    // session: 37 FPS at a 40 FPS cap, every 4th frame one refresh long (also seen in builds 89, 92 and 93) (see
    // ARM64EC/Vkd3dReport.cpp).
    {"FEX_VKD3D_CAP_GRID", "1"},
    // Build 113: GameNative's frame generation layer, FEX's D2R build (tools/lsfg/, layer 7). At 2x from a 30 FPS limit it
    // slept ~17 ms of every frame in the game's vkQueuePresentKHR before the real frame, with VKD3D holding its only
    // VkQueue, so every submit of the game waited (the build 55 problem again). The layer now presents the frame from its
    // own thread and the game's present returns at once. Its generated frame also went to the window system before the
    // GPU drew it (GameNative's window system shows an image as soon as it gets it, see FEX_VKD3D_PRESENT_DRAWN); it now
    // waits on that thread until drawn. 0 turns either off (LSFG_D2R_DEFER / LSFG_D2R_GEN_DRAWN in the Unix environment).
    // Build 114: set 0 or 1 in the game only; build 113 set 0 in Steam (no defaults there) and the game inherited it, so
    // its frame generation run had no present thread.
    {"FEX_LSFG_DEFER", "1"},
    {"FEX_LSFG_GEN_DRAWN", "1"},
    // Build 115 (layer 8): in the build 114 run the generated frame waited ~11 ms for the GPU (12+ in busy scenes) and came
    // only ~5 ms before the real frame instead of halfway: the layer started its copy and frame generation in the game's
    // present, after VKD3D's cap pre-sleep, and VKD3D had given the GPU the game's next frame meanwhile. The layer now
    // starts them at the game's submit that blits the frame into its swapchain image (LSFG_D2R_EARLY).
    {"FEX_LSFG_EARLY", "1"},
    // Build 116 (layer 9): in the build 115 runs the real frames went out 0.5-0.9 ms after their slot on average in stretches
    // of 5-10 s (0.07 otherwise; 37 of 166 play seconds in one run), the layer's present thread waking late from its
    // sleep. Presents are timed onto the display's refresh grid, so a late one can miss its refresh. The thread's sleep now
    // ends 1000 us before the slot and it waits the rest on the CPU (LSFG_D2R_PRESENT_EARLY_US, 0 = as before). The layer
    // also times its copy and frame generation on the GPU (LSFG_D2R_GPU_TIMES, before only with LSFG_DEBUG), for the cost of
    // the flow scale.
    {"FEX_LSFG_PRESENT_EARLY", "1000"},
    {"FEX_LSFG_GPU_TIMES", "1"},
    // Build 117 (layer 10): in the build 116 run the real frames still went out 1 ms or more after their slot 9% of the
    // time in fights: the layer's present thread (nice -10) woke 1-6 ms late from its timed sleeps on cores 3-6, which the
    // game's job workers (also nice -10) kept busy. It now runs at nice -20 (LSFG_D2R_DEFER_NICE; "off" leaves the nice
    // the thread starts with). The run also had 48-156 ms game frames (the main thread waiting for its core, see
    // FEX_MAINCORE_ESCAPE); the generated frame after such a freeze is made between two pictures far apart in time and
    // guesses a big jump (a chest's button label showed halfway across the screen once). A game frame that comes more
    // than FEX_LSFG_GEN_GAP ms after the one before now gets no generated frame, only itself at the generated frame's slot
    // (LSFG_D2R_GEN_GAP_MS, 0 = off).
    {"FEX_LSFG_PRESENT_NICE", "-20"},
    {"FEX_LSFG_GEN_GAP", "50"},
    // Build 122 (layer 11 + VKD3D fex35): in the build 121 run a zone around the map overlay smeared wherever trees moved
    // under it. Frame generation guesses motion from the pictures it gets, and the overlay and HUD barely move while the
    // world does. VKD3D now hands each frame over without D2R's UI layer too (FEX_VKD3D_UI_SPLIT), frame generation runs
    // on those, and the layer takes every pixel where the real frame differs from its frame without the UI layer from the
    // real frame (LSFG_D2R_UI; FEX_LSFG_UI_TOL: how many 1/255 steps a pixel may differ by and still count as the
    // same, 0 = any difference). The UI itself then moves at the game's frame rate.
    {"FEX_LSFG_UI", "1"},
    {"FEX_LSFG_UI_TOL", "0"},
    // Build 62: in the fex10 run (FEX build 61) a thread running the game's protection/loader code (outside any module)
    // was busy in 36% of the play time on the game's big cores, and those phases had 3-7 times as many frames of 40 ms or
    // more. Such threads go to the slowest cores, and their event calls are counted (see PriorityGuard.h, ARM64EC/LoaderWatch.h).
    // Build 63 tells the game's code from the protection's by the exe's range as loaded (build 62 moved three of the game's
    // worker threads, whose code is in the remapped D2R.exe).
    {"FEX_LOADER_CORES", "little"},
    {"FEX_LOADER_WATCH", "1"},
    // Build 65: in the build 64 log the loader search also moved a game thread (tid 784) to the slowest cores at 2:37. The
    // protection put int3 into the game's own code, and the handler that resolves each one (~800 a second) is protection
    // code, so the thread's samples looked like a loader's. On the slow cores it took twice the CPU, a whole core at
    // 3:12-3:24, right where the game rubber banded before the last area change. A thread that raises this many exceptions
    // within 2 s in the main exe's code is a game thread: never moved, moved back if it was (see PriorityGuard.h).
    {"FEX_LOADER_GAME_EXCEPTIONS", "400"},
    // Build 62: during play, unpack cache hits not in read-ahead memory are left to the game to unpack (1.9 ms average
    // disk reads during play in the fex10 run, around the 100-140 ms streaming hitches), and read-ahead is shorter
    // (see ARM64EC/NativeUnpackCache.cpp).
    {"FEX_UNPACK_CACHE_PLAY", "1"},
    // Build 83: unpack cache read-ahead grows along the game's reads instead of reading 8 MB past every hit. In the build 82
    // run 52% of the 27 GB read ahead was never used, and while the game streamed a new area during play storage read
    // ~1 GB/s, ~3/4 of it this read-ahead (see ARM64EC/NativeUnpackCache.cpp).
    {"FEX_UNPACK_CACHE_AHEAD", "ramp"},
    // Build 90: the main thread never reads an unpack cache hit from disk while frames are coming. In the build 89 run
    // storage was at its low clock during play and the main thread's 11 disk hits at an area entry took 36.9 ms (3.4 ms
    // each, against ~0.2 ms to unpack one), in a 105 ms frame (see ARM64EC/NativeUnpackCache.cpp).
    {"FEX_UNPACK_CACHE_MAIN_PLAY", "1"},
    // Build 90: the streams the main thread looks up in frames of play are learned across sessions and read into the
    // file cache at the start and at every loading screen (budget in MB). The same 105 ms frame unpacked 7.6 MB of
    // stored streams itself (20.9 ms) because they weren't in memory.
    {"FEX_UNPACK_CACHE_LEARN", "256"},
    // Build 91: new unpack cache streams start on a page of their own and are written to storage in 8 MB batches, then
    // dropped from the file cache. In the build 90 run the writer dropped each ~60 KB stream right after writing it,
    // which only started its write, and the next stream's first page (shared, streams were 64 bytes apart) waited for
    // that write: while a new area streamed in, ~2,400 streams a second took the writer a whole second of every second,
    // up to 510 a second weren't stored, and the main thread's core ran other work up to 14,000 times a second (see
    // ARM64EC/NativeUnpackCache.cpp).
    {"FEX_UNPACK_CACHE_FLUSH", "1"},
    // Build 84: FEX's translated-code buffer starts at its full 128 MB, and is renewed at a loading screen once half used
    // (10% at the first three: start-up, menus, entering the game), rather than filling (and dropping all code) in play.
    // In the build 83 log every thread reloaded its code at once at 6:21, 5,301 blocks on the main thread in one second,
    // with no file reads (see ARM64EC/CodeBuffer.h).
    {"FEX_CODE_BUFFER_START", "max"},
    {"FEX_CODE_BUFFER_RENEW", "50"},
    {"FEX_CODE_BUFFER_RENEW_EARLY", "10"},
    // Build 94: frames with fewer than 64 push descriptor updates each (FEX's Turnip build counts them) for 500 ms are a
    // loading screen too. D2R draws most of a loading screen at full speed (~7-9 updates a frame, play 190 or more): in
    // the build 93 run a 5 s loading screen at 40 FPS went unseen and the code buffer filled in play 2 minutes later
    // (see ARM64EC/LoadScreen.h).
    {"FEX_LOAD_LIGHT", "64"},
    // Build 85: in calm play of the build 84 run FEX placed ~78 blocks a second in the code buffer, ~8% of the 128 MB a
    // minute, more than D2R's whole working set (~13,000 blocks) every 3 minutes, nearly all loaded from the disk cache:
    // code that was dropped and runs again (D2R's protection decrypts its code pages on demand). Code that comes back the
    // same is now mapped again from the copy already in the buffer instead of placing another.
    {"FEX_CODE_REUSE", "1"},
    // Build 95: the same for translated code. In the build 94 run D2R's protection module rewrote its ~4.5 MB of code
    // pages every minute or two, and FEX translated the same functions again each time (up to 55 times in 10 s, 70-170 KB
    // each with self-checks, which the disk cache doesn't keep): 42 MB of the code buffer that filled at 12:47, and
    // 0.3-0.5 s of CPU a second on that thread. Code that comes back the same now maps its earlier translation again.
    {"FEX_JIT_REUSE", "1"},
    // Build 87: a main thread read of 256 KB or more of a game data file is asked of storage whole before it runs (many
    // requests at once instead of one read-ahead window after another). In the build 82-86 logs the main thread's cold
    // reads went at ~180 MB/s, and one 3968 KB read at an area's entry took 18-23 ms in every run (see
    // ARM64EC/GameFiles.cpp).
    // Build 88: off. In the build 87 log the same read took 17.4 ms (25.1 at another spot), still ~230 MB/s, and asking
    // cost ~1 ms each time. Build 88 times what those reads do instead ('slow main' details, 'storage clock' lines).
    {"FEX_GAME_FILES_AHEAD", "0"},
    // Build 89: the game data the main thread waited for storage for during play is remembered across sessions, read in
    // at the start and at every loading screen, and kept (budget in MB; see ARM64EC/GameFiles.h). In the build 88 log
    // storage ran at its low clock (75 of 300 MHz) during every slow main-thread read: in play the game reads in short
    // bursts, too short for Android to raise the clock, so a 3968 KB read took 20-21 ms (~200 MB/s, against ~950 MB/s in
    // loading screens), and an area entry had a 124 ms frame. The same reads come back in every session.
    {"FEX_GAME_FILES_LEARN", "256"},
    // Build 64: the build 63 log had ~1 s of 50-55 ms frames after each area load, with 14-35 ms a frame of GPU work
    // outside the game's render passes or command lists. vkd3d zero-fills every new small buffer and texture it places
    // in its own memory blocks on the GPU, and the game's queues wait for it; FEX's vkd3d-proton build (fex11) skips the
    // fill where the block's pages were never used since the driver zeroed them (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_SKIP_FRESH_CLEARS", "1"},
    // Build 66: GameNative's window system shows a presented image without waiting for the GPU, so a frame handed over
    // before the GPU drew it shows what the image held before (an older frame). In the build 65 log fights handed every
    // frame over ~41 ms before the GPU finished it. FEX's vkd3d-proton build (fex12) then waits for the GPU before
    // vkQueuePresentKHR (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_PRESENT_DRAWN", "1"},
    // Build 67: no change to the game. With the hitches gone, fights are GPU-bound at 23-29 FPS, and ~20 ms of a 45 ms
    // fight frame is the main pass, where a few lighting pixel shaders of ~7,000 instructions take 12-29 us a draw.
    // FEX's vkd3d-proton build (fex13) copies these shaders (by D3D bytecode hash, costliest first), their PSOs and
    // the driver's compiled code into the stats log, so their cost can be taken apart off the device (see
    // EnvironmentVariablesHandling.h, ARM64EC/Vkd3dReport.cpp).
    // Build 76: off. The copies did their job (builds 67-71 took the lighting shaders apart from them); they were 37% of a
    // log and kept the driver compiling 4 pipelines with its internal representations while the game loads. The hashes:
    // 5ff3d3024d2fe860,5b7fecfba4c4c6b0,fb5d9d516da6de30,0e155e00b71ab675,2d370003cb44c8a1,2f7464fc8033e6be,
    // 1071ee16e09ed9f2,ef0fa3aad756de0d,d04262aee603ba0a.
    {"FEX_VKD3D_SHADER_COPY", "0"},
    // VKD3D fex16: in the b74 and b77 logs the game's 1 s wait for its frame latency credit timed out during a hitch at
    // startup, and the frame it then ran without one left a credit too many for the whole session: one frame more in
    // flight, ~100 ms from frame start to screen with no hold-back (~67 with two frames). FEX's vkd3d-proton build
    // holds back one hand-out of the credit once it sees the game running ahead of the latency it set (see
    // EnvironmentVariablesHandling.h). That VKD3D build also turns it on by itself while frame start pacing is on.
    {"FEX_VKD3D_LATENCY_RESYNC", "1"},
    // Build 80 + VKD3D fex17: D2R keeps creating heaps while it is played (GPU memory grew 3.1 -> 3.8 GB in 95 s of the
    // b79 run), each a new driver allocation the kernel zeroes on the calling thread (~0.15 ms per MB). Twice the main
    // thread made 96 MB in one frame (~14 ms, the frame came late) and a 256 MB heap on a worker made Android reclaim
    // memory while the main thread waited. FEX's vkd3d-proton build makes spares on a background thread so the game's
    // threads take one instead (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_MEMORY_AHEAD", "1"},
    // Build 98 + VKD3D fex29: D2R's lighting pixel shaders index two of their root constant buffers with a register
    // (90 and 16 vectors declared), and vkd3d's shader converter declares any such buffer as 64 KiB, too big for the
    // GPU's constant registers, so Turnip reads them from memory through vkd3d's root constant buffer set, the push
    // descriptor set vkd3d pushes for nearly every draw. With the declared sizes they fit, and with FEX_TU_CP_UBO the
    // driver loads them before the draw (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_CBV_SIZE", "1"},
    // Build 108 + VKD3D fex30: D2R's compute shader 83e2eb4a6bf5f03b (a 3D lighting table blur, 8,192 filtered samples per
    // cell, 30 ms per run, in bursts of up to 19 runs when the game enters an area) made 34 of the 40 frames of 40 ms or
    // more in b107's 147 s of play. FEX's vkd3d-proton build swaps it for FEX's own version that samples each distinct
    // value once per group of cells (see EnvironmentVariablesHandling.h).
    // Build 109 + VKD3D fex31: b108's run took 12-13 ms per run with that version (was 30), now held back by the number
    // of instructions per summed product. VKD3D fex31 has two faster versions (4 and 2 cells per GPU thread); with alt,
    // binds of the shader take turns between them, so one session times both.
    // VKD3D fex32 (b109: 4 cells 8.5 ms per run, held back by shared memory reads): two versions with 8 cells per
    // thread that read shared memory 3 or 2 times per 8 products instead of 8; alt again takes turns.
    // Build 110: fex33's run timed D2R's ground clutter shader with FEX's early tests at 0.66 ms per frame in town (the
    // game's own 1.14) and 1.16 outdoors (1.19), never slower; FEX's versions only from now on.
    {"FEX_VKD3D_SHADER_SWAP", "1"},
    // Build 110 + VKD3D fex34: D2R clears its second 4096 x 4096 shadow map in a render pass of its own every frame
    // (0.55-0.69 ms of GPU time), though nothing draws into it. vkd3d remembers on the GPU when that image still holds
    // the cleared value and the GPU then skips the clear (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_CLEAR_MEMO", "1"},
    // Build 122 + VKD3D fex35: each frame also without D2R's UI layer, for the frame generation layer (see FEX_LSFG_UI).
    {"FEX_VKD3D_UI_SPLIT", "1"},
    // Build 68: the main pass's time grows ~14.6 us per draw at about the same shaded pixel count (b67: 238 draws 5.4 ms,
    // 963 draws 15.9 ms, ~0.85M pixels each), and a draw timed alone takes 23-33 us even with 64 vertices or fewer (fex9).
    // Before any pixel of a draw is shaded, Turnip runs the shader's preamble on one thread: in the lighting shaders 6-14
    // dependent memory reads and ~300 cycles of setup, not hidden (the copies say 'Early preamble = false'). Turnip then
    // compiles without that preamble (IR3_SHADER_DEBUG=nopreamble, see EnvironmentVariablesHandling.h).
    // Build 69: back off; b68 showed no change (main pass 13.3-14.1 us/draw with and without), and the dependent reads
    // just moved into every wave.
    // Build 104: alt. Since fex9/fex10 the lighting shaders' preamble reads no memory; it is 137 instructions (about 70
    // one-value constant stores) that the a7xx runs once per draw before the draw's waves may start. Build 103 counted
    // all wave states: lighting waves spend 25-30% of their life in HWAVE_WAIT (1,500-2,300 cycles each, more in small
    // draws), which no other pass shows (prepass 3-4% with equally small draws). alt = FEX's Turnip (fex13) compiles
    // those shaders a second time without preamble (+0.7% instructions, same registers) and pipelines use that binary
    // in every other second, so one run compares both ('np on/off' pass kinds). lit = always that binary, 1 = the old
    // IR3_SHADER_DEBUG=nopreamble for every shader, 0 = off.
    // Build 105: off. Build 104 measured it: the main lighting pass at the same draws +0.2% (+-0.6%) without preamble,
    // HWAVE_WAIT per wave unchanged (2,384 vs 2,342 cycles), whole GPU frame 25.0 ms both ways. FEX_TU_NOPREAMBLE=alt or
    // lit still turn it on.
    {"FEX_TU_NOPREAMBLE", "0"},
    // Build 69: FEX's own Turnip build (turnip-patches/). Stock Turnip invalidates the GPU's descriptor caches of all 8
    // descriptor sets on every descriptor update, and vkd3d pushes the root constants' descriptors (a push set) before
    // nearly every draw, so every draw of the ~1,000 in a fight's main pass starts with cold descriptor caches. FEX hands
    // the driver a link (TU_FEX_CHANNEL, see Common/TurnipChannel.h): in direct passes only the sets whose address changed
    // get invalidated, an identical push keeps its old copy, and FEX turns this on in every other second so one run
    // compares both ways ('turnip' lines, ARM64EC/TurnipReport.cpp). The driver also has the GPU count shader-core,
    // texture and cache events around the big passes.
    {"FEX_TU_CHANNEL", "1"},
    {"FEX_TU_DESC", "0"},
    // Build 76: off. The counter study is done (builds 70-74); each pass of 100 draws or more paid two GPU waits for idle
    // (and ~50 register reads) to be timed and counted, and the 'turnip pass' lines were 14% of a log.
    // Build 102: on again for one measuring run. Builds 100 and 101 removed command
    // processor work and lighting shader instructions without GPU time saved, so the main pass's limit isn't where
    // build 74's counters pointed any more; build 102's list (EnvironmentVariablesHandling.cpp) says where a lighting
    // wave's time goes and what its texture fetches wait on.
    // Build 103: on for a second measuring run with every wave state counter (build 102 left ~40% of a lighting wave's
    // time in states its list didn't count).
    // Build 107: a compute list; with driver fex16 every direct dispatch of 16+ workgroups is timed and counted too.
    {"FEX_TU_COUNTERS", Measuring ? "1" : "0"},
    {"FEX_TU_SAMPLE_DRAWS", Measuring ? "100" : "0"},
    // Build 71: build 70 found the main pass's lighting draws each wait for their setup step (the shader preamble: ~730
    // instructions, ~108 memory reads, 47 branches) inside the draw's first pixel wave. Qualcomm's own driver and stock
    // Turnip run it ahead of the draw ('early preamble'), but Turnip refuses here: with vkd3d's descriptor buffers it
    // treats the push set holding the root constant buffers as maybe not written, so those reads can't run early.
    // FEX's Turnip fex2 treats push sets as written (unpushed slots read as empty), and the preamble shrinks to ~250
    // branch-free instructions that run early (checked on the PC with the game's 4 lighting shaders).
    {"FEX_TU_EARLY_PREAMBLE", "1"},
    // Build 92: in fights the main pass's lighting draws are ~41% of the GPU frame (10.7 ms for ~1,200 draws), and each
    // draw first waits for its early preamble, which runs as one thread ahead of the draw's pixels. Turnip's compiler
    // placed that preamble's memory reads in 5 separate waits, with its 11 constant buffer copies (ldc.k) last, because
    // an ordering rule keeps them after every constant store and its scheduler issues reads only when nothing else is
    // left; it also stored 24 values up to four times (122 stores for 58 values). FEX's Turnip fex6 issues the reads
    // first and the copies right after (they overlap the math instead of adding a wait at the end), and stores each
    // value once: the 4 lighting shaders go from 5 waits and 243-253 preamble instructions to 2 waits and 174-184,
    // still early (checked on the PC with the game's shaders; each moved value checked to be the same).
    {"FEX_TU_PREAMBLE_LOADS", "1"},
    // Build 96: in fights the main pass's lighting draws cost ~8 us each (build 95: 1,427 draws, 11.35 ms of the ~28 ms
    // GPU frame), and the GPU only keeps about two draws' pixel work in flight, so each draw's cost is how long its
    // pixels' shader runs. In the 4 lighting shaders that time is set by the light and decal list loops: per light the
    // shader waits for the list entry (which light), then for the light's data at that entry, two memory round trips
    // one after the other, before ~100 math instructions. More lights in a fight, more of these waits per pixel. FEX's
    // Turnip fex7 reads the next light's list entry while the current light is worked on, so the light data and the
    // next entry go out together and share one wait (one round trip per light instead of two). An empty list reads
    // nothing (the first read is skipped when the loop doesn't run). Checked on the PC with the game's 4 shaders: 4
    // list loops each, same registers (24) and waves per core (8), early preamble kept.
    {"FEX_TU_LOOP_PREFETCH", "1"},
    // Build 72: in fights each frame draws two 4096x4096 shadow maps (and one 2048x2048), and each starts with a depth
    // clear that Turnip does with the GPU's 2D engine: ~1.1 ms per 4096 map at ~25 pixels per clock (the 'last tile
    // began' times of the pass meter), ~2.6 ms of the ~26 ms GPU frame. FEX's Turnip fex3 can clear them with one 3D draw
    // through the depth unit instead (what vkCmdClearAttachments does inside a pass), compressed like the game's own depth
    // writes; alt switches it on in every other second so one run measures both ways ('turnip pass 4096x4096 ... clear
    // 3d|2d' lines, and the pass meter's 'last tile began').
    // Build 74: build 73 measured the 3D clear ~0.55 ms faster per 4096 map, so it stays on; the clear draw still ran
    // its empty fragment shader on all 16.8 million pixels (every shader core busy ~0.5 ms). Turnip fex4 can run it with
    // the fragment shader disabled (the GPU's depth-only 'fast Z' mode, as Turnip does for depth-only pipelines); alt
    // now switches that on in every other second ('clear fastz|3d' lines).
    // Build 75: build 74 measured no gain from fast Z (1.19 vs 1.19 ms per 4096 map at the same load; the clear is
    // limited by writing depth to memory), so it is the 3D draw always, without the alternation.
    {"FEX_TU_DEPTH_CLEAR", "1"},
    // Build 77: build 76 found D2R's main thread in vkGetPhysicalDeviceMemoryProperties2 for 3% of every second of play
    // (~1 ms of every frame): DXGI's QueryVideoMemoryInfo asks it for the memory budget each frame, and Turnip reads all
    // of /proc/meminfo for every query. FEX's Turnip fex5 reads it at most every 250 ms and reuses the last value in
    // between (the budget is a rough estimate anyway); the 'turnip: memory budget' line counts queries, reads and time.
    {"FEX_TU_BUDGET_CACHE", "1"},
    // Build 98: the fex28 probe measured ~1.4 us more GPU time per long-shader draw when the root constant buffers
    // change between draws, and D2R changes them for nearly every main pass draw (~1,000-1,400 in fights). Stock Turnip
    // then rebinds and invalidates all 8 descriptor sets for each draw, and the lighting shader's preamble fetches ~100
    // vectors through the new descriptors with loads that wait on each other before the draw's pixels start. FEX's
    // Turnip fex9 has the command processor load those ranges straight into the shader's constants from the address
    // vkd3d pushed, and keeps the descriptor set state when only the root set changed and the shaders no longer read it
    // through descriptors.
    {"FEX_TU_CP_UBO", "1"},
    // Build 99: build 98's log showed the skipped descriptor set change working (a 350-draw main pass at every start
    // 1.72-2.39 -> 0.79 ms), but D2R's skinning vertex shaders index their bone matrices (cb2, 1536 vectors), which
    // doesn't fit the constants, so they still read the root set through descriptors: their draws (76% of shadow pass
    // draws) kept changing the descriptor set state and paid the command processor loads on top (shadow 1.98 -> 2.35
    // us/draw). FEX's Turnip fex10 reads those buffers through the shader's own UBO table, written from the pushed
    // descriptor before each draw, so the skinned draws skip the change too.
    {"FEX_TU_UBO_TABLE", "1"},
    // Build 100: build 99's log, pass time regressed on draws and pipeline switches: each switch costs ~1.5 us of GPU
    // time in the shadow passes and ~3.5 us in the depth prepass, 31-44% of their draws switch (fights: ~900 switches a
    // frame in depth passes, ~2 ms). Stock Turnip makes the command processor run ~16 state blocks again at every
    // switch (each pipeline has its own copy of its fixed-function state, so identical bytes still get a new address)
    // and resets every shader stage, so an unchanged vertex shader or the shared empty fragment shader of depth-only
    // pipelines is sent again too. FEX's Turnip fex11 keeps identical blocks and resets only the stage that changed.
    // alt = in every other second, so this run measures it ('turnip: pipeline switch' line, 'sw on/off' pass kinds).
    // Build 101: build 100 measured it: blocks run per switch 15.8 -> 4.1, but no GPU time saved (shadow 1.27 vs 1.27 ms
    // per 4096 map, main pass -1.4% +-2.3 at the same draws), so the per-switch cost is the draws' own work, not the
    // command stream. Off; FEX_TU_SWITCH=1 or alt still turn it on.
    {"FEX_TU_SWITCH", "0"},
    // Build 101: the main pass costs about one lighting shader wave per draw (build 70: ~12k GPU cycles a draw, ~2 draws
    // in flight), so its time follows the lighting shaders' length. They are ~6,600-7,200 instructions, 17% of them
    // alias.tex helpers (each lets one texture fetch read its coordinates from scattered registers, and is its own
    // instruction). FEX's Turnip fex12 compiles them a second time without those helpers (11-12% fewer instructions, with
    // more movs and 32 instead of 24 registers); whether a helper costs the GPU a cycle like a mov is not documented, so
    // alt uses the second binaries in every other second and the log compares the main pass at the same draws
    // ('turnip: no alias' line, 'na on/off' pass kinds).
    // Build 102: off. Build 101 measured it: the main pass at the same draws took 5.65 ms with the stock binaries and
    // 5.77 ms without alias.tex (+2..4%, +-4%), so the helpers cost about nothing; off saves compiling 113 big fragment
    // shaders twice. FEX_TU_NOALIAS=1 or alt still turn it on.
    {"FEX_TU_NOALIAS", "0"},
    // Build 105: build 104 left the lighting waves' HWAVE_WAIT (25-30% of their life) unexplained by the preamble. A
    // lighting draw takes about one wave life (cycles per draw 7,066, wave life 7,081), and the wait grows with the wave's
    // life and its texture waits, not with how many waves share a core. A Qualcomm patent (US11954758, dynamic wave
    // pairing) describes the a6xx/a7xx running two 64-pixel waves as a pair that parts at texture and branch blocks and
    // joins again, the one that got ahead waiting for the other. FEX's Turnip fex14 gives the big fragment shaders other
    // wave setups, and alt rotates them a second each: 0 stock, 1-3 the pair mode field the driver leaves at 0
    // (SP_PS_CNTL_1.WAVE_PAIR_MODE, undocumented), 4 the shaders compiled as 128-pixel waves (one wave, no partner;
    // same instructions and registers). A list of digits (e.g. 04) rotates only those, one digit = that setup always,
    // 0 = stock. Pass kinds carry the setup ('stock', 'pair1'..'pair3', 'w128').
    // Build 106: off. Build 105 measured it: 128-pixel waves halve the half-wave wait but make the lighting pass 12%
    // slower, pair modes 1-3 gave no reliable change (-3% pooled, halves +3% and -4.5%). FEX_TU_WAVE_PAIR=alt or digits
    // still turn it on when FEX_TU_DRAW_SETUP=0.
    {"FEX_TU_WAVE_PAIR", "0"},
    // Build 106: build 105 found about one lighting draw's waves on the shader cores at a time (5.9 waves per busy core
    // with room for 8, a draw costs one wave life), so the next draw doesn't start into the free slots. FEX's Turnip fex15
    // gives the big fragment shaders other draw setups, and alt rotates them a second each: 0 stock, 4 a second binary
    // with no preamble at all (push constants loaded by the command processor; build 104's binary kept a 6-instruction
    // copy, so the early preamble still ran before every draw), 5 a third binary with one constant load per root constant
    // buffer (12 loads of 6 buffers -> 6 for the main lighting shader), 6 SP_PS_CNTL_1.DEFER_WAVE_ALLOC_DIS 0, 7
    // EVICT_BUF_MODE 0 (the driver always writes 1 and 1; not documented). Digits pick a list (e.g. 045), one digit =
    // that setup always, 0 = off (then FEX_TU_WAVE_PAIR decides). Pass kinds carry the setup ('stock', 'nopre', 'merge',
    // 'defer', 'evict').
    // Build 107: 4 (the binary with no preamble at all) always. Build 106 measured it: two lighting draws on the GPU at
    // once 10% more often (dual fs 0.46 -> 0.50), the front end's wait for draw data -10%, lighting pass about -5% (-2%
    // and -7% in the run's halves; prepass and shadow passes within 1.5%). Merged loads cut the overlap 12% (more data
    // per draw), the two alloc settings changed nothing. FEX_TU_DRAW_SETUP=alt or digits still rotate setups, 0 = stock.
    {"FEX_TU_DRAW_SETUP", "4"},
    // Build 107: the ground clutter compute shader (0e21a3b5088ba1af, a 1152x1152 grid, ~1.2 ms of every frame, the
    // biggest of the ~2.9 ms of compute) appends each item it places to one list: an atomic add on one counter per item,
    // so thousands of lanes queue on one address. FEX's Turnip fex16 compiles compute shaders whose memory atomics hit
    // one address for the whole wave a second time with nir_opt_uniform_atomics (one lane adds the wave's total, each
    // lane gets its slot from that), as Mesa's AMD, Intel and Apple drivers do; only that shader qualifies (2234 -> 2308
    // instructions). alt = that binary in every other second, compared on the 'turnip dispatch' lines; 1 = always,
    // 0 = stock.
    // Build 108: off. In b107 the once-per-wave binary was 5.9% slower (1278 vs 1202 us per run, same 4-second blocks):
    // its atomics fell from 2457 to 514 per run, but only ~2,500 items are placed per run, so the counter never held the
    // shader up.
    {"FEX_TU_CS_ATOMICS", "0"},
    // Build 110: the lighting pass leaves ~4 of the GPU's 12 shader cores idle between draws: the next draw's setup isn't
    // ready. Every D2R draw changes one or two root constant buffers, and Turnip (fex9) then loads all of the draw's root
    // constant buffer ranges again (~100 vec4 per lighting draw, of which ~16 changed); in b106 more constants per draw
    // meant less draw overlap (+53% vec4: 12% less time with two fragment draws active). FEX's Turnip fex17 keeps ranges
    // whose buffer address didn't change in the shaders' constants and loads only the changed ones with the draw.
    // alt = in every other second, compared on the 'turnip pass ... keep on/off' lines; 1 = always; 0 = stock.
    // Build 111: off. In the build 110 log lighting passes took as long with kept constants as without (-0.1%, +-1.3%)
    // although ~30% fewer constants were loaded per draw, so constant loading isn't what holds the next draw back.
    {"FEX_TU_CONST_KEEP", "0"},
  };

  // In the order the stats log lists them.
  constexpr const char* Known[] = {
    "FEX_STATSLOG",
    "FEX_STATSLOG_APP",
    "FEX_STATSLOG_PROFILE",
    "FEX_STATSLOG_TIMELINE",
    "FEX_STATSLOG_SCAN",
    "FEX_THREADPIN",
    "FEX_NATIVE_ZLIB",
    "FEX_NATIVE_MD5",
    "FEX_UNPACK_CACHE",
    "FEX_UNPACK_CACHE_MAX_GB",
    "FEX_UNPACK_CACHE_RESET",
    "FEX_UNPACK_CACHE_READAHEAD_MB",
    "FEX_UNPACK_CACHE_DIRECT",
    "FEX_UNPACK_CACHE_PLAY",
    "FEX_UNPACK_CACHE_AHEAD",
    "FEX_UNPACK_CACHE_MAIN_PLAY",
    "FEX_UNPACK_CACHE_LEARN",
    "FEX_UNPACK_CACHE_FLUSH",
    "FEX_CODE_BUFFER_START",
    "FEX_CODE_BUFFER_RENEW",
    "FEX_CODE_BUFFER_RENEW_EARLY",
    "FEX_CODE_BUFFER_LOAD_MS",
    "FEX_LOAD_LIGHT",
    "FEX_LOAD_LIGHT_MS",
    "FEX_CODE_REUSE",
    "FEX_JIT_REUSE",
    "FEX_GAME_FILES_UNCACHED",
    "FEX_GAME_FILES_MS",
    "FEX_GAME_FILES_KEEP_MB",
    "FEX_GAME_FILES_GUARD_MB",
    "FEX_GAME_FILES_READ_WATCH",
    "FEX_GAME_FILES_AHEAD",
    "FEX_GAME_FILES_LEARN",
    "FEX_UNPACK_CACHE_SKIP_ADLER",
    "FEX_NATIVE_ADLER_NEON",
    "FEX_NATIVE_MD5_CACHE",
    "FEX_JOBWAIT",
    "FEX_JOBWAIT_BACKOFF",
    "FEX_DISKCACHE_PREFETCH",
    "FEX_DISKCACHE_PREFETCH_MB",
    "FEX_FRAMETIMES",
    "FEX_CODELOCK_POLITE",
    "FEX_PRIO",
    "FEX_HELPER_PRIO",
    "FEX_STALLPROBE",
    "FEX_MAINCORE",
    "FEX_MAINCORE_WAIT",
    "FEX_MAINCORE_ESCAPE",
    "FEX_MAINCORE_ESCAPE_LIVE",
    "FEX_MAINCORE_ESCAPE_REPEAT",
    "FEX_MAINCORE_ESCAPE_QUIET",
    "FEX_MAINCORE_ESCAPE_STAY",
    "FEX_MAINCORE_AWAY_SHARE",
    "FEX_MAINCORE_ESCAPE_NICE",
    "FEX_MAINCORE_OTHERS",
    "FEX_HELPER_CORES",
    "FEX_SERVER_NICE",
    "FEX_MAIN_NICE",
    "FEX_WORKER_NICE",
    "FEX_LOADER_CORES",
    "FEX_LOADER_WATCH",
    "FEX_LOADER_GAME_EXCEPTIONS",
    "FEX_SCHEDLOG",
    "FEX_GPULOG",
    "FEX_FRAMELATENCY",
    "FEX_VKD3D_NOSTAGGER",
    "FEX_RUMBLE",
    "FEX_RUMBLE_MS",
    "FEX_SMC_HOTPAGE",
    "FEX_VKD3D_FOLD_CLEARS",
    "FEX_VKD3D_PASSES",
    "FEX_VKD3D_TIMELINE",
    "FEX_VKD3D_CAP_PRESLEEP",
    "FEX_VKD3D_FRAME_START",
    "FEX_VKD3D_CAP_GRID",
    "FEX_LSFG_DEFER",
    "FEX_LSFG_GEN_DRAWN",
    "FEX_LSFG_EARLY",
    "FEX_LSFG_PRESENT_EARLY",
    "FEX_LSFG_GPU_TIMES",
    "FEX_LSFG_PRESENT_NICE",
    "FEX_LSFG_GEN_GAP",
    "FEX_LSFG_UI",
    "FEX_LSFG_UI_TOL",
    "FEX_VKD3D_SKIP_FRESH_CLEARS",
    "FEX_VKD3D_PRESENT_DRAWN",
    "FEX_VKD3D_SHADER_COPY",
    "FEX_VKD3D_LATENCY_RESYNC",
    "FEX_VKD3D_MEMORY_AHEAD",
    "FEX_VKD3D_CBV_SIZE",
    "FEX_VKD3D_SHADER_SWAP",
    "FEX_VKD3D_CLEAR_MEMO",
    "FEX_VKD3D_UI_SPLIT",
    "FEX_VKD3D_LOG_OFF",
    "FEX_TU_NOPREAMBLE",
    "FEX_TU_CHANNEL",
    "FEX_TU_DESC",
    "FEX_TU_COUNTERS",
    "FEX_TU_SAMPLE_DRAWS",
    "FEX_TU_EARLY_PREAMBLE",
    "FEX_TU_PREAMBLE_LOADS",
    "FEX_TU_LOOP_PREFETCH",
    "FEX_TU_DEPTH_CLEAR",
    "FEX_TU_BUDGET_CACHE",
    "FEX_TU_CP_UBO",
    "FEX_TU_UBO_TABLE",
    "FEX_TU_SWITCH",
    "FEX_TU_NOALIAS",
    "FEX_TU_WAVE_PAIR",
    "FEX_TU_DRAW_SETUP",
    "FEX_TU_CS_ATOMICS",
    "FEX_TU_CONST_KEEP",
  };

  bool UseD2RDefaults {};

  bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      const auto Lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
      };
      if (Lower(a[i]) != Lower(b[i])) {
        return false;
      }
    }
    return true;
  }

  const char* FromEnvironment(const char* Name) {
    const char* Value = getenv(Name);
    return Value && Value[0] ? Value : nullptr;
  }

  const char* FromDefaults(const char* Name) {
    if (UseD2RDefaults) {
      for (const auto& D : D2RDefaults) {
        if (strcmp(D.Name, Name) == 0) {
          return D.Value;
        }
      }
    }
    return nullptr;
  }
} // namespace

void InitSwitches(std::string_view AppName) {
  UseD2RDefaults = EqualsIgnoreCase(AppName, "D2R.exe");
}

const char* Switch(const char* Name) {
  if (const char* Value = FromEnvironment(Name)) {
    return Value;
  }
  return FromDefaults(Name);
}

fextl::string SwitchSummary() {
  fextl::string Line;
  for (const char* Name : Known) {
    const char* Environment = FromEnvironment(Name);
    const char* Default = FromDefaults(Name);
    Line += fextl::fmt::format(" {}={}{}", Name,
                               Environment ? Environment :
                               Default     ? Default :
                                             "-",
                               !Environment && Default ? "(default)" : "");
  }
  return Line;
}
} // namespace FEX::Windows
