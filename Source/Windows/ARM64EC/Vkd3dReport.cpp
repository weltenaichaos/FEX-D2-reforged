// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Copies the GPU pass and present timing reports of FEX's vkd3d-proton build into the stats log, and the shaders its shader copy took
$end_info$
*/

#include "Vkd3dReport.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <string_view>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);
extern "C" NTSTATUS WINAPI NtWriteVirtualMemory(HANDLE Process, void* Address, const void* Buffer, SIZE_T Size, SIZE_T* BytesWritten);
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
  constexpr uint32_t FlagPresentDrawn = 0x800;    // present report (VKD3D fex12): frames go to the window system only once drawn
  constexpr uint32_t FlagLatencyResync = 0x1000;  // present report (VKD3D fex16): frame credit resync is on
  constexpr uint32_t FlagMemoryAhead = 0x2000;    // gpu report (VKD3D fex17): GPU memory made ahead is on
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

  // struct vkd3d_fex_shader_copy in vkd3d-patches/ (VKD3D fex13): a table of copies vkd3d took of chosen shaders, their
  // PSOs and the driver's compiled code. Entries [0, Count) are complete; their bytes are never freed.
  struct CopyHeader {
    uint32_t Magic;
    uint32_t Version;
    uint32_t Count;
    uint32_t Flags;
    uint32_t Listed;
    uint32_t Dropped;
  };
  struct CopyBlob {
    uint64_t Hash; // the pixel shader the entry belongs to
    uint64_t Other;
    uint32_t Kind;
    uint32_t Rank;
    uint32_t Size;
    uint32_t Stage;
    uint64_t Data;
  };
  static_assert(sizeof(CopyHeader) == 24 && sizeof(CopyBlob) == 40);

  // struct vkd3d_fex_cap_grid in vkd3d-patches/ (VKD3D fex22): FEX writes the frame cap's exact frame time there, and
  // vkd3d's cap pre-sleep uses it instead of learning it from held presents.
  struct CapGridData {
    uint32_t Magic;
    uint32_t Version;
    uint64_t StepNs;
    uint64_t RefreshNs;
    uint32_t FpsLimit;
    uint32_t Writes;
  };
  static_assert(sizeof(CapGridData) == 32);
  constexpr uint32_t CapGridMagic = 0x43584546u; // 'FEXC'
  constexpr uint32_t CapGridVersion = 1;
  constexpr uint32_t CopyMagic = 0x53584546u; // 'FEXS'
  constexpr uint32_t CopyVersion = 1;
  constexpr uint32_t CopyMax = 512; // VKD3D fex22: 512 entries (compute shaders too); older builds have 128
  // Output a second at most, so the copies spread over the first seconds of play instead of one long log write.
  constexpr size_t CopyBytesPerSecond = 192 * 1024;
  constexpr size_t CopyBase64Line = 96; // bytes per base64 line (128 characters)
  constexpr size_t CopyTextLine = 1000; // longer text lines continue on '+' lines

  struct CopyState {
    uint64_t Address;
    bool Announced;
    bool DroppedNoted;
    uint32_t Next; // first entry not written out yet
    bool InBlob;   // Next is being written: Data holds its bytes
    CopyBlob Blob;
    fextl::vector<uint8_t> Data;
    size_t Offset;  // bytes of Data written so far
    uint64_t Check; // FNV-1a of the bytes written so far
  } Copy;

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

  // GameNative publishes the screen's refresh grid next to the cap layer's conf.toml (vsync.txt: vsync_ns=<a refresh
  // time>, period_ns=<refresh period>), and the layer reads it every second. It paces each frame on that grid and rounds
  // the frame time of fps_limit to whole refreshes: max(1, (1e9/fps_limit + period/2) / period) refreshes a frame
  // (lsfg-vk-android paceBaseFrame: "30 = every 2nd vsync at 60Hz, every 4th at 120Hz"). At 60 Hz a limit of 40 (25 ms)
  // becomes 2 refreshes = 30 FPS: the build 92 log ran at limit 40 and showed 30.0 FPS the whole run, presents let out on
  // a 33.3 ms grid. Noted at start and whenever the period changes (checked every 5 s).
  //
  // Build 97: FEX also hands the cap's frame time (the rounded one) to FEX's VKD3D (fex22 and later), whose cap pre-sleep
  // learned it from presents the cap held up to fex21 and could lock onto a wrong one (see WriteCapGrid). fps_limit is
  // read again every 5 s too, in case GameNative's setting changes in game.
  struct CapGrid {
    fextl::string ConfigPath; // LSFG_CONFIG, empty: none
    fextl::string VsyncPath;  // empty: no conf.toml known
    bool LayerLoaded;         // the cap layer is mapped into the process (at the announce)
    int FpsLimit;             // the last fps_limit in conf.toml, 0 = none
    int64_t PeriodNs;         // period last noted, -1 = no usable vsync.txt
    uint32_t Calls;
    uint64_t DataAddress;     // vkd3d_fex_cap_grid in d3d12core.dll (VKD3D fex22 and later), 0 = none
    int64_t StepWritten;      // the frame time last written there, -1 = none yet
    bool OldNoted;            // said that the VKD3D build can't take it
  };
  CapGrid Grid {.PeriodNs = -1, .StepWritten = -1};

  // fps_limit in the cap layer's conf.toml: the limit (0 = no such line), or -1 when the file can't be read. Caps gets the
  // lines worth noting when given.
  int ReadFpsLimit(const fextl::string& Config, fextl::string* Caps) {
    namespace Linux = FEX::Windows::Linux;
    static char Text[8192];
    if (Linux::ReadSmallFile(Config.c_str(), Text, sizeof(Text)) <= 0) {
      return -1;
    }
    int Limit = 0;
    std::string_view All {Text};
    while (!All.empty()) {
      const size_t End = All.find('\n');
      std::string_view Line = All.substr(0, End);
      All = End == std::string_view::npos ? std::string_view {} : All.substr(End + 1);
      while (!Line.empty() && (Line.front() == ' ' || Line.front() == '\t')) {
        Line.remove_prefix(1);
      }
      if (Caps && (Line.starts_with("fps_limit") || Line.starts_with("multiplier") || Line.starts_with("["))) {
        *Caps += Caps->empty() ? "" : ", ";
        Caps->append(Line.data(), std::min<size_t>(Line.size(), 80));
      }
      if (Line.starts_with("fps_limit")) {
        const size_t Eq = Line.find('=');
        if (Eq != std::string_view::npos) {
          Limit = atoi(fextl::string(Line.substr(Eq + 1)).c_str());
        }
      }
    }
    return Limit;
  }

  // The cap's frame time as the layer paces it: fps_limit rounded to whole refreshes on GameNative's grid, or its own
  // clock without a usable vsync.txt. 0 = no cap.
  int64_t CapStepNs() {
    if (Grid.FpsLimit <= 0) {
      return 0;
    }
    const int64_t Target = 1'000'000'000ll / Grid.FpsLimit;
    if (Grid.PeriodNs <= 0) {
      return Target;
    }
    return std::max<int64_t>(1, (Target + Grid.PeriodNs / 2) / Grid.PeriodNs) * Grid.PeriodNs;
  }

  // Writes the cap's frame time into VKD3D's vkd3d_fex_cap_grid when it changed. VKD3D's cap pre-sleep (fex22) uses it
  // instead of the period it learned from presents the cap held: in the fex21 log it learned 30.4 ms instead of the cap's
  // 25 ms while loading in (slow frames get long holds from the window system too, at the frame time) and kept it all
  // session, so the game ran at 37 FPS at a 40 FPS cap with every 4th frame one refresh long (also seen in builds 89, 92
  // and 93 at 30 FPS caps). Off with FEX_VKD3D_CAP_GRID=0 (VKD3D then learns it as before, with fex22's safer learning).
  fextl::string WriteCapGrid() {
    if (!Grid.DataAddress || !Grid.LayerLoaded) {
      return {};
    }
    const char* Switch = FEX::Windows::Switch("FEX_VKD3D_CAP_GRID");
    if (!Switch || !strcmp(Switch, "0")) {
      return {};
    }
    CapGridData Data {};
    if (!ReadMemory(Grid.DataAddress, &Data, sizeof(Data)) || Data.Magic != CapGridMagic || Data.Version != CapGridVersion) {
      if (Grid.OldNoted) {
        return {};
      }
      Grid.OldNoted = true;
      return fextl::fmt::format("# cap: this VKD3D build has a cap grid FEX doesn't know (magic {:08X} version {}): its pre-sleep learns the "
                                "cap's frame time on its own\n",
                                Data.Magic, Data.Version);
    }
    const int64_t Step = CapStepNs();
    if (Step == Grid.StepWritten) {
      return {};
    }
    const uint64_t RefreshNs = Grid.PeriodNs > 0 ? Grid.PeriodNs : 0;
    const uint32_t FpsLimit = Grid.FpsLimit > 0 ? Grid.FpsLimit : 0;
    const uint64_t StepNs = Step;
    const uint32_t Writes = Data.Writes + 1;
    SIZE_T Written {};
    // Refresh and limit first (VKD3D's log line reads them only once the step is there), the step as one 8-byte write.
    const bool Ok =
      NtWriteVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Grid.DataAddress + offsetof(CapGridData, RefreshNs)), &RefreshNs,
                           sizeof(RefreshNs), &Written) == STATUS_SUCCESS &&
      NtWriteVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Grid.DataAddress + offsetof(CapGridData, FpsLimit)), &FpsLimit,
                           sizeof(FpsLimit), &Written) == STATUS_SUCCESS &&
      NtWriteVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Grid.DataAddress + offsetof(CapGridData, StepNs)), &StepNs,
                           sizeof(StepNs), &Written) == STATUS_SUCCESS &&
      NtWriteVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Grid.DataAddress + offsetof(CapGridData, Writes)), &Writes,
                           sizeof(Writes), &Written) == STATUS_SUCCESS;
    if (!Ok) {
      Grid.DataAddress = 0;
      return "# cap: writing the cap's frame time into VKD3D failed; its pre-sleep learns it on its own\n";
    }
    Grid.StepWritten = Step;
    if (!Step) {
      return "# cap: no fps_limit: VKD3D's pre-sleep gets no frame time from FEX\n";
    }
    return fextl::fmt::format("# cap: VKD3D takes the cap's frame time from FEX: {:.3f} ms (fps_limit {}{}); its pre-sleep no longer learns "
                              "it from held presents (VKD3D fex22, FEX_VKD3D_CAP_GRID)\n",
                              Step / 1e6, FpsLimit,
                              RefreshNs ? fextl::fmt::format(", {} refresh{} of {:.3f} ms", Step / RefreshNs, Step / RefreshNs == 1 ? "" : "es",
                                                             RefreshNs / 1e6) :
                                          fextl::string(", the layer's own clock"));
  }

  fextl::string CapGridNote(bool First) {
    namespace Linux = FEX::Windows::Linux;
    if (Grid.VsyncPath.empty()) {
      return {};
    }
    fextl::string Out;
    if (!First && !Grid.ConfigPath.empty()) {
      const int Limit = ReadFpsLimit(Grid.ConfigPath, nullptr);
      if (Limit >= 0 && Limit != Grid.FpsLimit) {
        Out += fextl::fmt::format("# cap: fps_limit changed from {} to {} in the cap's conf.toml\n", Grid.FpsLimit, Limit);
        Grid.FpsLimit = Limit;
        First = true;
      }
    }
    static char Text[1024];
    int64_t Period = -1;
    if (Linux::ReadSmallFile(Grid.VsyncPath.c_str(), Text, sizeof(Text)) > 0) {
      std::string_view All {Text};
      while (!All.empty()) {
        const size_t End = All.find('\n');
        const std::string_view Line = All.substr(0, End);
        All = End == std::string_view::npos ? std::string_view {} : All.substr(End + 1);
        if (Line.starts_with("period_ns=")) {
          Period = strtoll(fextl::string(Line.substr(10)).c_str(), nullptr, 10);
        }
      }
      // The layer ignores a period outside 1-100 ms too.
      if (Period < 1'000'000 || Period > 100'000'000) {
        Period = -1;
      }
    }
    if (!First && Period == Grid.PeriodNs) {
      return Out;
    }
    Grid.PeriodNs = Period;
    if (Period < 0) {
      return Out + fextl::fmt::format("# cap: no usable vsync.txt next to the cap's conf.toml ({}): the layer paces fps_limit {} on its own clock, "
                                "without rounding to screen refreshes\n",
                                Grid.VsyncPath, Grid.FpsLimit);
    }
    const double Hz = 1e9 / static_cast<double>(Period);
    if (Grid.FpsLimit <= 0) {
      return Out + fextl::fmt::format("# cap: screen refresh {:.1f} Hz (GameNative's vsync.txt, period {:.3f} ms); no fps_limit, so no cap\n", Hz,
                                Period / 1e6);
    }
    const int64_t Target = 1'000'000'000ll / Grid.FpsLimit;
    const int64_t Steps = std::max<int64_t>(1, (Target + Period / 2) / Period);
    const double Fps = 1e9 / static_cast<double>(Steps * Period);
    const bool Rounded = Fps < Grid.FpsLimit - 0.5 || Fps > Grid.FpsLimit + 0.5;
    return Out + fextl::fmt::format("# cap: screen refresh {:.1f} Hz (GameNative's vsync.txt, period {:.3f} ms); the cap layer rounds fps_limit {} "
                              "({:.1f} ms a frame) to whole refreshes: one frame every {} refresh{} = {:.1f} FPS{}\n",
                              Hz, Period / 1e6, Grid.FpsLimit, Target / 1e6, Steps, Steps == 1 ? "" : "es", Fps,
                              Rounded ? fextl::fmt::format(" (not {}: {} FPS needs a refresh rate that is a whole multiple of it, "
                                                           "such as {} Hz)",
                                                           Grid.FpsLimit, Grid.FpsLimit, Grid.FpsLimit * ((90 + Grid.FpsLimit - 1) / Grid.FpsLimit)) :
                                        fextl::string {});
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
      const int Limit = ReadFpsLimit(Config, &Caps);
      if (Limit < 0) {
        Caps = "can't be read";
      } else {
        Grid.FpsLimit = Limit;
      }
    }

    Grid.ConfigPath = Config;
    Grid.LayerLoaded = !Layer.empty();
    if (Layer.empty() && Config.empty()) {
      return "# cap: GameNative's frame cap layer isn't loaded (no lsfg mapping, no LSFG_CONFIG)\n";
    }
    if (!Config.empty()) {
      const size_t Slash = Config.rfind('/');
      Grid.VsyncPath = (Slash == fextl::string::npos ? fextl::string {} : Config.substr(0, Slash + 1)) + "vsync.txt";
    }
    return fextl::fmt::format("# cap: GameNative's frame cap layer {} ({}); LSFG_CONFIG {}: {}\n", Layer.empty() ? "isn't mapped" : "is loaded",
                              Layer.empty() ? "-" : Layer, Config.empty() ? "-" : Config, Caps.empty() ? "-" : Caps) +
           CapGridNote(true);
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

  const char* CopyKindName(uint32_t Kind) {
    switch (Kind) {
    case 1: return "device";
    case 2: return "pixel shader dxbc";
    case 3: return "stage dxbc";
    case 4: return "pixel shader spirv";
    case 5: return "driver code";
    case 6: return "driver statistics";
    case 7: return "pso";
    case 8: return "root signature";
    case 9: return "note";
    case 10: return "compute shader dxbc";
    case 11: return "compute shader spirv";
    default: return "unknown";
    }
  }

  // Binary kinds go out as base64; text kinds as lines. Driver code is text unless vkd3d marked it binary (Other 1).
  bool CopyIsText(const CopyBlob& B) {
    return B.Kind == 1 || B.Kind == 6 || B.Kind == 7 || B.Kind == 9 || (B.Kind == 5 && B.Other != 1);
  }

  void CopyBase64(fextl::string& Out, uint32_t Index, const uint8_t* Bytes, size_t Size) {
    static constexpr char Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Out += fextl::fmt::format("  sc {} ", Index);
    for (size_t i = 0; i < Size; i += 3) {
      const uint32_t b0 = Bytes[i];
      const uint32_t b1 = i + 1 < Size ? Bytes[i + 1] : 0;
      const uint32_t b2 = i + 2 < Size ? Bytes[i + 2] : 0;
      const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
      Out += Digits[(v >> 18) & 63];
      Out += Digits[(v >> 12) & 63];
      Out += i + 1 < Size ? Digits[(v >> 6) & 63] : '=';
      Out += i + 2 < Size ? Digits[v & 63] : '=';
    }
    Out += '\n';
  }

  // Writes the next part of the shader copy table, at most CopyBytesPerSecond of it.
  void CopyLines(fextl::string& Out) {
    CopyHeader H {};
    if (!Copy.Address || !ReadMemory(Copy.Address, &H, sizeof(H)) || H.Magic != CopyMagic || H.Version != CopyVersion) {
      return;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!(H.Flags & FlagOn)) {
      return;
    }
    if (!Copy.Announced) {
      Copy.Announced = true;
      Out += fextl::fmt::format("# shader copy: FEX's VKD3D copies {} listed pixel shaders (FEX_VKD3D_SHADER_COPY), and from VKD3D fex22 "
                                "on every compute shader while the GPU pass meter runs (VKD3D_FEX_COPY_COMPUTE=0 turns that off), when "
                                "the game creates their PSOs; each copy is a 'shader copy N begin' note, '  sc N' lines (base64 bytes, or "
                                "'|' text lines with '+' continuations) and a 'shader copy N end' note with the byte count and "
                                "FNV-1a check\n",
                                H.Listed);
    }
    if (H.Dropped && !Copy.DroppedNoted) {
      Copy.DroppedNoted = true;
      Out += fextl::fmt::format("# shader copy: {} copies didn't fit vkd3d's table or byte budget and are missing\n", H.Dropped);
    }

    const size_t Start = Out.size();
    const uint32_t Count = std::min(H.Count, CopyMax);
    while (Out.size() - Start < CopyBytesPerSecond) {
      if (!Copy.InBlob) {
        if (Copy.Next >= Count) {
          return;
        }
        if (!ReadMemory(Copy.Address + sizeof(CopyHeader) + Copy.Next * sizeof(CopyBlob), &Copy.Blob, sizeof(CopyBlob))) {
          return;
        }
        if (Copy.Blob.Size > (64u << 20)) {
          Out += fextl::fmt::format("# shader copy {}: {} bytes is more than a copy can be, skipped\n", Copy.Next, Copy.Blob.Size);
          ++Copy.Next;
          continue;
        }
        Copy.Data.resize(Copy.Blob.Size);
        if (Copy.Blob.Size && !ReadMemory(Copy.Blob.Data, Copy.Data.data(), Copy.Blob.Size)) {
          Out += fextl::fmt::format("# shader copy {}: its {} bytes at {:X} can't be read, skipped\n", Copy.Next, Copy.Blob.Size, Copy.Blob.Data);
          ++Copy.Next;
          continue;
        }
        const auto& B = Copy.Blob;
        Out += fextl::fmt::format("# shader copy {} begin: {} of pixel shader {:016x} (rank {}), stage {:#x}, other {:016x}, {} bytes, "
                                  "{}\n",
                                  Copy.Next, CopyKindName(B.Kind), B.Hash, B.Rank, B.Stage, B.Other, B.Size, CopyIsText(B) ? "text" : "base64");
        Copy.InBlob = true;
        Copy.Offset = 0;
        Copy.Check = 14695981039346656037ull;
      }

      const auto& B = Copy.Blob;
      const uint8_t* Bytes = Copy.Data.data();
      const size_t Size = Copy.Data.size();
      while (Copy.Offset < Size && Out.size() - Start < CopyBytesPerSecond) {
        size_t End;
        if (CopyIsText(B)) {
          // One line of text, cut at CopyTextLine.
          End = Copy.Offset;
          while (End < Size && Bytes[End] != '\n' && End - Copy.Offset < CopyTextLine) {
            ++End;
          }
          const bool Continued = Copy.Offset > 0 && Bytes[Copy.Offset - 1] != '\n';
          Out += fextl::fmt::format("  sc {} {}", Copy.Next, Continued ? '+' : '|');
          // The check covers the text as written: carriage returns and zeros become spaces.
          for (size_t i = Copy.Offset; i < End; ++i) {
            const char c = Bytes[i] == '\r' || Bytes[i] == '\0' ? ' ' : static_cast<char>(Bytes[i]);
            Out += c;
            Copy.Check = (Copy.Check ^ static_cast<uint8_t>(c)) * 1099511628211ull;
          }
          Out += '\n';
          if (End < Size && Bytes[End] == '\n') {
            Copy.Check = (Copy.Check ^ '\n') * 1099511628211ull;
            ++End;
          }
        } else {
          End = std::min(Size, Copy.Offset + CopyBase64Line);
          CopyBase64(Out, Copy.Next, Bytes + Copy.Offset, End - Copy.Offset);
          for (size_t i = Copy.Offset; i < End; ++i) {
            Copy.Check = (Copy.Check ^ Bytes[i]) * 1099511628211ull;
          }
        }
        Copy.Offset = End;
      }
      if (Copy.Offset >= Size) {
        Out += fextl::fmt::format("# shader copy {} end: {} bytes, fnv1a {:016x}\n", Copy.Next, Size, Copy.Check);
        Copy.InBlob = false;
        Copy.Data.clear();
        Copy.Data.shrink_to_fit();
        ++Copy.Next;
      }
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
      Copy = {};
      Copy.Address = FindExport(Module, "vkd3d_fex_shader_copy");
      Grid.DataAddress = FindExport(Module, "vkd3d_fex_cap_grid");
      Grid.StepWritten = -1;
      Grid.OldNoted = false;
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
        Out += fextl::fmt::format("# vkd3d: d3d12core.dll at {:X} went away{}\n", ModuleAddress,
                                  Copy.InBlob ? fextl::fmt::format(" (shader copy {} cut off)", Copy.Next) : "");
        ModuleAddress = 0;
        Announced = false;
        for (auto& Other : Sources) {
          Other.Address = 0;
        }
        Copy = {};
        Grid.DataAddress = 0;
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
      // fex11 and older don't know the flag, so off there can also mean an older VKD3D component.
      const char* DrawnState = PresentFlags & FlagPresentDrawn ? "on" : "off (or VKD3D older than fex12)";
      const char* ResyncState = PresentFlags & FlagLatencyResync ? "on" : "off (or VKD3D older than fex16)";
      const char* AheadState = GpuFlags & FlagMemoryAhead ? "on" : "off (or VKD3D older than fex17)";
      CopyHeader CopyH {};
      const bool HasCopy = Copy.Address && ReadMemory(Copy.Address, &CopyH, sizeof(CopyH)) && CopyH.Magic == CopyMagic;
      const char* CopyState = !HasCopy ? "missing (VKD3D older than fex13)" : CopyH.Flags & FlagOn ? "on" : "off";
      Out.insert(0, fextl::fmt::format("# vkd3d: FEX's VKD3D build is loaded (d3d12core.dll at {:X}): clear folding {}, "
                                       "GPU pass meter {}, present timeline {}, cap pre-sleep {}, zero fills of never-used memory "
                                       "skipped {}, frames to the screen only once drawn {}, shader copy {}, frame credit resync {}, "
                                       "GPU memory made ahead {}, frame start pacing {}\n",
                                       ModuleAddress, PassFlags & FlagFoldClears ? "on" : "off", PassFlags & FlagOn ? "on" : "off",
                                       PresentState, PresentFlags & FlagCapPresleep ? "on" : "off", FreshState, DrawnState, CopyState,
                                       ResyncState, AheadState,
                                       !(PresentFlags & FlagFrameStart) ? "off" :
                                       PresentFlags & FlagMarginCaused ? "on (gradual, margin only from frames it made late: VKD3D fex4 or "
                                                                         "later)" :
                                       PresentFlags & FlagGradualStart ? "on (gradual, VKD3D fex3: an older VKD3D component is selected)" :
                                                                         "on (build 56 style: an older VKD3D component is selected)"));
      Out += CapLayerNote();
      Out += WriteCapGrid();
      if (!Grid.DataAddress && Grid.LayerLoaded && Grid.FpsLimit > 0) {
        Out += "# cap: this VKD3D build (older than fex22) learns the cap's frame time from held presents; FEX can't hand it over\n";
      }
    } else if (Announced && ++Grid.Calls % 5 == 0) {
      Out += CapGridNote(false);
      Out += WriteCapGrid();
    }
    CopyLines(Out);
    return Out;
  }
} // namespace

void Init() {
  FEX::Windows::StatsLogSetLineProvider(Lines);
}
} // namespace Vkd3dReport
