// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/IntervalList.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/fextl/set.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string_view>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::Context {
class Context;
}

namespace FEX::Windows {
/**
 * @brief Handles SMC and regular code invalidation
 */
class InvalidationTracker {
public:
  InvalidationTracker(FEXCore::Context::Context& CTX, const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads);
  void HandleMemoryProtectionNotification(uint64_t Address, uint64_t Size, ULONG Prot);
  void HandleProcessExecuteFlagsChange(ULONG Flags);
  void HandleImageMap(std::string_view Name, uint64_t Address);
  struct InvalidateContainingSectionResult {
    uint64_t SectionStart;
    uint64_t SectionSize;
  };
  InvalidateContainingSectionResult InvalidateContainingSection(uint64_t Address, bool Free);
  void InvalidateAlignedInterval(uint64_t Address, uint64_t Size, bool Free);
  void ReprotectRWXIntervals(uint64_t Address, uint64_t Size);
  bool HandleRWXAccessViolation(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC, uint64_t FaultAddress);

  // Unprotects any RWX intervals in the input interval and invalidates code
  // NOTE: CodeInvalidationMutex must be locked when calling this, and if true is returned, kept locked until the write ends.
  bool BeginUntrackedWriteLocked(uint64_t Address, uint64_t Size);

  FEXCore::HLE::ExecutableRangeInfo QueryExecutableRange(uint64_t Address);

  // Hot SMC pages (FEX_SMC_HOTPAGE). A page that takes Faults write faults within WindowMs is a page whose own code keeps
  // writing to it (a decryption loop next to the bytes it decrypts, data next to the code using it): every write faults,
  // invalidates the page, and the code is fetched again, one write at a time. Such a page stops being write-protected,
  // and code compiled from it checks its own bytes before each instruction instead (FEXCore asks IsFullSMCRange). 0 Faults
  // (the default) keeps upstream behaviour. Call before any guest code runs.
  void SetHotPageThreshold(uint32_t Faults, uint32_t WindowMs);
  bool IsFullSMCRange(uint64_t Address, uint64_t Size);
  struct HotPageEvent {
    uint64_t Page;
    uint32_t Faults;
    double WindowMs;
    uint64_t HotPages; // in total, after this one
  };
  // The pages that turned hot since the last call (at most a few kept), for the stats log.
  size_t TakeHotPageEvents(HotPageEvent* Out, size_t Max);

private:
  void DetectMonoBackpatcherBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC);
  void DisableSMCDetection();
  void InvalidateIntervalInternal(uint64_t Address, uint64_t Size);
  // NOTE: This assumed CodeInvalidationMutex is locked by the caller
  void InvalidateIntervalInternalLocked(uint64_t Address, uint64_t Size);

  // NOTE: If ForWriteLocked is true then this assumes CodeInvalidationMutex is locked by the caller,
  // and any code in the range will be invalidated before protection as RWX, otherwise protects as RX if false.
  bool ProtectRWXIntervalsInternal(uint64_t Address, uint64_t Size, bool ForWriteLocked);

  // Returns the correct protection for trapping (removing write) or untrapping (restoring write) an RWX interval.
  // For DEP-promoted regions (originally non-exec), uses PAGE_READONLY/PAGE_READWRITE instead of PAGE_EXECUTE_READ/PAGE_EXECUTE_READWRITE.
  // NOTE: Must be called with IntervalsLock held.
  ULONG GetTrapProt(uint64_t Address) const;
  ULONG GetUntrapProt(uint64_t Address) const;

  FEXCore::IntervalList<uint64_t> XIntervals;
  FEXCore::IntervalList<uint64_t> RWXIntervals;
  std::shared_mutex IntervalsLock;
  FEXCore::Context::Context& CTX;
  const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads;
  bool SMCDetectionDisabled {false};                    // Protected by IntervalsLock
  bool DEPDisabled {false};                             // Protected by IntervalsLock
  FEXCore::IntervalList<uint64_t> DEPPromotedIntervals; // Protected by IntervalsLock

  // Counts a write fault on Page; true when it turns the page hot. NOTE: CodeInvalidationMutex must be held exclusively.
  bool NoteWriteFaultLocked(uint64_t Page);
  // NOTE: Must be called with IntervalsLock held exclusively.
  void ForgetHotPagesLocked(uint64_t Start, uint64_t End);

  struct FaultCounter {
    uint64_t Page;
    uint64_t WindowStart;
    uint32_t Count;
  };
  FaultCounter FaultCounters[8] {}; // Protected by CodeInvalidationMutex (exclusive)
  uint32_t NextFaultCounter {};     // Protected by CodeInvalidationMutex (exclusive)
  uint32_t HotPageFaults {};
  uint64_t HotPageWindowTicks {};
  uint64_t CounterFrequency {1};
  std::atomic<bool> AnyHotPages {false};
  fextl::set<uint64_t> HotPages;    // Protected by IntervalsLock
  HotPageEvent HotPageEvents[8] {}; // Protected by HotPageEventsLock
  size_t HotPageEventCount {};
  std::mutex HotPageEventsLock;

  bool MonoBackpatcherDetectionPending {false};
  uint64_t MonoBase {0};
  uint64_t MonoEnd {0};
};
} // namespace FEX::Windows
