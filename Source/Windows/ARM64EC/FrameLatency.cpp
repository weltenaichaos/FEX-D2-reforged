// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Finds the swapchain frame latency handle the game's main thread waits on and allows more frames in flight
$end_info$
*/

#include "FrameLatency.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

extern "C" {
NTSTATUS WINAPI NtReleaseSemaphore(HANDLE Semaphore, ULONG Count, ULONG* Previous);
NTSTATUS WINAPI NtQuerySemaphore(HANDLE Semaphore, int InfoClass, void* Info, ULONG Length, ULONG* ReturnLength);
NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);

// Module.S ExitFunctionEC: a call into one of these two native functions (x9 there) from the thread whose TEB is
// FEXWaitWatch[1] is recorded in FEXWaitWatch: [0] = calls recorded so far, [2] and [3] unused, then from [4] a ring of
// 256 entries of (handle, timeout, function, x64 return address).
uint64_t FEXWaitWatchTargets[2];
alignas(64) uint64_t FEXWaitWatch[4 + 4 * 256];

// NativeRedirect.cpp: [0] = frame starts counted so far (0 until the frame clock is found).
extern uint64_t FEXFrameMarks[];
}

namespace FrameLatency {
namespace {
  constexpr size_t Ring = 256;
  constexpr uint32_t SyncOnly = SYNCHRONIZE;
  constexpr uint32_t SemaphoreQueryState = 0x0001;
  // vkd3d-proton's frame latency semaphore: CreateSemaphoreA(NULL, latency, DXGI_MAX_SWAP_CHAIN_BUFFERS, NULL).
  constexpr ULONG LatencySemaphoreMax = 16;
  constexpr size_t CPUAreaOffset = 0x1788;    // TEB->ChpeV2CpuAreaInfo
  constexpr size_t NativeTargetOffset = 0x50; // ChpeV2CpuAreaInfo->EmulatorDataInline, see ExitFunctionEC

  struct WaitCall {
    uint64_t Handle;
    uint64_t Timeout;
    uint64_t Function;
    uint64_t Caller;
  };

  struct Config {
    int Extra; // releases to add to a frame latency handle; 0 = watch only
    bool StatsLog;
    uint64_t MainTEB;
    uint64_t MainImage;
    uint64_t MainImageEnd;
  };
  Config C {};
  bool Requested {};

  // What a handle is, from NtQueryObject (and NtQuerySemaphore for semaphores).
  struct HandleInfo {
    fextl::string Type;
    uint32_t Access {};
    bool Semaphore {};
    bool HaveCount {};
    ULONG Count {};
    ULONG Max {};
    bool LatencyHandle() const {
      return Semaphore && Access == SyncOnly && (!HaveCount || Max == LatencySemaphoreMax);
    }
  };

  // The handle released (0 = none yet) and how many times, for the stats line.
  std::atomic<uint64_t> AppliedHandle {};
  std::atomic<uint32_t> AppliedTimes {};

  WaitCall ReadCall(uint64_t Index) {
    const uint64_t* Slot = &FEXWaitWatch[4 + 4 * (Index & (Ring - 1))];
    return {Slot[0], Slot[1], Slot[2], Slot[3]};
  }

  uint64_t CallCount() {
    return __atomic_load_n(&FEXWaitWatch[0], __ATOMIC_ACQUIRE);
  }

  fextl::string Ascii(const UNICODE_STRING& Text) {
    fextl::string Result;
    for (USHORT i = 0; Text.Buffer && i < Text.Length / sizeof(WCHAR); ++i) {
      const WCHAR c = Text.Buffer[i];
      Result.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
    }
    return Result;
  }

  // Opens Handle again with the rights to query and release it, if it is a semaphore. The caller closes it.
  HANDLE OpenSemaphoreFor(uint64_t Handle) {
    HANDLE Dup {};
    const NTSTATUS Status = NtDuplicateObject(NtCurrentProcess(), reinterpret_cast<HANDLE>(Handle), NtCurrentProcess(), &Dup,
                                              SemaphoreQueryState | SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, 0, 0);
    return Status == STATUS_SUCCESS ? Dup : nullptr;
  }

  HandleInfo Describe(uint64_t Handle) {
    HandleInfo Info;
    alignas(8) uint8_t Buffer[512] {};
    if (NtQueryObject(reinterpret_cast<HANDLE>(Handle), ObjectTypeInformation, Buffer, sizeof(Buffer), nullptr) == STATUS_SUCCESS) {
      Info.Type = Ascii(reinterpret_cast<const PUBLIC_OBJECT_TYPE_INFORMATION*>(Buffer)->TypeName);
    } else {
      Info.Type = "unknown";
    }
    PUBLIC_OBJECT_BASIC_INFORMATION Basic {};
    if (NtQueryObject(reinterpret_cast<HANDLE>(Handle), ObjectBasicInformation, &Basic, sizeof(Basic), nullptr) == STATUS_SUCCESS) {
      Info.Access = Basic.GrantedAccess;
    }
    Info.Semaphore = Info.Type == "Semaphore";
    if (Info.Semaphore) {
      if (HANDLE Dup = OpenSemaphoreFor(Handle)) {
        ULONG Counts[2] {};
        if (NtQuerySemaphore(Dup, 0, Counts, sizeof(Counts), nullptr) == STATUS_SUCCESS) {
          Info.HaveCount = true;
          Info.Count = Counts[0];
          Info.Max = Counts[1];
        }
        NtClose(Dup);
      }
    }
    return Info;
  }

  fextl::string DescribeText(const HandleInfo& Info) {
    fextl::string Text = fextl::fmt::format("{}, access {:#x}{}", Info.Type, Info.Access, Info.Access == SyncOnly ? " (wait only)" : "");
    if (Info.HaveCount) {
      Text += fextl::fmt::format(", count {} of {}", Info.Count, Info.Max);
    }
    return Text;
  }

  fextl::string CallerText(uint64_t Caller) {
    if (Caller >= C.MainImage && Caller < C.MainImageEnd) {
      return fextl::fmt::format("main exe+{:X}", Caller - C.MainImage);
    }
    return fextl::fmt::format("{:X}", Caller);
  }

  // Export Name of the module at Base: its entry, the code a hybrid-patchable entry's x64 thunk jumps to, and the ARM64EC
  // code the CHPE redirection table maps it to. ExitFunctionEC sees one of these.
  size_t ExportTargets(uint64_t Base, const char* Name, uint64_t (&Out)[3]) {
    size_t N {};
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE) {
      return 0;
    }
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return 0;
    }
    const auto& Dir = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!Dir.VirtualAddress || !Dir.Size) {
      return 0;
    }
    const auto* Exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(Base + Dir.VirtualAddress);
    const auto* Names = reinterpret_cast<const uint32_t*>(Base + Exports->AddressOfNames);
    const auto* Ordinals = reinterpret_cast<const uint16_t*>(Base + Exports->AddressOfNameOrdinals);
    const auto* Functions = reinterpret_cast<const uint32_t*>(Base + Exports->AddressOfFunctions);
    uint32_t RVA {};
    for (uint32_t i = 0; i < Exports->NumberOfNames; ++i) {
      if (strcmp(reinterpret_cast<const char*>(Base + Names[i]), Name) == 0 && Ordinals[i] < Exports->NumberOfFunctions) {
        RVA = Functions[Ordinals[i]];
        break;
      }
    }
    if (!RVA || (RVA >= Dir.VirtualAddress && RVA < Dir.VirtualAddress + Dir.Size)) {
      return 0;
    }
    Out[N++] = Base + RVA;

    // "mov rax, rsp; mov [rax+20h], rbx; push rbp; pop rbp; jmp target"
    constexpr uint8_t PatchableThunk[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9};
    const auto* Code = reinterpret_cast<const uint8_t*>(Base + RVA);
    if (memcmp(Code, PatchableThunk, sizeof(PatchableThunk)) == 0) {
      int32_t Rel {};
      memcpy(&Rel, Code + sizeof(PatchableThunk), sizeof(Rel));
      Out[N++] = Base + RVA + sizeof(PatchableThunk) + 4 + static_cast<int64_t>(Rel);
    }

    const auto& LoadConfigDir = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr size_t CHPEFieldEnd =
      offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64::CHPEMetadataPointer);
    if (LoadConfigDir.VirtualAddress && LoadConfigDir.Size >= CHPEFieldEnd) {
      const auto* LoadConfig = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(Base + LoadConfigDir.VirtualAddress);
      if (LoadConfig->Size >= CHPEFieldEnd && LoadConfig->CHPEMetadataPointer) {
        const auto* Metadata = reinterpret_cast<const IMAGE_ARM64EC_METADATA*>(LoadConfig->CHPEMetadataPointer);
        const auto* Entries = reinterpret_cast<const IMAGE_ARM64EC_REDIRECTION_ENTRY*>(Base + Metadata->RedirectionMetadata);
        for (ULONG i = 0; Metadata->RedirectionMetadata && i < Metadata->RedirectionMetadataCount && i < 65536; ++i) {
          if (Entries[i].Source == RVA && Entries[i].Destination) {
            Out[N++] = Base + Entries[i].Destination;
            break;
          }
        }
      }
    }
    return N;
  }

  uint64_t ModuleBase(const wchar_t* Name) {
    UNICODE_STRING Text {};
    size_t Length {};
    while (Name[Length]) {
      ++Length;
    }
    Text.Buffer = const_cast<wchar_t*>(Name);
    Text.Length = static_cast<USHORT>(Length * sizeof(wchar_t));
    Text.MaximumLength = Text.Length + sizeof(wchar_t);
    HMODULE Module {};
    return LdrGetDllHandle(nullptr, 0, &Text, &Module) == STATUS_SUCCESS ? reinterpret_cast<uint64_t>(Module) : 0;
  }

  // The native function the main thread entered last (ExitFunctionEC stores it in its CPU area). Read through
  // NtReadVirtualMemory: the main thread may have exited and its TEB be gone.
  uint64_t MainNativeTarget() {
    uint64_t CPUArea {}, Target {};
    SIZE_T Read {};
    if (NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<const void*>(C.MainTEB + CPUAreaOffset), &CPUArea, sizeof(CPUArea), &Read) !=
          STATUS_SUCCESS ||
        !CPUArea ||
        NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<const void*>(CPUArea + NativeTargetOffset), &Target, sizeof(Target), &Read) !=
          STATUS_SUCCESS) {
      return 0;
    }
    return Target;
  }

  struct Tally {
    uint32_t Calls;
    uint64_t Timeout;
    uint64_t Function;
    uint64_t Caller;
  };

  // Reads the calls recorded since Cursor into Out.
  void ReadCalls(uint64_t& Cursor, fextl::unordered_map<uint64_t, Tally>& Out) {
    const uint64_t Count = CallCount();
    if (Count - Cursor > Ring) {
      Cursor = Count - Ring;
    }
    for (; Cursor < Count; ++Cursor) {
      const auto Call = ReadCall(Cursor);
      auto& T = Out[Call.Handle];
      ++T.Calls;
      T.Timeout = Call.Timeout;
      T.Function = Call.Function;
      T.Caller = Call.Caller;
    }
  }

  void* WatchThread(void*) {
    const char* Names[2] = {"WaitForSingleObjectEx", "WaitForSingleObject"};
    uint64_t Candidates[2][3] {};
    size_t CandidateCount[2] {};
    bool Resolved {};
    bool Watching {};
    uint64_t Cursor {};
    uint64_t FramesAt = FEXFrameMarks[0];
    fextl::vector<uint64_t> LastBusy;
    uint32_t Tick {};
    fextl::unordered_map<uint64_t, Tally> Window;
    fextl::unordered_map<uint64_t, bool> Judged; // handle -> was a frame latency handle

    for (;; ++Tick) {
      LARGE_INTEGER Interval {.QuadPart = -200'000}; // 20ms
      NtDelayExecution(FALSE, &Interval);

      // Find which address ExitFunctionEC sees for the two wait functions: the one the main thread is seen entering.
      if (!Resolved && Tick % 5 == 0) {
        if (const uint64_t KernelBase = ModuleBase(L"kernelbase.dll")) {
          for (size_t i = 0; i < 2; ++i) {
            CandidateCount[i] = ExportTargets(KernelBase, Names[i], Candidates[i]);
          }
          Resolved = true;
          FEX::Windows::StatsLogNote(fextl::fmt::format("latency: kernelbase at {:X}; WaitForSingleObjectEx entries {}, "
                                                        "WaitForSingleObject "
                                                        "entries {}",
                                                        KernelBase, CandidateCount[0], CandidateCount[1]));
        }
      }
      if (Resolved && (!FEXWaitWatchTargets[0] || (!FEXWaitWatchTargets[1] && Tick % 5 == 0))) {
        const uint64_t Target = MainNativeTarget();
        for (size_t i = 0; i < 2; ++i) {
          for (size_t j = 0; Target && !FEXWaitWatchTargets[i] && j < CandidateCount[i]; ++j) {
            if (Candidates[i][j] == Target) {
              __atomic_store_n(&FEXWaitWatchTargets[i], Target, __ATOMIC_RELEASE);
              FEX::Windows::StatsLogNote(
                fextl::fmt::format("latency: main thread seen in kernelbase!{} (entry {:X}), watching its calls", Names[i], Target));
            }
          }
        }
        if (!Watching && (FEXWaitWatchTargets[0] || FEXWaitWatchTargets[1])) {
          Watching = true;
          Cursor = CallCount();
          FramesAt = FEXFrameMarks[0];
        }
      }
      if (!Watching || Tick % 50 != 0) {
        continue;
      }

      // Once a second: every handle the main thread waited on 10 times or more in this second and the one before.
      Window.clear();
      ReadCalls(Cursor, Window);
      const uint64_t Frames = FEXFrameMarks[0] - FramesAt;
      FramesAt = FEXFrameMarks[0];
      fextl::vector<uint64_t> Busy;
      for (const auto& [Handle, T] : Window) {
        if (T.Calls >= 10) {
          Busy.push_back(Handle);
        }
      }
      for (const uint64_t Handle : Busy) {
        if (std::find(LastBusy.begin(), LastBusy.end(), Handle) == LastBusy.end() || Judged.contains(Handle)) {
          continue;
        }
        const auto& T = Window[Handle];
        const HandleInfo Info = Describe(Handle);
        const bool Latency = Info.LatencyHandle();
        Judged[Handle] = Latency;
        const fextl::string Seen =
          fextl::fmt::format("main thread waits on handle {:#x} {}x/s (frames {}/s) in {} from {}, timeout {}: {}", Handle, T.Calls, Frames,
                             T.Function == FEXWaitWatchTargets[1] ? Names[1] : Names[0], CallerText(T.Caller),
                             static_cast<int64_t>(static_cast<int32_t>(T.Timeout)), DescribeText(Info));
        if (!Latency) {
          FEX::Windows::StatsLogNote(fextl::fmt::format("latency: {}; not a frame latency handle, left alone", Seen));
          continue;
        }
        if (!C.Extra) {
          FEX::Windows::StatsLogNote(
            fextl::fmt::format("latency: {} = a swapchain frame latency handle; left as the game set it (FEX_FRAMELATENCY=off)", Seen));
          continue;
        }
        HANDLE Dup = OpenSemaphoreFor(Handle);
        ULONG Previous {};
        const NTSTATUS Status = Dup ? NtReleaseSemaphore(Dup, static_cast<ULONG>(C.Extra), &Previous) : STATUS_ACCESS_DENIED;
        if (Dup) {
          NtClose(Dup);
        }
        if (Status == STATUS_SUCCESS) {
          AppliedTimes.store(static_cast<uint32_t>(C.Extra), std::memory_order_relaxed);
          AppliedHandle.store(Handle, std::memory_order_release);
        }
        FEX::Windows::StatsLogNote(
          fextl::fmt::format("latency: {} = a swapchain frame latency handle; released it {} more time{}: up to {} "
                             "frames in flight (count was {}), status={:#x} (FEX_FRAMELATENCY=off turns this off)",
                             Seen, C.Extra, C.Extra == 1 ? "" : "s", C.Extra + 1, Previous, static_cast<uint32_t>(Status)));
      }
      LastBusy = std::move(Busy);
    }
    return nullptr;
  }

  // Stats log side (log thread only): calls per handle since the last line, and the main thread's samples in each wait.
  struct LineState {
    uint64_t Cursor;
    bool Started;
    uint32_t Samples;
    fextl::unordered_map<uint64_t, uint32_t> WaitSamples;
    fextl::unordered_map<uint64_t, Tally> Calls;
    fextl::unordered_map<uint64_t, fextl::string> Kinds;
  };
  LineState* Line {};

  void SampleHook(uint64_t Target) {
    ++Line->Samples;
    if (Target && (Target == FEXWaitWatchTargets[0] || Target == FEXWaitWatchTargets[1])) {
      const uint64_t Count = CallCount();
      if (Count) {
        ++Line->WaitSamples[ReadCall(Count - 1).Handle];
      }
    }
  }

  fextl::string WaitsLine() {
    auto& L = *Line;
    if (!FEXWaitWatchTargets[0] && !FEXWaitWatchTargets[1]) {
      return {};
    }
    if (!L.Started) {
      L.Started = true;
      L.Cursor = CallCount();
      L.Samples = 0;
      L.WaitSamples.clear();
      return {};
    }
    L.Calls.clear();
    ReadCalls(L.Cursor, L.Calls);
    for (const auto& [Handle, N] : L.WaitSamples) {
      L.Calls.try_emplace(Handle);
    }
    fextl::vector<std::pair<uint64_t, uint64_t>> Order; // (sort key, handle)
    for (const auto& [Handle, T] : L.Calls) {
      const auto It = L.WaitSamples.find(Handle);
      Order.push_back({(It == L.WaitSamples.end() ? 0 : It->second) * 1000ull + T.Calls, Handle});
    }
    std::sort(Order.begin(), Order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const uint64_t Applied = AppliedHandle.load(std::memory_order_acquire);
    fextl::string Text = "  waits main (handle kind calls ms@caller timeout):";
    size_t Shown {};
    for (const auto& [Key, Handle] : Order) {
      if (Shown++ == 5) {
        break;
      }
      auto Kind = L.Kinds.find(Handle);
      if (Kind == L.Kinds.end()) {
        const auto Info = Describe(Handle);
        Kind = L.Kinds.emplace(Handle, fextl::fmt::format("{}{}", Info.Type, Info.LatencyHandle() ? "/latency" : "")).first;
      }
      const auto& T = L.Calls[Handle];
      const auto It = L.WaitSamples.find(Handle);
      const uint64_t Ms = L.Samples && It != L.WaitSamples.end() ? It->second * 1000ull / L.Samples : 0;
      Text += fextl::fmt::format(
        " {:#x} {}{} {}x {}ms", Handle, Kind->second,
        Handle == Applied ? fextl::fmt::format("+{}", AppliedTimes.load(std::memory_order_relaxed)) : fextl::string {}, T.Calls, Ms);
      if (T.Calls) {
        Text += fextl::fmt::format("@{} t={}", CallerText(T.Caller), static_cast<int64_t>(static_cast<int32_t>(T.Timeout)));
      }
      Text += ';';
    }
    if (L.Kinds.size() > 256) {
      L.Kinds.clear();
    }
    L.Samples = 0;
    L.WaitSamples.clear();
    return Shown ? Text + "\n" : fextl::string {};
  }
} // namespace

bool WantsFrameMarks() {
  return Requested;
}

void Init(uint64_t MainImageBase, bool StatsLog) {
  const char* Value = FEX::Windows::Switch("FEX_FRAMELATENCY");
  int Latency = Value && Value[0] >= '0' && Value[0] <= '9' ? atoi(Value) : 1;
  Latency = std::clamp(Latency, 1, 4);
  C.Extra = Latency - 1;
  C.StatsLog = StatsLog;
  if (!C.Extra && !StatsLog) {
    return;
  }
  Requested = true;
  C.MainTEB = reinterpret_cast<uint64_t>(NtCurrentTeb());
  C.MainImage = MainImageBase;
  if (C.MainImage) {
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(C.MainImage);
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(C.MainImage + Dos->e_lfanew);
    C.MainImageEnd = C.MainImage + Nt->OptionalHeader.SizeOfImage;
  }
  FEXWaitWatch[1] = C.MainTEB;

  auto Env = [](const char* Name) {
    const char* V = getenv(Name);
    return fextl::string(V && V[0] ? V : "-");
  };
  FEX::Windows::StatsLogNote(
    fextl::fmt::format("latency: {}; environment VKD3D_FRAME_RATE={} DXVK_FRAME_RATE={} VKD3D_SWAPCHAIN_LATENCY_FRAMES={} VKD3D_CONFIG={}",
                       C.Extra ? fextl::fmt::format("a swapchain frame latency handle the main thread waits on gets {} more frame{} in "
                                                    "flight "
                                                    "(FEX_FRAMELATENCY={}; off leaves it at the game's)",
                                                    C.Extra, C.Extra == 1 ? "" : "s", Latency) :
                                 fextl::string("frame latency left alone, main thread waits only logged (FEX_FRAMELATENCY=off)"),
                       Env("VKD3D_FRAME_RATE"), Env("DXVK_FRAME_RATE"), Env("VKD3D_SWAPCHAIN_LATENCY_FRAMES"), Env("VKD3D_CONFIG")));

  if (StatsLog) {
    Line = new LineState {};
    FEX::Windows::StatsLogSetMainSampleHook(SampleHook);
    FEX::Windows::StatsLogSetLineProvider(WaitsLine);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(WatchThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXFrameLatency");
}
} // namespace FrameLatency
