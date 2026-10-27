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

  // D2R.exe runs with what its tests have used since build 40, so a test needs no environment variables. Every other
  // program keeps upstream behaviour unless a variable is set. A variable set to 0 still turns a default off.
  constexpr Default D2RDefaults[] = {
#ifndef FEX_DISABLE_STATSLOG
    {"FEX_STATSLOG", "D:\\"}, // where the tests' logs have gone so far
    {"FEX_STATSLOG_PROFILE", "1"},
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
    {"FEX_VKD3D_PASSES", "1"},
    {"FEX_VKD3D_TIMELINE", "1"},
    // Build 56: the build 55 log showed the frame cap sleeping ~32 ms in every vkQueuePresentKHR on vkd3d's queue thread,
    // with the VkQueue the game's compute and copy queues share held all that time, and each frame waiting ~23 ms for the
    // queue thread after the game was done with it. FEX's vkd3d-proton build sleeps the cap's wait out before the present
    // without the VkQueue, and holds back the game's frame start by the waiting it measured, as far as the game's own
    // slow frames allow (see EnvironmentVariablesHandling.h).
    {"FEX_VKD3D_CAP_PRESLEEP", "1"},
    {"FEX_VKD3D_FRAME_START", "1"},
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
    // Build 84: FEX's translated-code buffer starts at its full 128 MB, and is renewed at a loading screen once half used
    // (10% at the first three: start-up, menus, entering the game), rather than filling (and dropping all code) in play.
    // In the build 83 log every thread reloaded its code at once at 6:21, 5,301 blocks on the main thread in one second,
    // with no file reads (see ARM64EC/CodeBuffer.h).
    {"FEX_CODE_BUFFER_START", "max"},
    {"FEX_CODE_BUFFER_RENEW", "50"},
    {"FEX_CODE_BUFFER_RENEW_EARLY", "10"},
    // Build 85: in calm play of the build 84 run FEX placed ~78 blocks a second in the code buffer, ~8% of the 128 MB a
    // minute, more than D2R's whole working set (~13,000 blocks) every 3 minutes, nearly all loaded from the disk cache:
    // code that was dropped and runs again (D2R's protection decrypts its code pages on demand). Code that comes back the
    // same is now mapped again from the copy already in the buffer instead of placing another.
    {"FEX_CODE_REUSE", "1"},
    // Build 87: a main thread read of 256 KB or more of a game data file is asked of storage whole before it runs (many
    // requests at once instead of one read-ahead window after another). In the build 82-86 logs the main thread's cold
    // reads went at ~180 MB/s, and one 3968 KB read at an area's entry took 18-23 ms in every run (see
    // ARM64EC/GameFiles.cpp).
    // Build 88: off. In the build 87 log the same read took 17.4 ms (25.1 at another spot), still ~230 MB/s, and asking
    // cost ~1 ms each time. Build 88 times what those reads do instead ('slow main' details, 'storage clock' lines).
    {"FEX_GAME_FILES_AHEAD", "0"},
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
    // Build 68: the main pass's time grows ~14.6 us per draw at about the same shaded pixel count (b67: 238 draws 5.4 ms,
    // 963 draws 15.9 ms, ~0.85M pixels each), and a draw timed alone takes 23-33 us even with 64 vertices or fewer (fex9).
    // Before any pixel of a draw is shaded, Turnip runs the shader's preamble on one thread: in the lighting shaders 6-14
    // dependent memory reads and ~300 cycles of setup, not hidden (the copies say 'Early preamble = false'). Turnip then
    // compiles without that preamble (IR3_SHADER_DEBUG=nopreamble, see EnvironmentVariablesHandling.h).
    // Build 69: back off; b68 showed no change (main pass 13.3-14.1 us/draw with and without), and the dependent reads
    // just moved into every wave.
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
    {"FEX_TU_COUNTERS", "0"},
    {"FEX_TU_SAMPLE_DRAWS", "0"},
    // Build 71: build 70 found the main pass's lighting draws each wait for their setup step (the shader preamble: ~730
    // instructions, ~108 memory reads, 47 branches) inside the draw's first pixel wave. Qualcomm's own driver and stock
    // Turnip run it ahead of the draw ('early preamble'), but Turnip refuses here: with vkd3d's descriptor buffers it
    // treats the push set holding the root constant buffers as maybe not written, so those reads can't run early.
    // FEX's Turnip fex2 treats push sets as written (unpushed slots read as empty), and the preamble shrinks to ~250
    // branch-free instructions that run early (checked on the PC with the game's 4 lighting shaders).
    {"FEX_TU_EARLY_PREAMBLE", "1"},
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
    "FEX_CODE_BUFFER_START",
    "FEX_CODE_BUFFER_RENEW",
    "FEX_CODE_BUFFER_RENEW_EARLY",
    "FEX_CODE_BUFFER_LOAD_MS",
    "FEX_CODE_REUSE",
    "FEX_GAME_FILES_UNCACHED",
    "FEX_GAME_FILES_MS",
    "FEX_GAME_FILES_KEEP_MB",
    "FEX_GAME_FILES_GUARD_MB",
    "FEX_GAME_FILES_READ_WATCH",
    "FEX_GAME_FILES_AHEAD",
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
    "FEX_VKD3D_SKIP_FRESH_CLEARS",
    "FEX_VKD3D_PRESENT_DRAWN",
    "FEX_VKD3D_SHADER_COPY",
    "FEX_VKD3D_LATENCY_RESYNC",
    "FEX_VKD3D_MEMORY_AHEAD",
    "FEX_TU_NOPREAMBLE",
    "FEX_TU_CHANNEL",
    "FEX_TU_DESC",
    "FEX_TU_COUNTERS",
    "FEX_TU_SAMPLE_DRAWS",
    "FEX_TU_EARLY_PREAMBLE",
    "FEX_TU_DEPTH_CLEAR",
    "FEX_TU_BUDGET_CACHE",
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
