// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include <FEXCore/fextl/string.h>

namespace FEXCore::Utils::WritePriorityMutex {
struct ContentionStats;
}

namespace FEX::Windows {
class StatAlloc;

// Opt-in per-second stats log for devices where FEX's SHM stats can't be read from outside (Android/GameNative).
//
//   FEX_STATSLOG=<directory>      enables it; writes <directory>\fexstats-<exe>-<HHMMSS>-<pid>.txt
//   FEX_STATSLOG_APP=<exe name>   optional; only log this process (e.g. D2R.exe)
//   FEX_STATSLOG_SCAN=1           optional; scan the main executable every second for the preload probe ("pre exe" lines)
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
// Provider runs on the log thread. Up to sixteen providers, in the order they were set.
void StatsLogSetLineProvider(fextl::string (*Provider)());
// The number of the last second the log sampled (1 for the first; the next one is being sampled now), 0 before the log
// runs or without it. A line provider runs right after its second was sampled, with this number.
uint64_t StatsLogSecond();

// Hook gets every sample (one per 2ms) of the main thread on the log thread: the native function it is in, or 0 while it
// runs emulated code or nothing is known. It returns 1 when that is a wait on a swapchain frame latency handle, 2 for
// another wait it watches, 0 otherwise (for the '# busy frame' lines).
void StatsLogSetMainSampleHook(uint8_t (*Hook)(uint64_t NativeTarget));

// Names a native function that isn't exported (FEX's own native redirects) for the native-call sampler's "native" lines.
void StatsLogNameNative(uint64_t Address, std::string_view Name);

// Frame times: Marks[0] counts the frame starts written so far and Marks[1..RingSize] holds the last RingSize of them as
// CNTVCT_EL0 values (RingSize a power of two), written by the game's main thread (FrameClock in ARM64EC/NativeRedirect.cpp).
// The log then says every second how long the frames took and, with FEX_STATSLOG_PROFILE=1, what the main thread was doing
// in every slow one.
void StatsLogSetFrameMarks(const uint64_t* Marks, size_t RingSize);

// Where FEX's code invalidation lock records its exclusive lock timings, printed every second as a "codelock" line.
FEXCore::Utils::WritePriorityMutex::ContentionStats* StatsLogCodeLockStats();

// Stall probe (FEX_STALLPROBE, on with FEX_STATSLOG_PROFILE=1 unless set to 0): a helper thread that every 4ms times how
// late its own sleep wakes up (CPU starvation), a VirtualQuery (Wine's memory lock), touching a fresh page (the kernel's
// memory map lock), Probe (FEX's global memory/SMC lock), and every 16ms a wineserver round trip. Each second's longest
// of each goes on a "probe" line, and every "# frame" line gets the longest ones that overlapped that frame.
void StatsLogSetLockProbe(void (*Probe)());
} // namespace FEX::Windows
