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
    {"FEX_NATIVE_ZLIB", "1"},      {"FEX_NATIVE_MD5", "1"}, {"FEX_UNPACK_CACHE", "1"},
    // Build 48: no thread at extreme Linux nice values (see PriorityGuard.h, Threads.cpp).
    {"FEX_PRIO", "normal"},        {"FEX_HELPER_PRIO", "normal"},
    // Build 49: the main thread alone on the fastest core, FEX's helpers on the slowest, the wineserver above the game
    // (see PriorityGuard.h).
    {"FEX_MAINCORE", "fast"},      {"FEX_HELPER_CORES", "little"}, {"FEX_SERVER_NICE", "-10"},
    // Build 51: the main thread may leave its core while it waits 40 ms a second or more for it (see PriorityGuard.h).
    {"FEX_MAINCORE_WAIT", "40"},
    // Build 50 tried one more frame in flight on the swapchain's frame latency handle (FEX_FRAMELATENCY=2). Heavy scenes
    // were no faster: the wait moved into vkd3d-proton's Present, the GPU or present path being the limit there. It only
    // added input lag, so build 51 leaves the handle as the game set it (see ARM64EC/FrameLatency.h).
    {"FEX_FRAMELATENCY", "off"},
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
    "FEX_SCHEDLOG",
    "FEX_GPULOG",
    "FEX_FRAMELATENCY",
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
