// SPDX-License-Identifier: MIT
#include "Windows/Common/EnvironmentVariablesHandling.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

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
  void SetupVkd3dFexSwitch(const char* SwitchName, const wchar_t (&VarText)[N], const char* What) {
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
                      "the game's next frame starts as late as the measured waiting allows (up to 20 ms, changing by at most "
                      "0.2 ms per frame up and 1 ms down, from VKD3D fex3 on), so it reads input closer to when the frame is "
                      "shown ('frame start' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_SKIP_FRESH_CLEARS", L"VKD3D_FEX_SKIP_FRESH_CLEARS",
                      "new GPU memory that vkd3d places in block pages never used since the driver zeroed them skips its "
                      "zero fill (VKD3D fex11 and later; 'gpu memory' line)");
  SetupVkd3dFexSwitch("FEX_VKD3D_PRESENT_DRAWN", L"VKD3D_FEX_PRESENT_DRAWN",
                      "each frame goes to the window system only once the GPU has drawn it, because GameNative's X server "
                      "shows an image as soon as it gets it and one handed over earlier showed an older frame (VKD3D fex12 "
                      "and later; 'screen handoff' line)");

  auto Sym = GetProcAddress(NtDll, "__wine_set_unix_env");

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
} // namespace FEX::Windows
