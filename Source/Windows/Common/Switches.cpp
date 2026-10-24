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
    // Build 51: the main thread may leave its core while it waits 40 ms a second or more for it (see PriorityGuard.h).
    {"FEX_MAINCORE_WAIT", "40"},
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
    {"FEX_LOADER_CORES", "little"},
    {"FEX_LOADER_WATCH", "1"},
    // Build 62: during play, unpack cache hits not in read-ahead memory are left to the game to unpack (1.9 ms average
    // disk reads during play in the fex10 run, around the 100-140 ms streaming hitches), and read-ahead is shorter
    // (see ARM64EC/NativeUnpackCache.cpp).
    {"FEX_UNPACK_CACHE_PLAY", "1"},
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
    "FEX_GAME_FILES_UNCACHED",
    "FEX_GAME_FILES_MS",
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
    "FEX_HELPER_CORES",
    "FEX_SERVER_NICE",
    "FEX_LOADER_CORES",
    "FEX_LOADER_WATCH",
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
