// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include <FEXCore/fextl/string.h>

namespace FEX::Windows {
class StatAlloc;

// Opt-in per-second stats log for devices where FEX's SHM stats can't be read from outside (Android/GameNative).
//
//   FEX_STATSLOG=<directory>      enables it; writes <directory>\fexstats-<exe>-<HHMMSS>-<pid>.txt
//   FEX_STATSLOG_APP=<exe name>   optional; only log this process (e.g. D2R.exe)
//   FEX_STATSLOG_PROFILE=1        optional; also sample which JIT blocks the busiest threads run and log the hottest every 30s,
//                                 with the full guest code of the hottest ones the first time they show up, and which native
//                                 functions they are in when outside emulated code (per-thread "native" lines)
//
// Returns true if the log should run in this process, in which case the caller must allocate stats slots for every
// thread and then call StartStatsLog().
bool StatsLogRequested(std::string_view AppName);
void StartStatsLog(const StatAlloc* Stats, std::string_view AppName);

// Adds a "# ..." line to the log. Lines noted before the log is opened are kept and written after its header.
// No-op unless FEX_STATSLOG is set.
void StatsLogNote(std::string_view Line);

// Classifies a disk cache hit for the preload probe: was the main executable's page holding GuestRIP already readable
// at an earlier once-per-second scan (so a background preload could have loaded it), or not.
void StatsLogDiskCacheHit(uint64_t GuestRIP);
// Sets the image the preload probe watches (the main executable). Call before StartStatsLog.
void StatsLogWatchImage(uint64_t Base);

// Adds the lines Provider returns (each indented and ending in a newline, or nothing) to every per-second block.
// Provider runs on the log thread.
void StatsLogSetLineProvider(fextl::string (*Provider)());

// Names a native function that isn't exported (FEX's own native redirects) for the native-call sampler's "native" lines.
void StatsLogNameNative(uint64_t Address, std::string_view Name);
} // namespace FEX::Windows
