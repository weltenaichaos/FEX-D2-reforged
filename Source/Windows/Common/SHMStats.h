// SPDX-License-Identifier: MIT
#pragma once

#include "Common/SHMStats.h"

namespace FEX::Windows {
class StatAlloc final : public FEX::SHMStats::StatAllocBase {
public:
  // LocalFallback: if the UnixLib can't provide the shared mapping (it isn't shipped in a GameNative .wcp), keep the
  // stats in a private in-process allocation instead so the in-process StatsLog can still read them.
  StatAlloc(FEXCore::SHMStats::AppType AppType, bool LocalFallback = false);
  virtual ~StatAlloc();

  FEXCore::SHMStats::ThreadStats* AllocateSlot(uint32_t TID) {
    return StatAllocBase::AllocateSlot(TID);
  }

  void DeallocateSlot(FEXCore::SHMStats::ThreadStats* AllocatedSlot) {
    if (!AllocatedSlot) {
      return;
    }

    StatAllocBase::DeallocateSlot(AllocatedSlot);
  }

  // Walks the live slot list the same way an external reader of the SHM region does. Racy against slot
  // (de)allocation by design; a reader may see a slot that was just freed (TID == 0), which is skipped.
  template<typename Fn>
  void ForEachSlot(Fn&& Func) const {
    if (!Head) {
      return;
    }

    uint32_t Offset = Head->Head.load(std::memory_order_relaxed);
    for (uint32_t Visited = 0; Offset && Visited < TotalSlotsFromSize(CurrentSize) + 1; ++Visited) {
      auto Slot = reinterpret_cast<FEXCore::SHMStats::ThreadStats*>(reinterpret_cast<uint64_t>(Base) + Offset);
      if (Slot->TID.load(std::memory_order_relaxed) != 0) {
        Func(*Slot);
      }
      Offset = Slot->Next.load(std::memory_order_relaxed);
    }
  }

private:
  uint32_t FrontendAllocateSlots(uint32_t NewSize) override;
  bool IsLocal {};
};

} // namespace FEX::Windows
