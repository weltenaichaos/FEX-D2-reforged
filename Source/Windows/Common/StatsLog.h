// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
class StatAlloc;

// Opt-in per-second stats log for devices where FEX's SHM stats can't be read from outside (Android/GameNative).
//
//   FEX_STATSLOG=<directory>      enables it; writes <directory>\fexstats-<exe>-<HHMMSS>-<pid>.txt
//   FEX_STATSLOG_APP=<exe name>   optional; only log this process (e.g. D2R.exe)
//   FEX_STATSLOG_PROFILE=1        optional; also sample the busiest threads' RIP and log the hottest code every 30s
//
// Returns true if the log should run in this process, in which case the caller must allocate stats slots for every
// thread and then call StartStatsLog().
bool StatsLogRequested(std::string_view AppName);
void StartStatsLog(const StatAlloc* Stats, std::string_view AppName);

// Adds a "# ..." line to the log. Lines noted before the log is opened are kept and written after its header.
// No-op unless FEX_STATSLOG is set.
void StatsLogNote(std::string_view Line);
} // namespace FEX::Windows
