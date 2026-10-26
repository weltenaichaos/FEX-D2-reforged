// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef ARCHITECTURE_x86_64
#include <x86intrin.h>
#endif

namespace FEXCore::SHMStats {
#ifdef ARCHITECTURE_arm64
/**
 * @brief Get the raw cycle counter with synchronizing isb.
 *
 * `CNTVCTSS_EL0` also does the same thing, but requires the FEAT_ECV feature.
 */
static inline uint64_t GetCycleCounter() {
  uint64_t Result {};
  __asm volatile(R"(
      isb;
      mrs %[Res], CNTVCT_EL0;
    )"
                 : [Res] "=r"(Result));
  return Result;
}
#else
static inline uint64_t GetCycleCounter() {
  unsigned dummy;
  uint64_t tsc = __rdtscp(&dummy);
  return tsc;
}
#endif
// FEXCore live-stats
constexpr uint8_t STATS_VERSION = 2;
enum class AppType : uint8_t {
  LINUX_32,
  LINUX_64,
  WIN_ARM64EC,
  WIN_WOW64,
};

// Only append new members to the end of {ThreadStatsHeader, ThreadStats} to allow old tools time to support new information.
// FEX isn't guaranteeing /not/ breaking compatibility with versions, but trying to not cause too much churn.
struct ThreadStatsHeader {
  uint8_t Version;
  AppType app_type;
  uint16_t ThreadStatsSize;
  char fex_version[48];
  std::atomic<uint32_t> Head;
  std::atomic<uint32_t> Size;
  uint32_t Pad;
};

struct ThreadStats {
  std::atomic<uint32_t> Next;
  std::atomic<uint32_t> TID;

  // Accumulated time (In unscaled CPU cycles!)
  uint64_t AccumulatedJITTime;
  uint64_t AccumulatedSignalTime;

  // Accumulated event counts
  uint64_t AccumulatedSIGBUSCount;
  uint64_t AccumulatedSMCCount;
  uint64_t AccumulatedFloatFallbackCount;

  uint64_t AccumulatedCacheMissCount;
  uint64_t AccumulatedCacheReadLockTime;
  uint64_t AccumulatedCacheWriteLockTime;

  uint64_t AccumulatedJITCount;

  uint64_t AccumulatedDiskCacheHitCount;
  uint64_t AccumulatedDiskCacheMissCount;
  uint64_t AccumulatedDiskCacheLookupTime;
  // Disk cache lookup breakdown: computing the lookup key, reading entries (and how many came from the file rather
  // than memory), applying relocations, and installing a hit into the code buffer and lookup cache.
  uint64_t AccumulatedDiskCacheKeyTime;
  uint64_t AccumulatedDiskCacheReadTime;
  uint64_t AccumulatedDiskCacheFileReadCount;
  uint64_t AccumulatedDiskCacheRelocTime;
  uint64_t AccumulatedDiskCacheInstallTime;

  // Longest single JIT request, exception, and disk cache lookup since the stats reader last reset them (in unscaled
  // CPU cycles), and the cycle counter at the moment each one ended. Written by the owning thread, reset by the reader.
  uint64_t MaxJITTime;
  uint64_t MaxJITAt;
  uint64_t MaxSignalTime;
  uint64_t MaxSignalAt;
  uint64_t MaxDiskCacheLookupTime;
  uint64_t MaxDiskCacheLookupAt;

  // Why compiles were not served from the disk cache (indexed by DiskCacheMissReason) and what happened when the
  // compiled code was offered back to it (indexed by DiskCacheStoreResult).
  uint64_t DiskCacheMisses[8];
  uint64_t DiskCacheStores[8];
  // The latest misses and store refusals: guest RIP in the low 56 bits, the reason in the top 8 bits. Rings indexed by
  // the counts, written by the owning thread only.
  uint64_t DiskCacheMissRIPs[16];
  uint64_t DiskCacheRejectRIPs[16];
  uint64_t DiskCacheMissRIPCount;
  uint64_t DiskCacheRejectRIPCount;
  // Time compiles spent waiting for the shared code invalidation lock, and the longest single wait with its end time.
  uint64_t AccumulatedCodeLockWaitTime;
  uint64_t MaxCodeLockWaitTime;
  uint64_t MaxCodeLockWaitAt;
  // Times the thread stopped for a suspend request (SuspendThread, GetThreadContext) while in emulated code.
  uint64_t AccumulatedSuspendCount;
  // Finer disk cache breakdown (build 84): of the key time, decoding the first bytes of code outside any file and asking the
  // frontend for the executable range (how many lookups reused the decoder's answer instead); of the install time, copying
  // the code into the code buffer.
  uint64_t AccumulatedDiskCacheDecodeTime;
  uint64_t AccumulatedDiskCacheRangeTime;
  uint64_t AccumulatedDiskCacheRangeReuseCount;
  uint64_t AccumulatedDiskCacheCopyTime;
};

enum DiskCacheMissReason : uint8_t {
  DC_MISS_NOT_READING,
  DC_MISS_ANON_OFF,     // not file backed and anonymous caching off
  DC_MISS_ANON_DECODE,  // not file backed and its first bytes didn't decode
  DC_MISS_RANGE,        // no executable range around the RIP
  DC_MISS_NO_INDEX,     // nothing stored under this key
  DC_MISS_NO_MATCH,     // entries under this key, but none matches the live guest code
  DC_MISS_BAD_ENTRY,    // a matching entry that couldn't be read or relocated
  DC_MISS_NOT_LOADED,   // a hit that wasn't installed (validation mode or code buffer refused it)
};

enum DiskCacheStoreResult : uint8_t {
  DC_STORE_QUEUED,
  DC_STORE_NO_KEY,      // the lookup had no key for it, so it was never offered
  DC_STORE_NOT_WRITING,
  DC_STORE_NO_BLOCK_INFO, // custom IR or no decoder information
  DC_STORE_RELOC,       // a relocation points outside the code's own file
  DC_STORE_DECODE,      // part of the block didn't decode cleanly
  DC_STORE_BUCKET,      // too many variants under this key already
  DC_STORE_DUPE,        // already stored (or in flight)
};

// What the disk cache writer did with queued entries, process wide.
struct DiskCacheWriterStats {
  std::atomic<uint64_t> Written;
  std::atomic<uint64_t> Failed;     // file write or lock failed, the entry only lives in memory this session
  std::atomic<uint64_t> MemoryOnly; // cache file full
};
extern DiskCacheWriterStats DiskCacheWriter;

inline void NoteDiskCacheMiss(ThreadStats* Stats, DiskCacheMissReason Reason, uint64_t GuestRIP) {
  if (!Stats) {
    return;
  }
  ++Stats->DiskCacheMisses[Reason];
  const uint64_t Count = Stats->DiskCacheMissRIPCount;
  Stats->DiskCacheMissRIPs[Count % 16] = (GuestRIP & ((1ull << 56) - 1)) | (static_cast<uint64_t>(Reason) << 56);
  std::atomic_ref<uint64_t>(Stats->DiskCacheMissRIPCount).store(Count + 1, std::memory_order_release);
}

inline void NoteDiskCacheStore(ThreadStats* Stats, DiskCacheStoreResult Result, uint64_t GuestRIP) {
  if (!Stats) {
    return;
  }
  ++Stats->DiskCacheStores[Result];
  if (Result == DC_STORE_QUEUED || Result == DC_STORE_DUPE) {
    return;
  }
  const uint64_t Count = Stats->DiskCacheRejectRIPCount;
  Stats->DiskCacheRejectRIPs[Count % 16] = (GuestRIP & ((1ull << 56) - 1)) | (static_cast<uint64_t>(Result) << 56);
  std::atomic_ref<uint64_t>(Stats->DiskCacheRejectRIPCount).store(Count + 1, std::memory_order_release);
}

// Ensure 16-byte alignment to take advantage of ARM single-copy atomicity.
static_assert(sizeof(ThreadStats) % 16 == 0, "Needs to be 16-byte aligned!");

template<typename T, size_t FlatOffset = 0>
class AccumulationBlock final {
public:
  // MaxStat, when given, points at a {longest duration, end time} pair that keeps the longest single block.
  AccumulationBlock(T* Stat, T* MaxStat = nullptr)
    : Begin {Stat ? GetCycleCounter() : 0}
    , Stat {Stat}
    , MaxStat {MaxStat} {}

  ~AccumulationBlock() {
    if (Stat) {
      const auto End = GetCycleCounter();
      const auto Duration = End - Begin + FlatOffset;
      auto ref = std::atomic_ref<T>(*Stat);
      ref.fetch_add(Duration, std::memory_order_relaxed);
      if (MaxStat) {
        auto Max = std::atomic_ref<T>(MaxStat[0]);
        if (Duration > Max.load(std::memory_order_relaxed)) {
          Max.store(Duration, std::memory_order_relaxed);
          std::atomic_ref<T>(MaxStat[1]).store(End, std::memory_order_relaxed);
        }
      }
    }
  }

private:
  uint64_t Begin;
  T* Stat;
  T* MaxStat;
};
#define UniqueScopeName2(name, line) name##line
#define UniqueScopeName(name, line) UniqueScopeName2(name, line)

#define FEXCORE_PROFILE_ACCUMULATION(ThreadState, Stat)                                                                          \
  FEXCore::SHMStats::AccumulationBlock<decltype(ThreadState->ThreadStats->Stat)> UniqueScopeName(ScopedAccumulation_, __LINE__)( \
    ThreadState->ThreadStats ? &ThreadState->ThreadStats->Stat : nullptr);
#define FEXCORE_PROFILE_ACCUMULATION_MAX(ThreadState, Stat, MaxStat)                                                             \
  FEXCore::SHMStats::AccumulationBlock<decltype(ThreadState->ThreadStats->Stat)> UniqueScopeName(ScopedAccumulation_, __LINE__)( \
    ThreadState->ThreadStats ? &ThreadState->ThreadStats->Stat : nullptr, ThreadState->ThreadStats ? &ThreadState->ThreadStats->MaxStat : nullptr);
#define FEXCORE_PROFILE_INSTANT_INCREMENT(ThreadState, Stat, value) \
  do {                                                              \
    if (ThreadState->ThreadStats) {                                 \
      ThreadState->ThreadStats->Stat += value;                      \
    }                                                               \
  } while (0)

} // namespace FEXCore::SHMStats
