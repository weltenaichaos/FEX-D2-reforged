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
  // ZeroWhenOff: with the switch set to 0 (by the user, or by the app's defaults), set the variable to 0 instead of leaving
  // it unset (for a VKD3D feature that is on unless 0). A program without the switch (no default for it, e.g. Steam)
  // leaves the variable alone: build 78 set 0 there too, and D2R, started from it, inherited the 0 and left it as it was.
  template<size_t N>
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
    if (!On && ZeroWhenOff && Value) {
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
  // FEX_TU_NOPREAMBLE: 0 off; 1 IR3_SHADER_DEBUG=nopreamble for every shader (below); alt (D2R default in build 104) =
  // the driver (fex13 and later) compiles the big fragment shaders (D2R's lighting shaders) a second time without
  // preamble and pipelines use those binaries in every other second (TurnipReport flips the mode bit, so without the
  // stats log it stays on); lit = those binaries always. Build 103: lighting waves spend 25-30% of their life in the
  // HWAVE_WAIT state, which no other pass shows; the lighting shaders' 137-instruction early preamble runs once per draw
  // before that draw's waves may start.
  uint32_t TurnipNoPreamble {};
  uint32_t ParseNoPreamble() {
    const char* Value = Switch("FEX_TU_NOPREAMBLE");
    if (!Value || (Value[0] == '0' && !Value[1])) {
      return 0;
    }
    if (Value[0] == 'a' || Value[0] == 'A' || (Value[0] == '2' && !Value[1])) {
      return 2;
    }
    if (Value[0] == 'l' || Value[0] == 'L' || (Value[0] == '3' && !Value[1])) {
      return 3;
    }
    return 1;
  }

  void SetupTurnipNoPreamble(FARPROC SetUnixEnv) {
    TurnipNoPreamble = ParseNoPreamble();
    const bool On = TurnipNoPreamble == 1;
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
      StatsLogNote(fextl::fmt::format("turnip: IR3_SHADER_DEBUG={} left alone ({}): the driver builds its per-draw shader "
                                      "preambles{}",
                                      Before,
                                      TurnipNoPreamble == 2 ? "FEX_TU_NOPREAMBLE=alt" :
                                      TurnipNoPreamble == 3 ? "FEX_TU_NOPREAMBLE=lit" :
                                                              "FEX_TU_NOPREAMBLE off",
                                      TurnipNoPreamble >= 2 ? ", and through FEX's link a second binary of the lighting shaders "
                                                              "without preamble (see the FEX link note)" :
                                                              ""));
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
  uint32_t TurnipSwitch {};
  uint32_t TurnipNoAlias {};

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
    // fex11 and later: 'Size' stays the base layout (older drivers check it), 'ExtSize' says the block holds the
    // extension the driver writes its pipeline switch counts (fex11) and no-alias counts (fex12) into.
    Chan->Size = Turnip::ChannelBaseSize;
    Chan->ExtSize = sizeof(Turnip::Channel);

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
    // FEX_TU_SWITCH: 1 = a graphics pipeline switch only sends what changed (driver fex11 and later), alt (D2R default in
    // build 100) = in every other second, so one run compares both; 0 = stock. Build 99: each switch cost ~1.5 us of GPU
    // time in the shadow passes and ~3.5 us in the depth prepass (31-44% of their draws switch, ~900 switches a frame in
    // fights); stock Turnip runs ~16 state blocks again at every switch, most of them byte-identical, and resets every
    // shader stage. Like FEX_TU_DESC, only TurnipReport flips it, so without the stats log alt stays on.
    const char* PipeSwitch = Switch("FEX_TU_SWITCH");
    TurnipSwitch = SwitchOff(PipeSwitch) ? 0 : (PipeSwitch[0] == 'a' || PipeSwitch[0] == 'A' || PipeSwitch[0] == '2') ? 2 : 1;
    // FEX_TU_NOALIAS: alt (D2R default in build 101) = the driver (fex12 and later) also compiles big fragment shaders
    // (D2R's lighting shaders) without alias.tex helper instructions, and pipelines use those binaries in every other
    // second, so one run compares both; 1 = always those binaries; 0 = stock (no second compile). Build 100: the main pass
    // costs about one lighting shader wave per draw, so a shorter shader should shorten it nearly 1:1; the lighting
    // shaders are ~7,000 instructions, 17% of them alias.tex (each its own instruction), and without them 11-12% shorter
    // (with more movs and 32 instead of 24 registers). Like FEX_TU_DESC, only TurnipReport flips it, so without the
    // stats log alt stays on.
    const char* NoAlias = Switch("FEX_TU_NOALIAS");
    TurnipNoAlias = SwitchOff(NoAlias) ? 0 : (NoAlias[0] == 'a' || NoAlias[0] == 'A' || NoAlias[0] == '2') ? 2 : 1;
    // FEX_TU_NOPREAMBLE alt/lit (SetupTurnipNoPreamble): the second binary without preamble, picked with the same mode bit.
    Chan->Mode = (TurnipDesc ? (Turnip::ModeChangedOnly | Turnip::ModePushReuse) : 0) |
                 (TurnipDepthClear ? Turnip::ModeDepthClear3d : 0) |
                 (TurnipDepthClear >= 2 ? Turnip::ModeDepthClearFastZ : 0) | (TurnipBudgetCache ? Turnip::ModeBudgetCache : 0) |
                 (TurnipSwitch ? Turnip::ModeSwitch : 0) |
                 ((TurnipNoAlias || TurnipNoPreamble >= 2) ? Turnip::ModeAltFs : 0);

    // FEX_TU_COUNTERS: 1 (D2R default in build 102) = the list below, 0 = none. FEX_TU_SAMPLE_DRAWS: render passes with at
    // least this many draws get timed and counted (D2R default 100 in build 102: the main pass, the depth prepasses and
    // the shadow maps in towns and fights).
    // Build 103's list, every wave state at once. Build 102 found the lighting pass limited by one lighting wave's own
    // start-to-finish time (the GPU keeps about two draws in flight, so each draw costs ~4,700 GPU cycles plus 54 per
    // wave), and the wave spending 15% on math, 3% on idle issue slots, 27% waiting for texture/memory data and ~40% in
    // states that list didn't count. The texture counters are dropped (102 has them: 40% L1 misses, 25% L2 misses);
    // these are all the shader core's wave state counters, so the shares add up and the biggest state is named.
    if (!SwitchOff(Switch("FEX_TU_COUNTERS"))) {
      static constexpr struct {
        uint32_t Group, Countable;
      } List[] = {
        {Turnip::GroupCp, 0}, // CP_ALWAYS_COUNT (GPU cycles, shared with KGSL)
        // Shader cores: busy, resident waves (integral over cycles), fragment wave lives.
        {Turnip::GroupSp, 0},  // SP_BUSY_CYCLES
        {Turnip::GroupSp, 8},  // SP_WAVE_CONTEXTS
        {Turnip::GroupSp, 10}, // SP_STAGE_WAVE_CYCLES (fragment wave lives)
        {Turnip::GroupSp, 11}, // SP_STAGE_WAVE_SAMPLES (fragment waves)
        // Issuing, by unit.
        {Turnip::GroupSp, 89}, // SP_WAVE_ALU_CYCLES (float math)
        {Turnip::GroupSp, 90}, // SP_WAVE_EFU_CYCLES (special math: square roots, reciprocals, powers, sines)
        {Turnip::GroupSp, 91}, // SP_WAVE_INT_CYCLES (integer math)
        {Turnip::GroupSp, 92}, // SP_WAVE_CSP_CYCLES
        {Turnip::GroupSp, 19}, // SP_WAVE_NOP_CYCLES (idle issue slots the compiler put in)
        // Waiting and the rest of a wave's life.
        {Turnip::GroupSp, 20},  // SP_WAVE_WAIT_CYCLES
        {Turnip::GroupSp, 21},  // SP_WAVE_FETCH_CYCLES (instruction fetch)
        {Turnip::GroupSp, 22},  // SP_WAVE_IDLE_CYCLES
        {Turnip::GroupSp, 24},  // SP_WAVE_LONG_SYNC_CYCLES (texture/memory results)
        {Turnip::GroupSp, 25},  // SP_WAVE_SHORT_SYNC_CYCLES
        {Turnip::GroupSp, 16},  // SP_WAVE_CTRL_CYCLES
        {Turnip::GroupSp, 17},  // SP_WAVE_LOAD_CYCLES
        {Turnip::GroupSp, 18},  // SP_WAVE_EMIT_CYCLES
        {Turnip::GroupSp, 23},  // SP_WAVE_END_CYCLES
        {Turnip::GroupSp, 26},  // SP_WAVE_JOIN_CYCLES
        {Turnip::GroupSp, 104}, // SP_WAVE_INPUT_CYCLES
        {Turnip::GroupSp, 105}, // SP_WAVE_OUTPUT_CYCLES
        {Turnip::GroupSp, 106}, // SP_WAVE_HWAVE_WAIT_CYCLES
        {Turnip::GroupSp, 107}, // SP_WAVE_HWAVE_SYNC
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
    // FEX_TU_PREAMBLE_LOADS: 1 (default) = preambles issue their memory loads first and store each value once (driver
    // fex6 and later); 0 = stock order.
    if (!SwitchOff(Switch("FEX_TU_PREAMBLE_LOADS"))) {
      Chan->Compile |= Turnip::CompilePreambleLoads;
    }
    // FEX_TU_LOOP_PREFETCH: 1 (default) = fragment shader loops that walk a list read the next entry one iteration
    // ahead (driver fex7 and later); 0 = stock.
    if (!SwitchOff(Switch("FEX_TU_LOOP_PREFETCH"))) {
      Chan->Compile |= Turnip::CompileLoopPrefetch;
    }
    // FEX_TU_CP_UBO: 1 (default) = the command processor loads root constant buffer ranges into the shaders' constants
    // before each draw, and draws that then don't read the root set through descriptors keep the descriptor set state
    // (driver fex9 and later); 0 = stock.
    if (!SwitchOff(Switch("FEX_TU_CP_UBO"))) {
      Chan->Compile |= Turnip::CompileCpUbo;
    }
    // FEX_TU_UBO_TABLE: 1 (default) = root constant buffers that shaders still read from memory (indexed bone matrices)
    // go through the shader's own UBO table, so those draws keep the descriptor set state too (driver fex10 and later,
    // only with FEX_TU_CP_UBO); 0 = they read through the root set's descriptor as before.
    if (!SwitchOff(Switch("FEX_TU_UBO_TABLE"))) {
      Chan->Compile |= Turnip::CompileUboTable;
    }
    // FEX_TU_NOALIAS (above): the second binaries are compiled only when asked for (part of the driver's shader keys, so
    // turning it on or off makes the driver compile its shaders anew once).
    if (TurnipNoAlias) {
      Chan->Compile |= Turnip::CompileNoAliasAlt;
    }
    // FEX_TU_NOPREAMBLE alt/lit: the same for the second binary without preamble (driver fex13 and later).
    if (TurnipNoPreamble >= 2) {
      Chan->Compile |= Turnip::CompileNoPreambleAlt;
    }

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
                                    "buffers {}, preamble loads first {}, list loops read ahead {}, root constant buffers loaded by the "
                                    "command processor {}, other root constant buffer reads through the shaders' UBO table {}, pipeline "
                                    "switches send only what changed {}, lighting shaders without alias.tex helpers {}, lighting "
                                    "shaders without preamble {}, depth clears at pass start {}, "
                                    "memory budget {}, {} GPU counters asked for, passes with {}+ draws timed and counted; only FEX's "
                                    "Turnip build (fex1 and later) answers, a '# turnip: driver' note follows once it did",
                                    Text, TurnipDesc == 2 ? "alternating each second (FEX_TU_DESC=alt)" : TurnipDesc ? "on (FEX_TU_DESC=1)" : "off (FEX_TU_DESC=0)",
                                    (Chan->Compile & Turnip::CompilePushSpeculate) ? "asked for (FEX_TU_EARLY_PREAMBLE=1, driver fex2 and later)" :
                                                                                     "off (FEX_TU_EARLY_PREAMBLE=0)",
                                    (Chan->Compile & Turnip::CompilePreambleLoads) ? "asked for (FEX_TU_PREAMBLE_LOADS=1, driver fex6 and later)" :
                                                                                     "off (FEX_TU_PREAMBLE_LOADS=0)",
                                    (Chan->Compile & Turnip::CompileLoopPrefetch) ? "asked for (FEX_TU_LOOP_PREFETCH=1, driver fex7 and later)" :
                                                                                    "off (FEX_TU_LOOP_PREFETCH=0)",
                                    (Chan->Compile & Turnip::CompileCpUbo) ? "asked for (FEX_TU_CP_UBO=1, driver fex9 and later)" :
                                                                             "off (FEX_TU_CP_UBO=0)",
                                    !(Chan->Compile & Turnip::CompileUboTable) ? "off (FEX_TU_UBO_TABLE=0)" :
                                    !(Chan->Compile & Turnip::CompileCpUbo)    ? "off (needs FEX_TU_CP_UBO=1)" :
                                                                                 "asked for (FEX_TU_UBO_TABLE=1, driver fex10 and later)",
                                    TurnipSwitch == 2 ? "in every other second (FEX_TU_SWITCH=alt, driver fex11 and later)" :
                                    TurnipSwitch      ? "on (FEX_TU_SWITCH=1, driver fex11 and later)" :
                                                        "off (FEX_TU_SWITCH=0)",
                                    TurnipNoAlias == 2 ? "in every other second (FEX_TU_NOALIAS=alt, driver fex12 and later; one-time "
                                                         "shader recompile)" :
                                    TurnipNoAlias      ? "always (FEX_TU_NOALIAS=1, driver fex12 and later; one-time shader recompile)" :
                                                         "off (FEX_TU_NOALIAS=0)",
                                    TurnipNoPreamble == 2 ? "in every other second (FEX_TU_NOPREAMBLE=alt, driver fex13 and later; "
                                                            "one-time shader recompile)" :
                                    TurnipNoPreamble == 3 ? "always (FEX_TU_NOPREAMBLE=lit, driver fex13 and later; one-time shader "
                                                            "recompile)" :
                                    TurnipNoPreamble == 1 ? "for every shader by IR3_SHADER_DEBUG (FEX_TU_NOPREAMBLE=1)" :
                                                            "off (FEX_TU_NOPREAMBLE=0)",
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
  SetupVkd3dFexSwitch("FEX_VKD3D_MEMORY_AHEAD", L"VKD3D_FEX_MEMORY_AHEAD",
                      "new GPU memory of 4-64 MB for the game's heaps comes from spares a background thread made before "
                      "the game asked, instead of the kernel zeroing it on the game's thread (VKD3D fex17 and later; 'gpu "
                      "memory ahead' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_CBV_SIZE", L"VKD3D_FEX_CBV_SIZE",
                      "a constant buffer a shader indexes with a register keeps the size the shader declares (up to 256 "
                      "vectors) instead of 64 KiB, so the driver can load it into the shader's constants; with Turnip fex9 "
                      "D2R's lighting shaders then stop reading the root constant buffer set through descriptors (VKD3D "
                      "fex29 and later; part of vkd3d's shader cache key)");
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
  uint32_t PipelineSwitchSwitch() {
    return TurnipSwitch;
  }
  uint32_t NoAliasSwitch() {
    return TurnipNoAlias;
  }
  uint32_t NoPreambleSwitch() {
    return TurnipNoPreamble;
  }
} // namespace Turnip
} // namespace FEX::Windows
