// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Reads the FEX link of FEX's Turnip build: descriptor fix and depth clear statistics, GPU counters around big render passes
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
#include <utility>
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
    // build 70 list
    {T::GroupCp, 0, "cycles"},
    {T::GroupCp, 11, "ctx_done"},
    {T::GroupCp, 15, "cp_idle"},
    {T::GroupCp, 22, "cp_pipe_stall"},
    {T::GroupHlsq, 2, "hlsq_sp_state_stall"},
    {T::GroupHlsq, 11, "hlsq_fs_data_wait"},
    {T::GroupHlsq, 12, "hlsq_dual_fs"},
    {T::GroupHlsq, 22, "hlsq_fs_stall"},
    {T::GroupSp, 0, "sp_busy"},
    {T::GroupSp, 8, "waves"},
    {T::GroupSp, 7, "sp_no_exec"},
    {T::GroupSp, 69, "sp_starve_hlsq"},
    {T::GroupSp, 10, "wave_life_cyc"},
    {T::GroupSp, 11, "wave_life_n"},
    {T::GroupSp, 97, "fs_waves_1x"},
    {T::GroupSp, 98, "fs_waves_2x"},
    {T::GroupSp, 101, "pixels"},
    {T::GroupSp, 89, "wave_alu"},
    {T::GroupSp, 20, "wave_wait"},
    {T::GroupSp, 22, "wave_idle"},
    {T::GroupSp, 24, "wave_long_sync"},
    {T::GroupSp, 25, "wave_short_sync"},
    {T::GroupSp, 119, "tex_lat_cyc"},
    {T::GroupSp, 120, "tex_lat_n"},
    // build 72 additions
    {T::GroupPc, 13, "pc_starve_cp"},
    {T::GroupPc, 48, "pc_starve_predraw"},
    {T::GroupVfd, 0, "vfd_busy"},
    {T::GroupTse, 0, "tse_busy"},
    {T::GroupRas, 0, "ras_busy"},
    {T::GroupRb, 0, "rb_busy"},
    {T::GroupRb, 23, "rb_2d_busy"},
    {T::GroupLrz, 0, "lrz_busy"},
    // build 74 additions
    {T::GroupCp, 2, "cp_busy"},
    {T::GroupCp, 16, "cp_starve_cmd"},
    {T::GroupCp, 17, "cp_starve_state"},
    {T::GroupCp, 23, "cp_sync_stall"},
    {T::GroupCp, 24, "cp_wfi_stall"},
    {T::GroupCp, 26, "cp_reg_writes"},
    {T::GroupCp, 27, "cp_state_loads"},
    {T::GroupCp, 29, "cp_draws"},
    {T::GroupCp, 30, "cp_reg_bunches"},
    {T::GroupCp, 45, "cp_dwords"},
    {T::GroupCp, 46, "cp_packets"},
    {T::GroupRb, 37, "rb_depth_write_stall"},
    // build 69 list
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

  // One kind of render pass: same size, attachments, first format, direct or tiled, descriptor mode, and for direct
  // passes that clear depth at their start, how that clear was done.
  struct PassKind {
    uint16_t Width, Height;
    uint8_t Colors, Attachments, Sysmem, Mode;
    uint32_t Format0;
    uint64_t Passes;
    uint64_t Draws;
    uint64_t Ticks;
    uint64_t Values[T::MaxCounters];

    bool Same(const T::Slot& S, uint8_t SlotMode) const {
      return Width == S.Width && Height == S.Height && Colors == S.Colors && Attachments == S.Attachments &&
             Sysmem == S.Sysmem && Mode == SlotMode && Format0 == S.Format0;
    }
  };
  constexpr size_t MaxKinds = 16;
  // Kinds printed per second, the ones with the most GPU time.
  constexpr size_t PrintKinds = 5;

  // PassKind::Mode bits: T::ModeChangedOnly, and for direct passes with a depth clear T::SlotDepthClear plus
  // T::ModeDepthClear3d when the 3D draw did it and T::ModeDepthClearFastZ when that draw ran without fragment shader.
  uint8_t KindMode(const T::Slot& S) {
    uint8_t Mode = S.Mode & T::ModeChangedOnly;
    if (S.Sysmem && (S.Mode & T::SlotDepthClear)) {
      Mode |= T::SlotDepthClear | (S.Mode & T::ModeDepthClear3d);
      if (S.Mode & T::ModeDepthClear3d) {
        Mode |= S.Mode & T::ModeDepthClearFastZ;
      }
    }
    return Mode;
  }

  T::Channel* Chan {};
  bool NotedDriver {};
  bool NotedMissing {};
  uint32_t Ticks {};
  uint32_t Seconds {};
  uint32_t CurrentMode {};
  uint64_t NextSeq {1};
  uint64_t Last[9][2] {};
  uint64_t BudgetLast[2] {}; // BudgetQueries, BudgetNs at the last report
  uint32_t BudgetLastReads {};
  uint32_t LoopPrefetchLast[2] {}; // LoopPrefetchLoads, LoopPrefetchShaders at the last report

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
    const char* EarlyPreamble = (Status & T::StatusPushSpeculate)                 ? "ON" :
                                !(Chan->Compile & T::CompilePushSpeculate) ? "off (FEX_TU_EARLY_PREAMBLE=0)" :
                                Chan->Patch < 2                            ? "NOT available (driver older than fex2)" :
                                                                             "NOT honored";
    const char* PreambleLoads = (Status & T::StatusPreambleLoads)              ? "ON" :
                                !(Chan->Compile & T::CompilePreambleLoads) ? "off (FEX_TU_PREAMBLE_LOADS=0)" :
                                Chan->Patch < 6                            ? "NOT available (driver older than fex6)" :
                                                                             "NOT honored";
    const char* LoopPrefetch = (Status & T::StatusLoopPrefetch)               ? "ON" :
                               !(Chan->Compile & T::CompileLoopPrefetch) ? "off (FEX_TU_LOOP_PREFETCH=0)" :
                               Chan->Patch < 7                           ? "NOT available (driver older than fex7)" :
                                                                           "NOT honored";
    const char* CpUbo = (Status & T::StatusCpUbo)               ? "ON" :
                        !(Chan->Compile & T::CompileCpUbo) ? "off (FEX_TU_CP_UBO=0)" :
                        Chan->Patch < 9                    ? "NOT available (driver older than fex9)" :
                                                             "NOT honored (needs an a7xx GPU like the a740)";
    const uint32_t ClearSwitch = FEX::Windows::Turnip::DepthClearSwitch();
    const char* DepthClear = !ClearSwitch                             ? "by the 2D engine as stock (FEX_TU_DEPTH_CLEAR=0)" :
                             Chan->Patch < 3                          ? "by the 2D engine: 3D draw NOT available (driver older than fex3)" :
                             !(Status & T::StatusDepthClear3d)        ? "by the 2D engine: 3D draw NOT honored" :
                             ClearSwitch == 1                         ? "by a 3D draw (FEX_TU_DEPTH_CLEAR=1)" :
                             Chan->Patch < 4                          ? "by a 3D draw, fast Z NOT available (driver older than fex4)" :
                             !(Status & T::StatusDepthClearFastZ)     ? "by a 3D draw, fast Z NOT honored" :
                             ClearSwitch == 2                         ? "by a 3D draw, without fragment shader (fast Z) in every other "
                                                                        "second (FEX_TU_DEPTH_CLEAR=alt)" :
                                                                        "by a 3D draw without fragment shader (fast Z, "
                                                                        "FEX_TU_DEPTH_CLEAR=fastz)";
    const char* Budget = !FEX::Windows::Turnip::BudgetCacheSwitch() ? "read on every query as stock (FEX_TU_BUDGET_CACHE=0)" :
                         Chan->Patch < 5                           ? "read on every query: the cache is NOT available (driver older than fex5)" :
                         !(Status & T::StatusBudgetCache)          ? "read on every query: the cache is NOT honored" :
                                                                     "read at most every 250 ms (FEX_TU_BUDGET_CACHE=1)";
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "turnip: driver '{}' (patch {}) attached, status {:#x}: descriptor fix {}{}, early preamble for root constant buffers {}, "
      "preamble loads first {}, list loops read ahead {}, root constant buffers loaded by the command processor {}, depth clears "
      "at the start of direct passes {}, available memory for the "
      "memory budget {}, {} of {} GPU counters reserved{}{}; ring {}x{} bytes; counters: {}",
      Driver, Chan->Patch, Status, (Status & T::StatusDescPatch) ? "honored" : "NOT honored",
      (Status & T::StatusNotA7xx) ? " (not an a7xx GPU)" : "", EarlyPreamble, PreambleLoads, LoopPrefetch, CpUbo, DepthClear, Budget,
      Chan->CountersReserved,
      Chan->CounterCount,
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

  int FindCounter(uint32_t Count, uint32_t Group, uint32_t Countable) {
    for (uint32_t i = 0; i < Count; ++i) {
      const auto& C = Chan->Counters[i];
      if (C.Reg && C.Group == Group && C.Countable == Countable) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // One pass kind's line: time per draw, then what the counters say about the shader cores and what feeds them, then
  // every counter per draw.
  fextl::string KindLine(const PassKind& K, uint32_t Count, bool DescSeen) {
    const double Ms = K.Ticks / 19200.0;
    const double Draws = K.Draws ? static_cast<double>(K.Draws) : 1.0;
    const auto Has = [&](uint32_t Group, uint32_t Countable) {
      return FindCounter(Count, Group, Countable) >= 0;
    };
    const auto V = [&](uint32_t Group, uint32_t Countable) {
      const int i = FindCounter(Count, Group, Countable);
      return i < 0 ? 0.0 : static_cast<double>(K.Values[i]);
    };
    const auto Ratio = [](double A, double B) {
      return B > 0 ? A / B : 0.0;
    };
    const char* Clear = !(K.Mode & T::SlotDepthClear)        ? "" :
                        !(K.Mode & T::ModeDepthClear3d)      ? " clear 2d" :
                        (K.Mode & T::ModeDepthClearFastZ)    ? " clear fastz" :
                                                               " clear 3d";
    fextl::string Line = fextl::fmt::format("  turnip pass {}x{} c{}/a{} fmt{} {}{}{}: {} passes, {} draws, gpu {:.1f} ms ({:.2f} us/draw)",
                                            K.Width, K.Height, K.Colors, K.Attachments, K.Format0, K.Sysmem ? "direct" : "tiled",
                                            DescSeen ? ((K.Mode & T::ModeChangedOnly) ? " desc on" : " desc off") : "", Clear,
                                            K.Passes, K.Draws, Ms, Ms * 1000.0 / Draws);
    const double Cycles = V(T::GroupCp, 0);
    if (Cycles > 0 && K.Ticks) {
      Line += fextl::fmt::format(" at {:.0f} MHz", Cycles / (K.Ticks / 19.2));
    }
    if (!Count) {
      return Line + '\n';
    }

    // Shader cores: how many are busy on average, how many waves each holds while busy, how many hold waves but run
    // none, how many wait for the front end to hand them work.
    const double Busy = V(T::GroupSp, 0);
    if (Cycles > 0 && Has(T::GroupSp, 0)) {
      Line += fextl::fmt::format(" | cores busy {:.2f}", Busy / Cycles);
      if (Has(T::GroupSp, 8)) {
        Line += fextl::fmt::format(", waves per busy core {:.2f}", Ratio(V(T::GroupSp, 8), Busy));
      }
      if (Has(T::GroupSp, 7)) {
        Line += fextl::fmt::format(", cores stalled {:.2f}", V(T::GroupSp, 7) / Cycles);
      }
      if (Has(T::GroupSp, 69)) {
        Line += fextl::fmt::format(", cores starved by hlsq {:.2f}", V(T::GroupSp, 69) / Cycles);
      }
    }
    // Waves: how many per draw, how many pixels, how long one lives, where its cycles go.
    if (Has(T::GroupSp, 97) || Has(T::GroupSp, 98)) {
      const double W1 = V(T::GroupSp, 97), W2 = V(T::GroupSp, 98);
      Line += fextl::fmt::format(" | fs waves/draw {:.1f} (2x {:.0f}%)", (W1 + W2) / Draws, 100.0 * Ratio(W2, W1 + W2));
    }
    if (Has(T::GroupSp, 101)) {
      Line += fextl::fmt::format(", px/draw {:.0f}", V(T::GroupSp, 101) / Draws);
    }
    if (Has(T::GroupSp, 10) && Has(T::GroupSp, 11)) {
      Line += fextl::fmt::format(", wave life {:.0f} cyc", Ratio(V(T::GroupSp, 10), V(T::GroupSp, 11)));
    }
    const double Waves = V(T::GroupSp, 8);
    if (Waves > 0) {
      static constexpr struct {
        uint32_t Countable;
        const char* Name;
      } Split[] = {{89, "alu"}, {20, "wait"}, {22, "idle"}, {24, "long-sync"}, {25, "short-sync"}};
      fextl::string Parts;
      for (const auto& P : Split) {
        if (Has(T::GroupSp, P.Countable)) {
          Parts += fextl::fmt::format(" {} {:.0f}%", P.Name, 100.0 * V(T::GroupSp, P.Countable) / Waves);
        }
      }
      if (!Parts.empty()) {
        Line += " | wave cycles" + Parts;
      }
    }
    if (Has(T::GroupSp, 119) && Has(T::GroupSp, 120)) {
      Line += fextl::fmt::format(" | tex latency {:.0f} cyc", Ratio(V(T::GroupSp, 119), V(T::GroupSp, 120)));
    }
    // Front end and command processor, per GPU cycle (1.00 = the whole pass).
    if (Cycles > 0) {
      static constexpr struct {
        uint32_t Group, Countable;
        const char* Name;
      } PerCycle[] = {{T::GroupHlsq, 12, "dual fs"},          {T::GroupHlsq, 2, "sp state stall"}, {T::GroupHlsq, 22, "fs stall"},
                      {T::GroupHlsq, 11, "fs data wait"},     {T::GroupCp, 15, "cp idle"},         {T::GroupCp, 22, "cp pipe stall"}};
      fextl::string Parts;
      for (const auto& P : PerCycle) {
        if (Has(P.Group, P.Countable)) {
          Parts += fextl::fmt::format("{}{} {:.2f}", Parts.empty() ? "" : ", ", P.Name, V(P.Group, P.Countable) / Cycles);
        }
      }
      if (!Parts.empty()) {
        Line += " | per cycle: " + Parts;
      }
    }
    // Units ahead of and behind the shader cores, per GPU cycle (build 72): primitive control waiting for the command
    // processor's next draw or for draw setup, and how busy vertex fetch, triangle setup, rasterizer, depth pre-test,
    // render backend and 2D engine are. One unit near 1.00 (or above, for units the counter sums over several copies of)
    // while the others idle is the limit.
    if (Cycles > 0) {
      static constexpr struct {
        uint32_t Group, Countable;
        const char* Name;
      } Units[] = {{T::GroupPc, 13, "pc waits for cp"},    {T::GroupPc, 48, "pc waits for draw setup"},
                   {T::GroupVfd, 0, "vertex fetch"},       {T::GroupTse, 0, "triangle setup"},
                   {T::GroupRas, 0, "rasterizer"},         {T::GroupLrz, 0, "depth pre-test"},
                   {T::GroupRb, 0, "render backend"},      {T::GroupRb, 23, "2d engine"},
                   {T::GroupRb, 37, "depth writes wait for memory"}};
      fextl::string Parts;
      for (const auto& U : Units) {
        if (Has(U.Group, U.Countable)) {
          Parts += fextl::fmt::format("{}{} {:.2f}", Parts.empty() ? "" : ", ", U.Name, V(U.Group, U.Countable) / Cycles);
        }
      }
      if (!Parts.empty()) {
        Line += " | units per cycle: " + Parts;
      }
    }
    // Command processor per draw (build 74), in GPU cycles: busy, then what it executed (register writes, register
    // bunches, state loads = constants/descriptors/shaders, draw packets) and what it waited for (command buffer data
    // or draw state data from memory, syncs, wait for idle); packets and dwords it read. Busy close to the pass's
    // cycles per draw with primitive control waiting for it means the command processor is the limit.
    if (Has(T::GroupCp, 2) || Has(T::GroupCp, 46)) {
      static constexpr struct {
        uint32_t Countable;
        const char* Name;
      } CpParts[] = {{2, "busy"},           {26, "register writes"},      {30, "register bunches"},
                     {27, "state loads"},   {29, "draw packets"},         {16, "waits for command data"},
                     {17, "waits for draw state data"}, {23, "sync waits"}, {24, "waits for idle"}};
      fextl::string Parts;
      for (const auto& P : CpParts) {
        if (Has(T::GroupCp, P.Countable)) {
          Parts += fextl::fmt::format("{}{} {:.0f}", Parts.empty() ? "" : ", ", P.Name, V(T::GroupCp, P.Countable) / Draws);
        }
      }
      if (Has(T::GroupCp, 46)) {
        Parts += fextl::fmt::format("{}packets {:.1f}", Parts.empty() ? "" : " | ", V(T::GroupCp, 46) / Draws);
      }
      if (Has(T::GroupCp, 45)) {
        Parts += fextl::fmt::format(", dwords {:.0f}", V(T::GroupCp, 45) / Draws);
      }
      if (!Parts.empty()) {
        Line += " | command processor per draw: " + Parts;
      }
    }
    if (Has(T::GroupCp, 11)) {
      Line += fextl::fmt::format(" | contexts/draw {:.2f}", V(T::GroupCp, 11) / Draws);
    }

    Line += "; per draw:";
    for (uint32_t i = 0; i < Count; ++i) {
      const auto& C = Chan->Counters[i];
      if (C.Reg) {
        Line += fextl::fmt::format(" {} {:.1f}", CounterLabel(C.Group, C.Countable), static_cast<double>(K.Values[i]) / Draws);
      }
    }
    return Line + '\n';
  }

  fextl::string Lines() {
    const uint32_t Status = __atomic_load_n(&Chan->Status, __ATOMIC_ACQUIRE);
    // Pick the mode for the next second first, so the flip happens at a steady pace. With both switches on alt the
    // depth clear flips every two seconds, so each of the four combinations gets seconds of its own.
    const uint32_t ModeThisSecond = CurrentMode;
    const uint32_t DescSw = FEX::Windows::Turnip::DescSwitch();
    const uint32_t ClearSw = FEX::Windows::Turnip::DepthClearSwitch();
    if (DescSw == 2 || ClearSw == 2) {
      ++Seconds;
      uint32_t Mode = CurrentMode;
      if (DescSw == 2) {
        Mode = (Mode & ~(T::ModeChangedOnly | T::ModePushReuse)) | ((Seconds & 1) ? (T::ModeChangedOnly | T::ModePushReuse) : 0);
      }
      if (ClearSw == 2) {
        // Build 74: the 3D draw stays on, its fast Z (no fragment shader) flips.
        const bool On = DescSw == 2 ? ((Seconds >> 1) & 1) : (Seconds & 1);
        Mode = (Mode & ~T::ModeDepthClearFastZ) | T::ModeDepthClear3d | (On ? T::ModeDepthClearFastZ : 0);
      }
      CurrentMode = Mode;
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
    uint64_t Clears[2], ClearPx[2], FastZ[2];
    Delta(6, Chan->DepthClears, Clears);
    Delta(7, Chan->DepthClearPx, ClearPx);
    const uint64_t FastZNow[2] = {Chan->DepthClearsFastZ, Chan->DepthClearPxFastZ};
    Delta(8, FastZNow, FastZ);
    const auto PerUpdate = [&](int i) {
      return Emits[i] ? fextl::fmt::format("{:.2f}", static_cast<double>(Flushed[i]) / Emits[i]) : fextl::string("-");
    };
    const uint64_t PushAll = Push[0] + Push[1];
    const bool DescSeen = FEX::Windows::Turnip::DescSwitch() != 0 || CmdBufs[1] != 0;
    if (DescSeen) {
      Out += fextl::fmt::format("  turnip: desc {} this second | cmdbufs off {} on {} | updates off {} on {} | sets per update off {} on {} "
                                "(stock 8) | push {} reused {:.0f}% | passes sampled {} small {}\n",
                                (ModeThisSecond & T::ModeChangedOnly) ? "on" : "off", CmdBufs[0], CmdBufs[1], Emits[0], Emits[1],
                                PerUpdate(0), PerUpdate(1), PushAll, PushAll ? 100.0 * (Reused[0] + Reused[1]) / PushAll : 0.0,
                                Misc[0], Misc[1]);
    } else {
      Out += fextl::fmt::format("  turnip: cmdbufs {} | descriptor updates {} | push {} | passes sampled {} small {}\n", CmdBufs[0],
                                Emits[0], PushAll, Misc[0], Misc[1]);
    }

    // Depth clears at the start of passes that may run direct, as recorded (fex3 and later): by the 2D engine or a 3D draw,
    // and of the 3D draws the ones without fragment shader (fast Z, fex4 and later).
    if (Chan->Patch >= 3 && (ClearSw != 0 || Clears[1] != 0)) {
      const char* Now = !(ModeThisSecond & T::ModeDepthClear3d)                         ? "2d" :
                        (ModeThisSecond & T::ModeDepthClearFastZ) && Chan->Patch >= 4 ? "fastz" :
                                                                                          "3d";
      Out += fextl::fmt::format("  turnip: depth clear {} this second | depth clears 2d engine {} ({:.1f} Mpx) 3d draw {} ({:.1f} Mpx), "
                                "of them fast z {} ({:.1f} Mpx)\n",
                                Now, Clears[0], ClearPx[0] / 1e6, Clears[1], ClearPx[1] / 1e6, FastZ[0], FastZ[1] / 1e6);
    }

    // The memory budget query (fex5 and later): how often the game asks, how often the driver read /proc/meminfo, and the
    // time it took (build 76: D2R's main thread in vkGetPhysicalDeviceMemoryProperties2 ~3% of every second).
    if (Chan->Patch >= 5) {
      const uint64_t Queries = Chan->BudgetQueries, Ns = Chan->BudgetNs;
      const uint32_t Reads = Chan->BudgetReads;
      const uint64_t DQueries = Queries - BudgetLast[0], DNs = Ns - BudgetLast[1];
      const uint32_t DReads = Reads - BudgetLastReads;
      BudgetLast[0] = Queries;
      BudgetLast[1] = Ns;
      BudgetLastReads = Reads;
      if (DQueries) {
        Out += fextl::fmt::format("  turnip: memory budget queries {} | /proc/meminfo reads {} | {:.1f} us a query, {:.2f} ms this second\n",
                                  DQueries, DReads, static_cast<double>(DNs) / 1e3 / static_cast<double>(DQueries),
                                  static_cast<double>(DNs) / 1e6);
      }
    }

    // List loops read ahead (fex7 and later): shaders compiled this second whose list reads moved one iteration ahead.
    // Shaders from a cache are not counted, so a run after the first may show none.
    if (Chan->Patch >= 7) {
      const uint32_t Loads = Chan->LoopPrefetchLoads, Shaders = Chan->LoopPrefetchShaders;
      if (Shaders != LoopPrefetchLast[1]) {
        Out += fextl::fmt::format("  turnip: list loops read ahead in {} new shaders ({} list reads) | this run {} shaders ({} reads)\n",
                                  Shaders - LoopPrefetchLast[1], Loads - LoopPrefetchLast[0], Shaders, Loads);
        LoopPrefetchLast[0] = Loads;
        LoopPrefetchLast[1] = Shaders;
      }
    }

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
    PassKind Kinds[MaxKinds];
    size_t KindCount = 0;
    PassKind Other {};
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
      const uint8_t SlotMode = KindMode(S);
      PassKind* K = nullptr;
      for (size_t i = 0; i < KindCount; ++i) {
        if (Kinds[i].Same(S, SlotMode)) {
          K = &Kinds[i];
          break;
        }
      }
      if (!K) {
        K = KindCount < MaxKinds ? &Kinds[KindCount++] : &Other;
        if (K != &Other) {
          *K = {};
          K->Width = S.Width;
          K->Height = S.Height;
          K->Colors = S.Colors;
          K->Attachments = S.Attachments;
          K->Sysmem = S.Sysmem;
          K->Mode = SlotMode;
          K->Format0 = S.Format0;
        }
      }
      K->Passes++;
      K->Draws += S.Draws;
      K->Ticks += S.TsEnd - S.TsBegin;
      const uint64_t* Values = reinterpret_cast<const uint64_t*>(Buffer + sizeof(T::Slot));
      for (uint32_t i = 0; i < Count; ++i) {
        if (Chan->Counters[i].Reg) {
          K->Values[i] += Values[Count + i] - Values[i];
        }
      }
    }

    // Biggest kinds first.
    PassKind* Order[MaxKinds];
    for (size_t i = 0; i < KindCount; ++i) {
      Order[i] = &Kinds[i];
    }
    for (size_t i = 1; i < KindCount; ++i) {
      for (size_t j = i; j > 0 && Order[j]->Ticks > Order[j - 1]->Ticks; --j) {
        std::swap(Order[j], Order[j - 1]);
      }
    }
    for (size_t i = PrintKinds; i < KindCount; ++i) {
      Other.Passes += Order[i]->Passes;
      Other.Draws += Order[i]->Draws;
      Other.Ticks += Order[i]->Ticks;
    }
    for (size_t i = 0; i < KindCount && i < PrintKinds; ++i) {
      Out += KindLine(*Order[i], Count, DescSeen);
    }
    if (Other.Passes) {
      Out += fextl::fmt::format("  turnip pass other: {} passes, {} draws, gpu {:.1f} ms\n", Other.Passes, Other.Draws, Other.Ticks / 19200.0);
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
