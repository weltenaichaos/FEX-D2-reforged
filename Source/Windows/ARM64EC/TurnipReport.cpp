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

#include <algorithm>
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
    // build 102 additions
    {T::GroupSp, 19, "wave_nop"},
    {T::GroupSp, 16, "wave_ctrl"},
    {T::GroupSp, 17, "wave_load"},
    {T::GroupSp, 18, "wave_emit"},
    {T::GroupSp, 23, "wave_end"},
    {T::GroupSp, 37, "fs_tex_inst"},
    {T::GroupTp, 0, "tp_busy"},
    {T::GroupTp, 6, "tp_l1_req"},
    {T::GroupUche, 9, "l2_req_tp"},
    {T::GroupUche, 4, "mem_beats_tp"},
    // build 103 additions
    {T::GroupSp, 90, "wave_efu"},
    {T::GroupSp, 91, "wave_int"},
    {T::GroupSp, 92, "wave_csp"},
    {T::GroupSp, 26, "wave_join"},
    {T::GroupSp, 104, "wave_input"},
    {T::GroupSp, 105, "wave_output"},
    {T::GroupSp, 106, "wave_hwave_wait"},
    {T::GroupSp, 107, "wave_hwave_sync"},
    // build 106 additions
    {T::GroupHlsq, 3, "hlsq_sp_fs_stall"},
    {T::GroupHlsq, 39, "hlsq_fs_starving_sp"},
    // build 107 additions (compute)
    {T::GroupSp, 1, "sp_alu_working"},
    {T::GroupSp, 2, "sp_efu_working"},
    {T::GroupSp, 5, "sp_stall_uche"},
    {T::GroupSp, 31, "gm_stores"},
    {T::GroupSp, 32, "gm_atomics"},
    {T::GroupSp, 45, "addr_locks"},
    {T::GroupSp, 56, "cs_inst"},
    {T::GroupSp, 77, "eu_working_cs"},
    {T::GroupSp, 100, "cs_threads"},
    {T::GroupHlsq, 0, "hlsq_busy"},
    {T::GroupHlsq, 1, "hlsq_stall_uche"},
    {T::GroupHlsq, 10, "hlsq_cs_dispatches"},
    {T::GroupUche, 15, "l2_write_req_sp"},
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

  // One kind of render pass: same size, attachments, first format, direct or tiled (and from fex14 the wave setup arm,
  // both in Sysmem), descriptor mode, and for direct passes that clear depth at their start, how that clear was done.
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

  // One kind of compute dispatch (driver fex16, slots with T::SlotDispatch): the same shader (its stock binary's
  // instruction count), workgroup size, and binary (stock, or atomics once per wave). Workgroup counts are summed; the
  // last grid is kept, with the smallest and biggest workgroup totals seen.
  struct DispatchKind {
    uint32_t Instr;
    uint32_t Local; // Slot::Format0
    bool Alt;
    uint16_t Gx, Gy;
    uint8_t Gz;
    uint64_t GroupsMin, GroupsMax;
    uint64_t Dispatches;
    uint64_t Groups;
    uint64_t Ticks;
    uint64_t Values[T::MaxCounters];
  };
  constexpr size_t MaxDispatchKinds = 16;
  constexpr size_t PrintDispatchKinds = 4;

  // Wave cycles (resident wave x cycle) by state: issuing float math (alu), special math (efu), integer math (int),
  // csp, idle issue slots the compiler put in (nop), waiting for an earlier instruction's result (wait), waiting for
  // texture/memory results (long-sync) or short results (short-sync), instruction fetch (fetch), flow control
  // (ctrl), loading inputs (load), sending outputs (emit), ending (end), join, input, output, half-wave wait/sync
  // (build 103), ready but another wave issued (idle). With 4+ states counted, 'rest' is what none of them covers.
  constexpr struct {
    uint32_t Countable;
    const char* Name;
  } WaveSplit[] = {{89, "alu"},   {90, "efu"},       {91, "int"},        {92, "csp"},   {19, "nop"},
                   {20, "wait"},  {24, "long-sync"}, {25, "short-sync"}, {21, "fetch"}, {16, "ctrl"},
                   {17, "load"},  {18, "emit"},      {23, "end"},        {26, "join"},  {104, "input"},
                   {105, "output"}, {106, "half-wave wait"}, {107, "half-wave sync"}, {22, "idle"}};

  // PassKind::Mode bits: T::ModeChangedOnly, T::ModeSwitch (fex11: pipeline switches sent only what changed),
  // T::ModeNoAlias (fex12: pipelines used the fragment shader binaries without alias.tex), and for
  // direct passes with a depth clear T::SlotDepthClear plus T::ModeDepthClear3d when the 3D draw did it and
  // T::ModeDepthClearFastZ when that draw ran without fragment shader.
  uint8_t KindMode(const T::Slot& S) {
    uint8_t Mode = S.Mode & (T::ModeChangedOnly | T::ModeSwitch | T::ModeNoAlias);
    if ((S.Sysmem & 1) && (S.Mode & T::SlotDepthClear)) {
      Mode |= T::SlotDepthClear | (S.Mode & T::ModeDepthClear3d);
      if (S.Mode & T::ModeDepthClear3d) {
        Mode |= S.Mode & T::ModeDepthClearFastZ;
      }
    }
    return Mode;
  }

  // FEX_TU_WAVE_PAIR asked for wave setup 4 (the 128-pixel binary), or FEX_TU_DRAW_SETUP for setup 4 (the binary with no
  // preamble at all): the second binary's mode bit follows the rotation.
  bool ArmOwnsAlt() {
    return FEX::Windows::Turnip::WavePairArms() & (1u << T::FsArmW128);
  }
  bool DrawSetup() {
    return FEX::Windows::Turnip::DrawSetupArms();
  }

  // Name of the second lighting shader binary (fex12: without alias.tex, fex13: without preamble, fex14: 128-pixel waves)
  // for the per-second line, and the short tag of the pass kinds ('na', 'np', or 'alt' with both).
  const char* AltName() {
    const bool Na = FEX::Windows::Turnip::NoAliasSwitch() != 0, Np = FEX::Windows::Turnip::NoPreambleSwitch() >= 2;
    if (ArmOwnsAlt() && DrawSetup()) {
      return Na ? "no preamble at all, no alias" : "no preamble at all";
    }
    if (ArmOwnsAlt()) {
      return Na && Np ? "128-pixel waves, no alias, no preamble" : Np ? "128-pixel waves, no preamble" : Na ? "128-pixel waves, no alias" :
                                                                                                           "128-pixel waves";
    }
    return Na && Np ? "no alias, no preamble" : Np ? "no preamble" : "no alias";
  }

  // Wave setup arm (fex14) or draw setup arm (fex15) of a pass kind or a mode, by name.
  const char* ArmTag(uint32_t Arm) {
    static constexpr const char* Tags[] = {"stock", "pair1", "pair2", "pair3", "w128", "merge", "defer", "evict"};
    return (Arm & 7) == T::FsArmAlt && DrawSetup() ? "nopre" : Tags[Arm & 7];
  }
  const char* AltTag() {
    const bool Na = FEX::Windows::Turnip::NoAliasSwitch() != 0, Np = FEX::Windows::Turnip::NoPreambleSwitch() >= 2;
    return Na && Np ? "alt" : Np ? "np" : "na";
  }

  T::Channel* Chan {};
  bool NotedDriver {};
  bool NotedMissing {};
  uint32_t Ticks {};
  uint32_t Seconds {};
  uint32_t CurrentMode {};
  uint64_t NextSeq {1};
  uint64_t Last[24][2] {};
  uint32_t NaShadersLast {};   // NaShaders at the last '# turnip: no alias' note
  uint32_t NaRefusedLast {};
  uint64_t BudgetLast[2] {}; // BudgetQueries, BudgetNs at the last report
  uint32_t BudgetLastReads {};
  uint32_t LoopPrefetchLast[2] {}; // LoopPrefetchLoads, LoopPrefetchShaders at the last report
  uint64_t ArmBindsLast[T::FsArms] {}; // ArmBinds at the last report (fex14)
  uint32_t SetupLast[4] {};            // NopreShaders, MergeShaders, CntlShaders, SetupRefused at the last note (fex15)
  uint32_t CsAltLast[2] {};            // CsAltShaders, CsAltRefused at the last note (fex16)

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
    const char* UboTable = (Status & T::StatusUboTable)               ? "ON" :
                           !(Chan->Compile & T::CompileUboTable) ? "off (FEX_TU_UBO_TABLE=0)" :
                           !(Status & T::StatusCpUbo)            ? "off (needs the command processor loads above)" :
                           Chan->Patch < 10                      ? "NOT available (driver older than fex10)" :
                                                                   "NOT honored";
    const uint32_t PipeSwitch = FEX::Windows::Turnip::PipelineSwitchSwitch();
    const char* Switches = !PipeSwitch                         ? "off (FEX_TU_SWITCH=0)" :
                           Chan->Patch < 11                    ? "NOT available (driver older than fex11)" :
                           !(Status & T::StatusSwitch)         ? "NOT honored (needs an a7xx GPU like the a740)" :
                           !(Status & T::StatusSwitchStats)    ? (PipeSwitch == 2 ? "in every other second (FEX_TU_SWITCH=alt), counts NOT written" :
                                                                                    "ON, counts NOT written") :
                           PipeSwitch == 2                     ? "in every other second (FEX_TU_SWITCH=alt)" :
                                                                 "ON (FEX_TU_SWITCH=1)";
    const uint32_t NoAliasSw = FEX::Windows::Turnip::NoAliasSwitch();
    const char* NoAlias = !NoAliasSw                          ? "off (FEX_TU_NOALIAS=0)" :
                          Chan->Patch < 12                    ? "NOT available (driver older than fex12)" :
                          !(Status & T::StatusNoAlias)        ? "NOT honored (needs an a7xx GPU like the a740)" :
                          !(Status & T::StatusNoAliasStats)   ? (NoAliasSw == 2 ? "in every other second (FEX_TU_NOALIAS=alt), counts NOT written" :
                                                                                  "ON, counts NOT written") :
                          NoAliasSw == 2                      ? "in every other second (FEX_TU_NOALIAS=alt)" :
                                                                "ON (FEX_TU_NOALIAS=1)";
    const uint32_t NoPreSw = FEX::Windows::Turnip::NoPreambleSwitch();
    const char* NoPre = NoPreSw == 1                       ? "for every shader by IR3_SHADER_DEBUG (FEX_TU_NOPREAMBLE=1)" :
                        NoPreSw == 0                       ? "off (FEX_TU_NOPREAMBLE=0)" :
                        Chan->Patch < 13                   ? "NOT available (driver older than fex13)" :
                        !(Status & T::StatusNoPreamble)    ? "NOT honored (needs an a7xx GPU that loads constants with the command "
                                                             "processor, like the a740)" :
                        !(Status & T::StatusNoAliasStats)  ? (NoPreSw == 2 ? "in every other second (FEX_TU_NOPREAMBLE=alt), counts NOT written" :
                                                                             "ON (FEX_TU_NOPREAMBLE=lit), counts NOT written") :
                        NoPreSw == 2                       ? "in every other second (FEX_TU_NOPREAMBLE=alt)" :
                                                             "ON (FEX_TU_NOPREAMBLE=lit)";
    // fex14: wave setups of the big fragment shaders (FEX_TU_WAVE_PAIR); fex15: draw setups (FEX_TU_DRAW_SETUP).
    const uint32_t Arms = FEX::Windows::Turnip::WavePairArms();
    fextl::string WaveSetups;
    if (!Arms) {
      WaveSetups = "stock (FEX_TU_WAVE_PAIR=0, FEX_TU_DRAW_SETUP=0)";
    } else if (Chan->Patch < 14 || (DrawSetup() && Chan->Patch < 15)) {
      WaveSetups = DrawSetup() ? "NOT available (driver older than fex15)" : "NOT available (driver older than fex14)";
    } else {
      fextl::string List;
      uint32_t N = 0;
      for (uint32_t a = 0; a < T::FsArms; ++a) {
        if (!(Arms & (1u << a))) {
          continue;
        }
        const uint32_t Need = a == 0                                ? 0 :
                              a < T::FsArmAlt                       ? T::StatusPair :
                              a == T::FsArmAlt                      ? (DrawSetup() ? T::StatusNoPreAll : T::StatusW128) :
                              a == T::FsArmMerge                    ? T::StatusMerge :
                                                                      T::StatusCntl;
        const bool Ok = !Need || (Status & Need) != 0;
        List += fextl::fmt::format("{}{}{}", N++ ? ", " : "", ArmTag(a), Ok ? "" : " (NOT honored)");
      }
      const uint32_t Stats = DrawSetup() ? (T::StatusArmStats | T::StatusSetupStats) : T::StatusArmStats;
      WaveSetups = fextl::fmt::format("{} {} ({}){}", N > 1 ? "rotating a second each:" : "always", List,
                                      DrawSetup() ? "FEX_TU_DRAW_SETUP" : "FEX_TU_WAVE_PAIR",
                                      (Status & Stats) == Stats ? "" : ", counts NOT written");
    }
    // fex16: compute shaders with atomics once per wave (FEX_TU_CS_ATOMICS), and the dispatch slots.
    const uint32_t CsSw = FEX::Windows::Turnip::CsAtomicsSwitch();
    const char* CsAtomics = !CsSw                               ? "off (FEX_TU_CS_ATOMICS=0)" :
                            Chan->Patch < 16                    ? "NOT available (driver older than fex16)" :
                            !(Status & T::StatusCsAtomics)      ? "NOT honored (needs a GPU with getfiberid, like the a740)" :
                            !(Status & T::StatusCsStats)        ? (CsSw == 2 ? "in every other second (FEX_TU_CS_ATOMICS=alt), counts NOT written" :
                                                                               "ON (FEX_TU_CS_ATOMICS=1), counts NOT written") :
                            CsSw == 2                           ? "in every other second (FEX_TU_CS_ATOMICS=alt)" :
                                                                  "ON (FEX_TU_CS_ATOMICS=1)";
    const char* Dispatches = Chan->Patch < 16                   ? "not timed (driver older than fex16)" :
                             !(Status & T::StatusCsStats)       ? "not timed (counts not written)" :
                             !Chan->SampleMinDraws              ? "not timed (FEX_TU_SAMPLE_DRAWS=0)" :
                                                                  "direct ones of 16+ workgroups timed and counted";
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
      "preamble loads first {}, list loops read ahead {}, root constant buffers loaded by the command processor {}, other root "
      "constant buffer reads through the shaders' UBO table {}, pipeline switches send only what changed {}, lighting shaders "
      "without alias.tex helpers {}, lighting shaders without preamble {}, lighting shader wave/draw setups {}, compute atomics once "
      "per wave {}, compute dispatches {}, depth clears at the start of direct passes {}, available memory for the memory budget {}, "
      "{} of {} GPU counters reserved{}{}; ring {}x{} bytes; counters: {}",
      Driver, Chan->Patch, Status, (Status & T::StatusDescPatch) ? "honored" : "NOT honored",
      (Status & T::StatusNotA7xx) ? " (not an a7xx GPU)" : "", EarlyPreamble, PreambleLoads, LoopPrefetch, CpUbo, UboTable, Switches, NoAlias, NoPre,
      WaveSetups, CsAtomics, Dispatches, DepthClear,
      Budget,
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
  fextl::string KindLine(const PassKind& K, uint32_t Count, bool DescSeen, bool SwitchSeen, bool NoAliasSeen, bool WaveSeen) {
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
    // fex14: the wave setup arm the pass was recorded with (stock, pair1-3, w128); with setup 4 asked for, the second
    // binary's bit is part of it.
    fextl::string Alt;
    if (WaveSeen) {
      Alt = fextl::fmt::format(" {}", ArmTag(K.Sysmem >> 4));
    }
    if (NoAliasSeen && !(WaveSeen && ArmOwnsAlt())) {
      Alt += fextl::fmt::format(" {} {}", AltTag(), (K.Mode & T::ModeNoAlias) ? "on" : "off");
    }
    fextl::string Line = fextl::fmt::format("  turnip pass {}x{} c{}/a{} fmt{} {}{}{}{}{}: {} passes, {} draws, gpu {:.1f} ms ({:.2f} us/draw)",
                                            K.Width, K.Height, K.Colors, K.Attachments, K.Format0, (K.Sysmem & 1) ? "direct" : "tiled",
                                            DescSeen ? ((K.Mode & T::ModeChangedOnly) ? " desc on" : " desc off") : "",
                                            SwitchSeen ? ((K.Mode & T::ModeSwitch) ? " sw on" : " sw off") : "",
                                            Alt,
                                            Clear,
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
    } else if (Has(T::GroupSp, 11)) {
      // Build 102: the wave life samples count every fragment wave.
      Line += fextl::fmt::format(" | fs waves/draw {:.1f}", V(T::GroupSp, 11) / Draws);
    }
    if (Has(T::GroupSp, 101)) {
      Line += fextl::fmt::format(", px/draw {:.0f}", V(T::GroupSp, 101) / Draws);
    }
    if (Has(T::GroupSp, 10) && Has(T::GroupSp, 11)) {
      Line += fextl::fmt::format(", wave life {:.0f} cyc", Ratio(V(T::GroupSp, 10), V(T::GroupSp, 11)));
    }
    // Wave cycles by state (WaveSplit).
    const double Waves = V(T::GroupSp, 8);
    if (Waves > 0) {
      fextl::string Parts;
      double Sum = 0;
      int Counted = 0;
      for (const auto& P : WaveSplit) {
        if (Has(T::GroupSp, P.Countable)) {
          const double Pct = 100.0 * V(T::GroupSp, P.Countable) / Waves;
          Parts += fextl::fmt::format(" {} {:.0f}%", P.Name, Pct);
          Sum += Pct;
          ++Counted;
        }
      }
      if (Counted >= 4) {
        Parts += fextl::fmt::format(" rest {:.0f}%", 100.0 - Sum);
      }
      if (!Parts.empty()) {
        Line += " | wave cycles" + Parts;
      }
    }
    // Texture path (build 102): texture instructions per fragment wave, how long a texture fetch takes on average and
    // how many of those a wave's life is, shader cores unable to send more texture requests and texture units busy
    // (both summed over the 6 cores, so 6.00 = all of them all the time), the texture L1 cache's line misses, the L2
    // cache's misses for texture requests (2 memory beats of 32 bytes per 64-byte request missed, Mesa's formula) and
    // the texture data read from memory per pass. Textures read compressed (UBWC) go through the decompressor and are
    // not in the L2 numbers.
    {
      fextl::string Parts;
      const auto Add = [&](const fextl::string& Part) {
        Parts += (Parts.empty() ? "" : ", ") + Part;
      };
      const double FsWaves = V(T::GroupSp, 11);
      if (Has(T::GroupSp, 37) && FsWaves > 0) {
        Add(fextl::fmt::format("tex instructions/wave {:.0f}", V(T::GroupSp, 37) / FsWaves));
      }
      if (Has(T::GroupTp, 2) && Has(T::GroupTp, 3) && V(T::GroupTp, 3) > 0) {
        const double Latency = V(T::GroupTp, 2) / V(T::GroupTp, 3);
        Add(fextl::fmt::format("fetch latency {:.0f} cyc", Latency));
        if (Has(T::GroupSp, 10) && FsWaves > 0 && Latency > 0) {
          Add(fextl::fmt::format("wave life = {:.1f} fetch latencies", V(T::GroupSp, 10) / FsWaves / Latency));
        }
      }
      if (Cycles > 0 && Has(T::GroupSp, 4)) {
        Add(fextl::fmt::format("cores can't send {:.2f}", V(T::GroupSp, 4) / Cycles));
      }
      if (Cycles > 0 && Has(T::GroupTp, 0)) {
        Add(fextl::fmt::format("tp busy {:.2f}", V(T::GroupTp, 0) / Cycles));
      }
      if (Has(T::GroupTp, 6) && Has(T::GroupTp, 7) && V(T::GroupTp, 6) > 0) {
        Add(fextl::fmt::format("L1 miss {:.1f}% of {:.0f} lines/draw", 100.0 * V(T::GroupTp, 7) / V(T::GroupTp, 6), V(T::GroupTp, 6) / Draws));
      }
      if (Has(T::GroupUche, 9) && V(T::GroupUche, 9) > 0) {
        if (Has(T::GroupUche, 4)) {
          Add(fextl::fmt::format("L2 miss {:.1f}% of {:.0f} requests/draw", 100.0 * 2.0 * V(T::GroupUche, 4) / V(T::GroupUche, 9),
                                 V(T::GroupUche, 9) / Draws));
        }
      }
      if (Has(T::GroupUche, 4) && K.Passes) {
        Add(fextl::fmt::format("memory {:.2f} MB/pass", V(T::GroupUche, 4) * 32.0 / 1e6 / static_cast<double>(K.Passes)));
      }
      if (!Parts.empty()) {
        Line += " | texture: " + Parts;
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
                      {T::GroupHlsq, 11, "fs data wait"},     {T::GroupHlsq, 3, "sp fs stage stall"},
                      {T::GroupHlsq, 39, "fs starving sp"},   {T::GroupCp, 15, "cp idle"},         {T::GroupCp, 22, "cp pipe stall"}};
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
    if (Has(T::GroupCp, 2) || Has(T::GroupCp, 46) || Has(T::GroupCp, 23)) {
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
        // Small per-draw values (sampled counters like the fetch latency's transactions) keep three decimals.
        const double PerDraw = static_cast<double>(K.Values[i]) / Draws;
        Line += PerDraw < 10.0 ? fextl::fmt::format(" {} {:.3f}", CounterLabel(C.Group, C.Countable), PerDraw) :
                                 fextl::fmt::format(" {} {:.1f}", CounterLabel(C.Group, C.Countable), PerDraw);
      }
    }
    return Line + '\n';
  }

  // One compute dispatch kind's line (build 107, driver fex16): time per dispatch, then the shader cores (busy, waves
  // per busy core, starved by the front end that starts waves), per workgroup (wave cycles = one wave's life for
  // 64-thread groups, threads, compute instructions), the wave states, memory per dispatch (atomics, stores, address
  // locks), stalls per cycle, texture latency, then every counter per dispatch.
  fextl::string DispatchLine(const DispatchKind& K, uint32_t Count) {
    const double Ms = K.Ticks / 19200.0;
    const double N = K.Dispatches ? static_cast<double>(K.Dispatches) : 1.0;
    const double Groups = K.Groups ? static_cast<double>(K.Groups) : 1.0;
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
    const uint32_t Lx = (K.Local & 0x3ff) + 1, Ly = ((K.Local >> 10) & 0x3ff) + 1, Lz = ((K.Local >> 20) & 0x3ff) + 1;
    const fextl::string Grid = K.GroupsMin == K.GroupsMax ?
                                 fextl::fmt::format("{}x{}x{}", K.Gx, K.Gy, K.Gz) :
                                 fextl::fmt::format("{:.0f} avg ({}-{}, last {}x{}x{})", static_cast<double>(K.Groups) / N, K.GroupsMin,
                                                    K.GroupsMax, K.Gx, K.Gy, K.Gz);
    fextl::string Line = fextl::fmt::format("  turnip dispatch cs{} {}x{}x{} {}: {} dispatches, groups {}, gpu {:.2f} ms ({:.1f} us/dispatch)",
                                            K.Instr, Lx, Ly, Lz, K.Alt ? "once per wave" : "stock", K.Dispatches, Grid, Ms,
                                            1000.0 * Ms / N);
    const double Cycles = V(T::GroupCp, 0);
    if (Cycles > 0 && K.Ticks) {
      Line += fextl::fmt::format(" at {:.0f} MHz", Cycles / (K.Ticks / 19.2));
    }
    if (!Count) {
      return Line + '\n';
    }
    const double Busy = V(T::GroupSp, 0);
    if (Cycles > 0 && Has(T::GroupSp, 0)) {
      Line += fextl::fmt::format(" | cores busy {:.2f}", Busy / Cycles);
      if (Has(T::GroupSp, 8)) {
        Line += fextl::fmt::format(", waves per busy core {:.2f}", Ratio(V(T::GroupSp, 8), Busy));
      }
      if (Has(T::GroupSp, 69)) {
        Line += fextl::fmt::format(", cores starved by hlsq {:.2f}", V(T::GroupSp, 69) / Cycles);
      }
    }
    {
      fextl::string Parts;
      if (Has(T::GroupSp, 8)) {
        Parts += fextl::fmt::format("wave cycles {:.0f}", V(T::GroupSp, 8) / Groups);
      }
      if (Has(T::GroupSp, 100)) {
        Parts += fextl::fmt::format("{}threads {:.1f}", Parts.empty() ? "" : ", ", V(T::GroupSp, 100) / Groups);
      }
      if (Has(T::GroupSp, 56)) {
        Parts += fextl::fmt::format("{}cs instructions {:.0f}", Parts.empty() ? "" : ", ", V(T::GroupSp, 56) / Groups);
      }
      if (!Parts.empty()) {
        Line += " | per group: " + Parts;
      }
    }
    const double Waves = V(T::GroupSp, 8);
    if (Waves > 0) {
      fextl::string Parts;
      double Sum = 0;
      int Counted = 0;
      for (const auto& P : WaveSplit) {
        if (Has(T::GroupSp, P.Countable)) {
          const double Pct = 100.0 * V(T::GroupSp, P.Countable) / Waves;
          Parts += fextl::fmt::format(" {} {:.0f}%", P.Name, Pct);
          Sum += Pct;
          ++Counted;
        }
      }
      if (Counted >= 4) {
        Parts += fextl::fmt::format(" rest {:.0f}%", 100.0 - Sum);
      }
      if (!Parts.empty()) {
        Line += " | wave cycles" + Parts;
      }
    }
    {
      static constexpr struct {
        uint32_t Group, Countable;
        const char* Name;
      } Mem[] = {{T::GroupSp, 32, "atomics"}, {T::GroupSp, 31, "stores"}, {T::GroupSp, 45, "address locks"},
                 {T::GroupUche, 15, "l2 write requests"}};
      fextl::string Parts;
      for (const auto& P : Mem) {
        if (Has(P.Group, P.Countable)) {
          Parts += fextl::fmt::format("{}{} {:.0f}", Parts.empty() ? "" : ", ", P.Name, V(P.Group, P.Countable) / N);
        }
      }
      if (!Parts.empty()) {
        Line += " | memory per dispatch: " + Parts;
      }
    }
    if (Cycles > 0) {
      static constexpr struct {
        uint32_t Group, Countable;
        const char* Name;
      } PerCycle[] = {{T::GroupSp, 1, "alu working"},        {T::GroupSp, 2, "efu working"}, {T::GroupSp, 77, "eu working cs"},
                      {T::GroupSp, 5, "cores stalled on l2"}, {T::GroupSp, 4, "cores stalled on tp"},
                      {T::GroupHlsq, 0, "hlsq busy"},         {T::GroupHlsq, 1, "hlsq stalled on l2"}};
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
    if (Has(T::GroupSp, 119) && Has(T::GroupSp, 120)) {
      Line += fextl::fmt::format(" | tex latency {:.0f} cyc", Ratio(V(T::GroupSp, 119), V(T::GroupSp, 120)));
    }
    Line += "; per dispatch:";
    for (uint32_t i = 0; i < Count; ++i) {
      const auto& C = Chan->Counters[i];
      if (C.Reg) {
        const double Per = static_cast<double>(K.Values[i]) / N;
        Line += Per < 10.0 ? fextl::fmt::format(" {} {:.3f}", CounterLabel(C.Group, C.Countable), Per) :
                             fextl::fmt::format(" {} {:.0f}", CounterLabel(C.Group, C.Countable), Per);
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
    const uint32_t PipeSw = FEX::Windows::Turnip::PipelineSwitchSwitch();
    const uint32_t NoAliasSw = FEX::Windows::Turnip::NoAliasSwitch();
    const uint32_t NoPreSw = FEX::Windows::Turnip::NoPreambleSwitch();
    // Build 104: the second lighting shader binary (fex12 without alias.tex, fex13 without preamble) shares one mode bit.
    // Build 105: with wave setup 4 (the 128-pixel binary) asked for, the wave setup rotation owns that bit.
    const uint32_t Arms = FEX::Windows::Turnip::WavePairArms();
    const uint32_t ArmCount = static_cast<uint32_t>(__builtin_popcount(Arms));
    const bool ArmRotate = ArmCount > 1;
    const bool AltFlip = (NoAliasSw == 2 || NoPreSw == 2) && !ArmOwnsAlt();
    const uint32_t CsSw = FEX::Windows::Turnip::CsAtomicsSwitch();
    if (DescSw == 2 || ClearSw == 2 || PipeSw == 2 || AltFlip || CsSw == 2 || ArmRotate) {
      ++Seconds;
      uint32_t Mode = CurrentMode;
      // Each alternating switch takes the next bit of the second count, so every combination gets seconds of its own.
      uint32_t Bit = 0;
      if (DescSw == 2) {
        const bool On = (Seconds >> Bit++) & 1;
        Mode = (Mode & ~(T::ModeChangedOnly | T::ModePushReuse)) | (On ? (T::ModeChangedOnly | T::ModePushReuse) : 0);
      }
      if (ClearSw == 2) {
        // Build 74: the 3D draw stays on, its fast Z (no fragment shader) flips.
        const bool On = (Seconds >> Bit++) & 1;
        Mode = (Mode & ~T::ModeDepthClearFastZ) | T::ModeDepthClear3d | (On ? T::ModeDepthClearFastZ : 0);
      }
      if (PipeSw == 2) {
        // Build 100: pipeline switches that send only what changed (driver fex11), on in odd seconds.
        const bool On = (Seconds >> Bit++) & 1;
        Mode = (Mode & ~T::ModeSwitch) | (On ? T::ModeSwitch : 0);
      }
      if (AltFlip) {
        // Build 101: pipelines use the lighting shader binaries without alias.tex (driver fex12) in odd seconds; build 104
        // the ones without preamble (driver fex13).
        const bool On = (Seconds >> Bit++) & 1;
        Mode = (Mode & ~T::ModeNoAlias) | (On ? T::ModeNoAlias : 0);
      }
      if (CsSw == 2) {
        // Build 107: compute dispatches use the binaries with atomics once per wave (driver fex16) in odd seconds.
        const bool On = (Seconds >> Bit++) & 1;
        Mode = (Mode & ~T::ModeCsAlt) | (On ? T::ModeCsAlt : 0);
      }
      if (ArmRotate) {
        // Build 105: the wave setups asked for (FEX_TU_WAVE_PAIR), build 106 the draw setups (FEX_TU_DRAW_SETUP), the
        // next one each second (after the flips above, so each of their combinations meets every setup). Setup 4's
        // second binary (128-pixel waves, or no preamble at all) also takes the second binary's mode bit.
        uint32_t Index = (Seconds >> Bit) % ArmCount;
        uint32_t Arm = 0;
        for (uint32_t a = 0; a < T::FsArms; ++a) {
          if ((Arms & (1u << a)) && Index-- == 0) {
            Arm = a;
            break;
          }
        }
        Mode = (Mode & ~T::ModeFsArmMask) | (Arm << T::ModeFsArmShift);
        if (ArmOwnsAlt()) {
          Mode = (Mode & ~T::ModeAltFs) | (Arm == T::FsArmW128 ? T::ModeAltFs : 0);
        }
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

    // Graphics pipeline binds as recorded this second (fex11 and later, counted at vkEndCommandBuffer): draw state blocks
    // per bind the command processor runs (new address or dirty) or skips (kept at the address it already had), and
    // with the switch on, the vertex/fragment stages reset because their shader changed or left alone, binds that took
    // the stock path, and blocks re-made at draws (dynamic state, depth plane) run or kept.
    const uint32_t PipeSwNow = FEX::Windows::Turnip::PipelineSwitchSwitch();
    const bool SwitchSeen = PipeSwNow != 0 && Chan->Patch >= 11 && (Status & T::StatusSwitch);
    if (Chan->Patch >= 11 && (Status & T::StatusSwitchStats)) {
      uint64_t Binds[2], Run[2], Kept[2], DrawGroups[2], Stages[2], Stock[2];
      Delta(9, Chan->SwBinds, Binds);
      Delta(10, Chan->SwGroupsRun, Run);
      Delta(11, Chan->SwGroupsKept, Kept);
      Delta(12, Chan->SwDrawGroups, DrawGroups);
      const uint64_t StagesNow[2] = {Chan->SwStageResets, Chan->SwStagesKept};
      Delta(13, StagesNow, Stages);
      const uint64_t StockNow[2] = {Chan->SwStock, 0};
      Delta(14, StockNow, Stock);
      const auto PerBind = [&](const uint64_t* V, int i) {
        return Binds[i] ? fextl::fmt::format("{:.1f}", static_cast<double>(V[i]) / Binds[i]) : fextl::string("-");
      };
      if (Binds[0] || Binds[1] || DrawGroups[0] || DrawGroups[1]) {
        Out += fextl::fmt::format("  turnip: pipeline switch {} this second | binds off {} on {} | blocks run per bind off {} on {} | kept "
                                  "per bind off {} on {} | stages reset {} kept {} | stock binds {} | blocks re-made at draws run {} kept {}\n",
                                  (ModeThisSecond & T::ModeSwitch) ? "on" : "off", Binds[0], Binds[1], PerBind(Run, 0), PerBind(Run, 1),
                                  PerBind(Kept, 0), PerBind(Kept, 1), Stages[0], Stages[1], Stock[0], DrawGroups[0], DrawGroups[1]);
      }
    }

    // Lighting shaders without alias.tex (fex12 and later): binds of pipelines whose fragment shader has the second binary,
    // with the stock binary or the one without alias.tex, as recorded this second; the mode of this second decides which
    // binary the game's command buffers used, so whole-frame GPU times can be split by it too. A note gives the shaders'
    // totals for both binaries whenever new ones were made (compiled, or loaded from the driver's cache).
    const bool NoAliasSeen = (NoAliasSw != 0 && Chan->Patch >= 12 && (Status & T::StatusNoAlias)) ||
                             (NoPreSw >= 2 && Chan->Patch >= 13 && (Status & T::StatusNoPreamble)) ||
                             (ArmOwnsAlt() && !DrawSetup() && Chan->Patch >= 14 && (Status & T::StatusW128)) ||
                             (ArmOwnsAlt() && DrawSetup() && Chan->Patch >= 15 && (Status & T::StatusNoPreAll));
    // Wave setups (fex14 and later) and draw setups (fex15 and later): pipeline binds whose fragment shader has them, by
    // the setup of the command buffer.
    const bool WaveSeen = Arms != 0 && Chan->Patch >= 14 &&
                          (Status & (T::StatusPair | T::StatusW128 | T::StatusNoPreAll | T::StatusMerge | T::StatusCntl));
    if (WaveSeen && DrawSetup() && (Status & T::StatusArmStats)) {
      // Build 106: the draw setups (FEX_TU_DRAW_SETUP): binds per setup this second, then this run's shaders that got each
      // setup's binary or states, the merged binaries' driver-loaded ranges and vec4s (stock -> merged), binaries
      // compiled but dropped, and pipelines that can or cannot use the merged binary.
      uint64_t ArmBinds[T::FsArms];
      bool Any = false;
      for (uint32_t a = 0; a < T::FsArms; ++a) {
        const uint64_t V = Chan->ArmBinds[a];
        ArmBinds[a] = V - ArmBindsLast[a];
        ArmBindsLast[a] = V;
        Any |= ArmBinds[a] != 0;
      }
      if (Any || ArmRotate) {
        fextl::string Shaders;
        if (Status & T::StatusSetupStats) {
          Shaders = fextl::fmt::format(" | binaries without preamble {}, merged loads {} (ranges {} -> {}, vec4 {} -> {}), alloc states {}, "
                                       "refused {} | merge pipelines can use {} cannot {}",
                                       Chan->NopreShaders, Chan->MergeShaders, Chan->MergeRanges[0], Chan->MergeRanges[1], Chan->MergeVec4[0],
                                       Chan->MergeVec4[1], Chan->CntlShaders, Chan->SetupRefused, Chan->MergePipes[0], Chan->MergePipes[1]);
        }
        Out += fextl::fmt::format("  turnip: draw setup {} this second | binds stock {} nopre {} merge {} defer {} evict {}{}\n",
                                  ArmTag((ModeThisSecond & T::ModeFsArmMask) >> T::ModeFsArmShift), ArmBinds[0], ArmBinds[T::FsArmAlt],
                                  ArmBinds[T::FsArmMerge], ArmBinds[T::FsArmDefer], ArmBinds[T::FsArmEvict], Shaders);
      }
      if (Status & T::StatusSetupStats) {
        const uint32_t Now[4] = {Chan->NopreShaders, Chan->MergeShaders, Chan->CntlShaders, Chan->SetupRefused};
        if (memcmp(Now, SetupLast, sizeof(Now)) != 0) {
          memcpy(SetupLast, Now, sizeof(Now));
          const uint32_t R0 = Chan->MergeRanges[0], R1 = Chan->MergeRanges[1], V0 = Chan->MergeVec4[0], V1 = Chan->MergeVec4[1];
          FEX::Windows::StatsLogNote(fextl::fmt::format(
            "turnip: draw setups: {} lighting shaders have a binary without preamble (push constants by the command processor), {} a "
            "binary with merged constant loads (driver-loaded ranges {} -> {}, {} -> {} vec4: {:.1f} -> {:.1f} loads per shader), {} "
            "the alloc setting states; {} binaries dropped; pipelines that can use the merged binary {}, that cannot {}",
            Now[0], Now[1], R0, R1, V0, V1, Now[1] ? static_cast<double>(R0) / Now[1] : 0.0, Now[1] ? static_cast<double>(R1) / Now[1] : 0.0,
            Now[2], Now[3], Chan->MergePipes[0], Chan->MergePipes[1]));
        }
      }
    } else if (WaveSeen && (Status & T::StatusArmStats)) {
      uint64_t ArmBinds[T::FsArms];
      bool Any = false;
      for (uint32_t a = 0; a < T::FsArms; ++a) {
        const uint64_t V = Chan->ArmBinds[a];
        ArmBinds[a] = V - ArmBindsLast[a];
        ArmBindsLast[a] = V;
        Any |= ArmBinds[a] != 0;
      }
      if (Any || ArmRotate) {
        Out += fextl::fmt::format("  turnip: wave setup {} this second | binds stock {} pair1 {} pair2 {} pair3 {} w128 {} | shaders with "
                                  "pair states {}{}\n",
                                  ArmTag((ModeThisSecond & T::ModeFsArmMask) >> T::ModeFsArmShift), ArmBinds[0], ArmBinds[1], ArmBinds[2],
                                  ArmBinds[3], ArmBinds[4], Chan->PairShaders,
                                  ArmOwnsAlt() ? fextl::fmt::format(" | 128-pixel binaries {} refused {}", Chan->NaShaders, Chan->NaRefused) :
                                                 fextl::string());
      }
    }
    if (Chan->Patch >= 12 && (Status & T::StatusNoAliasStats)) {
      uint64_t NaBinds[2];
      Delta(15, Chan->NaBinds, NaBinds);
      const uint32_t Shaders = Chan->NaShaders, Refused = Chan->NaRefused;
      const uint32_t Pipes[2] = {Chan->NaPipes[0], Chan->NaPipes[1]};
      if (NoAliasSeen || NaBinds[0] || NaBinds[1]) {
        Out += fextl::fmt::format("  turnip: {} {} this second | binds stock {} second binary {} | shaders {} refused {} | pipelines "
                                  "can use {} cannot {}\n",
                                  AltName(), (ModeThisSecond & T::ModeNoAlias) ? "on" : "off", NaBinds[0], NaBinds[1], Shaders, Refused,
                                  Pipes[0], Pipes[1]);
      }
      if (Shaders != NaShadersLast || Refused != NaRefusedLast) {
        NaShadersLast = Shaders;
        NaRefusedLast = Refused;
        const auto Pct = [](uint32_t A, uint32_t B) {
          return B ? 100.0 * (static_cast<double>(A) - static_cast<double>(B)) / static_cast<double>(B) : 0.0;
        };
        uint32_t V[7][2];
        const volatile uint32_t* Src[7] = {Chan->NaInstr, Chan->NaNops, Chan->NaAlias, Chan->NaMovs, Chan->NaRegs, Chan->NaSsStall, Chan->NaSyStall};
        for (int i = 0; i < 7; ++i) {
          V[i][0] = Src[i][0];
          V[i][1] = Src[i][1];
        }
        // fex13: preamble instructions of both binaries.
        const auto Preamble = Chan->Patch >= 13 ? fextl::fmt::format(", preamble instructions {} -> {}", Chan->NaPreamble[0], Chan->NaPreamble[1]) :
                                                  fextl::string();
        FEX::Windows::StatsLogNote(fextl::fmt::format(
          "turnip: {}: {} fragment shaders have a second binary ({} refused), totals stock -> second: "
          "instructions {} -> {} ({:+.1f}%){}, nops {} -> {}, alias.tex {} -> {}, movs {} -> {}, registers {} -> {}, estimated "
          "(ss) stalls {} -> {}, (sy) stalls {} -> {}; pipelines that can use them {}, that cannot (reduced constants) {}",
          AltName(), Shaders, Refused, V[0][0], V[0][1], Pct(V[0][1], V[0][0]), Preamble, V[1][0], V[1][1], V[2][0], V[2][1], V[3][0],
          V[3][1], V[4][0], V[4][1], V[5][0], V[5][1], V[6][0], V[6][1], Pipes[0], Pipes[1]));
      }
    }

    // Compute atomics once per wave (fex16 and later): binds of compute pipelines whose shader has the second binary, with
    // the stock binary or that one, as recorded this second, this run's shaders with it, and dispatches that got a slot.
    // A note gives both binaries' totals whenever new ones were made.
    if (Chan->Patch >= 16 && (Status & T::StatusCsStats)) {
      uint64_t CsBinds[2], Sampled[2];
      Delta(16, Chan->CsBinds, CsBinds);
      const uint64_t SampledNow[2] = {Chan->DispatchesSampled, 0};
      Delta(17, SampledNow, Sampled);
      const uint32_t Shaders = Chan->CsAltShaders, Refused = Chan->CsAltRefused;
      if (CsSw != 0 || CsBinds[0] || CsBinds[1] || Sampled[0]) {
        Out += fextl::fmt::format("  turnip: compute atomics once per wave {} this second | binds stock {} once per wave {} | shaders {} "
                                  "refused {} | dispatches sampled {}\n",
                                  (ModeThisSecond & T::ModeCsAlt) ? "on" : "off", CsBinds[0], CsBinds[1], Shaders, Refused, Sampled[0]);
      }
      if (Shaders != CsAltLast[0] || Refused != CsAltLast[1]) {
        CsAltLast[0] = Shaders;
        CsAltLast[1] = Refused;
        const uint32_t I0 = Chan->CsAltInstr[0], I1 = Chan->CsAltInstr[1];
        FEX::Windows::StatsLogNote(fextl::fmt::format(
          "turnip: compute atomics once per wave: {} compute shaders have a second binary ({} atomics made once per wave), "
          "instructions stock -> second {} -> {} ({:+.1f}%); {} dropped (other layout or workgroup size)",
          Shaders, Chan->CsAltAtomics, I0, I1, I0 ? 100.0 * (static_cast<double>(I1) - static_cast<double>(I0)) / I0 : 0.0, Refused));
      }
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
    DispatchKind DKinds[MaxDispatchKinds];
    size_t DKindCount = 0;
    DispatchKind DOther {};
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
      const uint64_t* Values = reinterpret_cast<const uint64_t*>(Buffer + sizeof(T::Slot));
      if (S.Attachments == T::SlotDispatch && Chan->Patch >= 16) {
        // Build 107: a compute dispatch (driver fex16).
        const bool Alt = (S.Sysmem & 2) != 0;
        const uint64_t G = static_cast<uint64_t>(S.Width) * S.Height * S.Colors;
        DispatchKind* D = nullptr;
        for (size_t i = 0; i < DKindCount; ++i) {
          if (DKinds[i].Instr == S.Draws && DKinds[i].Local == S.Format0 && DKinds[i].Alt == Alt) {
            D = &DKinds[i];
            break;
          }
        }
        if (!D) {
          D = DKindCount < MaxDispatchKinds ? &DKinds[DKindCount++] : &DOther;
          if (D != &DOther) {
            *D = {};
            D->Instr = S.Draws;
            D->Local = S.Format0;
            D->Alt = Alt;
            D->GroupsMin = G;
          }
        }
        D->Gx = S.Width;
        D->Gy = S.Height;
        D->Gz = S.Colors;
        D->GroupsMin = std::min(D->GroupsMin, G);
        D->GroupsMax = std::max(D->GroupsMax, G);
        D->Dispatches++;
        D->Groups += G;
        D->Ticks += S.TsEnd - S.TsBegin;
        for (uint32_t i = 0; i < Count; ++i) {
          if (Chan->Counters[i].Reg) {
            D->Values[i] += Values[Count + i] - Values[i];
          }
        }
        continue;
      }
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
      Out += KindLine(*Order[i], Count, DescSeen, SwitchSeen, NoAliasSeen, WaveSeen);
    }
    if (Other.Passes) {
      Out += fextl::fmt::format("  turnip pass other: {} passes, {} draws, gpu {:.1f} ms\n", Other.Passes, Other.Draws, Other.Ticks / 19200.0);
    }

    // Compute dispatch kinds (build 107), the ones with the most GPU time first, and all of them summed.
    if (DKindCount || DOther.Dispatches) {
      DispatchKind* DOrder[MaxDispatchKinds];
      uint64_t AllDispatches = DOther.Dispatches, AllTicks = DOther.Ticks;
      for (size_t i = 0; i < DKindCount; ++i) {
        DOrder[i] = &DKinds[i];
        AllDispatches += DKinds[i].Dispatches;
        AllTicks += DKinds[i].Ticks;
      }
      for (size_t i = 1; i < DKindCount; ++i) {
        for (size_t j = i; j > 0 && DOrder[j]->Ticks > DOrder[j - 1]->Ticks; --j) {
          std::swap(DOrder[j], DOrder[j - 1]);
        }
      }
      uint64_t RestDispatches = DOther.Dispatches, RestTicks = DOther.Ticks;
      for (size_t i = PrintDispatchKinds; i < DKindCount; ++i) {
        RestDispatches += DOrder[i]->Dispatches;
        RestTicks += DOrder[i]->Ticks;
      }
      Out += fextl::fmt::format("  turnip dispatches: {} sampled, gpu {:.2f} ms ({} kinds)", AllDispatches, AllTicks / 19200.0, DKindCount);
      if (RestDispatches) {
        Out += fextl::fmt::format(" | not listed below: {} dispatches, {:.2f} ms", RestDispatches, RestTicks / 19200.0);
      }
      Out += '\n';
      for (size_t i = 0; i < DKindCount && i < PrintDispatchKinds; ++i) {
        Out += DispatchLine(*DOrder[i], Count);
      }
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
