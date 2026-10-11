// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
class StatAlloc;

// Opt-in per-second stats log for devices where FEX's SHM stats can't be read from outside (Android/GameNative).
//
//   FEX_STATSLOG=<directory>      enables it; writes <directory>\fexstats-<exe>-<pid>.txt
//   FEX_STATSLOG_APP=<exe name>   optional; only log this process (e.g. D2R.exe)
//
// Returns true if the log should run in this process, in which case the caller must allocate stats slots for every
// thread and then call StartStatsLog().
bool StatsLogRequested(std::string_view AppName);
void StartStatsLog(const StatAlloc* Stats, std::string_view AppName);
} // namespace FEX::Windows
