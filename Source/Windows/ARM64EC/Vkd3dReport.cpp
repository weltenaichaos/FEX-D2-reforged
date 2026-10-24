// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Copies the GPU pass and present timing reports of FEX's vkd3d-proton build into the stats log
$end_info$
*/

#include "Vkd3dReport.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);
extern "C" NTSTATUS WINAPI RtlQueryEnvironmentVariable_U(PWSTR Environment, PUNICODE_STRING Name, PUNICODE_STRING Value);

namespace Vkd3dReport {
namespace {
  // struct vkd3d_fex_report in vkd3d-patches/ (libs/vkd3d/vkd3d_private.h). seq is odd while vkd3d writes the text.
  struct Header {
    uint32_t Magic;
    uint32_t Version;
    uint32_t Seq;
    uint32_t Flags;
  };
  constexpr uint32_t ReportVersion = 1;
  constexpr size_t TextSize = 4096;
  constexpr uint32_t FlagOn = 0x1;
  constexpr uint32_t FlagFoldClears = 0x2;
  constexpr uint32_t FlagFrameStart = 0x4;
  constexpr uint32_t FlagCapPresleep = 0x8;
  constexpr uint32_t FlagGradualStart = 0x10; // VKD3D fex3 and later: the frame start hold-back changes by small steps
  constexpr uint32_t FlagMarginCaused = 0x20; // VKD3D fex4 and later: only frames the hold-back made late grow its margin
  constexpr uint32_t FlagSkipFreshClears = 0x400; // gpu report (VKD3D fex11): zero fills of never-used block pages skipped
  constexpr uint32_t FlagDevice = 0x80000000u;

  struct Source {
    const char* Export;
    uint32_t Magic;
    uint64_t Address;
    uint32_t LastSeq;
    uint32_t Flags;
  };

  enum { Pass, Present, Gpu, SourceCount };
  Source Sources[SourceCount] = {
    {"vkd3d_fex_pass_report", 0x50584546u /* 'FEXP' */, 0, 0, 0},
    {"vkd3d_fex_present_report", 0x54584546u /* 'FEXT' */, 0, 0, 0},
    // VKD3D fex11: new memory's zero fills, vkd3d's own transfer submissions, GPU time between render passes.
    {"vkd3d_fex_gpu_report", 0x47584546u /* 'FEXG' */, 0, 0, 0},
  };

  uint64_t ModuleAddress;
  bool NotFexBuild;
  bool Announced;
  fextl::vector<uint64_t> SeenNotes;
  char Text[TextSize];

  // The module may be unloaded while the log thread looks: read through NtReadVirtualMemory, which fails instead of
  // faulting.
  bool ReadMemory(uint64_t Address, void* Out, size_t Size) {
    SIZE_T Read {};
    return NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<const void*>(Address), Out, Size, &Read) == STATUS_SUCCESS && Read == Size;
  }

  uint64_t FindModule() {
    static constexpr wchar_t Name[] = L"d3d12core.dll";
    UNICODE_STRING Text {sizeof(Name) - sizeof(wchar_t), sizeof(Name), const_cast<wchar_t*>(Name)};
    HMODULE Module {};
    return LdrGetDllHandle(nullptr, 0, &Text, &Module) == STATUS_SUCCESS ? reinterpret_cast<uint64_t>(Module) : 0;
  }

  uint64_t FindExport(uint64_t Module, const char* Name) {
    ANSI_STRING Text {static_cast<USHORT>(strlen(Name)), static_cast<USHORT>(strlen(Name) + 1), const_cast<char*>(Name)};
    void* Address {};
    return LdrGetProcedureAddress(reinterpret_cast<HMODULE>(Module), &Text, 0, &Address) == STATUS_SUCCESS ? reinterpret_cast<uint64_t>(Address) : 0;
  }

  // GameNative's FPS limit: when its Lossless Scaling Vulkan layer (liblsfg-vk-layer.so) is armed, the layer paces each
  // vkQueuePresentKHR to fps_limit from its conf.toml (environment variable LSFG_CONFIG). Says whether the layer is mapped
  // into the process and what its config caps, once.
  fextl::string CapLayerNote() {
    namespace Linux = FEX::Windows::Linux;
    fextl::string Maps;
    const int Fd = Linux::Open("/proc/self/maps");
    if (Fd >= 0) {
      static char Chunk[65536];
      for (uint64_t Offset = 0; Offset < (64ull << 20);) {
        const int64_t Len = Linux::Pread(Fd, Chunk, sizeof(Chunk), Offset);
        if (Len <= 0) {
          break;
        }
        Maps.append(Chunk, Len);
        Offset += Len;
      }
      Linux::Close(Fd);
    }
    fextl::string Layer;
    const size_t At = Maps.find("lsfg");
    if (At != fextl::string::npos) {
      const size_t Space = Maps.rfind(' ', At);
      const size_t Begin = Space == fextl::string::npos ? At : Space + 1;
      const size_t End = Maps.find('\n', At);
      Layer = Maps.substr(Begin, End == fextl::string::npos ? fextl::string::npos : End - Begin);
    }

    static constexpr wchar_t NameText[] = L"LSFG_CONFIG";
    UNICODE_STRING Name {sizeof(NameText) - sizeof(wchar_t), sizeof(NameText), const_cast<wchar_t*>(NameText)};
    wchar_t Value[512];
    UNICODE_STRING ValueText {0, sizeof(Value) - sizeof(wchar_t), Value};
    fextl::string Config;
    fextl::string Caps;
    if (RtlQueryEnvironmentVariable_U(nullptr, &Name, &ValueText) == STATUS_SUCCESS) {
      for (size_t i = 0; i < ValueText.Length / sizeof(wchar_t); ++i) {
        Config += Value[i] < 0x80 ? static_cast<char>(Value[i]) : '?';
      }
      static char Text[8192];
      if (Linux::ReadSmallFile(Config.c_str(), Text, sizeof(Text)) > 0) {
        std::string_view All {Text};
        while (!All.empty()) {
          const size_t End = All.find('\n');
          std::string_view Line = All.substr(0, End);
          All = End == std::string_view::npos ? std::string_view {} : All.substr(End + 1);
          while (!Line.empty() && (Line.front() == ' ' || Line.front() == '\t')) {
            Line.remove_prefix(1);
          }
          if (Line.starts_with("fps_limit") || Line.starts_with("multiplier") || Line.starts_with("[")) {
            Caps += Caps.empty() ? "" : ", ";
            Caps.append(Line.data(), std::min<size_t>(Line.size(), 80));
          }
        }
      } else {
        Caps = "can't be read";
      }
    }

    if (Layer.empty() && Config.empty()) {
      return "# cap: GameNative's frame cap layer isn't loaded (no lsfg mapping, no LSFG_CONFIG)\n";
    }
    return fextl::fmt::format("# cap: GameNative's frame cap layer {} ({}); LSFG_CONFIG {}: {}\n", Layer.empty() ? "isn't mapped" : "is loaded",
                              Layer.empty() ? "-" : Layer, Config.empty() ? "-" : Config, Caps.empty() ? "-" : Caps);
  }

  enum class ReadResult { Gone, Unchanged, New };

  // Copies the report's text into Text when vkd3d published a new one since the last call.
  ReadResult ReadReport(Source& S) {
    for (int Try = 0; Try < 3; ++Try) {
      Header Before {}, After {};
      if (!ReadMemory(S.Address, &Before, sizeof(Before)) || Before.Magic != S.Magic || Before.Version != ReportVersion) {
        return ReadResult::Gone;
      }
      S.Flags = Before.Flags;
      if (Before.Seq & 1) {
        continue;
      }
      if (Before.Seq == S.LastSeq) {
        return ReadResult::Unchanged;
      }
      if (!ReadMemory(S.Address + sizeof(Header), Text, TextSize) || !ReadMemory(S.Address, &After, sizeof(After))) {
        return ReadResult::Gone;
      }
      if (After.Seq == Before.Seq) {
        Text[TextSize - 1] = 0;
        S.LastSeq = Before.Seq;
        return ReadResult::New;
      }
    }
    return ReadResult::Unchanged;
  }

  uint64_t Hash(std::string_view Line) {
    uint64_t H = 14695981039346656037ull;
    for (char c : Line) {
      H = (H ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    }
    return H;
  }

  // Data lines go in as they are; each distinct '#' note only the first time.
  void AppendLines(fextl::string& Out) {
    std::string_view All {Text, strnlen(Text, TextSize)};
    while (!All.empty()) {
      const size_t End = All.find('\n');
      const std::string_view Line = All.substr(0, End);
      All = End == std::string_view::npos ? std::string_view {} : All.substr(End + 1);
      if (Line.empty()) {
        continue;
      }
      if (Line[0] == '#') {
        const uint64_t H = Hash(Line);
        if (std::find(SeenNotes.begin(), SeenNotes.end(), H) != SeenNotes.end()) {
          continue;
        }
        if (SeenNotes.size() < 256) {
          SeenNotes.push_back(H);
        }
      } else if (Line[0] != ' ') {
        Out += "  ";
      }
      Out.append(Line.data(), Line.size());
      Out += '\n';
    }
  }

  fextl::string Lines() {
    fextl::string Out;
    if (NotFexBuild) {
      return Out;
    }

    if (!ModuleAddress) {
      // d3d12core.dll comes in when the game creates its D3D12 device.
      const uint64_t Module = FindModule();
      if (!Module) {
        return Out;
      }
      for (auto& S : Sources) {
        S.Address = FindExport(Module, S.Export);
        S.LastSeq = 0;
        S.Flags = 0;
      }
      ModuleAddress = Module;
      if (!Sources[Pass].Address && !Sources[Present].Address) {
        NotFexBuild = true;
        return fextl::fmt::format("# vkd3d: the game's d3d12core.dll (at {:X}) has no FEX reports: it isn't FEX's VKD3D build, so clear "
                                  "folding and the gpu/present/queue lines are off\n",
                                  Module);
      }
    }

    for (auto& S : Sources) {
      if (!S.Address) {
        continue;
      }
      const ReadResult R = ReadReport(S);
      if (R == ReadResult::Gone) {
        // Unloaded: look for it again next second.
        Out += fextl::fmt::format("# vkd3d: d3d12core.dll at {:X} went away\n", ModuleAddress);
        ModuleAddress = 0;
        Announced = false;
        for (auto& Other : Sources) {
          Other.Address = 0;
        }
        return Out;
      }
      if (R == ReadResult::New) {
        AppendLines(Out);
      }
    }

    const uint32_t PassFlags = Sources[Pass].Flags;
    const uint32_t PresentFlags = Sources[Present].Flags;
    const uint32_t GpuFlags = Sources[Gpu].Flags;
    if (!Announced && (PassFlags & FlagDevice) && (!Sources[Present].Address || (PresentFlags & FlagDevice))) {
      Announced = true;
      const char* PresentState = !Sources[Present].Address ? "missing" : PresentFlags & FlagOn ? "on" : "off";
      const char* FreshState = !Sources[Gpu].Address ? "missing (VKD3D older than fex11)" :
                               GpuFlags & FlagSkipFreshClears ? "on" : "off";
      Out.insert(0, fextl::fmt::format("# vkd3d: FEX's VKD3D build is loaded (d3d12core.dll at {:X}): clear folding {}, "
                                       "GPU pass meter {}, present timeline {}, cap pre-sleep {}, zero fills of never-used memory "
                                       "skipped {}, frame start pacing {}\n",
                                       ModuleAddress, PassFlags & FlagFoldClears ? "on" : "off", PassFlags & FlagOn ? "on" : "off",
                                       PresentState, PresentFlags & FlagCapPresleep ? "on" : "off", FreshState,
                                       !(PresentFlags & FlagFrameStart) ? "off" :
                                       PresentFlags & FlagMarginCaused ? "on (gradual, margin only from frames it made late: VKD3D fex4 or "
                                                                         "later)" :
                                       PresentFlags & FlagGradualStart ? "on (gradual, VKD3D fex3: an older VKD3D component is selected)" :
                                                                         "on (build 56 style: an older VKD3D component is selected)"));
      Out += CapLayerNote();
    }
    return Out;
  }
} // namespace

void Init() {
  FEX::Windows::StatsLogSetLineProvider(Lines);
}
} // namespace Vkd3dReport
