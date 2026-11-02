// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Build 146: the game's pipe traffic, for the stats log (FEX_PIPE_WATCH, logging build only).
//
// In the build 143 log the main thread read a pipe (\Device\NamedPipe\Win32.Pipes.00000324.00000004) twice every game
// frame, and once a frame it waited about 0.84 ms on an event from Steam's tier0_s64.dll: about 37 ms of the main
// thread's time each second went to that, very likely a round trip to steam.exe for every frame. The log didn't say who
// is at the other end, what goes back and forth, or where the time goes. With this on:
//
//   - every pipe the main thread reads or writes gets a '# pipe watch' note with its name and, for an anonymous pipe
//     (Wine names those Win32.Pipes.<pid of the process that made it>.<n>), the Windows process that made it, from the
//     list of Windows processes (written once, with each process's id, name and parent)
//   - a 'pipe watch' line each second: the main thread's reads and writes of each pipe (count, bytes, time, longest),
//     other threads' use of it, and the main thread's messages by size and first 8 bytes (what kind of message)
//   - a 'pipe peer' line each second for the process that made the pipe (steam.exe, found among this app's Linux
//     processes by name): how long its threads ran, how long they waited for a CPU, and its busiest threads
//
// WriteFile is followed like ReadFile (Module.S ExitFunctionEC runs FEXNativeWriteFile in place of kernelbase's
// WriteFile once this finds it; only with the stats log on). Nothing the game sees changes. FEX_PIPE_WATCH=0 turns it off.
namespace PipeWatch {
// Call on the process's first thread (the game's main thread) in process init, after the switches are read.
void Init(bool StatsLog);
// From the ReadFile watch (GameFiles.cpp), for every read of a file it doesn't follow itself. Main = the main thread.
void NoteRead(void* File, const void* Buffer, uint64_t Bytes, bool Queued, uint64_t Ticks, bool Main);
} // namespace PipeWatch
