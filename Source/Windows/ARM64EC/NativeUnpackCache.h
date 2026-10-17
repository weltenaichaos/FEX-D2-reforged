// SPDX-License-Identifier: MIT
// Unpack cache: whole zlib streams the game decompressed before, stored on disk so the next time the same stream is
// asked for (also after a restart) its output is read back instead of decompressed. NativeZlib.c decides what can be
// cached and applies a hit to the game's inflate state; NativeUnpackCache.cpp stores and finds the data.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set once by NativeUnpackCache::Init, before the inflate_fast stub is first handed out.
extern int FEXUnpackCacheEnabled;

struct FEXUnpackCacheCounters {
  uint64_t Lookups;
  uint64_t KeyTicks; // hashing a stream's input to look it up
  uint64_t Hits;
  uint64_t HitBytes;
  uint64_t HitTicks; // finding, reading and checking a hit
  uint64_t Stores;     // new streams queued for writing
  uint64_t StoreBytes;
  uint64_t StoreTicks; // checking and queueing new streams on the game's threads
  uint64_t Dropped;    // new streams not stored because the write buffer was full
  uint64_t Written;    // queued streams the writer thread wrote
  uint64_t WriteTicks; // the writer thread's time
  uint64_t Failures;   // read and write errors, and data that didn't match its hash (treated as misses)
};
extern struct FEXUnpackCacheCounters FEXUnpackCacheStats;

// Reads the stored output for Key into Out (OutCap bytes available) and checks it. Returns 1 with the output size and the
// offset of the stream's 4-byte trailer from the input start, or 0 (Out may have been written to).
int FEXUnpackCacheGet(const uint64_t Key[2], uint8_t* Out, uint32_t OutCap, uint32_t* Produced, uint32_t* Trailer);

// Queues Produced bytes of output to be stored under Key; they can be found once written. Silently does nothing when
// the cache or the write buffer is full, or the cache already has Key.
void FEXUnpackCachePut(const uint64_t Key[2], const uint8_t* Out, uint32_t Produced, uint32_t Trailer);

#ifdef __cplusplus
}

#include <FEXCore/fextl/string.h>

namespace NativeUnpackCache {
// Reads FEX_UNPACK_CACHE_MAX_GB (default 16) and FEX_UNPACK_CACHE_RESET (1 = start empty), and opens the cache. Call
// once (NativeRedirect does, when FEX_UNPACK_CACHE=1 and it finds the game's inflate_fast) before handing out the stubs
// that use it. Returns whether the cache is on.
bool Init();

// One stats log line for the last second, or nothing if the cache wasn't used.
fextl::string StatsLine();
} // namespace NativeUnpackCache
#endif
