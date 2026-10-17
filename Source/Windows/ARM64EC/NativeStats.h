// SPDX-License-Identifier: MIT
// Counters shared by the native replacements (NativeZlib.c, NativeMD5.c) and NativeRedirect.cpp, which logs them.
#pragma once

#include <stdint.h>

// ARM64EC clang defines __arm64ec__ (and __x86_64__), not __aarch64__. The plain C fallbacks are for host tests only
// (FEX_NATIVE_HOST_TEST also selects them on an ARM64 Linux host, where x18 holds no TEB).
#if (defined(__aarch64__) || defined(__arm64ec__)) && !defined(FEX_NATIVE_HOST_TEST)
#define FEX_NATIVE_ARM64 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct FEXNativeCallStats {
  uint64_t Calls;
  uint64_t Ticks; // CNTVCT_EL0 ticks spent inside the function
  uint64_t Bytes; // inflate_fast: bytes written, adler32_z and MD5: bytes hashed
};

extern struct FEXNativeCallStats FEXNativeInflateFastStats;
extern struct FEXNativeCallStats FEXNativeAdler32ZStats;
extern struct FEXNativeCallStats FEXNativeMD5UpdateStats;
extern struct FEXNativeCallStats FEXNativeMD5TransformStats;

// zlib streams seen starting in inflate_fast, to tell what the game decompresses and how much of it is a repeat of
// output it already produced (same buffer size and first 4 KiB).
#define FEX_NATIVE_ZLIB_MAGIC_SLOTS 48
#define FEX_NATIVE_ZLIB_SIZE_BUCKETS 8 // buffer size <1K, <4K, <16K, <64K, <256K, <1M, <4M, >=4M

struct FEXNativeZlibMagic {
  uint32_t Magic; // first 4 output bytes, little endian, or 0 for all streams that don't start with 4 printable characters
  uint32_t Used;
  uint64_t Streams;
  uint64_t Repeats;
  uint64_t Bytes; // sum of output buffer sizes
};

// Who asked: the first three return addresses into the executable found on the stack (RVAs), see FindCallers.
#define FEX_NATIVE_ZLIB_CALLER_SLOTS 32

struct FEXNativeZlibCaller {
  uint32_t RVA[3];
  uint32_t Used;
  uint64_t Streams;
  uint64_t Repeats;
  uint64_t Bytes;
};

struct FEXNativeZlibStreams {
  uint64_t Streams;
  uint64_t Repeats;
  uint64_t Bytes;
  uint64_t RepeatBytes;
  uint64_t Unlisted; // streams whose magic didn't fit in the table
  uint64_t Oneshot;  // streams whose first inflate_fast call already produced all but the last 1 KiB of the buffer
  uint64_t SizeBuckets[FEX_NATIVE_ZLIB_SIZE_BUCKETS];
  struct FEXNativeZlibMagic Magics[FEX_NATIVE_ZLIB_MAGIC_SLOTS];
  struct FEXNativeZlibCaller Callers[FEX_NATIVE_ZLIB_CALLER_SLOTS];
};

// Copies the stream counters consistently (takes the same lock as the writers).
void FEXNativeZlibStreamSnapshot(struct FEXNativeZlibStreams* Out);

// Set by NativeRedirect.cpp before any guest code runs.
extern int FEXNativeZlibTrackStreams;
extern uint64_t FEXNativeImageBase;
extern uint64_t FEXNativeTextBegin;
extern uint64_t FEXNativeTextEnd;
// FEX_UNPACK_CACHE_SKIP_ADLER (default 1): answer inflate()'s adler32 check of a cache hit's output with the stream's own
// trailer instead of computing it (the output was already checked against its stored hash).
extern int FEXNativeSkipHitAdler;
// FEX_NATIVE_ADLER_NEON (default 1): compute adler32 with NEON instead of zlib's C code.
extern int FEXNativeAdlerNeon;

#ifdef __cplusplus
}
#endif
