// SPDX-License-Identifier: MIT
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/SHMStats.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

// Exported by ntdll (Defs/ntdll.def) but not declared by the bundled headers.
extern "C" NTSTATUS WINAPI NtOpenThread(HANDLE* Handle, ACCESS_MASK Access, const OBJECT_ATTRIBUTES* Attr, const CLIENT_ID* Id);
extern "C" NTSTATUS WINAPI RtlSystemTimeToLocalTime(const LARGE_INTEGER* SystemTime, LARGE_INTEGER* LocalTime);
extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);

namespace FEX::Windows {
namespace {
  constexpr size_t MaxTracked = 512;
  constexpr size_t MaxPrinted = 8;
  // Threads whose InSimulation flag is sampled during the next second: the busiest ones of the previous second.
  constexpr size_t MaxSampled = 16;
  constexpr int64_t SampleInterval100ns = 20'000; // 2ms
  // Timeline (on unless FEX_STATSLOG_TIMELINE=0): for the TimelineThreads busiest threads, one character per 10ms bin
  // saying how much of that bin the thread spent in emulated x86 code. Together with the longest single JIT request,
  // exception and disk cache lookup per second (and when in the second it ended) this shows what a short hitch was made of.
  constexpr size_t TimelineThreads = 6;
  constexpr size_t TimelineBins = 100;
  // Longest single events shorter than this are not printed.
  constexpr double MaxEventPrintMs = 2.0;

  // FEX_STATSLOG_PROFILE=1: every tick, note which JIT block each of the ProfiledThreads busiest threads is in while it runs
  // emulated code, by reading the block header pointer the JIT stores on every block entry. Nothing is suspended. Every
  // ProfileWindowSeconds the hottest blocks are written with their guest RIP and guest code bytes.
  constexpr size_t ProfiledThreads = 4;
  constexpr uint64_t ProfileWindowSeconds = 30;
  constexpr size_t ProfilePrinted = 40;
  constexpr size_t ProfileBytes = 128;
  // The hottest ProfileDumped blocks of each window also get their whole guest code written once ("#B" lines), so the
  // functions behind them can be identified offline.
  constexpr size_t ProfileDumped = 8;
  constexpr size_t ProfileDumpMaxBytes = 4096;

  // Mirrors of FEXCore's CPUBackend::JITCodeHeader / JITCodeTail (Interface/Core/CPUBackend.h).
  struct JITCodeHeaderMirror {
    uint32_t OffsetToBlockTail;
  };
  struct JITCodeTailMirror {
    size_t Size;
    uint64_t RIP;
    size_t GuestSize;
  };

  // Every counter we report, sampled once per interval and diffed against the previous sample.
  struct Sample {
    uint32_t TID;
    uint64_t CPUTime100ns;
    uint64_t JITTime;
    uint64_t JITCount;
    uint64_t SignalTime;
    uint64_t SIGBUSCount;
    uint64_t SMCCount;
    uint64_t CacheMissCount;
    uint64_t CacheLockTime;
    uint64_t FloatFallbackCount;
    uint64_t DiskCacheHitCount;
    uint64_t DiskCacheMissCount;
    uint64_t DiskCacheLookupTime;
    uint64_t DiskCacheKeyTime;
    uint64_t DiskCacheReadTime;
    uint64_t DiskCacheFileReadCount;
    uint64_t DiskCacheRelocTime;
    uint64_t DiskCacheInstallTime;
    // Longest single events of the last second (cycles) and when they ended (cycle counter), see ThreadStats.
    uint64_t MaxJITTime;
    uint64_t MaxJITAt;
    uint64_t MaxSignalTime;
    uint64_t MaxSignalAt;
    uint64_t MaxDiskCacheLookupTime;
    uint64_t MaxDiskCacheLookupAt;
  };

  // Tells whether a thread is running translated x86 code (InSimulation set) or native ARM64EC code (Wine, VKD3D, the
  // driver, FEX itself outside the JIT) by polling its CHPE CPU area.
  struct SampledThread {
    uint32_t TID;
    const FEXCore::SHMStats::ThreadStats* Slot;
    const volatile BOOLEAN* InSimulation;
    uint32_t Samples;
    uint32_t InSimulationSamples;
    const volatile uint64_t* BlockHeader;
    bool IsMain;
    uint8_t BinSamples[TimelineBins];
    uint8_t BinInSimulation[TimelineBins];
  };

  struct ProfileCount {
    uint32_t Main;
    uint32_t Other;
  };

  // Preload probe: once per second the log thread scans the main executable's pages and remembers the second each page
  // was first seen readable. A disk cache hit in the executable whose page was already readable at an earlier scan could
  // have been loaded in the background ahead of time ("early"); one whose page only just became readable could not
  // ("late"). D2R's protector decrypts its code page by page, so this tells how much a background preload could cover.
  struct ImageWatch {
    uint64_t Base;
    uint64_t Size;
    std::atomic<uint32_t>* FirstReadable; // second of the first scan that saw the page readable, 0 = not yet
    std::atomic<uint32_t> Second;         // second of the latest completed scan
  };
  std::atomic<ImageWatch*> Watch {};
  uint64_t WatchBase {};
  std::atomic<uint64_t> HitsEarly {}, HitsLate {}, HitsOther {}, HitsBoot {};

  bool IsReadable(const MEMORY_BASIC_INFORMATION& Info) {
    return Info.State == MEM_COMMIT && !(Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (Info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
  }

  // Runs on the log thread only. Returns how many of the image's pages are readable right now.
  uint64_t ScanImage(uint32_t Seconds, uint64_t& TotalPages) {
    auto* W = Watch.load(std::memory_order_acquire);
    if (!W) {
      if (!WatchBase) {
        return 0;
      }
      // The image is one allocation: walk its regions to find its size.
      uint64_t End = WatchBase;
      MEMORY_BASIC_INFORMATION Info {};
      while (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(End), MemoryBasicInformation, &Info, sizeof(Info), nullptr) ==
               STATUS_SUCCESS &&
             reinterpret_cast<uint64_t>(Info.AllocationBase) == WatchBase && Info.RegionSize) {
        End = reinterpret_cast<uint64_t>(Info.BaseAddress) + Info.RegionSize;
      }
      if (End == WatchBase) {
        return 0;
      }
      W = new ImageWatch {.Base = WatchBase, .Size = End - WatchBase};
      W->FirstReadable = new std::atomic<uint32_t>[W->Size / 4096]();
      Watch.store(W, std::memory_order_release);
    }

    TotalPages = W->Size / 4096;
    uint64_t Readable {};
    uint64_t Address = W->Base;
    MEMORY_BASIC_INFORMATION Info {};
    while (Address < W->Base + W->Size &&
           NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), MemoryBasicInformation, &Info, sizeof(Info), nullptr) ==
             STATUS_SUCCESS &&
           Info.RegionSize) {
      const uint64_t RegionEnd = std::min(reinterpret_cast<uint64_t>(Info.BaseAddress) + Info.RegionSize, W->Base + W->Size);
      if (IsReadable(Info)) {
        for (uint64_t Page = (Address - W->Base) / 4096; Page < (RegionEnd - W->Base) / 4096; ++Page) {
          uint32_t Expected = 0;
          W->FirstReadable[Page].compare_exchange_strong(Expected, Seconds, std::memory_order_relaxed);
          ++Readable;
        }
      }
      Address = RegionEnd;
    }
    W->Second.store(Seconds, std::memory_order_release);
    return Readable;
  }

  // CPU clock per core from sysfs (through Wine's Z: drive), in MHz, as "c0/c1/.../c7". Empty when unreadable.
  constexpr int MaxClockCores = 8;
  bool ClocksUnreadable {};

  fextl::string ReadCoreClocks() {
    if (ClocksUnreadable) {
      return {};
    }
    fextl::string Out;
    bool Any {};
    for (int Core = 0; Core < MaxClockCores; ++Core) {
      const auto Path = fextl::fmt::format("Z:\\sys\\devices\\system\\cpu\\cpu{}\\cpufreq\\scaling_cur_freq", Core);
      uint64_t KHz {};
      if (FILE* F = fopen(Path.c_str(), "r")) {
        char Buf[32] {};
        const size_t Len = fread(Buf, 1, sizeof(Buf) - 1, F);
        fclose(F);
        for (size_t i = 0; i < Len && Buf[i] >= '0' && Buf[i] <= '9'; ++i) {
          KHz = KHz * 10 + (Buf[i] - '0');
        }
      }
      Any |= KHz != 0;
      Out += fextl::fmt::format("{}{}", Core ? "/" : "", KHz ? fextl::fmt::format("{}", KHz / 1000) : fextl::string("-"));
    }
    if (!Any) {
      ClocksUnreadable = true;
      return {};
    }
    return Out;
  }

  struct LogState {
    const StatAlloc* Stats;
    FILE* File;
    uint64_t CounterFreq;
    Sample Prev[MaxTracked];
    size_t PrevCount;
    SampledThread Sampled[MaxSampled];
    size_t SampledCount;
    bool Profile;
    bool Timeline;
    uint64_t SecondStart; // cycle counter when the current second's sampling began
    fextl::unordered_map<uint64_t, ProfileCount> ProfileCounts;
    uint32_t ProfileSamples[2];
    fextl::vector<uint64_t> DumpedRIPs;
  };

  std::atomic<fextl::string (*)()> LineProvider {};

  uint64_t GetCounter() {
    uint64_t Result {};
    __asm volatile("isb; mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Result));
    return Result;
  }

  uint64_t GetCounterFrequency() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Result));
    return Result ? Result : 1;
  }

  HANDLE OpenThreadForQuery(uint32_t TID) {
    HANDLE Handle {};
    CLIENT_ID ClientId {
      .UniqueProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId())),
      .UniqueThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(TID)),
    };
    OBJECT_ATTRIBUTES Attr {.Length = sizeof(Attr)};
    if (NtOpenThread(&Handle, THREAD_QUERY_LIMITED_INFORMATION, &Attr, &ClientId) != STATUS_SUCCESS) {
      return nullptr;
    }
    return Handle;
  }

  uint64_t QueryThreadCPUTime(uint32_t TID) {
    HANDLE Handle = OpenThreadForQuery(TID);
    if (!Handle) {
      return 0;
    }
    KERNEL_USER_TIMES Times {};
    uint64_t Result {};
    if (NtQueryInformationThread(Handle, ThreadTimes, &Times, sizeof(Times), nullptr) == STATUS_SUCCESS) {
      Result = Times.KernelTime.QuadPart + Times.UserTime.QuadPart;
    }
    NtClose(Handle);
    return Result;
  }

  fextl::string QueryThreadName(uint32_t TID) {
    HANDLE Handle = OpenThreadForQuery(TID);
    if (!Handle) {
      return {};
    }
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 128 * sizeof(WCHAR)] {};
    fextl::string Result;
    ULONG Length {};
    if (NtQueryInformationThread(Handle, static_cast<THREADINFOCLASS>(38) /* ThreadNameInformation */, Buffer, sizeof(Buffer), &Length) ==
        STATUS_SUCCESS) {
      const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
      for (USHORT i = 0; Name->Buffer && i < Name->Length / sizeof(WCHAR); ++i) {
        const WCHAR c = Name->Buffer[i];
        Result.push_back(c >= 0x20 && c < 0x7f && c != ' ' ? static_cast<char>(c) : '_');
      }
    }
    NtClose(Handle);
    return Result;
  }

  const CHPE_V2_CPU_AREA_INFO* QueryCPUArea(uint32_t TID) {
    HANDLE Handle {};
    CLIENT_ID ClientId {
      .UniqueProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId())),
      .UniqueThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(TID)),
    };
    OBJECT_ATTRIBUTES Attr {.Length = sizeof(Attr)};
    if (NtOpenThread(&Handle, THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, &Attr, &ClientId) != STATUS_SUCCESS) {
      return nullptr;
    }
    THREAD_BASIC_INFORMATION Info {};
    const NTSTATUS Err = NtQueryInformationThread(Handle, ThreadBasicInformation, &Info, sizeof(Info), nullptr);
    NtClose(Handle);
    if (Err != STATUS_SUCCESS || !Info.TebBaseAddress) {
      return nullptr;
    }
    // TEB->ChpeV2CpuAreaInfo, see ThreadCPUArea in ARM64EC/Module.cpp. Wine keeps TEBs mapped after thread exit.
    return *reinterpret_cast<CHPE_V2_CPU_AREA_INFO* const*>(reinterpret_cast<uintptr_t>(Info.TebBaseAddress) + 0x1788);
  }

  // EmulatorData[0] is FEX's CpuStateFrame for the thread, see ThreadCPUArea in ARM64EC/Module.cpp.
  const volatile uint64_t* BlockHeaderPointer(const CHPE_V2_CPU_AREA_INFO* Area) {
    const auto* Frame = Area ? static_cast<const FEXCore::Core::CpuStateFrame*>(Area->EmulatorData[0]) : nullptr;
    return Frame ? &Frame->State.InlineJITBlockHeader : nullptr;
  }

  void ProfileTick(LogState* State) {
    for (size_t i = 0; i < std::min(State->SampledCount, ProfiledThreads); ++i) {
      const auto& T = State->Sampled[i];
      if (!T.BlockHeader || !T.InSimulation || T.Slot->TID.load(std::memory_order_relaxed) != T.TID || !*T.InSimulation) {
        continue;
      }
      const uint64_t Header = *T.BlockHeader;
      if (!Header) {
        continue;
      }
      auto& C = State->ProfileCounts[Header];
      ++(T.IsMain ? C.Main : C.Other);
      ++State->ProfileSamples[T.IsMain ? 0 : 1];
    }
  }

  void WriteProfile(LogState* State, uint64_t Seconds) {
    struct Entry {
      uint64_t Header;
      ProfileCount C;
    };
    fextl::vector<Entry> Entries;
    Entries.reserve(State->ProfileCounts.size());
    for (const auto& [Header, C] : State->ProfileCounts) {
      Entries.push_back({Header, C});
    }
    const size_t Printed = std::min(Entries.size(), ProfilePrinted);
    std::partial_sort(Entries.begin(), Entries.begin() + Printed, Entries.end(),
                      [](const Entry& a, const Entry& b) { return a.C.Main + a.C.Other > b.C.Main + b.C.Other; });

    auto Text = fextl::fmt::format("# profile t={}-{} samples main={} other={} blocks={}\n", Seconds - ProfileWindowSeconds + 1, Seconds,
                                   State->ProfileSamples[0], State->ProfileSamples[1], Entries.size());
    for (size_t i = 0; i < Printed; ++i) {
      const auto& E = Entries[i];
      // The block may have been freed since it was sampled, so read it through NtReadVirtualMemory, which fails safely.
      JITCodeHeaderMirror Header {};
      JITCodeTailMirror Tail {};
      if (NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(E.Header), &Header, sizeof(Header), nullptr) != STATUS_SUCCESS ||
          NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(E.Header + Header.OffsetToBlockTail), &Tail, sizeof(Tail), nullptr) !=
            STATUS_SUCCESS) {
        Text += fextl::fmt::format("#P block={:X} main={} other={} (freed)\n", E.Header, E.C.Main, E.C.Other);
        continue;
      }
      MEMORY_BASIC_INFORMATION Info {};
      NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Tail.RIP), MemoryBasicInformation, &Info, sizeof(Info), nullptr);
      uint8_t Bytes[ProfileBytes] {};
      SIZE_T Read {};
      NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Tail.RIP), Bytes, std::min(sizeof(Bytes), Tail.GuestSize), &Read);
      fextl::string Hex;
      for (SIZE_T b = 0; b < Read; ++b) {
        Hex += fextl::fmt::format("{:02x}", Bytes[b]);
      }
      Text += fextl::fmt::format("#P rip={:X} base={:X} guestsize={} hostsize={} main={} other={} bytes={}\n", Tail.RIP,
                                 reinterpret_cast<uintptr_t>(Info.AllocationBase), Tail.GuestSize, Tail.Size, E.C.Main, E.C.Other, Hex);

      if (i < ProfileDumped && Tail.GuestSize > ProfileBytes &&
          std::find(State->DumpedRIPs.begin(), State->DumpedRIPs.end(), Tail.RIP) == State->DumpedRIPs.end()) {
        fextl::vector<uint8_t> Code(std::min(Tail.GuestSize, ProfileDumpMaxBytes));
        SIZE_T CodeRead {};
        NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Tail.RIP), Code.data(), Code.size(), &CodeRead);
        if (CodeRead == Code.size()) {
          State->DumpedRIPs.push_back(Tail.RIP);
          fextl::string CodeHex;
          for (const auto Byte : Code) {
            CodeHex += fextl::fmt::format("{:02x}", Byte);
          }
          Text += fextl::fmt::format("#B rip={:X} size={} bytes={}\n", Tail.RIP, Code.size(), CodeHex);
        }
      }
    }
    fwrite(Text.data(), 1, Text.size(), State->File);

    State->ProfileCounts.clear();
    State->ProfileSamples[0] = State->ProfileSamples[1] = 0;
  }

  // Waits one second, polling the sampled threads' InSimulation flag every few milliseconds.
  void SampleFor1s(LogState* State) {
    const uint64_t Start = GetCounter();
    const uint64_t End = Start + State->CounterFreq;
    State->SecondStart = Start;
    for (size_t i = 0; i < State->SampledCount; ++i) {
      memset(State->Sampled[i].BinSamples, 0, sizeof(State->Sampled[i].BinSamples));
      memset(State->Sampled[i].BinInSimulation, 0, sizeof(State->Sampled[i].BinInSimulation));
    }
    LARGE_INTEGER Interval {.QuadPart = -SampleInterval100ns};
    do {
      NtDelayExecution(FALSE, &Interval);
      const uint64_t Now = GetCounter();
      const size_t Bin = std::min<size_t>((Now - Start) * TimelineBins / State->CounterFreq, TimelineBins - 1);
      for (size_t i = 0; i < State->SampledCount; ++i) {
        auto& T = State->Sampled[i];
        // FEX clears the slot's TID in ThreadTerm, well before the thread's CPU area goes away.
        if (!T.InSimulation || T.Slot->TID.load(std::memory_order_relaxed) != T.TID) {
          continue;
        }
        const bool In = *T.InSimulation;
        ++T.Samples;
        T.InSimulationSamples += In ? 1 : 0;
        if (T.BinSamples[Bin] < 255) {
          ++T.BinSamples[Bin];
          T.BinInSimulation[Bin] += In ? 1 : 0;
        }
      }
      if (State->Profile) {
        ProfileTick(State);
      }
    } while (GetCounter() < End);
  }

  const SampledThread* FindSampled(const LogState* State, uint32_t TID) {
    for (size_t i = 0; i < State->SampledCount; ++i) {
      if (State->Sampled[i].TID == TID) {
        return &State->Sampled[i];
      }
    }
    return nullptr;
  }

  void* LogThread(void* Arg) {
    auto* State = static_cast<LogState*>(Arg);
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    uint64_t Seconds {};

    fextl::vector<Sample> Current;
    Current.reserve(MaxTracked);
    fextl::vector<const FEXCore::SHMStats::ThreadStats*> Slots;
    Slots.reserve(MaxTracked);

    for (;;) {
      SampleFor1s(State);
      ++Seconds;

      Current.clear();
      Slots.clear();
      State->Stats->ForEachSlot([&](const FEXCore::SHMStats::ThreadStats& Slot) {
        if (Current.size() == MaxTracked) {
          return;
        }
        const uint32_t TID = Slot.TID.load(std::memory_order_relaxed);
        Slots.push_back(&Slot);
        Current.push_back({
          .TID = TID,
          .CPUTime100ns = QueryThreadCPUTime(TID),
          .JITTime = Slot.AccumulatedJITTime,
          .JITCount = Slot.AccumulatedJITCount,
          .SignalTime = Slot.AccumulatedSignalTime,
          .SIGBUSCount = Slot.AccumulatedSIGBUSCount,
          .SMCCount = Slot.AccumulatedSMCCount,
          .CacheMissCount = Slot.AccumulatedCacheMissCount,
          .CacheLockTime = Slot.AccumulatedCacheReadLockTime + Slot.AccumulatedCacheWriteLockTime,
          .FloatFallbackCount = Slot.AccumulatedFloatFallbackCount,
          .DiskCacheHitCount = Slot.AccumulatedDiskCacheHitCount,
          .DiskCacheMissCount = Slot.AccumulatedDiskCacheMissCount,
          .DiskCacheLookupTime = Slot.AccumulatedDiskCacheLookupTime,
          .DiskCacheKeyTime = Slot.AccumulatedDiskCacheKeyTime,
          .DiskCacheReadTime = Slot.AccumulatedDiskCacheReadTime,
          .DiskCacheFileReadCount = Slot.AccumulatedDiskCacheFileReadCount,
          .DiskCacheRelocTime = Slot.AccumulatedDiskCacheRelocTime,
          .DiskCacheInstallTime = Slot.AccumulatedDiskCacheInstallTime,
        });
        // Take the longest single events and reset them for the next second.
        auto& Cur = Current.back();
        auto* Mutable = const_cast<FEXCore::SHMStats::ThreadStats*>(&Slot);
        auto Take = [](uint64_t& Field) {
          return std::atomic_ref<uint64_t>(Field).exchange(0, std::memory_order_relaxed);
        };
        Cur.MaxJITTime = Take(Mutable->MaxJITTime);
        Cur.MaxJITAt = Take(Mutable->MaxJITAt);
        Cur.MaxSignalTime = Take(Mutable->MaxSignalTime);
        Cur.MaxSignalAt = Take(Mutable->MaxSignalAt);
        Cur.MaxDiskCacheLookupTime = Take(Mutable->MaxDiskCacheLookupTime);
        Cur.MaxDiskCacheLookupAt = Take(Mutable->MaxDiskCacheLookupAt);
      });

      struct Delta {
        Sample D;
        double CPUPercent;
        const FEXCore::SHMStats::ThreadStats* Slot;
      };
      fextl::vector<Delta> Deltas;
      Deltas.reserve(Current.size());
      Sample Total {};
      double TotalCPU {};

      for (size_t CurIndex = 0; CurIndex < Current.size(); ++CurIndex) {
        const auto& Cur = Current[CurIndex];
        const Sample* Old = nullptr;
        for (size_t i = 0; i < State->PrevCount; ++i) {
          if (State->Prev[i].TID == Cur.TID) {
            Old = &State->Prev[i];
            break;
          }
        }
        const Sample Zero {};
        if (!Old) {
          Old = &Zero;
        }
        // A reused slot (new thread, same TID) or a reset can make counters go backwards; treat that as a fresh start.
        auto Diff = [](uint64_t New, uint64_t OldV) {
          return New >= OldV ? New - OldV : New;
        };
        Delta Entry {
          .D =
            {
              .TID = Cur.TID,
              .CPUTime100ns = Diff(Cur.CPUTime100ns, Old->CPUTime100ns),
              .JITTime = Diff(Cur.JITTime, Old->JITTime),
              .JITCount = Diff(Cur.JITCount, Old->JITCount),
              .SignalTime = Diff(Cur.SignalTime, Old->SignalTime),
              .SIGBUSCount = Diff(Cur.SIGBUSCount, Old->SIGBUSCount),
              .SMCCount = Diff(Cur.SMCCount, Old->SMCCount),
              .CacheMissCount = Diff(Cur.CacheMissCount, Old->CacheMissCount),
              .CacheLockTime = Diff(Cur.CacheLockTime, Old->CacheLockTime),
              .FloatFallbackCount = Diff(Cur.FloatFallbackCount, Old->FloatFallbackCount),
              .DiskCacheHitCount = Diff(Cur.DiskCacheHitCount, Old->DiskCacheHitCount),
              .DiskCacheMissCount = Diff(Cur.DiskCacheMissCount, Old->DiskCacheMissCount),
              .DiskCacheLookupTime = Diff(Cur.DiskCacheLookupTime, Old->DiskCacheLookupTime),
              .DiskCacheKeyTime = Diff(Cur.DiskCacheKeyTime, Old->DiskCacheKeyTime),
              .DiskCacheReadTime = Diff(Cur.DiskCacheReadTime, Old->DiskCacheReadTime),
              .DiskCacheFileReadCount = Diff(Cur.DiskCacheFileReadCount, Old->DiskCacheFileReadCount),
              .DiskCacheRelocTime = Diff(Cur.DiskCacheRelocTime, Old->DiskCacheRelocTime),
              .DiskCacheInstallTime = Diff(Cur.DiskCacheInstallTime, Old->DiskCacheInstallTime),
              .MaxJITTime = Cur.MaxJITTime,
              .MaxJITAt = Cur.MaxJITAt,
              .MaxSignalTime = Cur.MaxSignalTime,
              .MaxSignalAt = Cur.MaxSignalAt,
              .MaxDiskCacheLookupTime = Cur.MaxDiskCacheLookupTime,
              .MaxDiskCacheLookupAt = Cur.MaxDiskCacheLookupAt,
            },
        };
        Entry.Slot = Slots[CurIndex];
        // 100ns units over a 1s interval: 10'000'000 == 100%.
        Entry.CPUPercent = static_cast<double>(Entry.D.CPUTime100ns) / 100'000.0;
        TotalCPU += Entry.CPUPercent;
        Total.JITTime += Entry.D.JITTime;
        Total.JITCount += Entry.D.JITCount;
        Total.SignalTime += Entry.D.SignalTime;
        Total.SIGBUSCount += Entry.D.SIGBUSCount;
        Total.SMCCount += Entry.D.SMCCount;
        Total.CacheMissCount += Entry.D.CacheMissCount;
        Total.FloatFallbackCount += Entry.D.FloatFallbackCount;
        Total.DiskCacheHitCount += Entry.D.DiskCacheHitCount;
        Total.DiskCacheMissCount += Entry.D.DiskCacheMissCount;
        Total.DiskCacheLookupTime += Entry.D.DiskCacheLookupTime;
        Deltas.push_back(Entry);
      }

      State->PrevCount = std::min(Current.size(), MaxTracked);
      std::copy_n(Current.begin(), State->PrevCount, State->Prev);

      std::sort(Deltas.begin(), Deltas.end(), [](const Delta& a, const Delta& b) { return a.CPUPercent > b.CPUPercent; });

      auto Line = fextl::fmt::format("t={} threads={} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} x87fb={} "
                                     "dc={}/{} dcl={:.1f}ms\n",
                                     Seconds, Current.size(), TotalCPU, Total.JITTime * MsPerTick, Total.JITCount, Total.SMCCount,
                                     Total.SIGBUSCount, Total.SignalTime * MsPerTick, Total.CacheMissCount, Total.FloatFallbackCount,
                                     Total.DiskCacheHitCount, Total.DiskCacheMissCount, Total.DiskCacheLookupTime * MsPerTick);

      size_t Printed {};
      for (const auto& Entry : Deltas) {
        const auto& D = Entry.D;
        const bool Interesting = Entry.CPUPercent >= 3.0 || D.JITCount || D.SMCCount || D.SIGBUSCount || D.DiskCacheHitCount;
        if (!Interesting || Printed == MaxPrinted) {
          continue;
        }
        ++Printed;
        // emu = share of the second this thread spent inside x86 emulation (translated code or FEX's JIT).
        fextl::string Emu = "-";
        if (const auto* T = FindSampled(State, D.TID); T && T->Samples) {
          Emu = fextl::fmt::format("{}%", (T->InSimulationSamples * 100 + T->Samples / 2) / T->Samples);
        }
        Line += fextl::fmt::format("  tid={} {} cpu={:.0f}% emu={} jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} lock={:.1f}ms "
                                   "x87fb={} dc={}\n",
                                   D.TID, QueryThreadName(D.TID), Entry.CPUPercent, Emu, D.JITTime * MsPerTick, D.JITCount, D.SMCCount,
                                   D.SIGBUSCount, D.SignalTime * MsPerTick, D.CacheMissCount, D.CacheLockTime * MsPerTick,
                                   D.FloatFallbackCount, D.DiskCacheHitCount);
        // Longest single events of the second: duration and the 10ms timeline bin in which each one ended.
        fextl::string Max;
        auto AddMax = [&](const char* Name, uint64_t Time, uint64_t At) {
          const double Ms = Time * MsPerTick;
          if (Ms < MaxEventPrintMs) {
            return;
          }
          const int64_t Offset = At > State->SecondStart ? static_cast<int64_t>((At - State->SecondStart) * TimelineBins / State->CounterFreq) : -1;
          Max += fextl::fmt::format(" {}={:.1f}ms@{}", Name, Ms, Offset);
        };
        AddMax("jit", D.MaxJITTime, D.MaxJITAt);
        AddMax("sig", D.MaxSignalTime, D.MaxSignalAt);
        AddMax("dcl", D.MaxDiskCacheLookupTime, D.MaxDiskCacheLookupAt);
        if (!Max.empty()) {
          Line += fextl::fmt::format("    max{}\n", Max);
        }
        // Disk cache lookup breakdown (dcl = whole lookup incl. misses; key = lookup key/anon decode; read = entry reads, file
        // = how many came from the cache file; rel = relocations; inst = putting a hit into the code buffer).
        if (D.DiskCacheHitCount || D.DiskCacheMissCount) {
          Line += fextl::fmt::format("    dc hit={} miss={} dcl={:.1f}ms key={:.1f}ms read={:.1f}ms file={} rel={:.1f}ms inst={:.1f}ms\n",
                                     D.DiskCacheHitCount, D.DiskCacheMissCount, D.DiskCacheLookupTime * MsPerTick,
                                     D.DiskCacheKeyTime * MsPerTick, D.DiskCacheReadTime * MsPerTick, D.DiskCacheFileReadCount,
                                     D.DiskCacheRelocTime * MsPerTick, D.DiskCacheInstallTime * MsPerTick);
        }
      }

      // Timeline of the busiest threads, see TimelineThreads. Per 10ms bin: ' ' not sampled, '.' all native (Wine, VKD3D,
      // the driver, or waiting), '1'-'9' tenths of the bin in emulated x86 code, '#' all emulated.
      if (State->Timeline) {
        for (size_t i = 0; i < std::min(State->SampledCount, TimelineThreads); ++i) {
          const auto& T = State->Sampled[i];
          if (!T.Samples) {
            continue;
          }
          char Bins[TimelineBins + 1] {};
          for (size_t b = 0; b < TimelineBins; ++b) {
            if (!T.BinSamples[b]) {
              Bins[b] = ' ';
            } else {
              const uint32_t Tenths = (T.BinInSimulation[b] * 10 + T.BinSamples[b] / 2) / T.BinSamples[b];
              Bins[b] = Tenths == 0 ? '.' : Tenths >= 10 ? '#' : static_cast<char>('0' + Tenths);
            }
          }
          Line += fextl::fmt::format("  tl tid={} {:<12}|{}|\n", T.TID, QueryThreadName(T.TID).substr(0, 12), Bins);
        }
      }

      // Preload probe, see ImageWatch. boot = hits before the first scan (game start).
      uint64_t TotalPages {};
      const uint64_t ReadablePages = ScanImage(static_cast<uint32_t>(Seconds), TotalPages);
      Line += fextl::fmt::format("  pre exe early={} late={} other={} boot={} readable={}/{}\n", HitsEarly.exchange(0),
                                 HitsLate.exchange(0), HitsOther.exchange(0), HitsBoot.exchange(0), ReadablePages, TotalPages);

      // CPU clocks in MHz per core 0-7 (0-2 little, 3-6 middle, 7 the X3 on a Snapdragon 8 Gen 2).
      if (const auto Clocks = ReadCoreClocks(); !Clocks.empty()) {
        Line += fextl::fmt::format("  clk={}\n", Clocks);
      } else if (ClocksUnreadable && Seconds == 1) {
        Line += "  clk=unreadable\n";
      }

      if (const auto Provider = LineProvider.load(std::memory_order_acquire)) {
        Line += Provider();
      }

      fwrite(Line.data(), 1, Line.size(), State->File);

      if (State->Profile && Seconds % ProfileWindowSeconds == 0) {
        WriteProfile(State, Seconds);
      }

      // Sample the busiest threads during the next second, reusing the CPU area lookup for threads already sampled.
      SampledThread Next[MaxSampled];
      size_t NextCount {};
      for (const auto& Entry : Deltas) {
        if (NextCount == MaxSampled || Entry.CPUPercent < 3.0) {
          break;
        }
        const auto* Old = FindSampled(State, Entry.D.TID);
        const bool Reuse = Old && Old->Slot == Entry.Slot;
        auto& N = Next[NextCount++];
        if (Reuse && Old->InSimulation && (!State->Profile || Old->BlockHeader)) {
          N = *Old;
          N.Samples = N.InSimulationSamples = 0;
          continue;
        }
        const auto* Area = QueryCPUArea(Entry.D.TID);
        N = {
          .TID = Entry.D.TID,
          .Slot = Entry.Slot,
          .InSimulation = Area ? &Area->InSimulation : nullptr,
        };
        if (State->Profile) {
          N.BlockHeader = BlockHeaderPointer(Area);
          N.IsMain = QueryThreadName(N.TID) == "Main";
        }
      }
      std::copy_n(Next, NextCount, State->Sampled);
      State->SampledCount = NextCount;
    }

    return nullptr;
  }

  FILE* NoteFile;
  fextl::string PendingNotes;

  bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      auto Lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      };
      if (Lower(a[i]) != Lower(b[i])) {
        return false;
      }
    }
    return true;
  }
} // namespace

bool StatsLogRequested(std::string_view AppName) {
#ifdef FEX_DISABLE_STATSLOG
  return false;
#endif
  const char* Dir = getenv("FEX_STATSLOG");
  if (!Dir || !Dir[0] || (Dir[0] == '0' && !Dir[1])) {
    return false;
  }
  const char* App = getenv("FEX_STATSLOG_APP");
  return !App || !App[0] || EqualsIgnoreCase(App, AppName);
}

void StartStatsLog(const StatAlloc* Stats, std::string_view AppName) {
  if (!Stats) {
    return;
  }

  fextl::string Dir = getenv("FEX_STATSLOG");
  // FEX_STATSLOG=1 means "pick a default": the per-user temp directory.
  if (Dir == "1") {
    const char* Temp = getenv("TEMP");
    Dir = Temp ? Temp : "C:\\";
  }
  while (!Dir.empty() && (Dir.back() == '\\' || Dir.back() == '/')) {
    Dir.pop_back();
  }

  // Wine reuses small PIDs, so add the local time: on Android shared storage an existing log often can't be replaced.
  LARGE_INTEGER SystemTime {}, LocalTime {};
  NtQuerySystemTime(&SystemTime);
  RtlSystemTimeToLocalTime(&SystemTime, &LocalTime);
  const uint64_t SecondOfDay = (static_cast<uint64_t>(LocalTime.QuadPart) / 10'000'000ULL) % 86400;
  const auto Path = fextl::fmt::format("{}\\fexstats-{}-{:02}{:02}{:02}-{}.txt", Dir, AppName, SecondOfDay / 3600, (SecondOfDay / 60) % 60,
                                       SecondOfDay % 60, GetCurrentProcessId());
  FILE* File = fopen(Path.c_str(), "w");
  if (!File) {
    LogMan::Msg::EFmt("FEX_STATSLOG: can't open {}", Path);
    return;
  }

  auto Header = fextl::fmt::format("# FEX stats for {} (pid {}), one block per second. cpu = thread CPU time; jit = time/blocks compiled; "
                                   "smc = writes to translated code; sigbus = unaligned atomic fixups; sig = time in exception "
                                   "handling; miss = lookup cache misses; lock = lookup cache lock time; x87fb = x87 fallbacks; dc = disk "
                                   "cache hits; emu = share of the second spent in emulated x86 code (the rest of cpu is native: "
                                   "Wine, VKD3D, driver)\n",
                                   AppName, GetCurrentProcessId());
  // No fflush: FEX's CRT fwrite goes straight to WriteFile, and its fflush is an UNIMPLEMENTED() stub that
  // terminates the process.
  fwrite(Header.data(), 1, Header.size(), File);
  if (!PendingNotes.empty()) {
    fwrite(PendingNotes.data(), 1, PendingNotes.size(), File);
    PendingNotes.clear();
  }
  NoteFile = File;

  // Lives for the rest of the process.
  auto* State = new LogState {.Stats = Stats, .File = File, .CounterFreq = GetCounterFrequency()};
  const char* Profile = getenv("FEX_STATSLOG_PROFILE");
  State->Profile = Profile && Profile[0] == '1';
  const char* Timeline = getenv("FEX_STATSLOG_TIMELINE");
  State->Timeline = !(Timeline && Timeline[0] == '0');
  const auto Switches = fextl::fmt::format("# switches: FEX_STATSLOG_PROFILE={} FEX_STATSLOG_TIMELINE={} FEX_THREADPIN={} FEX_NATIVE_ZLIB={}\n",
                                           State->Profile ? 1 : 0, State->Timeline ? 1 : 0, getenv("FEX_THREADPIN") ? getenv("FEX_THREADPIN") : "-",
                                           getenv("FEX_NATIVE_ZLIB") ? getenv("FEX_NATIVE_ZLIB") : "-");
  fwrite(Switches.data(), 1, Switches.size(), File);
  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(LogThread, State, {.LowPriority = false, .Internal = true}, "FEXStatsLog");
}

void StatsLogWatchImage(uint64_t Base) {
  WatchBase = Base;
}

void StatsLogDiskCacheHit(uint64_t GuestRIP) {
#ifdef FEX_DISABLE_STATSLOG
  return;
#endif
  const auto* W = Watch.load(std::memory_order_acquire);
  if (!W) {
    HitsBoot.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (GuestRIP < W->Base || GuestRIP >= W->Base + W->Size) {
    HitsOther.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t First = W->FirstReadable[(GuestRIP - W->Base) / 4096].load(std::memory_order_relaxed);
  const uint32_t Second = W->Second.load(std::memory_order_acquire);
  (First && First < Second ? HitsEarly : HitsLate).fetch_add(1, std::memory_order_relaxed);
}

void StatsLogSetLineProvider(fextl::string (*Provider)()) {
  LineProvider.store(Provider, std::memory_order_release);
}

void StatsLogNote(std::string_view Line) {
#ifdef FEX_DISABLE_STATSLOG
  return;
#endif
  const char* Dir = getenv("FEX_STATSLOG");
  if (!Dir || !Dir[0] || (Dir[0] == '0' && !Dir[1])) {
    return;
  }
  auto Text = fextl::fmt::format("# {}\n", Line);
  if (NoteFile) {
    fwrite(Text.data(), 1, Text.size(), NoteFile);
  } else if (PendingNotes.size() < 64 * 1024) {
    PendingNotes += Text;
  }
}
} // namespace FEX::Windows
