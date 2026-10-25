// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Reads the FEX link of FEX's Turnip build: descriptor fix statistics and GPU counters around big render passes
$end_info$
*/

#include "TurnipReport.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/TurnipChannel.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);

namespace TurnipReport {
namespace {
  namespace T = FEX::Windows::Turnip;

  struct CounterName {
    uint32_t Group, Countable;
    const char* Name;
  };
  constexpr CounterName Names[] = {
    {T::GroupSp, 0, "sp_busy"},
    {T::GroupSp, 8, "waves"},
    {T::GroupSp, 9, "wave_cyc"},
    {T::GroupSp, 14, "fs_cyc"},
    {T::GroupSp, 4, "sp_stall_tp"},
    {T::GroupSp, 21, "wave_fetch"},
    {T::GroupSp, 51, "icl1_req"},
    {T::GroupSp, 52, "icl1_miss"},
    {T::GroupSp, 85, "icl1_miss_cyc"},
    {T::GroupSp, 142, "sp_stche_miss_fs"},
    {T::GroupSp, 44, "fs_inst"},
    {T::GroupTp, 7, "tp_l1_miss"},
    {T::GroupTp, 2, "tp_lat_cyc"},
    {T::GroupTp, 3, "tp_lat_trans"},
    {T::GroupHlsq, 55, "hlsq_stche_miss_fs"},
  };

  fextl::string CounterLabel(uint32_t Group, uint32_t Countable) {
    for (const auto& N : Names) {
      if (N.Group == Group && N.Countable == Countable) {
        return N.Name;
      }
    }
    return fextl::fmt::format("g{}c{}", Group, Countable);
  }

  // Slots the driver allocates are 48 bytes + 16 per counter, rounded up to 64.
  constexpr size_t MaxSlotSize = 1024;

  struct ModeTotals {
    uint64_t Passes;
    uint64_t Draws;
    uint64_t Ticks;
    uint64_t Pixels;
    uint64_t Values[T::MaxCounters];
  };

  T::Channel* Chan {};
  bool NotedDriver {};
  bool NotedMissing {};
  uint32_t Ticks {};
  uint32_t Seconds {};
  uint32_t CurrentMode {};
  uint64_t NextSeq {1};
  uint64_t Last[6][2] {};

  bool ReadSlot(uint64_t RingCpu, uint32_t Index, uint32_t SlotSize, uint8_t* Out) {
    SIZE_T Read = 0;
    const auto Address = reinterpret_cast<const void*>(RingCpu + static_cast<uint64_t>(Index) * SlotSize);
    return NtReadVirtualMemory(NtCurrentProcess(), Address, Out, SlotSize, &Read) == STATUS_SUCCESS && Read == SlotSize;
  }

  void NoteDriver(uint32_t Status) {
    fextl::string Counters;
    for (uint32_t i = 0; i < Chan->CounterCount && i < T::MaxCounters; ++i) {
      const auto& C = Chan->Counters[i];
      Counters += fextl::fmt::format("{}{} {}", i ? ", " : "", CounterLabel(C.Group, C.Countable),
                                     C.Reg ? "ok" : C.Result == 1 ? "not tried" : fextl::fmt::format("failed {}", C.Result));
    }
    char Driver[sizeof(Chan->Driver) + 1] {};
    memcpy(Driver, Chan->Driver, sizeof(Chan->Driver));
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "turnip: driver '{}' (patch {}) attached, status {:#x}: descriptor fix {}{}, {} of {} GPU counters reserved{}{}; ring {}x{} bytes; "
      "counters: {}",
      Driver, Chan->Patch, Status, (Status & T::StatusDescPatch) ? "honored" : "NOT honored",
      (Status & T::StatusNotA7xx) ? " (not an a7xx GPU)" : "", Chan->CountersReserved, Chan->CounterCount,
      (Status & T::StatusNotKgsl) ? " (not KGSL)" : "", (Status & T::StatusRingFailed) ? ", ring allocation FAILED" : "",
      Chan->RingCount, Chan->SlotSize, Counters.empty() ? "-" : Counters));
  }

  void Delta(int Index, const volatile uint64_t* Now, uint64_t Out[2]) {
    for (int i = 0; i < 2; ++i) {
      const uint64_t V = Now[i];
      Out[i] = V - Last[Index][i];
      Last[Index][i] = V;
    }
  }

  fextl::string Lines() {
    const uint32_t Status = __atomic_load_n(&Chan->Status, __ATOMIC_ACQUIRE);
    // Pick the mode for the next second first, so the flip happens at a steady pace.
    const uint32_t ModeThisSecond = CurrentMode;
    if (FEX::Windows::Turnip::DescSwitch() == 2) {
      ++Seconds;
      CurrentMode = (Seconds & 1) ? (T::ModeChangedOnly | T::ModePushReuse) : 0;
      __atomic_store_n(&Chan->Mode, CurrentMode, __ATOMIC_RELAXED);
    }
    ++Ticks;
    if (!(Status & T::StatusAttached)) {
      if (!NotedMissing && Ticks == 120) {
        NotedMissing = true;
        FEX::Windows::StatsLogNote("turnip: no driver attached to the FEX link after 120 s: the game's Vulkan driver isn't FEX's "
                                   "Turnip build (stock Turnip ignores TU_FEX_CHANNEL), so no descriptor fix and no GPU counters");
      }
      return {};
    }
    fextl::string Out;
    if (!NotedDriver) {
      NotedDriver = true;
      NoteDriver(Status);
    }

    uint64_t CmdBufs[2], Emits[2], Flushed[2], Push[2], Reused[2], Misc[2];
    Delta(0, Chan->CmdBufs, CmdBufs);
    Delta(1, Chan->DescEmits, Emits);
    Delta(2, Chan->DescSetsFlushed, Flushed);
    Delta(3, Chan->PushTotal, Push);
    Delta(4, Chan->PushReused, Reused);
    const uint64_t PassNow[2] = {Chan->PassesSampled, Chan->PassesSmall};
    Delta(5, PassNow, Misc);
    const auto PerUpdate = [&](int i) {
      return Emits[i] ? fextl::fmt::format("{:.2f}", static_cast<double>(Flushed[i]) / Emits[i]) : fextl::string("-");
    };
    const uint64_t PushAll = Push[0] + Push[1];
    Out += fextl::fmt::format("  turnip: desc {} this second | cmdbufs off {} on {} | updates off {} on {} | sets per update off {} on {} "
                              "(stock 8) | push {} reused {:.0f}% | passes sampled {} small {}\n",
                              (ModeThisSecond & T::ModeChangedOnly) ? "on" : "off", CmdBufs[0], CmdBufs[1], Emits[0], Emits[1],
                              PerUpdate(0), PerUpdate(1), PushAll, PushAll ? 100.0 * (Reused[0] + Reused[1]) / PushAll : 0.0,
                              Misc[0], Misc[1]);

    const uint64_t RingCpu = __atomic_load_n(&Chan->RingCpu, __ATOMIC_ACQUIRE);
    const uint32_t RingCount = Chan->RingCount;
    const uint32_t SlotSize = Chan->SlotSize;
    const uint32_t Count = Chan->CounterCount;
    if (!RingCpu || !RingCount || (RingCount & (RingCount - 1)) || SlotSize < sizeof(T::Slot) || SlotSize > MaxSlotSize ||
        sizeof(T::Slot) + 2 * sizeof(uint64_t) * Count > SlotSize) {
      return Out;
    }

    // Passes the GPU finished since last time. A slot not done yet stops the walk if it is recent (its command buffer
    // may still be queued), older ones are skipped (never submitted, or overwritten).
    ModeTotals Totals[2] {};
    const uint64_t End = __atomic_load_n(&Chan->RingNext, __ATOMIC_ACQUIRE);
    if (End > NextSeq + RingCount) {
      NextSeq = End - RingCount;
    }
    alignas(8) uint8_t Buffer[MaxSlotSize];
    uint32_t Read = 0;
    while (NextSeq < End && Read < 2048) {
      const uint32_t Index = static_cast<uint32_t>(NextSeq & (RingCount - 1));
      if (!ReadSlot(RingCpu, Index, SlotSize, Buffer)) {
        break;
      }
      ++Read;
      T::Slot S;
      memcpy(&S, Buffer, sizeof(S));
      if (S.Seq != NextSeq || S.Done != static_cast<uint32_t>(NextSeq)) {
        if (End - NextSeq <= 256) {
          break;
        }
        ++NextSeq;
        continue;
      }
      ++NextSeq;
      auto& M = Totals[(S.Mode & T::ModeChangedOnly) ? 1 : 0];
      M.Passes++;
      M.Draws += S.Draws;
      M.Ticks += S.TsEnd - S.TsBegin;
      M.Pixels += static_cast<uint64_t>(S.Width) * S.Height;
      const uint64_t* Values = reinterpret_cast<const uint64_t*>(Buffer + sizeof(T::Slot));
      for (uint32_t i = 0; i < Count; ++i) {
        if (Chan->Counters[i].Reg) {
          M.Values[i] += Values[Count + i] - Values[i];
        }
      }
    }

    for (int m = 0; m < 2; ++m) {
      const auto& M = Totals[m];
      if (!M.Passes || !M.Draws) {
        continue;
      }
      const double Ms = M.Ticks / 19200.0;
      fextl::string Line = fextl::fmt::format("  turnip pass {}: {} passes, {} draws, gpu {:.1f} ms ({:.2f} us/draw); per draw:", m ? "on" : "off",
                                              M.Passes, M.Draws, Ms, Ms * 1000.0 / M.Draws);
      uint64_t IclReq = 0, IclMiss = 0, TpCyc = 0, TpTrans = 0;
      for (uint32_t i = 0; i < Count; ++i) {
        const auto& C = Chan->Counters[i];
        if (!C.Reg) {
          continue;
        }
        Line += fextl::fmt::format(" {} {:.1f}", CounterLabel(C.Group, C.Countable), static_cast<double>(M.Values[i]) / M.Draws);
        if (C.Group == T::GroupSp && C.Countable == 51) IclReq = M.Values[i];
        if (C.Group == T::GroupSp && C.Countable == 52) IclMiss = M.Values[i];
        if (C.Group == T::GroupTp && C.Countable == 2) TpCyc = M.Values[i];
        if (C.Group == T::GroupTp && C.Countable == 3) TpTrans = M.Values[i];
      }
      if (IclReq) {
        Line += fextl::fmt::format(" | icl1 miss {:.2f}%", 100.0 * IclMiss / IclReq);
      }
      if (TpTrans) {
        Line += fextl::fmt::format(" | tp latency {:.0f} cyc", static_cast<double>(TpCyc) / TpTrans);
      }
      Out += Line;
      Out += '\n';
    }
    return Out;
  }
} // namespace

void Init() {
  Chan = FEX::Windows::Turnip::GetChannel();
  if (!Chan) {
    return;
  }
  CurrentMode = Chan->Mode;
  FEX::Windows::StatsLogSetLineProvider(Lines);
}
} // namespace TurnipReport
