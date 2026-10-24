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
  uint64_t Stores;     // new streams being stored
  uint64_t StoreBytes;
  uint64_t StoreTicks; // storing new streams on the game's threads (a copy into the ring, or the whole write)
  uint64_t Dropped;    // new streams not stored because the write buffer was full
  uint64_t Written;    // new streams written to disk
  uint64_t WriteTicks; // hashing and writing them
  uint64_t Failures;   // read and write errors, and data that didn't match its hash (treated as misses)
  uint64_t RamHits;    // hits copied from read-ahead memory instead of read from disk
  uint64_t RamTicks;   // copying them
  uint64_t DiskTicks;  // reading the other hits from disk
  uint64_t VerifyTicks; // checking hits against their hash
  uint64_t AheadChunks; // read-ahead: chunks read into memory
  uint64_t AheadBytes;
  uint64_t AheadTicks;
  uint64_t AheadWasted; // chunks dropped from memory without serving a hit
  uint64_t AdlerSkips;  // adler32 checks of a hit's output answered with the stream's own trailer instead of computed
  uint64_t AdlerSkipBytes;
  uint64_t MD5Lookups;  // MD5 updates looked up in the MD5 cache
  uint64_t MD5Hits;
  uint64_t MD5HitBytes;
  uint64_t MD5Ticks;    // hashing and looking them up (and applying hits)
  uint64_t MD5Stores;
  uint64_t AheadDirect; // read-ahead chunks read with direct reads, past the kernel's file cache
  uint64_t CacheDrops;  // reads and writes of data.bin dropped from the kernel's file cache afterwards
  uint64_t PlayLookups;   // lookups while playing (few lookups a second, see FEX_UNPACK_CACHE_PLAY)
  uint64_t PlaySkips;     // of those, hits not in read-ahead memory, left to the game to unpack instead of read from disk
  uint64_t PlaySkipBytes;
};
extern struct FEXUnpackCacheCounters FEXUnpackCacheStats;

// Reads the stored output for Key into Out (OutCap bytes available) and checks it. Returns 1 with the output size and the
// offset of the stream's 4-byte trailer from the input start, or 0 (Out may have been written to).
int FEXUnpackCacheGet(const uint64_t Key[2], uint8_t* Out, uint32_t OutCap, uint32_t* Produced, uint32_t* Trailer);

// Queues Produced bytes of output to be stored under Key; they can be found once written. Silently does nothing when
// the cache or the write buffer is full, or the cache already has Key.
void FEXUnpackCachePut(const uint64_t Key[2], const uint8_t* Out, uint32_t Produced, uint32_t Trailer);

// MD5 cache: the MD5 state after the game's MD5 update ran over Bytes bytes (whole 64-byte blocks) from a given state,
// keyed by a hash of that state and those bytes (NativeMD5.c). Kept in md5.bin next to the unpack cache. Set once by
// NativeUnpackCache::Init, before which the functions below must not be called.
extern int FEXMD5CacheEnabled;
int FEXMD5CacheGet(const uint64_t Key[2], uint64_t Bytes, uint32_t State[4]);
void FEXMD5CachePut(const uint64_t Key[2], uint64_t Bytes, const uint32_t State[4]);

#ifdef __cplusplus
}

#include <FEXCore/fextl/string.h>

namespace NativeUnpackCache {
// Starts the threads that write new streams to disk and read stored data ahead; they wait until Init opens the cache.
// Call at process start when the cache is requested.
void StartWriter();

// Reads FEX_UNPACK_CACHE_MAX_GB (default 16), FEX_UNPACK_CACHE_RESET (1 = start empty),
// FEX_UNPACK_CACHE_READAHEAD_MB (memory for reading stored data ahead, default 64, 0 = off) and
// FEX_UNPACK_CACHE_DIRECT (1 = data.bin is read past the kernel's file cache, see NativeUnpackCache.cpp; D2R default 1),
// and opens the cache, plus
// the MD5 cache if MD5Cache. Call once (NativeRedirect does, when FEX_UNPACK_CACHE=1 and it finds the game's
// inflate_fast) before handing out the stubs that use it. Returns whether the cache is on.
bool Init(bool MD5Cache);

// One stats log line for the last second, or nothing if the cache wasn't used.
fextl::string StatsLine();
} // namespace NativeUnpackCache
#endif
