// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/string.h>

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
//
// Build 82: the most recently read data stays (FEX_GAME_FILES_KEEP_MB, default 512). Dropping everything every 500 ms
// meant every read of the game's data came from storage: 26 GB in the 400 s fex19 run, as much as the game asked for, and
// the main thread's reads of 25-35 MB right before area changes took 50-114 ms. The game's ReadFile calls now go through a
// FEX wrapper (FEX_GAME_FILES_READ_WATCH, on unless 0) that notes which 1 MB pieces of which file each read touched, and
// the drops leave out
//   - the pieces of the last quarter of the budget read by anyone (and the piece after each read, the kernel's readahead),
//   - the pieces of the rest of the budget read by the main thread (its reads are the ones a frame waits for; an area
//     load reading GBs on other threads doesn't push them out).
// When the device's available memory falls below FEX_GAME_FILES_GUARD_MB (default 2048) everything is dropped as
// before. FEX_GAME_FILES_KEEP_MB=0 drops everything as builds 60-81 did. A "game reads" line says every second how much
// the game read, how long the main thread waited for it with and without the data kept, how far back the same piece was
// last read (how big a budget would have kept it), and the main thread's slow reads.
//
// Build 89: the game data the main thread waited for storage for during play is learned (FEX_GAME_FILES_LEARN, the
// budget in MB, D2R default 256; 0 turns it off). The build 88 log found the storage controller (UFS) at its low clock,
// 75 of 300 MHz, during every slow read of the main thread: Android raises it only when storage is busy most of a
// 60 ms window, as in loading screens (~950 MB/s), while the game's reads in play come in short bursts (~200 MB/s: a
// 3968 KB read took 20-21 ms, and the 0:47 area entry had a 124 ms frame). The same reads come back in every session
// (00-00000008.data at 763 MB, 554 MB and 388 MB in every log since build 82). So
//   - a main thread read of a watched file that came from storage and took 1 ms or more, in a frame shorter than 250 ms
//     (play, not a loading screen), is added to a list of ranges (whole 64 KB blocks) in FEX's cache folder
//     (GameReadsLearned.txt: file name and size, offset, length, sessions it was read in, the last of them),
//   - at the start and at every loading screen (no frame for 250 ms), when storage runs fast anyway, FEXFileCache asks
//     storage for every range of the list (WILLNEED), and the drops leave those pieces in the file cache,
//   - a range not read in 30 sessions, or in a file whose size changed (a game update), is dropped; past the budget the
//     ranges read longest ago go first.
// It learns in one session and helps from the next. The 'learned reads' line counts what was learned and asked for,
// the 'game reads' line the main thread's reads of learned data.
namespace GameFiles {
// Call on the process's first thread, in process init, after the switches are read.
void Init(bool StatsLog);

// Whether the frame clock (NativeRedirect.cpp) should run: learning tells play from loading screens by frame times.
bool WantsFrameMarks();

// Build 90, for the unpack cache's learned list (NativeUnpackCache.cpp). Whether FEXFileCache runs (Init found
// FEX_GAME_FILES_UNCACHED=1); whether the calling thread is the game's main thread; frame marks written so far (0 without
// the frame clock); whether frames are coming (the last one began less than 250 ms ago, and from build 94 they are not
// GPU-light, LoadScreen.h: play, not a loading screen); what frame Frame (a FrameNow() value) was: 1 play, 0 a loading
// screen's, -1 not known yet, -2 too long ago.
bool Running();
bool OnMainThread();
uint64_t FrameNow();
bool FramesComing();
int FrameKind(uint64_t Frame);
// Hook called on FEXFileCache once per pass (every 500 ms) with whether a loading screen just started and whether
// available memory is below the memory guard.
void SetPassHook(void (*Hook)(bool LoadStart, bool LowMemory));

// GameFilesStore.cpp: the learned list's file (kept apart from GameFiles.cpp's NT and Linux headers).
namespace Store {
  fextl::string LearnedPath();
  bool ReadText(const fextl::string& Path, fextl::string& Out, size_t MaxBytes);
  bool WriteText(const fextl::string& Path, const fextl::string& Text);
} // namespace Store
} // namespace GameFiles
