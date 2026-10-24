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
//   FEX_GAME_FILES_UNCACHED=1  a FEX thread (FEXFileCache) drops the game's open data.NNN files from the file cache every
//                              FEX_GAME_FILES_MS (the D2R default). Pages a read still needs are kept by the kernel (in
//                              use, or not read in yet); pages already read are freed at once, so the game's reads no
//                              longer push other apps' files out of memory, and reading them again later is a normal
//                              read, not a refault.
//   FEX_GAME_FILES_MS=N        time between two drops (default 500; 100-10000)
//
// The files are found among the process's open Linux files (/proc/self/fd), by name: a file data.<digits> in a folder
// named data. With the stats log on, a "game files" line says every second how many data files were open and by how much
// the file cache shrank at the drops.
namespace GameFiles {
// Call on the process's first thread, in process init, after the switches are read.
void Init(bool StatsLog);
} // namespace GameFiles
