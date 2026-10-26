// SPDX-License-Identifier: MIT

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include "InvalidationTracker.h"
#include <windef.h>
#include <winternl.h>

#include <algorithm>
#include <iterator>
#include <atomic>

namespace FEX::Windows {
InvalidationTracker::InvalidationTracker(FEXCore::Context::Context& CTX, const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads)
  : CTX {CTX}
  , Threads {Threads} {
  FEX_CONFIG_OPT(SMCChecks, SMCCHECKS);
  SMCDetectionDisabled = (SMCChecks == FEXCore::Config::CONFIG_SMC_NONE);

  MEMORY_BASIC_INFORMATION Info;
  uint64_t Address = 0;

  while (VirtualQuery(reinterpret_cast<LPCVOID>(Address), &Info, sizeof(Info))) {
    uint64_t BaseAddress = reinterpret_cast<uint64_t>(Info.BaseAddress);
    if (Info.State == MEM_COMMIT) {
      HandleMemoryProtectionNotification(BaseAddress, Info.RegionSize, Info.Protect);
    }

    Address = BaseAddress + Info.RegionSize;
  }
}

static uint64_t ReadCounter() {
  uint64_t Result {};
  __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Result));
  return Result;
}

static uint64_t ReadCounterFrequency() {
  uint64_t Result {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Result));
  return Result ? Result : 1;
}

static bool ProtHasExec(ULONG Prot) {
  return (Prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool ProtIsReadable(ULONG Prot) {
  return (Prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                  PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool ProtIsWritable(ULONG Prot) {
  return (Prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void InvalidationTracker::HandleMemoryProtectionNotification(uint64_t Address, uint64_t Size, ULONG Prot) {
  const auto AlignedBase = Address & FEXCore::Utils::FEX_PAGE_MASK;
  const auto AlignedSize = (Address - AlignedBase + Size + FEXCore::Utils::FEX_PAGE_SIZE - 1) & FEXCore::Utils::FEX_PAGE_MASK;

  DropReason Reason = DropReason::ProtectToExec;
  const bool NeedsInvalidate = [&]() {
    std::unique_lock Lock(IntervalsLock);

    FEXCore::IntervalList<uint64_t>::Interval ProtInterval {AlignedBase, AlignedBase + AlignedSize};
    const bool WasExec = XIntervals.Intersect(ProtInterval);

    const bool HasExec = ProtHasExec(Prot);
    const bool EffectiveExec = HasExec || (DEPDisabled && ProtIsReadable(Prot));
    const bool EffectiveRWX = EffectiveExec && ProtIsWritable(Prot);
    if (!EffectiveRWX) {
      // Code there can't be written without another protection change (which invalidates it), so no need to check itself.
      ForgetHotPagesLocked(AlignedBase, AlignedBase + AlignedSize);
    }

    if (EffectiveExec) {
      Reason = WasExec ? DropReason::ProtectExecToExec : DropReason::ProtectToExec;
      XIntervals.Insert(ProtInterval);
      if (EffectiveRWX) {
        LogMan::Msg::DFmt("Add SMC interval: {:X} - {:X}", AlignedBase, AlignedBase + AlignedSize);
        RWXIntervals.Insert(ProtInterval);
      }
      if (DEPDisabled && !HasExec) {
        DEPPromotedIntervals.Insert(ProtInterval);
      }
      return true;
    } else if (WasExec) {
      Reason = DropReason::ProtectExecRemoved;
      XIntervals.Remove(ProtInterval);
      RWXIntervals.Remove(ProtInterval);
      if (DEPDisabled) {
        DEPPromotedIntervals.Remove(ProtInterval);
      }
      return true;
    }

    return false;
  }();

  if (NeedsInvalidate) {
    // IntervalsLock cannot be held during invalidation
    std::scoped_lock CodeLock(CTX.GetCodeInvalidationMutex());
    const uint64_t Dropped = InvalidateAndCountLocked(AlignedBase, AlignedSize, Reason);
    if (Dropped && Reason != DropReason::ProtectToExec) {
      const int Bit = (Prot & 0xFF) ? __builtin_ctz(Prot & 0xFF) : 0;
      DropProtBits[Reason == DropReason::ProtectExecToExec ? 0 : 1][Bit].fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void InvalidationTracker::HandleProcessExecuteFlagsChange(ULONG Flags) {
  const bool DisableDEP = (Flags & MEM_EXECUTE_OPTION_ENABLE) != 0;

  std::scoped_lock CodeLock(CTX.GetCodeInvalidationMutex());
  std::unique_lock Lock(IntervalsLock);

  if (DisableDEP == DEPDisabled) {
    return;
  }

  DEPDisabled = DisableDEP;

  if (DisableDEP) {
    DEPPromotedIntervals.Clear();

    MEMORY_BASIC_INFORMATION Info;
    uint64_t Address = 0;

    while (VirtualQuery(reinterpret_cast<LPCVOID>(Address), &Info, sizeof(Info))) {
      uint64_t BaseAddress = reinterpret_cast<uint64_t>(Info.BaseAddress);
      if (Info.State == MEM_COMMIT && ProtIsReadable(Info.Protect) && !ProtHasExec(Info.Protect)) {
        const auto AlignedBase = BaseAddress & FEXCore::Utils::FEX_PAGE_MASK;
        const auto AlignedSize = (BaseAddress - AlignedBase + Info.RegionSize + FEXCore::Utils::FEX_PAGE_SIZE - 1) & FEXCore::Utils::FEX_PAGE_MASK;
        FEXCore::IntervalList<uint64_t>::Interval ProtInterval {AlignedBase, AlignedBase + AlignedSize};

        XIntervals.Insert(ProtInterval);
        if (ProtIsWritable(Info.Protect)) {
          RWXIntervals.Insert(ProtInterval);
        }
        DEPPromotedIntervals.Insert(ProtInterval);
      }

      Address = BaseAddress + Info.RegionSize;
    }
  } else {
    for (const auto& Interval : DEPPromotedIntervals) {
      XIntervals.Remove(Interval);
      RWXIntervals.Remove(Interval);
    }
    DEPPromotedIntervals.Clear();
  }

  // Invalidate all cached code: previously-compiled blocks may contain NoExec stubs for addresses
  // that are now executable (or reference regions whose executability just changed).
  InvalidateIntervalInternalLocked(0, std::numeric_limits<uint64_t>::max(), DropReason::Other);
}

void InvalidationTracker::HandleImageMap(std::string_view Name, uint64_t Address) {
  auto* Nt = RtlImageNtHeader(reinterpret_cast<HMODULE>(Address));
  auto* SectionsBegin = IMAGE_FIRST_SECTION(Nt);
  auto* SectionsEnd = SectionsBegin + Nt->FileHeader.NumberOfSections;
  uint64_t LastExecutableSectionEnd = 0;

  for (auto* Section = SectionsBegin; Section != SectionsEnd; Section++) {
    if (Section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
      std::unique_lock Lock(IntervalsLock);

      uint64_t SectionBase = Address + Section->VirtualAddress;
      uint64_t SectionEnd = SectionBase + Section->Misc.VirtualSize;
      XIntervals.Insert({SectionBase, SectionEnd});
      LastExecutableSectionEnd = std::max(LastExecutableSectionEnd, SectionEnd);
      if (Section->Characteristics & IMAGE_SCN_MEM_WRITE) {
        LogMan::Msg::DFmt("Add image SMC interval: {:X} - {:X}", SectionBase, SectionBase + Section->Misc.VirtualSize);
        RWXIntervals.Insert({SectionBase, SectionBase + Section->Misc.VirtualSize});
      }
    }
  }

  FEX_CONFIG_OPT(MonoHacks, MONOHACKS);
  if (MonoHacks && (Name == "mono-2.0-bdwgc.dll" || Name == "mono.dll")) {
    FEX_CONFIG_OPT(MaxInst, MAXINST);
    FEX_CONFIG_OPT(Multiblock, MULTIBLOCK);
    if (Multiblock && MaxInst() >= 500) {
      // Require these settings to ensure we can safely hook all SMC sites in a single block
      CTX.MarkMonoDetected();
      MonoBackpatcherDetectionPending = true;
      MonoBase = Address;
      MonoEnd = LastExecutableSectionEnd;
    } else {
      LogMan::Msg::IFmt("Not applying mono hacks, Multiblock with MaxInst >= 500 required");
    }
  }
}

InvalidationTracker::InvalidateContainingSectionResult
InvalidationTracker::InvalidateContainingSection(uint64_t Address, bool Free, DropReason Reason) {
  MEMORY_BASIC_INFORMATION Info;
  if (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), MemoryBasicInformation, &Info, sizeof(Info), nullptr)) {
    return {Address, 0};
  }

  const auto SectionBase = reinterpret_cast<uint64_t>(Info.AllocationBase);
  auto SectionSize = reinterpret_cast<uint64_t>(Info.BaseAddress) + Info.RegionSize - SectionBase;

  while (!NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(SectionBase + SectionSize), MemoryBasicInformation, &Info,
                               sizeof(Info), nullptr) &&
         reinterpret_cast<uint64_t>(Info.AllocationBase) == SectionBase) {
    SectionSize += Info.RegionSize;
  }

  InvalidateIntervalInternal(SectionBase, SectionSize, Reason);

  if (Free) {
    std::unique_lock Lock(IntervalsLock);
    XIntervals.Remove({SectionBase, SectionBase + SectionSize});
    RWXIntervals.Remove({SectionBase, SectionBase + SectionSize});
    ForgetHotPagesLocked(SectionBase, SectionBase + SectionSize);
  }

  return {SectionBase, SectionSize};
}

void InvalidationTracker::InvalidateAlignedInterval(uint64_t Address, uint64_t Size, bool Free, DropReason Reason) {
  if (!Address) {
    // Match the Windows behaviour when passed a NULL base address.
    Size = std::numeric_limits<uint64_t>::max();
  }

  const auto AlignedBase = Address & FEXCore::Utils::FEX_PAGE_MASK;
  const auto AlignedSize = std::max(Size, (Address - AlignedBase + Size + FEXCore::Utils::FEX_PAGE_SIZE - 1) & FEXCore::Utils::FEX_PAGE_MASK);

  InvalidateIntervalInternal(AlignedBase, AlignedSize, Free && Reason == DropReason::Other ? DropReason::Free : Reason);

  if (Free) {
    std::unique_lock Lock(IntervalsLock);
    XIntervals.Remove({AlignedBase, AlignedBase + AlignedSize});
    RWXIntervals.Remove({AlignedBase, AlignedBase + AlignedSize});
    ForgetHotPagesLocked(AlignedBase, AlignedBase + AlignedSize);
  }
}

void InvalidationTracker::ReprotectRWXIntervals(uint64_t Address, uint64_t Size) {
  ProtectRWXIntervalsInternal(Address, Size, false);
}

bool InvalidationTracker::HandleRWXAccessViolation(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPc, uint64_t FaultAddress) {
  const auto [NeedsInvalidate, UntrapProt] = [&](uint64_t Address) -> std::pair<bool, ULONG> {
    std::shared_lock Lock(IntervalsLock);
    if (!RWXIntervals.Query(Address).Enclosed) {
      return {false, 0};
    }
    return {true, GetUntrapProt(Address)};
  }(FaultAddress);

  if (NeedsInvalidate) {
    // IntervalsLock cannot be held during invalidation
    {
      std::scoped_lock Lock(CTX.GetCodeInvalidationMutex());

      InvalidateIntervalInternalLocked(FaultAddress & FEXCore::Utils::FEX_PAGE_MASK, FEXCore::Utils::FEX_PAGE_SIZE, DropReason::WriteFault);
      // A page that keeps faulting stays writable from now on (ProtectRWXIntervalsInternal skips it), and its code is
      // compiled to check itself. Done under the exclusive lock, so no compile sees the page change state mid-way.
      NoteWriteFaultLocked(FaultAddress & FEXCore::Utils::FEX_PAGE_MASK);

      // Invalidate, then unprotect the faulting page with the compilation lock held to ensure that any racing invalidations are not dropped.
      ULONG TmpProt;
      void* TmpAddress = reinterpret_cast<void*>(FaultAddress);
      SIZE_T TmpSize = 1;
      NtProtectVirtualMemory(NtCurrentProcess(), &TmpAddress, &TmpSize, UntrapProt, &TmpProt);
    }
    DetectMonoBackpatcherBlock(Thread, HostPc);
    return true;
  }
  return false;
}

bool InvalidationTracker::BeginUntrackedWriteLocked(uint64_t Address, uint64_t Size) {
  return ProtectRWXIntervalsInternal(Address, Size, true);
}

// Clamps an interval-list answer to the OS region containing Address. The interval lists describe what was
// mapped or protected as executable at notification time, at region granularity; they say nothing about whether
// a page can be read right now. A decrypt-on-demand protector keeps the pages it has not yet decrypted at
// PAGE_NOACCESS inside a region the lists consider executable (D2R's whole decrypted image is one such interval),
// and it may flip protections through paths that never notify FEX. The decoder and the disk cache lookup read
// guest memory under the compilation lock based on this answer, so an inaccessible page must end the range:
// the block then stops in front of it and execution reaches the page through a NoExec stub, which raises the
// fault the protector's handler expects, outside the compiler.
static FEXCore::HLE::ExecutableRangeInfo ClampToAccessible(FEXCore::HLE::ExecutableRangeInfo Result, uint64_t Address,
                                                           const MEMORY_BASIC_INFORMATION& Info) {
  if (!Result.Size) {
    return Result;
  }

  static std::atomic<uint64_t> ClampCount {};

  const bool Accessible = Info.State == MEM_COMMIT && ProtIsReadable(Info.Protect) && !(Info.Protect & PAGE_GUARD);
  if (!Accessible) {
    const auto Count = ClampCount.fetch_add(1);
    if (Count < 32 || (Count & 1023) == 0) {
      LogMan::Msg::IFmt("[xrange] {:X} is tracked executable ({:X}-{:X}) but not readable: state={:X} prot={:X} (#{})", Address,
                        Result.Base, Result.Base + Result.Size, Info.State, Info.Protect, Count);
    }
    return {};
  }

  const auto RegionBase = reinterpret_cast<uint64_t>(Info.BaseAddress) & FEXCore::Utils::FEX_PAGE_MASK;
  const auto RegionEnd = RegionBase + ((Info.RegionSize + FEXCore::Utils::FEX_PAGE_SIZE - 1) & FEXCore::Utils::FEX_PAGE_MASK);
  const auto Base = std::max(Result.Base, RegionBase);
  const auto End = std::min(Result.Base + Result.Size, RegionEnd);
  if (Address < Base || Address >= End) {
    return {};
  }
  return {Base, End - Base, Result.Writable};
}

FEXCore::HLE::ExecutableRangeInfo InvalidationTracker::QueryExecutableRange(uint64_t Address) {
  MEMORY_BASIC_INFORMATION Info;
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(Address), &Info, sizeof(Info))) {
    return {};
  }

  // Assumes IntervalsLock is held.
  const auto BuildResult = [this](uint64_t Address) -> FEXCore::HLE::ExecutableRangeInfo {
    const auto XResult = XIntervals.Query(Address);
    if (!XResult.Enclosed) {
      return {};
    }
    const auto RWXResult = RWXIntervals.Query(Address);
    if (RWXResult.Enclosed) {
      return {RWXResult.Interval.Offset, RWXResult.Interval.End - RWXResult.Interval.Offset, true};
    } else if (RWXResult.Size && RWXResult.Size < XResult.Size) {
      return {XResult.Interval.Offset, RWXResult.Interval.Offset - XResult.Interval.Offset, false};
    }
    return {XResult.Interval.Offset, XResult.Interval.End - XResult.Interval.Offset, false};
  };

  {
    std::shared_lock Lock(IntervalsLock);
    const auto Result = BuildResult(Address);
    if (Result.Size) {
      return ClampToAccessible(Result, Address, Info);
    }
  }

  // The interval lists are only ever populated from allocation/protection notifications and from PE image
  // maps. An executable view of a non-image section (NtMapViewOfSection with SECTION_MAP_EXECUTE) goes
  // through neither: it has no PE header for HandleImageMap to walk, and its executability comes from the
  // view's access rights rather than a subsequent NtProtectVirtualMemory call. Such a region is therefore
  // never tracked, and executing it is rejected even though the OS considers the page executable.
  //
  // Protectors that avoid W^X by double-mapping one section - executable at one address, writable at
  // another - land exactly here (Blizzard's *_loader.dll among them). Rather than add a notification for
  // every mapping path, treat a miss as "ask the OS": if it says the page is executable, trust it and
  // start tracking the region.
  if (Info.State != MEM_COMMIT) {
    return {};
  }

  const bool HasExec = ProtHasExec(Info.Protect);
  const bool EffectiveExec = HasExec || (DEPDisabled && ProtIsReadable(Info.Protect));
  if (!EffectiveExec) {
    return {};
  }

  const auto BaseAddress = reinterpret_cast<uint64_t>(Info.BaseAddress);
  const auto AlignedBase = BaseAddress & FEXCore::Utils::FEX_PAGE_MASK;
  const auto AlignedSize =
    (BaseAddress - AlignedBase + Info.RegionSize + FEXCore::Utils::FEX_PAGE_SIZE - 1) & FEXCore::Utils::FEX_PAGE_MASK;

  {
    std::unique_lock Lock(IntervalsLock);

    // Insert directly instead of going through HandleMemoryProtectionNotification: that would also run
    // InvalidateIntervalInternal, which takes the code invalidation mutex while a block is being compiled.
    // Nothing needs invalidating anyway - a range that was never in XIntervals has no compiled code, and
    // unmapping already invalidates through InvalidateContainingSection.
    FEXCore::IntervalList<uint64_t>::Interval ProtInterval {AlignedBase, AlignedBase + AlignedSize};

    XIntervals.Insert(ProtInterval);
    if (ProtIsWritable(Info.Protect)) {
      LogMan::Msg::DFmt("Add SMC interval: {:X} - {:X}", AlignedBase, AlignedBase + AlignedSize);
      RWXIntervals.Insert(ProtInterval);
    }
    if (DEPDisabled && !HasExec) {
      DEPPromotedIntervals.Insert(ProtInterval);
    }
  }

  std::shared_lock Lock(IntervalsLock);
  return ClampToAccessible(BuildResult(Address), Address, Info);
}

void InvalidationTracker::DetectMonoBackpatcherBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPc) {
  if (!MonoBackpatcherDetectionPending) {
    return;
  }

  if (!CTX.IsAddressInCodeBuffer(Thread, HostPc)) {
    return;
  }

  uint64_t RIP = CTX.RestoreRIPFromHostPC(Thread, HostPc);
  if (!RIP || RIP < MonoBase || RIP >= MonoEnd) {
    return;
  }

  static constexpr uint8_t XChgOp = 0x87;
  if (*reinterpret_cast<uint8_t*>(RIP) != XChgOp && *reinterpret_cast<uint8_t*>(RIP + 1) != XChgOp) {
    return;
  }

  uint64_t BlockEntry = CTX.GetGuestBlockEntry(Thread);
  LogMan::Msg::DFmt("Detected mono backpatcher at: {:X}", BlockEntry);
  DisableSMCDetection();
  {
    std::scoped_lock CodeLock(CTX.GetCodeInvalidationMutex());
    CTX.MarkMonoBackpatcherBlock(BlockEntry);
  }
  InvalidateAlignedInterval(BlockEntry, FEXCore::Utils::FEX_PAGE_SIZE, false);
}

void InvalidationTracker::DisableSMCDetection() {
  std::unique_lock Lock(IntervalsLock);
  SMCDetectionDisabled = true;
  uint64_t Address = 0;

  // Reprotect all RWX intervals as writable
  FEXCore::IntervalList<uint64_t>::QueryResult Query;
  do {
    Query = RWXIntervals.Query(Address);
    if (Query.Enclosed) {
      void* TmpAddress = reinterpret_cast<void*>(Address);
      SIZE_T TmpSize = static_cast<SIZE_T>(Query.Size);
      ULONG TmpProt;
      NtProtectVirtualMemory(NtCurrentProcess(), &TmpAddress, &TmpSize, GetUntrapProt(Address), &TmpProt);
    }
    Address += Query.Size;
  } while (Query.Size);
}

ULONG InvalidationTracker::GetTrapProt(uint64_t Address) const {
  if (DEPDisabled && DEPPromotedIntervals.Query(Address).Enclosed) {
    return PAGE_READONLY;
  }
  return PAGE_EXECUTE_READ;
}

ULONG InvalidationTracker::GetUntrapProt(uint64_t Address) const {
  if (DEPDisabled && DEPPromotedIntervals.Query(Address).Enclosed) {
    return PAGE_READWRITE;
  }
  return PAGE_EXECUTE_READWRITE;
}

void InvalidationTracker::InvalidateIntervalInternal(uint64_t Address, uint64_t Size, DropReason Reason) {
  std::scoped_lock CodeLock(CTX.GetCodeInvalidationMutex());
  InvalidateIntervalInternalLocked(Address, Size, Reason);
}

void InvalidationTracker::InvalidateIntervalInternalLocked(uint64_t Address, uint64_t Size, DropReason Reason) {
  InvalidateAndCountLocked(Address, Size, Reason);
}

uint64_t InvalidationTracker::InvalidateAndCountLocked(uint64_t Address, uint64_t Size, DropReason Reason) {
  // NOTE: This assumes CodeInvalidationMutex is locked by the caller. Invalidations are serialized by it, so the change
  // of the dropped count belongs to this one.
  const uint64_t Before = CTX.GetCodeBufferUse().Dropped;
  CTX.InvalidateCodeBuffersCodeRange(Address, Size);
  for (auto Thread : Threads) {
    CTX.InvalidateThreadCachedCodeRange(Thread.second, Address, Size);
  }
  const uint64_t Dropped = CTX.GetCodeBufferUse().Dropped - Before;
  const auto Index = static_cast<size_t>(Reason);
  DropCalls[Index].fetch_add(1, std::memory_order_relaxed);
  DropPages[Index].fetch_add(Size >= (uint64_t {1} << 40) ? 0 : Size >> 12, std::memory_order_relaxed);
  DropBlocks[Index].fetch_add(Dropped, std::memory_order_relaxed);
  return Dropped;
}

InvalidationTracker::DropCounts InvalidationTracker::GetDropCounts() {
  DropCounts Out {};
  for (size_t i = 0; i < size_t(DropReason::Count); ++i) {
    Out.Calls[i] = DropCalls[i].load(std::memory_order_relaxed);
    Out.Pages[i] = DropPages[i].load(std::memory_order_relaxed);
    Out.Blocks[i] = DropBlocks[i].load(std::memory_order_relaxed);
  }
  for (size_t k = 0; k < 2; ++k) {
    for (size_t b = 0; b < 8; ++b) {
      Out.ProtBits[k][b] = DropProtBits[k][b].load(std::memory_order_relaxed);
    }
  }
  return Out;
}

bool InvalidationTracker::ProtectRWXIntervalsInternal(uint64_t Address, uint64_t Size, bool ForWriteLocked) {
  const auto End = Address + Size;
  std::shared_lock Lock(IntervalsLock);

  if (SMCDetectionDisabled) {
    return false;
  }

  bool HitRWXInterval = false;
  do {
    const auto Query = RWXIntervals.Query(Address);
    if (Query.Enclosed) {
      if (!HitRWXInterval) {
        if (ForWriteLocked) {
          // If we are protecting as writable, then the entire range must be invalidated before any protections are
          // applied and the invalidation mutex must be locked throughout.
          // Do this lazily only when an RWX region is actually hit.
          // NOTE: This assumes CodeInvalidationMutex is locked by the caller
          InvalidateIntervalInternalLocked(Address, Size, DropReason::ReadFile);
        }
        HitRWXInterval = true;
      }
      const uint64_t ChunkEnd = std::min(End, Address + Query.Size);
      const ULONG Prot = ForWriteLocked ? GetUntrapProt(Address) : GetTrapProt(Address);
      const auto Protect = [Prot](uint64_t From, uint64_t To) {
        void* TmpAddress = reinterpret_cast<void*>(From);
        SIZE_T TmpSize = static_cast<SIZE_T>(To - From);
        ULONG TmpProt;
        NtProtectVirtualMemory(NtCurrentProcess(), &TmpAddress, &TmpSize, Prot, &TmpProt);
      };
      if (ForWriteLocked || !AnyHotPages.load(std::memory_order_acquire)) {
        Protect(Address, ChunkEnd);
      } else {
        // Trap everything but the hot pages: their code checks itself.
        uint64_t Cursor = Address;
        for (auto It = HotPages.lower_bound(Address & FEXCore::Utils::FEX_PAGE_MASK); It != HotPages.end() && *It < ChunkEnd; ++It) {
          if (*It > Cursor) {
            Protect(Cursor, *It);
          }
          Cursor = std::max(Cursor, *It + FEXCore::Utils::FEX_PAGE_SIZE);
        }
        if (Cursor < ChunkEnd) {
          Protect(Cursor, ChunkEnd);
        }
      }
    } else if (!Query.Size) {
      // No more regions past `Address` in the interval list
      break;
    }

    Address += Query.Size;
  } while (Address < End);

  return HitRWXInterval;
}

void InvalidationTracker::SetHotPageThreshold(uint32_t Faults, uint32_t WindowMs) {
  CounterFrequency = ReadCounterFrequency();
  HotPageFaults = Faults;
  HotPageWindowTicks = std::max<uint64_t>(WindowMs, 1) * CounterFrequency / 1000;
}

bool InvalidationTracker::IsFullSMCRange(uint64_t Address, uint64_t Size) {
  if (!AnyHotPages.load(std::memory_order_acquire)) {
    return false;
  }
  std::shared_lock Lock(IntervalsLock);
  const auto It = HotPages.lower_bound(Address & FEXCore::Utils::FEX_PAGE_MASK);
  return It != HotPages.end() && *It < Address + Size;
}

bool InvalidationTracker::NoteWriteFaultLocked(uint64_t Page) {
  if (!HotPageFaults) {
    return false;
  }
  const uint64_t Now = ReadCounter();
  FaultCounter* Counter = nullptr;
  for (auto& C : FaultCounters) {
    if (C.Page == Page && C.Count) {
      Counter = &C;
      break;
    }
  }
  if (!Counter) {
    Counter = &FaultCounters[NextFaultCounter++ % std::size(FaultCounters)];
    *Counter = {Page, Now, 0};
  }
  if (Now - Counter->WindowStart > HotPageWindowTicks) {
    Counter->WindowStart = Now;
    Counter->Count = 0;
  }
  if (++Counter->Count < HotPageFaults) {
    return false;
  }

  const uint32_t Faults = Counter->Count;
  const double WindowMs = static_cast<double>(Now - Counter->WindowStart) * 1000.0 / static_cast<double>(CounterFrequency);
  *Counter = {};
  uint64_t Total {};
  {
    std::unique_lock Lock(IntervalsLock);
    HotPages.insert(Page);
    Total = HotPages.size();
  }
  AnyHotPages.store(true, std::memory_order_release);
  {
    std::scoped_lock Lock(HotPageEventsLock);
    if (HotPageEventCount < std::size(HotPageEvents)) {
      HotPageEvents[HotPageEventCount++] = {Page, Faults, WindowMs, Total};
    }
  }
  return true;
}

void InvalidationTracker::ForgetHotPagesLocked(uint64_t Start, uint64_t End) {
  if (!AnyHotPages.load(std::memory_order_relaxed)) {
    return;
  }
  HotPages.erase(HotPages.lower_bound(Start), HotPages.lower_bound(End));
  if (HotPages.empty()) {
    AnyHotPages.store(false, std::memory_order_release);
  }
}

size_t InvalidationTracker::TakeHotPageEvents(HotPageEvent* Out, size_t Max) {
  std::scoped_lock Lock(HotPageEventsLock);
  const size_t N = std::min(Max, HotPageEventCount);
  std::copy(HotPageEvents, HotPageEvents + N, Out);
  HotPageEventCount = 0;
  return N;
}

} // namespace FEX::Windows
