// SPDX-License-Identifier: MIT
#pragma once

// Loader call watch (FEX_LOADER_WATCH, with the stats log). In the fex10 run (FEX build 61) D2R's loader thread (see
// FEX_LOADER_CORES in Common/PriorityGuard.h) spent its busy phases in a loop of CreateEventW, WaitForMultipleObjects and
// CloseHandle, at ~0.65 of a core plus ~0.25 of a core of wineserver work: with Wine, creating and closing an event are
// wineserver round trips, and waiting on a new event fetches its file descriptor from the wineserver first. Reusing
// events would save most of that, but only if the loop's events are its own and short-lived, which a log can't tell yet.
//
// Once the priority guard finds a loader thread, Module.S ExitFunctionEC runs FEX's wrappers in place of kernelbase's
// CreateEventW, CloseHandle and WaitForMultipleObjects for that thread's calls (other threads' calls are untouched). The
// wrappers call Wine's functions unchanged and count what the calls were. Every second a "loader calls" line says, per
// function: calls, time in them, the arguments (event type, handle counts, timeouts), the results, whether the waits and
// closes were on events the thread had just created (and how long those lived), and where in the game's code the calls
// came from.
//
//   FEX_LOADER_WATCH=1  watch the loader threads' calls (the D2R default, with the stats log)
//   FEX_LOADER_WATCH=0  don't
namespace LoaderWatch {
// Call in process init, after the switches are read and before the priority guard starts.
void Init(bool StatsLog);
} // namespace LoaderWatch
