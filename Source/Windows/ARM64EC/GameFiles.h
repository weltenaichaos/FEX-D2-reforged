// SPDX-License-Identifier: MIT
#pragma once

// The game's data files out of Android's file cache (FEX_GAME_FILES_UNCACHED). D2R reads its game data from CASC
// archives (<game>/Data/data/data.000, data.001, ...) with ReadFile, which Wine passes on as normal Linux reads, so every
// area load leaves hundreds of MB of them in the kernel's file cache. In the build 58 log the second area load filled
// memory that way while the game's own memory and GPU memory grew: the kernel had to reclaim memory on the game's threads,
// files other apps had used came back from storage at ~200 MB/s ("refaults", which Android's memory killer reads as
// thrashing), and the memory killer closed 20 background apps at once. Android then restarted them for ~35 s on 5-6 of
// the 8 cores, the game's slow stretch.
//
//   FEX_GAME_FILES_UNCACHED=1  a FEX thread (FEXFileCache) drops the game's big files from the file cache every
//                              FEX_GAME_FILES_MS (the D2R default). Pages a read still needs are kept by the kernel (in
//                              use, or not read in yet); pages already read are freed at once, so the game's reads no
//                              longer push other apps' files out of memory, and reading them again later is a normal
//                              read, not a refault.
//   FEX_GAME_FILES_MS=N        time between two drops (default 500; 100-10000)
//
// The files: every file of 32 MB or more in the folder of the game's exe and its subfolders (D2R's CASC archives), opened
// once by the thread itself, plus any other file of 32 MB or more the game has open (looked for in /proc/self/fd every
// 2 s). Build 59 looked only among the game's open files for data.NNN names and found none in the whole build 59 log, so
// nothing was dropped. FEX's own caches and the driver's shader caches are left alone. With the stats log on, a
// "game files" line says every second how many files are dropped and by how much the file cache shrank at the drops.
namespace GameFiles {
// Call on the process's first thread, in process init, after the switches are read.
void Init(bool StatsLog);
} // namespace GameFiles
