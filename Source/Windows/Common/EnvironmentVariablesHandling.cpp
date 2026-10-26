// SPDX-License-Identifier: MIT
#include "Windows/Common/EnvironmentVariablesHandling.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"
#include "Windows/Common/TurnipChannel.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <iterator>
#include <libloaderapi.h>
#include <stdlib.h>
#include <winternl.h>
#include <wine/unixlib.h>

extern "C" {
NTSTATUS WINAPI RtlQueryEnvironmentVariable_U(PWSTR Environment, PUNICODE_STRING Name, PUNICODE_STRING Value);
NTSTATUS WINAPI RtlSetEnvironmentVariable(PWSTR* Environment, PUNICODE_STRING Name, PUNICODE_STRING Value);
}

namespace FEX::Windows {
namespace {
  constexpr NTSTATUS StatusVariableNotFound = static_cast<NTSTATUS>(0xC0000100);

  fextl::string Narrow(const wchar_t* Text, size_t Len) {
    fextl::string Out;
    for (size_t i = 0; i < Len; ++i) {
      Out += Text[i] < 0x80 ? static_cast<char>(Text[i]) : '?';
    }
    return Out;
  }

  // Whether the comma/semicolon/space separated list holds Flag as a whole entry, as vkd3d-proton parses it.
  bool HasFlag(const wchar_t* List, size_t Len, const wchar_t* Flag, size_t FlagLen) {
    size_t Start = 0;
    for (size_t i = 0; i <= Len; ++i) {
      if (i == Len || List[i] == L',' || List[i] == L';' || List[i] == L' ' || List[i] == L'\t') {
        if (i - Start == FlagLen) {
          size_t j = 0;
          while (j < FlagLen && List[Start + j] == Flag[j]) {
            ++j;
          }
          if (j == FlagLen) {
            return true;
          }
        }
        Start = i + 1;
      }
    }
    return false;
  }

  // See EnvironmentVariablesHandling.h. vkd3d-proton reads VKD3D_CONFIG with GetEnvironmentVariableA when the game
  // creates its D3D12 device, long after this runs, so changing the process environment here is enough.
  void SetupVkd3dConfig() {
    const char* NoStagger = Switch("FEX_VKD3D_NOSTAGGER");
    if (!NoStagger || (NoStagger[0] == '0' && !NoStagger[1])) {
      return;
    }

    static constexpr wchar_t NameText[] = L"VKD3D_CONFIG";
    static constexpr wchar_t Flag[] = L"no_staggered_submit";
    constexpr size_t FlagLen = std::size(Flag) - 1;
    UNICODE_STRING Name {sizeof(NameText) - sizeof(wchar_t), sizeof(NameText), const_cast<wchar_t*>(NameText)};

    wchar_t Value[512];
    UNICODE_STRING Old {0, static_cast<USHORT>(sizeof(Value) - (FlagLen + 2) * sizeof(wchar_t)), Value};
    const NTSTATUS QueryStatus = RtlQueryEnvironmentVariable_U(nullptr, &Name, &Old);
    size_t Len = 0;
    if (QueryStatus == 0) {
      Len = Old.Length / sizeof(wchar_t);
    } else if (QueryStatus != StatusVariableNotFound) {
      StatsLogNote(fextl::fmt::format("vkd3d: VKD3D_CONFIG can't be read (status {:#x}), left alone; staggered submits stay on",
                                      static_cast<uint32_t>(QueryStatus)));
      return;
    }
    const auto Before = Len ? Narrow(Value, Len) : fextl::string("-");
    if (HasFlag(Value, Len, Flag, FlagLen)) {
      StatsLogNote(fextl::fmt::format("vkd3d: VKD3D_CONFIG={} already turns staggered submits off", Before));
      return;
    }

    if (Len) {
      Value[Len++] = L',';
    }
    for (size_t i = 0; i < FlagLen; ++i) {
      Value[Len++] = Flag[i];
    }
    Value[Len] = 0;
    UNICODE_STRING New {static_cast<USHORT>(Len * sizeof(wchar_t)), static_cast<USHORT>((Len + 1) * sizeof(wchar_t)), Value};
    const NTSTATUS SetStatus = RtlSetEnvironmentVariable(nullptr, &Name, &New);
    StatsLogNote(fextl::fmt::format("vkd3d: VKD3D_CONFIG={} -> {}, status {:#x}: vkd3d-proton submits each frame's GPU work in one go "
                                    "and leaves cross-queue waits to the GPU, instead of its queue thread waiting for the GPU "
                                    "between batches (FEX_VKD3D_NOSTAGGER=0 leaves VKD3D_CONFIG alone)",
                                    Before, Narrow(Value, Len), static_cast<uint32_t>(SetStatus)));
  }

  // See EnvironmentVariablesHandling.h: turns on a feature of FEX's vkd3d-proton build by setting Var=1, unless the
  // variable is already set.
  template<size_t N>
  // ZeroWhenOff: with the switch off, set the variable to 0 instead of leaving it unset (for a VKD3D feature that is on
  // unless 0).
  void SetupVkd3dFexSwitch(const char* SwitchName, const wchar_t (&VarText)[N], const char* What, bool ZeroWhenOff = false) {
    const char* Value = Switch(SwitchName);
    const bool On = Value && !(Value[0] == '0' && !Value[1]);
    const auto Var = Narrow(VarText, N - 1);
    UNICODE_STRING Name {sizeof(VarText) - sizeof(wchar_t), sizeof(VarText), const_cast<wchar_t*>(VarText)};

    wchar_t Old[32];
    UNICODE_STRING OldValue {0, sizeof(Old) - sizeof(wchar_t), Old};
    const NTSTATUS QueryStatus = RtlQueryEnvironmentVariable_U(nullptr, &Name, &OldValue);
    if (QueryStatus != StatusVariableNotFound) {
      StatsLogNote(fextl::fmt::format("vkd3d: {}={} was already set, left alone ({})", Var,
                                      QueryStatus == 0 ? Narrow(Old, OldValue.Length / sizeof(wchar_t)) : fextl::string("?"), What));
      return;
    }
    if (!On && ZeroWhenOff) {
      static constexpr wchar_t ZeroText[] = L"0";
      UNICODE_STRING Zero {sizeof(wchar_t), sizeof(ZeroText), const_cast<wchar_t*>(ZeroText)};
      const NTSTATUS SetStatus = RtlSetEnvironmentVariable(nullptr, &Name, &Zero);
      StatsLogNote(fextl::fmt::format("vkd3d: {}=0 (status {:#x}, {} off): {} stays off", Var, static_cast<uint32_t>(SetStatus),
                                      SwitchName, What));
      return;
    }
    if (!On) {
      StatsLogNote(fextl::fmt::format("vkd3d: {} not set ({} off): {} stays off", Var, SwitchName, What));
      return;
    }

    static constexpr wchar_t OneText[] = L"1";
    UNICODE_STRING One {sizeof(wchar_t), sizeof(OneText), const_cast<wchar_t*>(OneText)};
    const NTSTATUS SetStatus = RtlSetEnvironmentVariable(nullptr, &Name, &One);
    StatsLogNote(fextl::fmt::format("vkd3d: {}=1 (status {:#x}): {} (only FEX's VKD3D build reads it; {}=0 turns it off)", Var,
                                    static_cast<uint32_t>(SetStatus), What, SwitchName));
  }

  // See EnvironmentVariablesHandling.h: hands the switch's list of pixel shader hashes to FEX's vkd3d-proton build.
  void SetupVkd3dShaderCopy() {
    const char* Value = Switch("FEX_VKD3D_SHADER_COPY");
    static constexpr wchar_t VarText[] = L"VKD3D_FEX_SHADER_COPY";
    UNICODE_STRING Name {sizeof(VarText) - sizeof(wchar_t), sizeof(VarText), const_cast<wchar_t*>(VarText)};

    wchar_t Old[512];
    UNICODE_STRING OldValue {0, sizeof(Old) - sizeof(wchar_t), Old};
    const NTSTATUS QueryStatus = RtlQueryEnvironmentVariable_U(nullptr, &Name, &OldValue);
    if (QueryStatus != StatusVariableNotFound) {
      StatsLogNote(fextl::fmt::format("vkd3d: VKD3D_FEX_SHADER_COPY={} was already set, left alone",
                                      QueryStatus == 0 ? Narrow(Old, OldValue.Length / sizeof(wchar_t)) : fextl::string("?")));
      return;
    }
    if (!Value || (Value[0] == '0' && !Value[1])) {
      StatsLogNote("vkd3d: VKD3D_FEX_SHADER_COPY not set (FEX_VKD3D_SHADER_COPY off): no shaders are copied into the log");
      return;
    }

    // Only hex digits and separators reach vkd3d.
    wchar_t Text[512];
    size_t Len = 0;
    size_t Hashes = 0;
    bool InHash = false;
    for (const char* p = Value; *p && Len + 1 < std::size(Text); ++p) {
      const char c = *p;
      const bool Hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!Hex && c != ',' && c != ';' && c != ' ') {
        StatsLogNote(fextl::fmt::format("vkd3d: FEX_VKD3D_SHADER_COPY={} isn't a list of hex shader hashes: no shaders are copied", Value));
        return;
      }
      Hashes += Hex && !InHash;
      InHash = Hex;
      Text[Len++] = static_cast<wchar_t>(c);
    }
    Text[Len] = 0;
    UNICODE_STRING New {static_cast<USHORT>(Len * sizeof(wchar_t)), static_cast<USHORT>((Len + 1) * sizeof(wchar_t)), Text};
    const NTSTATUS SetStatus = RtlSetEnvironmentVariable(nullptr, &Name, &New);
    StatsLogNote(fextl::fmt::format("vkd3d: VKD3D_FEX_SHADER_COPY={} (status {:#x}): the D3D bytecode, PSO and root signature of these {} "
                                    "pixel shaders, and for the first four the SPIR-V, the other stages and the driver's compiled "
                                    "code, go into this log as 'shader copy' blocks; rendering is unchanged (VKD3D fex13 and later; "
                                    "FEX_VKD3D_SHADER_COPY=0 turns it off)",
                                    Narrow(Text, Len), static_cast<uint32_t>(SetStatus), Hashes));
  }
  // See EnvironmentVariablesHandling.h: Turnip's shader compiler reads IR3_SHADER_DEBUG from the Unix environment once,
  // when the game's Vulkan device is created, long after this runs. Wine copied the Unix environment into the process
  // environment at start, so a value set by the launcher can be read here and is kept.
  void SetupTurnipNoPreamble(FARPROC SetUnixEnv) {
    const char* Value = Switch("FEX_TU_NOPREAMBLE");
    const bool On = Value && !(Value[0] == '0' && !Value[1]);
    static constexpr wchar_t NameText[] = L"IR3_SHADER_DEBUG";
    static constexpr wchar_t Flag[] = L"nopreamble";
    constexpr size_t FlagLen = std::size(Flag) - 1;
    UNICODE_STRING Name {sizeof(NameText) - sizeof(wchar_t), sizeof(NameText), const_cast<wchar_t*>(NameText)};

    wchar_t Old[256];
    UNICODE_STRING OldValue {0, static_cast<USHORT>(sizeof(Old) - (FlagLen + 2) * sizeof(wchar_t)), Old};
    const NTSTATUS QueryStatus = RtlQueryEnvironmentVariable_U(nullptr, &Name, &OldValue);
    size_t Len = QueryStatus == 0 ? OldValue.Length / sizeof(wchar_t) : 0;
    const auto Before = Len ? Narrow(Old, Len) : fextl::string("-");

    if (!On) {
      StatsLogNote(fextl::fmt::format("turnip: IR3_SHADER_DEBUG={} left alone (FEX_TU_NOPREAMBLE off): the driver builds its "
                                      "per-draw shader preambles",
                                      Before));
      return;
    }
    if (QueryStatus != 0 && QueryStatus != StatusVariableNotFound) {
      StatsLogNote(fextl::fmt::format("turnip: IR3_SHADER_DEBUG can't be read (status {:#x}), left alone: the driver builds its "
                                      "per-draw shader preambles",
                                      static_cast<uint32_t>(QueryStatus)));
      return;
    }
    if (HasFlag(Old, Len, Flag, FlagLen)) {
      StatsLogNote(fextl::fmt::format("turnip: IR3_SHADER_DEBUG={} already turns the shader preambles off", Before));
      return;
    }
    if (!SetUnixEnv) {
      StatsLogNote("turnip: this Wine has no __wine_set_unix_env, IR3_SHADER_DEBUG can't be set: the driver builds its per-draw "
                   "shader preambles");
      return;
    }

    if (Len) {
      Old[Len++] = L',';
    }
    for (size_t i = 0; i < FlagLen; ++i) {
      Old[Len++] = Flag[i];
    }
    Old[Len] = 0;
    const auto After = Narrow(Old, Len);
    auto WineSetEnv = reinterpret_cast<decltype(__wine_set_unix_env)>(SetUnixEnv);
    const NTSTATUS SetStatus = WineSetEnv("IR3_SHADER_DEBUG", After.c_str());
    UNICODE_STRING New {static_cast<USHORT>(Len * sizeof(wchar_t)), static_cast<USHORT>((Len + 1) * sizeof(wchar_t)), Old};
    RtlSetEnvironmentVariable(nullptr, &Name, &New);
    StatsLogNote(fextl::fmt::format("turnip: IR3_SHADER_DEBUG={} -> {} (status {:#x}): Turnip's shader compiler builds no long "
                                    "per-draw preamble, the one-thread setup step that runs before any pixel of a draw is "
                                    "shaded (6-14 memory reads one after another in the main lighting shaders); that work "
                                    "moves into the shader and the constant uploads stay in a short preamble. The driver "
                                    "compiles its shaders anew once (its cache key changes); the 'shader copy' statistics "
                                    "show the new preamble size. FEX_TU_NOPREAMBLE=0 turns it off",
                                    Before, After, static_cast<uint32_t>(SetStatus)));
  }

  Turnip::Channel* TurnipChannelPtr {};
  uint32_t TurnipDesc {};
  uint32_t TurnipDepthClear {};
  bool TurnipBudgetCache {};

  bool SwitchOff(const char* Value) {
    return !Value || (Value[0] == '0' && !Value[1]);
  }

  // See EnvironmentVariablesHandling.h and TurnipChannel.h. The driver reads TU_FEX_CHANNEL when the game's Vulkan
  // device is created; a stock Turnip ignores it.
  void SetupTurnipChannel(FARPROC SetUnixEnv) {
    if (SwitchOff(Switch("FEX_TU_CHANNEL"))) {
      StatsLogNote("turnip: no FEX link (FEX_TU_CHANNEL=0): FEX's Turnip build behaves like stock Turnip");
      return;
    }
    if (!SetUnixEnv) {
      StatsLogNote("turnip: this Wine has no __wine_set_unix_env, TU_FEX_CHANNEL can't be set: no FEX link to the driver");
      return;
    }

    void* Base {};
    SIZE_T Size = 0x1000;
    const NTSTATUS AllocStatus = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (AllocStatus != 0 || !Base) {
      StatsLogNote(fextl::fmt::format("turnip: the FEX link's memory can't be allocated (status {:#x})", static_cast<uint32_t>(AllocStatus)));
      return;
    }
    auto* Chan = static_cast<Turnip::Channel*>(Base);
    Chan->Magic = Turnip::Magic;
    Chan->Version = Turnip::Version;
    Chan->Size = sizeof(Turnip::Channel);

    // FEX_TU_DESC: 0 stock, 1 always on, alt (default) = on in every other second (TurnipReport flips it; without the
    // stats log nothing flips it, so it stays on).
    const char* Desc = Switch("FEX_TU_DESC");
    TurnipDesc = SwitchOff(Desc) ? 0 : (Desc[0] == 'a' || Desc[0] == 'A') ? 2 : 1;
    // FEX_TU_DEPTH_CLEAR: 0 stock (2D engine), 1 3D draw (D2R default from build 75), fastz = 3D draw with the fragment
    // shader disabled (driver fex4), alt (D2R default in build 74) = 3D draw always, fast Z in every other second. Build 73
    // measured the 3D draw against the 2D engine (alt then flipped those): ~0.55 ms less per 4096x4096 shadow map; build 74
    // measured fast Z: no gain. Like FEX_TU_DESC, only TurnipReport flips it, so without the stats log alt stays on (with
    // fast Z).
    const char* DepthClear = Switch("FEX_TU_DEPTH_CLEAR");
    TurnipDepthClear = SwitchOff(DepthClear)                              ? 0 :
                       (DepthClear[0] == 'a' || DepthClear[0] == 'A') ? 2 :
                       (DepthClear[0] == 'f' || DepthClear[0] == 'F') ? 3 :
                                                                          1;
    // FEX_TU_BUDGET_CACHE: 1 (default) = the driver's memory budget query reads /proc/meminfo at most every 250 ms (driver
    // fex5 and later); 0 = on every call, as stock. Build 76: D2R's main thread spent 3% of every second of play (~1 ms of
    // every frame) in vkGetPhysicalDeviceMemoryProperties2, which DXGI's QueryVideoMemoryInfo calls each frame for the
    // budget, and stock Turnip reads the whole of /proc/meminfo for every call.
    TurnipBudgetCache = !SwitchOff(Switch("FEX_TU_BUDGET_CACHE"));
    Chan->Mode = (TurnipDesc ? (Turnip::ModeChangedOnly | Turnip::ModePushReuse) : 0) |
                 (TurnipDepthClear ? Turnip::ModeDepthClear3d : 0) |
                 (TurnipDepthClear >= 2 ? Turnip::ModeDepthClearFastZ : 0) | (TurnipBudgetCache ? Turnip::ModeBudgetCache : 0);

    // FEX_TU_COUNTERS: 1 (default) = the list below, 0 = none. FEX_TU_SAMPLE_DRAWS: render passes with at least this many
    // draws get timed and counted (default 100: the main pass, the depth prepass and the shadow maps in towns and
    // fights). Build 74's list: build 73 found primitive control waiting for the command processor's next draw ~43% of
    // the main pass and the command processor's output stalled ~42%, with every draw taking a new state context, so the
    // fixed cost per draw (~6,000 GPU cycles in the main pass, ~2,000 in the shadow maps) sits in the command processor.
    // The command processor counters say where its time per draw goes: executing register writes, state loads
    // (constants, descriptors), draw packets, waiting for its command or draw state data to arrive from memory, or for
    // syncs, and how many packets and dwords a draw takes. Dropped from build 73's list: units that read ~0.8-0.9 busy
    // per cycle in every pass (vertex fetch, triangle setup, rasterizer, depth pre-test), the 2D engine (depth clears
    // are 3D draws now), draw setup starve (always 0) and the wave life/kind/alu/long-sync counters (known).
    if (!SwitchOff(Switch("FEX_TU_COUNTERS"))) {
      static constexpr struct {
        uint32_t Group, Countable;
      } List[] = {
        // Clock and command processor: GPU cycles, how many state contexts the draws used, whether the command
        // processor waits on the GPU (pipe out stall), and (build 74) where its own time goes. A7xx has 14 command
        // processor counters, counter 0 shared with KGSL's always-on count: this list takes the other 13.
        {Turnip::GroupCp, 0},  // CP_ALWAYS_COUNT (GPU cycles, shared with KGSL)
        {Turnip::GroupCp, 11}, // CP_CONTEXT_DONE
        {Turnip::GroupCp, 22}, // CP_SQE_PIPE_OUT_STALL
        {Turnip::GroupCp, 2},  // CP_BUSY_CYCLES
        {Turnip::GroupCp, 16}, // CP_SQE_PM4_STARVE_RB_IB (waiting for command buffer data from memory)
        {Turnip::GroupCp, 17}, // CP_SQE_PM4_STARVE_SDS (waiting for draw state data from memory)
        {Turnip::GroupCp, 23}, // CP_SQE_SYNC_STALL
        {Turnip::GroupCp, 24}, // CP_SQE_PM4_WFI_STALL (wait for idle)
        {Turnip::GroupCp, 26}, // CP_SQE_T4_EXEC (register writes)
        {Turnip::GroupCp, 27}, // CP_SQE_LOAD_STATE_EXEC (constant/descriptor/shader loads)
        {Turnip::GroupCp, 29}, // CP_SQE_DRAW_EXEC (draw packets)
        {Turnip::GroupCp, 30}, // CP_SQE_CTXT_REG_BUNCH_EXEC (register bunches)
        {Turnip::GroupCp, 45}, // CP_PM4_DATA (dwords)
        {Turnip::GroupCp, 46}, // CP_PM4_HEADERS (packets)
        // Shader front end: two pixel shader states active at once (draw overlap), waits.
        {Turnip::GroupHlsq, 11}, // HLSQ_FS_DATA_WAIT_PROGRAMMING
        {Turnip::GroupHlsq, 12}, // HLSQ_DUAL_FS_PROG_ACTIVE
        {Turnip::GroupHlsq, 22}, // HLSQ_FS_STALL_CYCLES
        // Shader cores: occupancy, starvation, pixels (fast Z depth clears run no pixels through them).
        {Turnip::GroupSp, 0},   // SP_BUSY_CYCLES
        {Turnip::GroupSp, 8},   // SP_WAVE_CONTEXTS
        {Turnip::GroupSp, 69},  // SP_STARVE_CYCLES_HLSQ
        {Turnip::GroupSp, 101}, // SP_PIXELS
        // Geometry front end and back end: primitive control waiting for the next draw, render backend.
        {Turnip::GroupPc, 13},  // PC_STARVE_CYCLES_DI (waiting for the command processor's next draw)
        {Turnip::GroupRb, 0},   // RB_BUSY_CYCLES (render backend)
        {Turnip::GroupRb, 37},  // RB_STALL_CYCLES_CCU_DEPTH_WRITE (depth writes waiting for the cache/memory)
      };
      static_assert(std::size(List) <= Turnip::MaxCounters);
      for (const auto& C : List) {
        auto& Out = Chan->Counters[Chan->CounterCount++];
        Out.Group = C.Group;
        Out.Countable = C.Countable;
        Out.Result = 1;
      }
    }
    const char* MinDraws = Switch("FEX_TU_SAMPLE_DRAWS");
    Chan->SampleMinDraws = MinDraws ? static_cast<uint32_t>(strtoul(MinDraws, nullptr, 10)) : 100;
    Chan->RingSlots = 4096;

    // FEX_TU_EARLY_PREAMBLE: 1 (default) = loads through push descriptor sets (vkd3d's root constant buffers) may run
    // in the early preamble, ahead of the draw (driver fex2 and later); 0 = stock.
    Chan->Compile = SwitchOff(Switch("FEX_TU_EARLY_PREAMBLE")) ? 0 : Turnip::CompilePushSpeculate;

    const auto Text = fextl::fmt::format("{:x}", reinterpret_cast<uintptr_t>(Chan));
    auto WineSetEnv = reinterpret_cast<decltype(__wine_set_unix_env)>(SetUnixEnv);
    const NTSTATUS SetStatus = WineSetEnv("TU_FEX_CHANNEL", Text.c_str());
    if (SetStatus != 0) {
      StatsLogNote(fextl::fmt::format("turnip: TU_FEX_CHANNEL can't be set (status {:#x}): no FEX link to the driver",
                                      static_cast<uint32_t>(SetStatus)));
      return;
    }
    TurnipChannelPtr = Chan;
    StatsLogNote(fextl::fmt::format("turnip: FEX link TU_FEX_CHANNEL={} set: descriptor fix {}, early preamble for root constant "
                                    "buffers {}, depth clears at pass start {}, memory budget {}, {} GPU counters asked for, passes "
                                    "with {}+ draws timed and counted; only FEX's Turnip build (fex1 and later) answers, a '# turnip: "
                                    "driver' note follows once it did",
                                    Text, TurnipDesc == 2 ? "alternating each second (FEX_TU_DESC=alt)" : TurnipDesc ? "on (FEX_TU_DESC=1)" : "off (FEX_TU_DESC=0)",
                                    Chan->Compile ? "asked for (FEX_TU_EARLY_PREAMBLE=1, driver fex2 and later)" : "off (FEX_TU_EARLY_PREAMBLE=0)",
                                    TurnipDepthClear == 2 ? "by a 3D draw, without fragment shader (fast Z, driver fex4 and later) "
                                                            "in every other second (FEX_TU_DEPTH_CLEAR=alt)" :
                                    TurnipDepthClear == 3 ? "by a 3D draw without fragment shader (fast Z, FEX_TU_DEPTH_CLEAR=fastz, "
                                                            "driver fex4 and later)" :
                                    TurnipDepthClear      ? "by a 3D draw (FEX_TU_DEPTH_CLEAR=1, driver fex3 and later)" :
                                                            "by the 2D engine as stock (FEX_TU_DEPTH_CLEAR=0)",
                                    TurnipBudgetCache ? "reads /proc/meminfo at most every 250 ms (FEX_TU_BUDGET_CACHE=1, driver fex5 "
                                                        "and later)" :
                                                        "reads /proc/meminfo on every query as stock (FEX_TU_BUDGET_CACHE=0)",
                                    Chan->CounterCount, Chan->SampleMinDraws));
  }
} // namespace

void SetupEnvironmentVariableValues(HMODULE NtDll) {
  SetupVkd3dConfig();
  SetupVkd3dFexSwitch("FEX_VKD3D_FOLD_CLEARS", L"VKD3D_FEX_FOLD_CLEARS",
                      "a full screen clear made between render passes becomes the start of the next pass that draws to "
                      "that image, instead of a pass of its own that writes the whole image to memory for the next pass "
                      "to read back");
  SetupVkd3dFexSwitch("FEX_VKD3D_PASSES", L"VKD3D_FEX_PASSES", "GPU time per render pass on the 'gpu' lines");
  SetupVkd3dFexSwitch("FEX_VKD3D_TIMELINE", L"VKD3D_FEX_TIMELINE",
                      "each frame's way from the game's Present() to the window system, and what vkd3d's queue threads do, on "
                      "the 'present' and 'queue' lines");
  SetupVkd3dFexSwitch("FEX_VKD3D_CAP_PRESLEEP", L"VKD3D_FEX_PRESENT_PRESLEEP",
                      "a frame cap that sleeps inside vkQueuePresentKHR gets its wait slept out before the present, without "
                      "holding the VkQueue the game's compute and copy queues share ('cap' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_FRAME_START", L"VKD3D_FEX_FRAME_START",
                      "the game's next frame starts as late as the measured waiting allows (VKD3D sets the most it holds back: "
                      "20 ms up to fex7, 30 ms from fex8, 50 ms from fex14, VKD3D_FEX_START_MAX_MS; from fex15 below the "
                      "shortest spare time of the last 256 frames, VKD3D_FEX_START_WINDOW; changing by at most 0.2 ms per "
                      "frame up and 1 ms down), so it reads input closer to when the frame is shown ('frame start' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_SKIP_FRESH_CLEARS", L"VKD3D_FEX_SKIP_FRESH_CLEARS",
                      "new GPU memory that vkd3d places in block pages never used since the driver zeroed them skips its "
                      "zero fill (VKD3D fex11 and later; 'gpu memory' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_PRESENT_DRAWN", L"VKD3D_FEX_PRESENT_DRAWN",
                      "each frame goes to the window system only once the GPU has drawn it, because GameNative's X server "
                      "shows an image as soon as it gets it and one handed over earlier showed an older frame (VKD3D fex12 "
                      "and later; 'screen handoff' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_LATENCY_RESYNC", L"VKD3D_FEX_LATENCY_RESYNC",
                      "when the game runs frames ahead of the frame latency it set (it started a frame without taking a "
                      "credit from its frame latency handle, e.g. after its wait timed out in a hitch), vkd3d holds back "
                      "that many hand-outs of the credit, so the game is back at 2 frames in flight (VKD3D fex16 and later; "
                      "'frame credits (resync)' line)",
                      true);
  SetupVkd3dShaderCopy();

  auto Sym = GetProcAddress(NtDll, "__wine_set_unix_env");
  SetupTurnipNoPreamble(Sym);
  SetupTurnipChannel(Sym);

  // On an ARM system we are expecting uncached memory to have marked overhead due to atomic memory usage.
  // If `tu_override_uncached_as_cache_coherent` isn't already controlled by the user, then set it here.
  // This lets the `turnip` mesa driver allocate uncached memory types as cached coherent instead if the hardware supports it.
  //
  // The only downside for this approach is that the GPU /may/ need to snoop CPU cachelines which could add some additional load
  // on to the GPU side, but is unlikely to matter due to x86 emulation using excessive load-acquire/store-release semantic memory operations.
  //
  // Other UMA ARM platforms will run in to the same problem, with their own workarounds:
  // - NVIDIA Tegra/Spark: Their proprietary driver always maps uncached as cached-coherent.
  // - Windows Snapdragon: Same as Tegra
  // - Asahi,Mali,PowerVR,Exynos,etc: Unknown behaviour
  //
  // This `__wine_set_unix_env` path may also not be long-term viable.
  // Make sure to communicate with the Wine/Proton people if this interface is expected to change!
  // A /possible/ workaround for Proton would be to set the environment variable in the launch script.
  // - That would have the downside that arm64 native Windows applications would get workaround set, which isn't strictly necessary.
  if (Sym && getenv("tu_override_uncached_as_cache_coherent") == nullptr) {
    auto WineSetEnv = reinterpret_cast<decltype(__wine_set_unix_env)>(Sym);
    WineSetEnv("tu_override_uncached_as_cache_coherent", "true");
  }
}

namespace Turnip {
  Channel* GetChannel() {
    return TurnipChannelPtr;
  }
  uint32_t DescSwitch() {
    return TurnipDesc;
  }
  uint32_t DepthClearSwitch() {
    return TurnipDepthClear;
  }
  bool BudgetCacheSwitch() {
    return TurnipBudgetCache;
  }
} // namespace Turnip
} // namespace FEX::Windows
