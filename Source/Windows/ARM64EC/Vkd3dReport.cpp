// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Copies the GPU pass and present timing reports of FEX's vkd3d-proton build into the stats log
$end_info$
*/

#include "Vkd3dReport.h"
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
  constexpr uint32_t FlagDevice = 0x80000000u;

  struct Source {
    const char* Export;
    uint32_t Magic;
    uint64_t Address;
    uint32_t LastSeq;
    uint32_t Flags;
  };

  enum { Pass, Present, SourceCount };
  Source Sources[SourceCount] = {
    {"vkd3d_fex_pass_report", 0x50584546u /* 'FEXP' */, 0, 0, 0},
    {"vkd3d_fex_present_report", 0x54584546u /* 'FEXT' */, 0, 0, 0},
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
    if (!Announced && (PassFlags & FlagDevice) && (!Sources[Present].Address || (PresentFlags & FlagDevice))) {
      Announced = true;
      Out.insert(0, fextl::fmt::format("# vkd3d: FEX's VKD3D build is loaded (d3d12core.dll at {:X}): clear folding {}, "
                                       "GPU pass meter {}, present timeline {}\n",
                                       ModuleAddress, PassFlags & FlagFoldClears ? "on" : "off", PassFlags & FlagOn ? "on" : "off",
                                       !Sources[Present].Address ? "missing" :
                                       PresentFlags & FlagOn     ? "on" :
                                                                   "off"));
    }
    return Out;
  }
} // namespace

void Init() {
  FEX::Windows::StatsLogSetLineProvider(Lines);
}
} // namespace Vkd3dReport
