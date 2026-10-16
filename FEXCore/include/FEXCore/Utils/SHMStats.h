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
};

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
