// SPDX-License-Identifier: MIT
#include "Windows/Common/EnvironmentVariablesHandling.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"
#include "Windows/Common/TurnipChannel.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <cstring>
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
  // PassValues: values (separated by '|') that are passed on as they are; any other 'on' value sets 1.
  template<size_t N>
  void SetupVkd3dFexSwitch(const char* SwitchName, const wchar_t (&VarText)[N], const char* What, bool ZeroWhenOff = false,
                           const char* PassValues = nullptr) {
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

    wchar_t SetText[16] = L"1";
    size_t SetLen = 1;
    if (PassValues) {
      const size_t ValueLen = strlen(Value);
      for (const char* p = PassValues; *p;) {
        const char* End = strchr(p, '|');
        const size_t Len = End ? static_cast<size_t>(End - p) : strlen(p);
        if (Len == ValueLen && Len < std::size(SetText) && !strncmp(p, Value, Len)) {
          for (size_t i = 0; i < Len; ++i) {
            SetText[i] = static_cast<wchar_t>(Value[i]);
          }
          SetText[Len] = 0;
          SetLen = Len;
          break;
        }
        p += Len + (End ? 1 : 0);
      }
    }
    UNICODE_STRING Set {static_cast<USHORT>(SetLen * sizeof(wchar_t)), static_cast<USHORT>((SetLen + 1) * sizeof(wchar_t)), SetText};
    const NTSTATUS SetStatus = RtlSetEnvironmentVariable(nullptr, &Name, &Set);
    StatsLogNote(fextl::fmt::format("vkd3d: {}={} (status {:#x}): {} (only FEX's VKD3D build reads it; {}=0 turns it off)", Var,
                                    Narrow(SetText, SetLen), static_cast<uint32_t>(SetStatus), What, SwitchName));
  }

  // FEX_VKD3D_LOG_OFF=1 (D2R default in the build without the stats log): vkd3d-proton's own text log off. By default it
  // writes its FIXME and INFO lines through Wine's debug output; VKD3D_DEBUG and VKD3D_SHADER_DEBUG set to none stop that.
  // A variable that is already set is left alone.
  void SetupVkd3dLogOff() {
    const char* Value = Switch("FEX_VKD3D_LOG_OFF");
    if (!Value || (Value[0] == '0' && !Value[1])) {
      return;
    }
    static constexpr struct {
      const wchar_t* Text;
      USHORT Bytes;
    } Vars[] = {
      {L"VKD3D_DEBUG", sizeof(L"VKD3D_DEBUG") - sizeof(wchar_t)},
      {L"VKD3D_SHADER_DEBUG", sizeof(L"VKD3D_SHADER_DEBUG") - sizeof(wchar_t)},
    };
    static constexpr wchar_t NoneText[] = L"none";
    for (const auto& Var : Vars) {
      UNICODE_STRING Name {Var.Bytes, static_cast<USHORT>(Var.Bytes + sizeof(wchar_t)), const_cast<wchar_t*>(Var.Text)};
      wchar_t Old[32];
      UNICODE_STRING OldValue {0, sizeof(Old) - sizeof(wchar_t), Old};
      if (RtlQueryEnvironmentVariable_U(nullptr, &Name, &OldValue) != StatusVariableNotFound) {
        continue;
      }
      UNICODE_STRING None {sizeof(NoneText) - sizeof(wchar_t), sizeof(NoneText), const_cast<wchar_t*>(NoneText)};
      RtlSetEnvironmentVariable(nullptr, &Name, &None);
    }
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
  uint32_t TurnipWaveArms {}; // FEX_TU_DRAW_SETUP or FEX_TU_WAVE_PAIR: bit N = arm N takes part
  bool TurnipDrawSetup {};    // the arms come from FEX_TU_DRAW_SETUP (arm 4 = no preamble at all, 5-7 fex15 setups)
  uint32_t TurnipCsAtomics {}; // FEX_TU_CS_ATOMICS: 0 off, 1 always, 2 alt
  uint32_t TurnipConstKeep {}; // FEX_TU_CONST_KEEP: 0 off, 1 always, 2 alt

  // FEX_TU_WAVE_PAIR: 0 = off; alt = arms 0-4 a second each; digits 0-4 = those arms (one digit other than 0 = that arm
  // always).
  uint32_t ParseWaveArms(const char* Value) {
    if (!Value || !Value[0]) {
      return 0;
    }
    if (Value[0] == 'a' || Value[0] == 'A') {
      return 0x1f;
    }
    uint32_t Arms = 0;
    for (const char* c = Value; *c; ++c) {
      if (*c >= '0' && *c <= '4') {
        Arms |= 1u << (*c - '0');
      }
    }
    return Arms == 1 ? 0 : Arms;
  }

  // FEX_TU_DRAW_SETUP: 0 = off; alt = arms 0, 4, 5, 6, 7 a second each; digits 0-7 = those arms (one digit other than 0 =
  // that arm always).
  uint32_t ParseDrawSetupArms(const char* Value) {
    if (!Value || !Value[0]) {
      return 0;
    }
    if (Value[0] == 'a' || Value[0] == 'A') {
      return 0xf1;
    }
    uint32_t Arms = 0;
    for (const char* c = Value; *c; ++c) {
      if (*c >= '0' && *c <= '7') {
        Arms |= 1u << (*c - '0');
      }
    }
    return Arms == 1 ? 0 : Arms;
  }

  bool SwitchOff(const char* Value) {
    return !Value || (Value[0] == '0' && !Value[1]);
  }

  // The draw setups FEX_TU_DRAW_SETUP asked for, for the FEX link note.
  fextl::string DrawSetupText(uint32_t Arms) {
    static constexpr const char* Names[] = {"stock",
                                            "pair mode 1",
                                            "pair mode 2",
                                            "pair mode 3",
                                            "binary with no preamble at all",
                                            "one constant load per root constant buffer",
                                            "DEFER_WAVE_ALLOC_DIS 0",
                                            "EVICT_BUF_MODE 0"};
    fextl::string List;
    uint32_t N = 0;
    for (uint32_t a = 0; a < Turnip::FsArms; ++a) {
      if (Arms & (1u << a)) {
        List += fextl::fmt::format("{}{}", N++ ? ", " : "", Names[a]);
      }
    }
    return fextl::fmt::format("{} {} (FEX_TU_DRAW_SETUP, driver fex15 and later; one-time recompile of those shaders{})",
                              N > 1 ? "rotating a second each:" : "always", List,
                              (Arms & (1u << Turnip::FsArmAlt)) ? "; the second binary's mode bit follows the setup without preamble" :
                                                                  "");
  }

  // The wave setups FEX_TU_WAVE_PAIR asked for, for the FEX link note.
  fextl::string WaveArmsText(uint32_t Arms) {
    if (TurnipDrawSetup) {
      return DrawSetupText(Arms);
    }
    if (!Arms) {
      return "stock (FEX_TU_WAVE_PAIR=0, FEX_TU_DRAW_SETUP=0)";
    }
    static constexpr const char* Names[] = {"stock", "pair mode 1", "pair mode 2", "pair mode 3", "128-pixel waves"};
    fextl::string List;
    uint32_t N = 0;
    for (uint32_t a = 0; a < 5; ++a) {
      if (Arms & (1u << a)) {
        List += fextl::fmt::format("{}{}", N++ ? ", " : "", Names[a]);
      }
    }
    return fextl::fmt::format("{} {} (FEX_TU_WAVE_PAIR, driver fex14 and later; one-time recompile of those shaders{})",
                              N > 1 ? "rotating a second each:" : "always", List,
                              (Arms & (1u << Turnip::FsArmW128)) ? "; the second binary's mode bit follows setup 128-pixel waves" : "");
  }

  // The value this process got for a Linux environment variable when it started (from /proc/self/environ), or "-".
  fextl::string StartUnixValue(const char* Variable) {
    static char Env[65536];
    static int64_t Len = -1;
    if (Len < 0) {
      Len = FEX::Windows::Linux::ReadSmallFile("/proc/self/environ", Env, sizeof(Env));
    }
    const size_t VarLen = strlen(Variable);
    for (int64_t i = 0; i < Len;) {
      const char* Entry = Env + i;
      const size_t EntryLen = strnlen(Entry, static_cast<size_t>(Len - i));
      if (EntryLen > VarLen && !strncmp(Entry, Variable, VarLen) && Entry[VarLen] == '=') {
        return fextl::string(Entry + VarLen + 1, EntryLen - VarLen - 1);
      }
      i += static_cast<int64_t>(EntryLen) + 1;
    }
    return "-";
  }

  // See EnvironmentVariablesHandling.h: the frame generation layer's own defaults are on.
  // Only a process where the switch has a value (D2R's defaults, or the user's setting) touches the variables, and it sets
  // them both ways: a process inherits its Linux environment from the one that started it, and build 113 set 0 in Steam
  // (no D2R defaults there, so the switch read as off), so the game ran without layer 7's present thread. Build 78 had the
  // same leak with a VKD3D switch.
  void SetupLsfgSwitches(FARPROC SetUnixEnv) {
    struct LsfgSwitch {
      const char* Name;
      const char* Variable;
      const char* What;
      bool Number; // layer 9: the variable gets the switch's number (0 when off) instead of 0 or 1
    };
    static constexpr LsfgSwitch Switches[] = {
      {"FEX_LSFG_DEFER", "LSFG_D2R_DEFER", "frames presented inside the game's vkQueuePresentKHR, as before layer 7", false},
      {"FEX_LSFG_GEN_DRAWN", "LSFG_D2R_GEN_DRAWN", "generated frames presented without waiting until the GPU drew them", false},
      {"FEX_LSFG_EARLY", "LSFG_D2R_EARLY",
       "a frame's copy and frame generation start in the game's present, after VKD3D's cap pre-sleep, as up to layer 7", false},
      {"FEX_LSFG_PRESENT_EARLY", "LSFG_D2R_PRESENT_EARLY_US",
       "the layer's present thread sleeps until a present's slot, as up to layer 8 (the number: microseconds before the slot "
       "it waits on the CPU from)",
       true},
      {"FEX_LSFG_GPU_TIMES", "LSFG_D2R_GPU_TIMES", "no GPU times of the layer's copy and frame generation", false},
      {"FEX_LSFG_PRESENT_NICE", "LSFG_D2R_DEFER_NICE",
       "the layer's present thread keeps the nice it starts with (the number: its nice, -20 to 19; layer 9 used -10)", true},
      {"FEX_LSFG_GEN_GAP", "LSFG_D2R_GEN_GAP_MS",
       "every game frame gets its generated frame, however long it took (the number: milliseconds since the frame before "
       "after which a frame gets none)",
       true},
    };
    for (const auto& S : Switches) {
      const char* Value = Switch(S.Name);
      if (!Value) {
        continue;
      }
      const bool Off = SwitchOff(Value);
      // A number switch passes its digits (up to 6, after a minus sign from build 117); anything else sets 0 or 1 as the
      // other switches.
      char Digits[9] {};
      size_t DigitCount = 0;
      size_t SignCount = 0;
      if (S.Number && !Off) {
        const char* c = Value;
        if (*c == '-' && c[1] >= '0' && c[1] <= '9') {
          Digits[SignCount++] = *c++;
        }
        for (; *c >= '0' && *c <= '9' && DigitCount < 6; ++c) {
          Digits[SignCount + DigitCount++] = *c;
        }
      }
      // A number switch without a number ("off") is off too.
      const bool NumberOff = S.Number && !Off && !DigitCount;
      const char* Set = Off || NumberOff ? "0" : (DigitCount ? Digits : "1");
      const auto Before = StartUnixValue(S.Variable);
      if (!SetUnixEnv) {
        StatsLogNote(fextl::fmt::format("framegen: this Wine has no __wine_set_unix_env, {} can't be set (it is {})", S.Variable, Before));
        continue;
      }
      reinterpret_cast<decltype(__wine_set_unix_env)>(SetUnixEnv)(S.Variable, Set);
      StatsLogNote(fextl::fmt::format("framegen: {}={} -> {}={} (it was {} when the game started{}){}{}", S.Name, Value, S.Variable, Set,
                                      Before, Before == "-" ? "" : ", set by the program that started it", Off || NumberOff ? ": " : "",
                                      Off || NumberOff ? S.What : ""));
    }
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
    // FEX_TU_DRAW_SETUP, or without it FEX_TU_WAVE_PAIR (see EnvironmentVariablesHandling.h): the draw or wave setup of
    // big fragment shaders, starting with the lowest arm asked for; TurnipReport moves to the next one each second.
    TurnipWaveArms = ParseDrawSetupArms(Switch("FEX_TU_DRAW_SETUP"));
    TurnipDrawSetup = TurnipWaveArms != 0;
    if (!TurnipDrawSetup) {
      TurnipWaveArms = ParseWaveArms(Switch("FEX_TU_WAVE_PAIR"));
    }
    const uint32_t FirstArm = TurnipWaveArms ? static_cast<uint32_t>(__builtin_ctz(TurnipWaveArms)) : 0;
    // FEX_TU_CS_ATOMICS: alt (D2R default in build 107) = compute shaders whose atomics hit one address for the whole wave
    // also get a binary that does them once per wave (driver fex16 and later), used in every other second (TurnipReport
    // flips the mode bit; without the stats log alt stays on); 1 = that binary always; 0 = stock.
    const char* CsAtomics = Switch("FEX_TU_CS_ATOMICS");
    TurnipCsAtomics = SwitchOff(CsAtomics) ? 0 : (CsAtomics[0] == 'a' || CsAtomics[0] == 'A' || CsAtomics[0] == '2') ? 2 : 1;
    // FEX_TU_CONST_KEEP: alt (D2R default in build 110) = root constant buffer ranges whose address didn't change stay in
    // the shaders' constants instead of being loaded again with every draw (driver fex17 and later), in every other second
    // (TurnipReport flips the mode bit; without the stats log alt stays on); 1 = always; 0 = stock. Build 106: the
    // constants loaded per lighting draw (~100 vec4, all of them every draw) hold the next draw back; only ~16 of them change.
    const char* ConstKeep = Switch("FEX_TU_CONST_KEEP");
    TurnipConstKeep = SwitchOff(ConstKeep) ? 0 : (ConstKeep[0] == 'a' || ConstKeep[0] == 'A' || ConstKeep[0] == '2') ? 2 : 1;
    // FEX_TU_NOPREAMBLE alt/lit (SetupTurnipNoPreamble): the second binary without preamble, picked with the same mode bit.
    // With wave setup 4 (the 128-pixel binary) asked for, that bit follows the wave setup instead.
    const bool AltFs = (TurnipWaveArms & (1u << Turnip::FsArmW128)) ? FirstArm == Turnip::FsArmW128 :
                                                                       (TurnipNoAlias || TurnipNoPreamble >= 2);
    Chan->Mode = (TurnipDesc ? (Turnip::ModeChangedOnly | Turnip::ModePushReuse) : 0) |
                 (TurnipDepthClear ? Turnip::ModeDepthClear3d : 0) |
                 (TurnipDepthClear >= 2 ? Turnip::ModeDepthClearFastZ : 0) | (TurnipBudgetCache ? Turnip::ModeBudgetCache : 0) |
                 (TurnipSwitch ? Turnip::ModeSwitch : 0) | (AltFs ? Turnip::ModeAltFs : 0) |
                 (FirstArm << Turnip::ModeFsArmShift) | (TurnipCsAtomics ? Turnip::ModeCsAlt : 0) |
                 (TurnipConstKeep ? Turnip::ModeConstKeep : 0);

    // FEX_TU_COUNTERS: 1 (D2R default in build 102) = the list below, 0 = none. FEX_TU_SAMPLE_DRAWS: render passes with at
    // least this many draws get timed and counted (D2R default 100 in build 102: the main pass, the depth prepasses and
    // the shadow maps in towns and fights).
    // Build 103's list, every wave state at once. Build 102 found the lighting pass limited by one lighting wave's own
    // start-to-finish time (the GPU keeps about two draws in flight, so each draw costs ~4,700 GPU cycles plus 54 per
    // wave), and the wave spending 15% on math, 3% on idle issue slots, 27% waiting for texture/memory data and ~40% in
    // states that list didn't count. The texture counters are dropped (102 has them: 40% L1 misses, 25% L2 misses);
    // these are all the shader core's wave state counters, so the shares add up and the biggest state is named.
    // Build 105: special math (efu, 0.4%) and join (~1%) made room for the 64/128-pixel wave counts.
    // Build 106: build 105 found about one lighting draw's waves on the cores at a time (5.9 waves per busy core with
    // room for 8, cycles per draw = one wave life), so that list added what the front end that starts draws and their
    // waves does (HLSQ dual fs, constant programming waits, stalls; primitive control and command processor waits).
    // Build 107: a compute list. The dispatches are about 2.9 ms of a busy frame's GPU time (the ground clutter shader
    // 0e21a3b5088ba1af alone ~1.2 ms), timed per shader by VKD3D fex22 but never counted; driver fex16 times and counts
    // every direct dispatch of 16+
    // workgroups with the same list as the passes. It says whether a dispatch is limited by its math (alu, special math
    // for sin/cos), by waits for texture/memory results, by the shared atomic counter (atomics, address locks, stalls on
    // the L2 cache), or by how fast new waves start (cores starved by the front end, wave life). The passes keep time,
    // cycles, occupancy and the same wave states.
    if (!SwitchOff(Switch("FEX_TU_COUNTERS"))) {
      static constexpr struct {
        uint32_t Group, Countable;
      } List[] = {
        {Turnip::GroupCp, 0}, // CP_ALWAYS_COUNT (GPU cycles, shared with KGSL)
        // Shader cores: busy, resident waves (integral over cycles), starved by the front end, compute threads and
        // instructions.
        {Turnip::GroupSp, 0},   // SP_BUSY_CYCLES
        {Turnip::GroupSp, 8},   // SP_WAVE_CONTEXTS
        {Turnip::GroupSp, 69},  // SP_STARVE_CYCLES_HLSQ
        {Turnip::GroupSp, 100}, // SP_CS_INVOCATIONS
        {Turnip::GroupSp, 56},  // SP_CS_INSTRUCTIONS
        // Wave states (wave x cycle).
        {Turnip::GroupSp, 89},  // SP_WAVE_ALU_CYCLES (float math)
        {Turnip::GroupSp, 90},  // SP_WAVE_EFU_CYCLES (special math: sin, cos, rcp, sqrt)
        {Turnip::GroupSp, 91},  // SP_WAVE_INT_CYCLES
        {Turnip::GroupSp, 24},  // SP_WAVE_LONG_SYNC_CYCLES (texture/memory/atomic results)
        {Turnip::GroupSp, 25},  // SP_WAVE_SHORT_SYNC_CYCLES
        {Turnip::GroupSp, 20},  // SP_WAVE_WAIT_CYCLES
        {Turnip::GroupSp, 19},  // SP_WAVE_NOP_CYCLES
        {Turnip::GroupSp, 21},  // SP_WAVE_FETCH_CYCLES (instruction fetch)
        {Turnip::GroupSp, 16},  // SP_WAVE_CTRL_CYCLES (flow control)
        {Turnip::GroupSp, 22},  // SP_WAVE_IDLE_CYCLES (ready, another wave issued)
        {Turnip::GroupSp, 106}, // SP_WAVE_HWAVE_WAIT_CYCLES
        // Memory: atomics, stores, address locks (atomics on one address queue behind each other), stalls.
        {Turnip::GroupSp, 32}, // SP_GM_ATOMICS
        {Turnip::GroupSp, 31}, // SP_GM_STORE_INSTRUCTIONS
        {Turnip::GroupSp, 45}, // SP_ADDR_LOCK_COUNT
        {Turnip::GroupSp, 5},  // SP_STALL_CYCLES_UCHE
        {Turnip::GroupSp, 4},  // SP_STALL_CYCLES_TP
        {Turnip::GroupSp, 119}, // SP_TEXTURE_FETCH_LATENCY_CYCLES
        {Turnip::GroupSp, 120}, // SP_TEXTURE_FETCH_LATENCY_SAMPLES
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
    // FEX_TU_WAVE_PAIR (driver fex14 and later): pair mode states for arms 1-3 (made when a shader is uploaded), the
    // 128-pixel second binary for arm 4 (part of the shader keys, so a one-time recompile of the big fragment shaders).
    if (TurnipWaveArms & 0xe) {
      Chan->Compile |= Turnip::CompilePairAlt;
    }
    if (!TurnipDrawSetup && (TurnipWaveArms & (1u << Turnip::FsArmW128))) {
      Chan->Compile |= Turnip::CompileW128Alt;
    }
    // FEX_TU_DRAW_SETUP (driver fex15 and later): arm 4 = the second binary with no preamble at all, arm 5 = the third
    // binary with merged constant loads (both part of the shader keys: one-time recompile), arms 6/7 = the alloc setting
    // states (made when a shader is uploaded).
    if (TurnipDrawSetup && (TurnipWaveArms & (1u << Turnip::FsArmAlt))) {
      Chan->Compile |= Turnip::CompileNoPreAllAlt;
    }
    if (TurnipDrawSetup && (TurnipWaveArms & (1u << Turnip::FsArmMerge))) {
      Chan->Compile |= Turnip::CompileMergeAlt;
    }
    if (TurnipDrawSetup && (TurnipWaveArms & ((1u << Turnip::FsArmDefer) | (1u << Turnip::FsArmEvict)))) {
      Chan->Compile |= Turnip::CompileCntlAlt;
    }
    // FEX_TU_CS_ATOMICS (driver fex16 and later): the compute binaries with atomics once per wave (part of the shader
    // keys: one-time recompile of compute shaders).
    if (TurnipCsAtomics) {
      Chan->Compile |= Turnip::CompileCsAtomicsAlt;
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
                                    "shaders without preamble {}, lighting shader wave/draw setups {}, compute atomics once per wave {}, "
                                    "unchanged root constant buffers kept in the constants {}, depth clears at pass start {}, memory budget {}, {} GPU counters asked for, passes with {}+ draws "
                                    "(and from driver fex16 direct dispatches of 16+ workgroups) timed and counted; only FEX's "
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
                                    WaveArmsText(TurnipWaveArms),
                                    TurnipCsAtomics == 2 ? "in every other second (FEX_TU_CS_ATOMICS=alt, driver fex16 and later; "
                                                           "one-time compute shader recompile)" :
                                    TurnipCsAtomics      ? "always (FEX_TU_CS_ATOMICS=1, driver fex16 and later; one-time compute "
                                                           "shader recompile)" :
                                                           "off (FEX_TU_CS_ATOMICS=0)",
                                    TurnipConstKeep == 2 ? "in every other second (FEX_TU_CONST_KEEP=alt, driver fex17 and later)" :
                                    TurnipConstKeep      ? "always (FEX_TU_CONST_KEEP=1, driver fex17 and later)" :
                                                           "off (FEX_TU_CONST_KEEP=0)",
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
  SetupVkd3dLogOff();
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
  SetupVkd3dFexSwitch("FEX_VKD3D_SHADER_SWAP", L"VKD3D_FEX_SHADER_SWAP",
                      "D2R's 3D lighting table blur compute shader (83e2eb4a6bf5f03b, 8,192 samples per cell, 30 ms per run) "
                      "is swapped for FEX's own version with the same bindings that samples each distinct value once per "
                      "group of cells (VKD3D fex30 and later; '# shader swap' note); from VKD3D fex31 alt = binds take turns "
                      "between two versions, 1 = first only, 2 = second only (VKD3D fex31: 4 and 2 cells per GPU "
                      "thread; fex32: 8 cells per thread, 4 x by 2 y and 8 in a row; fex33: 8 in a row only, plus "
                      "D2R's ground clutter compute shader with its on-screen and mask tests before its density noise, "
                      "taking turns with the game's own; 1 = FEX's versions, 2 = the game's own clutter shader)",
                      true, "alt|1|2");
  SetupVkd3dFexSwitch("FEX_VKD3D_CLEAR_MEMO", L"VKD3D_FEX_CLEAR_MEMO",
                      "a big depth image (4M+ pixels) whose clear gets a render pass of its own gets a 'still clear' flag "
                      "on the GPU; the GPU skips that clear while the image still holds the value, and every draw, clear, "
                      "copy, resolve or discard that may write the image resets the flag (VKD3D fex34 and later; D2R's "
                      "empty second shadow map, 0.55-0.69 ms per frame; 'gpu clear memo' line; alt = on and off by turns "
                      "every second)",
                      true, "alt|1");
  SetupVkd3dShaderCopy();

  auto Sym = GetProcAddress(NtDll, "__wine_set_unix_env");
  SetupTurnipNoPreamble(Sym);
  SetupTurnipChannel(Sym);
  SetupLsfgSwitches(Sym);

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
  uint32_t WavePairArms() {
    return TurnipWaveArms;
  }
  bool DrawSetupArms() {
    return TurnipDrawSetup;
  }
  uint32_t CsAtomicsSwitch() {
    return TurnipCsAtomics;
  }
  uint32_t ConstKeepSwitch() {
    return TurnipConstKeep;
  }
} // namespace Turnip
} // namespace FEX::Windows
