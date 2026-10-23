// SPDX-License-Identifier: MIT
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/SHMStats.h"
#include "Windows/Common/Switches.h"

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

  // Main thread hitches (with FEX_STATSLOG_PROFILE=1): a run of at least HitchMinBins timeline bins in which the thread
  // named "Main" ran emulated code without a break gets a "# hitch" line with the JIT blocks it was sampled in, so long
  // frames in the game's own code can be traced to the functions behind them. The hottest blocks of each hitch get their
  // guest code written once ("#B" lines).
  constexpr size_t HitchMinBins = 10; // 100ms
  constexpr size_t HitchPrinted = 6;
  constexpr size_t HitchDumped = 3;
  constexpr size_t MaxHitchLines = 400;
  constexpr size_t MaxDumps = 512;
  constexpr size_t MaxMainSamples = 1024;
  // A run of at least HitchMinBins bins in which Main was sampled only outside emulated code (waiting, or in Wine, VKD3D or
  // the driver) gets a "# stall" line the same way: at 30 FPS the game never waits that long between frames.
  // Both lines also name the other threads that ran emulated code for at least HitchOtherMinPercent of the run, with the
  // blocks they were in: usually the job Main is waiting for, or the lock it is waiting on.
  constexpr uint32_t HitchOtherMinPercent = 20;
  constexpr uint32_t HitchOtherMinSamples = 10;
  constexpr size_t HitchOthersPrinted = 4;
  constexpr size_t HitchOtherBlocksPrinted = 3;
  constexpr size_t MaxOtherSamples = 8192;

  // Native-call sampler (with FEX_STATSLOG_PROFILE=1): ExitFunctionEC (ARM64EC/Module.S) stores the native function it
  // enters in the thread's CHPE CPU area (EmulatorDataInline). Samples of a thread outside emulated code count against the
  // last native function it entered from x86 code: a Wine/Windows API (named by the module's exports), one of FEX's own
  // native redirects, or the native code an x86 callback returned to. Waiting shows up as the wait function.
  constexpr size_t CPUAreaNativeTargetOffset = 0x50;
  constexpr size_t MaxNativeTargets = 12;
  constexpr size_t NativePrinted = 5;
  // Marks a native target among the Main thread's hitch samples, which otherwise hold JIT block headers.
  constexpr uint64_t NativeSampleFlag = 1ull << 63;

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
    // Disk cache outcomes (see FEXCore::SHMStats::DiskCacheMissReason / DiskCacheStoreResult) and how far the RIP rings got.
    uint64_t DCMisses[8];
    uint64_t DCStores[8];
    uint64_t DCMissRIPCount;
    uint64_t DCRejectRIPCount;
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
    // Native-call sampler, see CPUAreaNativeTargetOffset. Reset every second.
    const volatile uint64_t* NativeTarget;
    struct {
      uint64_t Target;
      uint32_t Count;
    } Native[MaxNativeTargets];
    uint32_t NativeOther;
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

    // Main thread hitch recorder, see HitchMinBins. Samples of the current second in bin order, and the run in progress,
    // which can go on into the next second.
    struct MainSample {
      uint8_t Bin;
      uint64_t Header;
    };
    MainSample MainSamples[MaxMainSamples];
    size_t MainSampleCount;
    // The other sampled threads' samples of the current second, in time order: block header when In, native target |
    // NativeSampleFlag otherwise (0 when unknown).
    struct OtherSample {
      uint8_t Bin;
      bool In;
      uint32_t TID;
      uint64_t Value;
    };
    OtherSample OtherSamples[MaxOtherSamples];
    size_t OtherSampleCount;
    struct RunThread {
      uint32_t Samples;
      uint32_t InSim;
      fextl::unordered_map<uint64_t, uint32_t> Counts;
    };
    // A Main run in progress (busy: emulated code in every bin; idle: none in any bin), which can go on into the next second.
    struct Run {
      size_t Bins;
      size_t FullBins;
      uint64_t StartSecond;
      size_t StartBin;
      uint32_t Samples;
      fextl::unordered_map<uint64_t, uint32_t> Counts;
      fextl::unordered_map<uint32_t, RunThread> Others;
      size_t Lines;
    };
    Run Busy;
    Run Idle;

    // Names of native functions seen by the native-call sampler, and the export tables of the modules they are in.
    struct ModuleExports {
      fextl::string Name;
      fextl::vector<std::pair<uint32_t, uint32_t>> Functions; // RVA, name RVA; sorted by RVA
    };
    fextl::unordered_map<uint64_t, fextl::string> NativeNames;
    fextl::unordered_map<uint64_t, ModuleExports> Modules;
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

  // What the game is told about memory (GlobalMemoryStatusEx reads the same two system queries) and what the process
  // uses. A game that sizes its caches from free memory may evict and reload data when this is low.
  fextl::string MemoryLine() {
    SYSTEM_BASIC_INFORMATION Basic {};
    if (NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), nullptr) != STATUS_SUCCESS) {
      return {};
    }
    const uint64_t PageSize = Basic.PhysicalPageSize ? Basic.PhysicalPageSize : 4096;
    const auto MB = [PageSize](uint64_t Pages) {
      return Pages * PageSize / (1024 * 1024);
    };
    auto Line = fextl::fmt::format("  mem total={}MB", MB(Basic.NumberOfPhysicalPages));

    // Larger than any version of the structure, so the query can't fail on its size.
    union {
      SYSTEM_PERFORMANCE_INFORMATION Info;
      uint8_t Raw[2048];
    } Perf {};
    if (NtQuerySystemInformation(SystemPerformanceInformation, &Perf, sizeof(Perf), nullptr) == STATUS_SUCCESS) {
      Line += fextl::fmt::format(" avail={}MB commit={}MB/{}MB", MB(Perf.Info.AvailablePages), MB(Perf.Info.TotalCommittedPages),
                                 MB(Perf.Info.TotalCommitLimit));
    }

    VM_COUNTERS Counters {};
    if (NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters, &Counters, sizeof(Counters), nullptr) == STATUS_SUCCESS) {
      Line += fextl::fmt::format(" | process ws={}MB peak={}MB pagefile={}MB virtual={}MB", Counters.WorkingSetSize >> 20,
                                 Counters.PeakWorkingSetSize >> 20, Counters.PagefileUsage >> 20, Counters.VirtualSize >> 20);
    }
    return Line + "\n";
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

  const volatile uint64_t* NativeTargetPointer(const CHPE_V2_CPU_AREA_INFO* Area) {
#ifdef ARCHITECTURE_arm64ec
    return Area ? reinterpret_cast<const volatile uint64_t*>(reinterpret_cast<uintptr_t>(Area) + CPUAreaNativeTargetOffset) : nullptr;
#else
    return nullptr;
#endif
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

  // Reads a JIT block's guest RIP and size from its header. The block may have been freed since it was sampled, so this
  // goes through NtReadVirtualMemory, which fails safely.
  bool ReadBlockTail(uint64_t HeaderAddress, JITCodeTailMirror& Tail) {
    JITCodeHeaderMirror Header {};
    return NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(HeaderAddress), &Header, sizeof(Header), nullptr) == STATUS_SUCCESS &&
           NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(HeaderAddress + Header.OffsetToBlockTail), &Tail, sizeof(Tail),
                               nullptr) == STATUS_SUCCESS;
  }

  uint64_t AllocationBase(uint64_t Address) {
    MEMORY_BASIC_INFORMATION Info {};
    NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), MemoryBasicInformation, &Info, sizeof(Info), nullptr);
    return reinterpret_cast<uintptr_t>(Info.AllocationBase);
  }

  // Writes a block's whole guest code once per session as a "#B" line.
  void DumpBlock(LogState* State, uint64_t RIP, size_t GuestSize, fextl::string& Text) {
    if (State->DumpedRIPs.size() >= MaxDumps || std::find(State->DumpedRIPs.begin(), State->DumpedRIPs.end(), RIP) != State->DumpedRIPs.end()) {
      return;
    }
    fextl::vector<uint8_t> Code(std::min(GuestSize, ProfileDumpMaxBytes));
    SIZE_T CodeRead {};
    NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(RIP), Code.data(), Code.size(), &CodeRead);
    if (Code.empty() || CodeRead != Code.size()) {
      return;
    }
    State->DumpedRIPs.push_back(RIP);
    fextl::string CodeHex;
    for (const auto Byte : Code) {
      CodeHex += fextl::fmt::format("{:02x}", Byte);
    }
    Text += fextl::fmt::format("#B rip={:X} size={} bytes={}\n", RIP, Code.size(), CodeHex);
  }

  // Names for native functions that aren't exported (FEX's own redirect targets), see StatsLogNameNative.
  constexpr size_t MaxNamedNatives = 32;
  struct NamedNative {
    std::atomic<bool> Ready;
    uint64_t Address;
    char Name[48];
  };
  NamedNative NamedNatives[MaxNamedNatives];
  std::atomic<size_t> NamedNativeCount {};

  // Reads memory that may have gone away since it was seen (an unloaded module), failing safely.
  bool ReadMemory(uint64_t Address, void* Out, size_t Size) {
    SIZE_T Read {};
    return Size && NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), Out, Size, &Read) == STATUS_SUCCESS && Read == Size;
  }

  fextl::string ReadCString(uint64_t Address, size_t Max) {
    fextl::string Out;
    char Chunk[16];
    while (Out.size() < Max && ReadMemory(Address + Out.size(), Chunk, sizeof(Chunk))) {
      for (const char c : Chunk) {
        if (!c || Out.size() == Max) {
          return Out;
        }
        Out.push_back(c > 0x20 && c < 0x7f ? c : '_');
      }
    }
    return Out;
  }

  // The file name (without .dll/.exe) of the image or file mapped at Base, empty for anonymous memory.
  fextl::string MappedFileName(uint64_t Base) {
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 600 * sizeof(WCHAR)] {};
    if (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Base), static_cast<MEMORY_INFORMATION_CLASS>(2) /* MemoryMappedFilenameInformation */,
                             Buffer, sizeof(Buffer), nullptr) != STATUS_SUCCESS) {
      return {};
    }
    const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
    fextl::string Out;
    for (USHORT i = 0; Name->Buffer && i < Name->Length / sizeof(WCHAR); ++i) {
      const WCHAR c = Name->Buffer[i];
      if (c == '\\' || c == '/') {
        Out.clear();
      } else {
        Out.push_back(c > 0x20 && c < 0x7f ? static_cast<char>(c) : '_');
      }
    }
    const size_t n = Out.size();
    auto Lower = [](char c) {
      return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (n > 4 && Out[n - 4] == '.' &&
        ((Lower(Out[n - 3]) == 'd' && Lower(Out[n - 2]) == 'l' && Lower(Out[n - 1]) == 'l') ||
         (Lower(Out[n - 3]) == 'e' && Lower(Out[n - 2]) == 'x' && Lower(Out[n - 1]) == 'e'))) {
      Out.resize(n - 4);
    }
    return Out;
  }

  // Like ReadMemory, but falls back to page-sized pieces when one large read is refused.
  bool ReadMemoryChunked(uint64_t Address, void* Out, size_t Size) {
    if (ReadMemory(Address, Out, Size)) {
      return true;
    }
    auto* Dest = static_cast<uint8_t*>(Out);
    size_t Done = 0;
    while (Done < Size) {
      const uint64_t At = Address + Done;
      const size_t Piece = std::min<size_t>(Size - Done, 4096 - (At & 4095));
      if (!ReadMemory(At, Dest + Done, Piece)) {
        return false;
      }
      Done += Piece;
    }
    return true;
  }

  // ARM64EC hybrid-patchable exports point at a small x64 thunk ("mov rax, rsp; mov [rax+20h], rbx; push rbp; pop rbp;
  // jmp target") so that x86 code can hook them; ExitFunctionEC then enters the jmp target, which carries no name of its own.
  constexpr uint8_t PatchableThunk[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9};
  constexpr size_t PatchableThunkSize = sizeof(PatchableThunk) + 4;

  // The module's name and its exported functions, from its export directory, including the code behind patchable exports.
  const LogState::ModuleExports& ModuleExportsFor(LogState* State, uint64_t Base) {
    auto [It, Inserted] = State->Modules.try_emplace(Base);
    auto& M = It->second;
    if (!Inserted) {
      return M;
    }
    M.Name = fextl::fmt::format("{:X}", Base);
    IMAGE_DOS_HEADER Dos {};
    IMAGE_NT_HEADERS64 Nt {};
    if (!ReadMemory(Base, &Dos, sizeof(Dos)) || Dos.e_magic != IMAGE_DOS_SIGNATURE || !ReadMemory(Base + Dos.e_lfanew, &Nt, sizeof(Nt)) ||
        Nt.Signature != IMAGE_NT_SIGNATURE || Nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      if (auto File = MappedFileName(Base); !File.empty()) {
        M.Name = std::move(File);
      }
      return M;
    }
    if (auto File = MappedFileName(Base); !File.empty()) {
      M.Name = std::move(File);
    }
    const auto Dir = Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY Exports {};
    if (!Dir.VirtualAddress || !Dir.Size || !ReadMemory(Base + Dir.VirtualAddress, &Exports, sizeof(Exports))) {
      return M;
    }
    if (auto Name = ReadCString(Base + Exports.Name, 64); !Name.empty()) {
      auto Lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      };
      const size_t n = Name.size();
      if (n > 4 && Name[n - 4] == '.' && Lower(Name[n - 3]) == 'd' && Lower(Name[n - 2]) == 'l' && Lower(Name[n - 1]) == 'l') {
        Name.resize(n - 4);
      }
      M.Name = std::move(Name);
    }
    const uint32_t Names = std::min<uint32_t>(Exports.NumberOfNames, 32768);
    const uint32_t Functions = std::min<uint32_t>(Exports.NumberOfFunctions, 65536);
    fextl::vector<uint32_t> NameRVAs(Names), FunctionRVAs(Functions);
    fextl::vector<uint16_t> Ordinals(Names);
    if (!Names || !Functions || !ReadMemoryChunked(Base + Exports.AddressOfNames, NameRVAs.data(), Names * sizeof(uint32_t)) ||
        !ReadMemoryChunked(Base + Exports.AddressOfNameOrdinals, Ordinals.data(), Names * sizeof(uint16_t)) ||
        !ReadMemoryChunked(Base + Exports.AddressOfFunctions, FunctionRVAs.data(), Functions * sizeof(uint32_t))) {
      StatsLogNote(fextl::fmt::format("native names: {} at {:X}: export tables unreadable (names={} functions={})", M.Name, Base, Names, Functions));
      return M;
    }
    for (uint32_t i = 0; i < Names; ++i) {
      if (Ordinals[i] >= Functions) {
        continue;
      }
      const uint32_t RVA = FunctionRVAs[Ordinals[i]];
      // Forwarders point into the export directory itself.
      if (!RVA || (RVA >= Dir.VirtualAddress && RVA < Dir.VirtualAddress + Dir.Size)) {
        continue;
      }
      M.Functions.push_back({RVA, NameRVAs[i]});
    }
    std::sort(M.Functions.begin(), M.Functions.end());

    // Follow patchable thunks to the code they jump to, reading each page of export entry points once.
    const size_t Exported = M.Functions.size();
    size_t Thunks = 0;
    uint8_t Page[4096 + PatchableThunkSize];
    uint64_t PageRVA = ~0ull;
    size_t PageLength = 0;
    for (size_t i = 0; i < Exported; ++i) {
      const uint32_t RVA = M.Functions[i].first;
      const uint64_t Want = RVA & ~4095ull;
      if (Want != PageRVA) {
        PageRVA = Want;
        PageLength = ReadMemory(Base + PageRVA, Page, sizeof(Page)) ? sizeof(Page) : ReadMemory(Base + PageRVA, Page, 4096) ? 4096 : 0;
      }
      uint8_t Entry[PatchableThunkSize];
      if ((RVA & 4095) + PatchableThunkSize <= PageLength) {
        memcpy(Entry, Page + (RVA & 4095), sizeof(Entry));
      } else if (!ReadMemory(Base + RVA, Entry, sizeof(Entry))) {
        continue;
      }
      if (memcmp(Entry, PatchableThunk, sizeof(PatchableThunk)) != 0) {
        continue;
      }
      int32_t Rel {};
      memcpy(&Rel, Entry + sizeof(PatchableThunk), sizeof(Rel));
      const int64_t Target = static_cast<int64_t>(RVA) + static_cast<int64_t>(PatchableThunkSize) + Rel;
      if (Target > 0 && Target < Nt.OptionalHeader.SizeOfImage) {
        M.Functions.push_back({static_cast<uint32_t>(Target), M.Functions[i].second});
        ++Thunks;
      }
    }
    std::sort(M.Functions.begin(), M.Functions.end());
    StatsLogNote(fextl::fmt::format("native names: {} at {:X}: {} exports, {} patchable", M.Name, Base, Exported, Thunks));
    return M;
  }

  // Exports further than this below an address are not taken as the function it is in.
  constexpr uint32_t MaxNameDistance = 0x1000;

  // "module!export" (plus the offset when Address is inside the function rather than at its start), a name FEX gave it,
  // or "module+RVA" / the address.
  fextl::string NativeName(LogState* State, uint64_t Address) {
    if (const auto It = State->NativeNames.find(Address); It != State->NativeNames.end()) {
      return It->second;
    }
    fextl::string Name;
    for (size_t i = 0; i < std::min(NamedNativeCount.load(std::memory_order_acquire), MaxNamedNatives); ++i) {
      if (NamedNatives[i].Ready.load(std::memory_order_acquire) && NamedNatives[i].Address == Address) {
        Name = NamedNatives[i].Name;
        break;
      }
    }
    if (Name.empty()) {
      if (const uint64_t Base = AllocationBase(Address); !Base || Address - Base > 0xffffffffull) {
        Name = fextl::fmt::format("{:X}", Address);
      } else {
        const auto& M = ModuleExportsFor(State, Base);
        const uint32_t RVA = static_cast<uint32_t>(Address - Base);
        auto It = std::upper_bound(M.Functions.begin(), M.Functions.end(), RVA, [](uint32_t Value, const auto& F) { return Value < F.first; });
        if (It == M.Functions.begin() || RVA - std::prev(It)->first > MaxNameDistance) {
          Name = fextl::fmt::format("{}+{:X}", M.Name, RVA);
        } else {
          --It;
          Name = fextl::fmt::format("{}!{}", M.Name, ReadCString(Base + It->second, 64));
          if (RVA != It->first) {
            Name += fextl::fmt::format("+{:X}", RVA - It->first);
          }
        }
      }
    }
    if (State->NativeNames.size() < 4096) {
      State->NativeNames.emplace(Address, Name);
    }
    return Name;
  }

  void CountNative(SampledThread& T, uint64_t Target) {
    for (auto& N : T.Native) {
      if (N.Target == Target) {
        ++N.Count;
        return;
      }
      if (!N.Target) {
        N.Target = Target;
        N.Count = 1;
        return;
      }
    }
    ++T.NativeOther;
  }

  // The native functions a thread was sampled in during the last second, as shares of all its samples.
  fextl::string NativeLine(LogState* State, const SampledThread& T) {
    if (!T.Samples) {
      return {};
    }
    std::pair<uint32_t, uint64_t> Entries[MaxNativeTargets];
    size_t Count {};
    uint32_t Native {};
    for (const auto& N : T.Native) {
      if (N.Target) {
        Entries[Count++] = {N.Count, N.Target};
        Native += N.Count;
      }
    }
    if (!Count) {
      return {};
    }
    std::sort(Entries, Entries + Count, [](const auto& a, const auto& b) { return a.first > b.first; });
    const auto Percent = [&T](uint32_t Samples) {
      return (Samples * 100 + T.Samples / 2) / T.Samples;
    };
    fextl::string Out = "    native";
    uint32_t Printed {};
    for (size_t i = 0; i < std::min(Count, NativePrinted); ++i) {
      Out += fextl::fmt::format(" {}={}%", NativeName(State, Entries[i].second), Percent(Entries[i].first));
      Printed += Entries[i].first;
    }
    if (const uint32_t Rest = Native - Printed + T.NativeOther) {
      Out += fextl::fmt::format(" other={}%", Percent(Rest));
    }
    return Out + "\n";
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
      JITCodeTailMirror Tail {};
      if (!ReadBlockTail(E.Header, Tail)) {
        Text += fextl::fmt::format("#P block={:X} main={} other={} (freed)\n", E.Header, E.C.Main, E.C.Other);
        continue;
      }
      uint8_t Bytes[ProfileBytes] {};
      SIZE_T Read {};
      NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Tail.RIP), Bytes, std::min(sizeof(Bytes), Tail.GuestSize), &Read);
      fextl::string Hex;
      for (SIZE_T b = 0; b < Read; ++b) {
        Hex += fextl::fmt::format("{:02x}", Bytes[b]);
      }
      Text += fextl::fmt::format("#P rip={:X} base={:X} guestsize={} hostsize={} main={} other={} bytes={}\n", Tail.RIP,
                                 AllocationBase(Tail.RIP), Tail.GuestSize, Tail.Size, E.C.Main, E.C.Other, Hex);

      if (i < ProfileDumped && Tail.GuestSize > ProfileBytes) {
        DumpBlock(State, Tail.RIP, Tail.GuestSize, Text);
      }
    }
    fwrite(Text.data(), 1, Text.size(), State->File);

    State->ProfileCounts.clear();
    State->ProfileSamples[0] = State->ProfileSamples[1] = 0;
  }

  // Top entries of a sample count map, split into JIT blocks and native targets (NativeSampleFlag).
  void SplitCounts(const fextl::unordered_map<uint64_t, uint32_t>& Counts, fextl::vector<std::pair<uint32_t, uint64_t>>& Blocks,
                   fextl::vector<std::pair<uint32_t, uint64_t>>& Natives) {
    for (const auto& [Value, Count] : Counts) {
      if (Value & NativeSampleFlag) {
        Natives.push_back({Count, Value & ~NativeSampleFlag});
      } else {
        Blocks.push_back({Count, Value});
      }
    }
    const auto ByCount = [](const auto& a, const auto& b) {
      return a.first > b.first;
    };
    std::sort(Blocks.begin(), Blocks.end(), ByCount);
    std::sort(Natives.begin(), Natives.end(), ByCount);
  }

  void AppendBlocks(LogState* State, const fextl::vector<std::pair<uint32_t, uint64_t>>& Blocks, size_t Printed, size_t Dumped,
                    fextl::string& Text, fextl::string& Dumps) {
    for (size_t i = 0; i < std::min(Blocks.size(), Printed); ++i) {
      JITCodeTailMirror Tail {};
      if (!ReadBlockTail(Blocks[i].second, Tail)) {
        Text += fextl::fmt::format(" freed x{}", Blocks[i].first);
        continue;
      }
      Text += fextl::fmt::format(" {:X}@{:X} x{}", Tail.RIP, AllocationBase(Tail.RIP), Blocks[i].first);
      if (i < Dumped) {
        DumpBlock(State, Tail.RIP, Tail.GuestSize, Dumps);
      }
    }
  }

  void AppendNatives(LogState* State, const fextl::vector<std::pair<uint32_t, uint64_t>>& Natives, size_t Printed, fextl::string& Text) {
    if (Natives.empty()) {
      return;
    }
    Text += " native:";
    for (size_t i = 0; i < std::min(Natives.size(), Printed); ++i) {
      Text += fextl::fmt::format(" {} x{}", NativeName(State, Natives[i].second), Natives[i].first);
    }
  }

  fextl::string FormatRun(LogState* State, const LogState::Run& R, const char* Kind) {
    fextl::vector<std::pair<uint32_t, uint64_t>> Blocks, Natives;
    SplitCounts(R.Counts, Blocks, Natives);
    auto Text = fextl::fmt::format("# {} t={}.{:02} {}ms full={}ms samples={} blocks={}:", Kind, R.StartSecond, R.StartBin, R.Bins * 10,
                                   R.FullBins * 10, R.Samples, Blocks.size());
    fextl::string Dumps;
    AppendBlocks(State, Blocks, HitchPrinted, HitchDumped, Text, Dumps);
    // Where the thread was outside emulated code during the run.
    AppendNatives(State, Natives, 3, Text);

    // Other threads that ran emulated code for a good part of the run meanwhile.
    fextl::vector<std::pair<uint32_t, const LogState::RunThread*>> Others;
    for (const auto& [TID, T] : R.Others) {
      if (T.InSim >= HitchOtherMinSamples && T.InSim * 100 >= T.Samples * HitchOtherMinPercent) {
        Others.push_back({TID, &T});
      }
    }
    std::sort(Others.begin(), Others.end(), [](const auto& a, const auto& b) { return a.second->InSim > b.second->InSim; });
    for (size_t i = 0; i < std::min(Others.size(), HitchOthersPrinted); ++i) {
      const auto& T = *Others[i].second;
      Text += fextl::fmt::format(" | tid={} {} emu={}%:", Others[i].first, QueryThreadName(Others[i].first), T.InSim * 100 / T.Samples);
      fextl::vector<std::pair<uint32_t, uint64_t>> OtherBlocks, OtherNatives;
      SplitCounts(T.Counts, OtherBlocks, OtherNatives);
      AppendBlocks(State, OtherBlocks, HitchOtherBlocksPrinted, 2, Text, Dumps);
      AppendNatives(State, OtherNatives, 1, Text);
    }
    if (Others.size() > HitchOthersPrinted) {
      Text += fextl::fmt::format(" | +{} busy", Others.size() - HitchOthersPrinted);
    }
    return Text + "\n" + Dumps;
  }

  void StartRun(LogState::Run& R, uint64_t Seconds, size_t Bin) {
    R.StartSecond = Seconds;
    R.StartBin = Bin;
    R.FullBins = 0;
    R.Samples = 0;
    R.Counts.clear();
    R.Others.clear();
  }

  // Adds bin Bin's samples to the run. s and o are cursors into this second's Main and other samples, which are in bin order.
  void AddRunBin(LogState* State, LogState::Run& R, size_t Bin, size_t& s, size_t& o) {
    while (s < State->MainSampleCount && State->MainSamples[s].Bin < Bin) {
      ++s;
    }
    for (; s < State->MainSampleCount && State->MainSamples[s].Bin == Bin; ++s) {
      ++R.Counts[State->MainSamples[s].Header];
      ++R.Samples;
    }
    while (o < State->OtherSampleCount && State->OtherSamples[o].Bin < Bin) {
      ++o;
    }
    for (; o < State->OtherSampleCount && State->OtherSamples[o].Bin == Bin; ++o) {
      const auto& O = State->OtherSamples[o];
      auto& T = R.Others[O.TID];
      ++T.Samples;
      T.InSim += O.In ? 1 : 0;
      if (O.Value) {
        ++T.Counts[O.Value];
      }
    }
  }

  void EndRun(LogState* State, LogState::Run& R, const char* Kind, fextl::string& Out) {
    if (R.Bins >= HitchMinBins && R.Lines < MaxHitchLines) {
      ++R.Lines;
      Out += FormatRun(State, R, Kind);
    }
    R.Bins = 0;
  }

  // Finds runs of emulated code (hitch) and of no emulated code (stall) on the Main thread in this second's timeline bins, see
  // HitchMinBins.
  fextl::string MainHitches(LogState* State, uint64_t Seconds) {
    const SampledThread* Main = nullptr;
    for (size_t i = 0; i < State->SampledCount; ++i) {
      if (State->Sampled[i].IsMain) {
        Main = &State->Sampled[i];
        break;
      }
    }
    fextl::string Out;
    size_t s = 0;
    size_t o = 0;
    for (size_t b = 0; b < TimelineBins; ++b) {
      bool Busy = false;
      bool Idle = false;
      bool Full = false;
      if (Main && Main->BinSamples[b]) {
        const uint32_t Tenths = (Main->BinInSimulation[b] * 10 + Main->BinSamples[b] / 2) / Main->BinSamples[b];
        Busy = Tenths > 0;
        Idle = Main->BinInSimulation[b] == 0;
        Full = Tenths >= 10;
      }
      if (Busy) {
        if (!State->Busy.Bins) {
          StartRun(State->Busy, Seconds, b);
        }
        ++State->Busy.Bins;
        State->Busy.FullBins += Full ? 1 : 0;
        AddRunBin(State, State->Busy, b, s, o);
      } else if (State->Busy.Bins) {
        EndRun(State, State->Busy, "hitch", Out);
      }
      if (Idle) {
        if (!State->Idle.Bins) {
          StartRun(State->Idle, Seconds, b);
        }
        ++State->Idle.Bins;
        AddRunBin(State, State->Idle, b, s, o);
      } else if (State->Idle.Bins) {
        EndRun(State, State->Idle, "stall", Out);
      }
    }
    State->MainSampleCount = 0;
    State->OtherSampleCount = 0;
    return Out;
  }

  // Waits one second, polling the sampled threads' InSimulation flag every few milliseconds.
  void SampleFor1s(LogState* State) {
    const uint64_t Start = GetCounter();
    const uint64_t End = Start + State->CounterFreq;
    State->SecondStart = Start;
    for (size_t i = 0; i < State->SampledCount; ++i) {
      memset(State->Sampled[i].BinSamples, 0, sizeof(State->Sampled[i].BinSamples));
      memset(State->Sampled[i].BinInSimulation, 0, sizeof(State->Sampled[i].BinInSimulation));
      memset(State->Sampled[i].Native, 0, sizeof(State->Sampled[i].Native));
      State->Sampled[i].NativeOther = 0;
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
        if (In && T.IsMain && T.BlockHeader && State->MainSampleCount < MaxMainSamples) {
          if (const uint64_t Header = *T.BlockHeader) {
            State->MainSamples[State->MainSampleCount++] = {static_cast<uint8_t>(Bin), Header};
          }
        }
        uint64_t NativeTarget {};
        if (!In && T.NativeTarget) {
          if (const uint64_t Target = *T.NativeTarget) {
            NativeTarget = Target;
            CountNative(T, Target);
            if (T.IsMain && State->MainSampleCount < MaxMainSamples) {
              State->MainSamples[State->MainSampleCount++] = {static_cast<uint8_t>(Bin), Target | NativeSampleFlag};
            }
          }
        }
        if (State->Profile && !T.IsMain && State->OtherSampleCount < MaxOtherSamples) {
          const uint64_t Value = In ? (T.BlockHeader ? *T.BlockHeader : 0) : (NativeTarget ? NativeTarget | NativeSampleFlag : 0);
          State->OtherSamples[State->OtherSampleCount++] = {static_cast<uint8_t>(Bin), In, T.TID, Value};
        }
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

  constexpr const char* DCMissNames[8] = {"notreading", "anonoff", "anondecode", "range", "noindex", "nomatch", "badentry", "notloaded"};
  constexpr const char* DCStoreNames[8] = {"queued", "nokey", "notwriting", "customir", "reloc", "decode", "bucket", "dupe"};
  constexpr size_t DCWherePrinted = 4;
  constexpr size_t DCWhereRVAs = 3;

  // Disk cache misses or store refusals of the last second, grouped by the module (or anonymous allocation) the RIP is in.
  struct DCWhere {
    uint32_t Count;
    uint32_t Reasons[8];
    fextl::vector<uint32_t> RVAs;
  };

  void AddDCWhere(fextl::unordered_map<uint64_t, DCWhere>& Where, uint64_t Packed) {
    const uint64_t RIP = Packed & ((1ull << 56) - 1);
    const uint8_t Reason = static_cast<uint8_t>(Packed >> 56) & 7;
    const uint64_t Base = AllocationBase(RIP);
    auto& W = Where[Base];
    ++W.Count;
    ++W.Reasons[Reason];
    const uint32_t RVA = static_cast<uint32_t>(RIP - Base);
    if (W.RVAs.size() < DCWhereRVAs && std::find(W.RVAs.begin(), W.RVAs.end(), RVA) == W.RVAs.end()) {
      W.RVAs.push_back(RVA);
    }
  }

  fextl::string FormatDCWhere(LogState* State, const char* Title, const fextl::unordered_map<uint64_t, DCWhere>& Where, const char* const* Names) {
    if (Where.empty()) {
      return {};
    }
    fextl::vector<std::pair<uint32_t, uint64_t>> Order;
    for (const auto& [Base, W] : Where) {
      Order.push_back({W.Count, Base});
    }
    std::sort(Order.begin(), Order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    auto Out = fextl::fmt::format("  {}", Title);
    for (size_t i = 0; i < std::min(Order.size(), DCWherePrinted); ++i) {
      const uint64_t Base = Order[i].second;
      const auto& W = Where.at(Base);
      const auto& M = ModuleExportsFor(State, Base);
      Out += fextl::fmt::format(" {} x{} [", M.Name, W.Count);
      bool First = true;
      for (int r = 0; r < 8; ++r) {
        if (W.Reasons[r]) {
          Out += fextl::fmt::format("{}{} {}", First ? "" : " ", Names[r], W.Reasons[r]);
          First = false;
        }
      }
      Out += "]";
      for (const auto RVA : W.RVAs) {
        Out += fextl::fmt::format(" +{:X}", RVA);
      }
      Out += ";";
    }
    if (Order.size() > DCWherePrinted) {
      Out += fextl::fmt::format(" +{} more", Order.size() - DCWherePrinted);
    }
    return Out + "\n";
  }

  // Process page faults that had to read from storage, and bytes read from storage, from procfs (through Wine's Z: drive).
  bool IOUnreadable {};
  bool IOHavePrev {};
  uint64_t IOPrevMajorFaults {}, IOPrevReadBytes {};

  bool ReadSmallFile(const char* Path, char* Buf, size_t Size) {
    FILE* F = fopen(Path, "r");
    if (!F) {
      return false;
    }
    const size_t Len = fread(Buf, 1, Size - 1, F);
    fclose(F);
    Buf[Len] = 0;
    return Len != 0;
  }

  fextl::string IOLine() {
    if (IOUnreadable) {
      return {};
    }
    uint64_t MajorFaults {}, ReadBytes {};
    bool HaveFaults {}, HaveRead {};
    char Buf[1024];
    if (ReadSmallFile("Z:\\proc\\self\\stat", Buf, sizeof(Buf))) {
      // Fields after the command name, which is in parentheses and may contain spaces: state is field 3, majflt field 12.
      if (const char* P = strrchr(Buf, ')')) {
        int Field = 2;
        for (++P; *P && Field < 12; ++P) {
          if (*P == ' ') {
            ++Field;
            if (Field == 12) {
              MajorFaults = strtoull(P + 1, nullptr, 10);
              HaveFaults = true;
            }
          }
        }
      }
    }
    if (ReadSmallFile("Z:\\proc\\self\\io", Buf, sizeof(Buf))) {
      if (const char* P = strstr(Buf, "\nread_bytes:")) {
        ReadBytes = strtoull(P + 12, nullptr, 10);
        HaveRead = true;
      }
    }
    if (!HaveFaults && !HaveRead) {
      IOUnreadable = true;
      return "  io=unreadable\n";
    }
    fextl::string Out;
    if (IOHavePrev) {
      Out = fextl::fmt::format("  io majflt={} read={:.1f}MB\n", HaveFaults ? MajorFaults - std::min(MajorFaults, IOPrevMajorFaults) : 0,
                               HaveRead ? static_cast<double>(ReadBytes - std::min(ReadBytes, IOPrevReadBytes)) / (1024.0 * 1024.0) : 0.0);
    }
    IOPrevMajorFaults = MajorFaults;
    IOPrevReadBytes = ReadBytes;
    IOHavePrev = true;
    return Out;
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
        Cur.DCMissRIPCount = std::atomic_ref<uint64_t>(Mutable->DiskCacheMissRIPCount).load(std::memory_order_acquire);
        Cur.DCRejectRIPCount = std::atomic_ref<uint64_t>(Mutable->DiskCacheRejectRIPCount).load(std::memory_order_acquire);
        for (int i = 0; i < 8; ++i) {
          Cur.DCMisses[i] = Slot.DiskCacheMisses[i];
          Cur.DCStores[i] = Slot.DiskCacheStores[i];
        }
      });

      uint64_t DCMisses[8] {}, DCStores[8] {};
      fextl::unordered_map<uint64_t, DCWhere> MissWhere, RejectWhere;

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
        for (int i = 0; i < 8; ++i) {
          DCMisses[i] += Diff(Cur.DCMisses[i], Old->DCMisses[i]);
          DCStores[i] += Diff(Cur.DCStores[i], Old->DCStores[i]);
        }
        {
          // The newest ring entries since last second (at most 16 each).
          const uint64_t MissFrom = Cur.DCMissRIPCount >= Old->DCMissRIPCount ? Old->DCMissRIPCount : 0;
          for (uint64_t i = std::max(MissFrom, Cur.DCMissRIPCount >= 16 ? Cur.DCMissRIPCount - 16 : 0); i < Cur.DCMissRIPCount; ++i) {
            AddDCWhere(MissWhere, Entry.Slot->DiskCacheMissRIPs[i % 16]);
          }
          const uint64_t RejectFrom = Cur.DCRejectRIPCount >= Old->DCRejectRIPCount ? Old->DCRejectRIPCount : 0;
          for (uint64_t i = std::max(RejectFrom, Cur.DCRejectRIPCount >= 16 ? Cur.DCRejectRIPCount - 16 : 0); i < Cur.DCRejectRIPCount; ++i) {
            AddDCWhere(RejectWhere, Entry.Slot->DiskCacheRejectRIPs[i % 16]);
          }
        }
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
        if (State->Profile) {
          if (const auto* T = FindSampled(State, D.TID)) {
            Line += NativeLine(State, *T);
          }
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

      if (State->Profile) {
        Line += MainHitches(State, Seconds);
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

      // Disk cache outcomes: why compiles missed, what happened to the compiled code, and what the writer did with it.
      {
        static uint64_t PrevWritten, PrevFailed, PrevMemoryOnly;
        const auto& Writer = FEXCore::SHMStats::DiskCacheWriter;
        const uint64_t Written = Writer.Written.load(std::memory_order_relaxed);
        const uint64_t Failed = Writer.Failed.load(std::memory_order_relaxed);
        const uint64_t MemoryOnly = Writer.MemoryOnly.load(std::memory_order_relaxed);
        fextl::string Outcome;
        auto Add = [&Outcome](const char* Name, uint64_t Value) {
          if (Value) {
            Outcome += fextl::fmt::format(" {}={}", Name, Value);
          }
        };
        for (int i = 0; i < 8; ++i) {
          Add(DCMissNames[i], DCMisses[i]);
        }
        Outcome += " | store";
        for (int i = 0; i < 8; ++i) {
          Add(DCStoreNames[i], DCStores[i]);
        }
        Outcome += " | writer";
        Add("written", Written - PrevWritten);
        Add("failed", Failed - PrevFailed);
        Add("memonly", MemoryOnly - PrevMemoryOnly);
        PrevWritten = Written;
        PrevFailed = Failed;
        PrevMemoryOnly = MemoryOnly;
        if (Outcome != " | store | writer") {
          Line += fextl::fmt::format("  dco miss{}\n", Outcome);
        }
        Line += FormatDCWhere(State, "dc missed at:", MissWhere, DCMissNames);
        Line += FormatDCWhere(State, "dc refused at:", RejectWhere, DCStoreNames);
      }

      Line += IOLine();

      if (Seconds % 10 == 1) {
        Line += MemoryLine();
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
          // The game may name a thread after it was first sampled.
          if (State->Profile && !N.IsMain) {
            N.IsMain = QueryThreadName(N.TID) == "Main";
          }
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
          N.NativeTarget = NativeTargetPointer(Area);
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

namespace {
// Set by CI (FEX_BUILD_NUMBER / FEX_BUILD_INFO in Source/Windows/Common/CMakeLists.txt).
#ifdef FEX_BUILD_NUMBER
  constexpr const char* BuildNumberSuffix = "-b" FEX_BUILD_NUMBER;
#else
  constexpr const char* BuildNumberSuffix = "";
#endif
#ifdef FEX_BUILD_INFO
  constexpr const char* BuildInfo = FEX_BUILD_INFO;
#else
  constexpr const char* BuildInfo = "local";
#endif
} // namespace

bool StatsLogRequested(std::string_view AppName) {
#ifdef FEX_DISABLE_STATSLOG
  return false;
#endif
  const char* Dir = Switch("FEX_STATSLOG");
  if (!Dir || (Dir[0] == '0' && !Dir[1])) {
    return false;
  }
  const char* App = Switch("FEX_STATSLOG_APP");
  return !App || EqualsIgnoreCase(App, AppName);
}

void StartStatsLog(const StatAlloc* Stats, std::string_view AppName) {
  if (!Stats) {
    return;
  }

  const char* Temp = getenv("TEMP");
  const fextl::string TempDir = Temp ? Temp : "C:\\";
  fextl::string Dir = Switch("FEX_STATSLOG");
  // FEX_STATSLOG=1 means "pick a default": the per-user temp directory.
  if (Dir == "1") {
    Dir = TempDir;
  }

  // Wine reuses small PIDs, so add the local time: on Android shared storage an existing log often can't be replaced.
  // The build number tells which build a log came from.
  LARGE_INTEGER SystemTime {}, LocalTime {};
  NtQuerySystemTime(&SystemTime);
  RtlSystemTimeToLocalTime(&SystemTime, &LocalTime);
  const uint64_t SecondOfDay = (static_cast<uint64_t>(LocalTime.QuadPart) / 10'000'000ULL) % 86400;
  auto PathIn = [&](fextl::string Folder) {
    while (!Folder.empty() && (Folder.back() == '\\' || Folder.back() == '/')) {
      Folder.pop_back();
    }
    return fextl::fmt::format("{}\\fexstats-{}{}-{:02}{:02}{:02}-{}.txt", Folder, AppName, BuildNumberSuffix, SecondOfDay / 3600,
                              (SecondOfDay / 60) % 60, SecondOfDay % 60, GetCurrentProcessId());
  };
  auto Path = PathIn(Dir);
  FILE* File = fopen(Path.c_str(), "w");
  if (!File && Dir != TempDir) {
    LogMan::Msg::EFmt("FEX_STATSLOG: can't open {}, trying {}", Path, TempDir);
    Path = PathIn(TempDir);
    File = fopen(Path.c_str(), "w");
  }
  if (!File) {
    LogMan::Msg::EFmt("FEX_STATSLOG: can't open {}", Path);
    return;
  }

  auto Header = fextl::fmt::format("# FEX stats for {} (pid {}), one block per second. cpu = thread CPU time; jit = time/blocks compiled; "
                                   "smc = writes to translated code; sigbus = unaligned atomic fixups; sig = time in exception "
                                   "handling; miss = lookup cache misses; lock = lookup cache lock time; x87fb = x87 fallbacks; dc = disk "
                                   "cache hits; emu = share of the second spent in emulated x86 code (the rest of cpu is native: "
                                   "Wine, VKD3D, driver); dco = why compiles missed the disk cache and what happened to the code "
                                   "compiled instead; io = page faults that read storage, and storage reads; with FEX_STATSLOG_PROFILE=1, "
                                   "'# hitch' / '# stall' = Main ran emulated code / no emulated code for 100ms or more, with the other "
                                   "threads that were busy in emulated code meanwhile; jobwait = the game's frame wait loop: tries that found "
                                   "nothing to run (idle) or found jobs, time paused, longest uninterrupted idle wait\n",
                                   AppName, GetCurrentProcessId());
  // No fflush: FEX's CRT fwrite goes straight to WriteFile, and its fflush is an UNIMPLEMENTED() stub that
  // terminates the process.
  fwrite(Header.data(), 1, Header.size(), File);
  const auto Build = fextl::fmt::format("# build: {}, log file {}\n", BuildInfo, Path);
  fwrite(Build.data(), 1, Build.size(), File);
  if (!PendingNotes.empty()) {
    fwrite(PendingNotes.data(), 1, PendingNotes.size(), File);
    PendingNotes.clear();
  }
  NoteFile = File;

  // Lives for the rest of the process.
  auto* State = new LogState {.Stats = Stats, .File = File, .CounterFreq = GetCounterFrequency()};
  const char* Profile = Switch("FEX_STATSLOG_PROFILE");
  State->Profile = Profile && Profile[0] == '1';
  const char* Timeline = Switch("FEX_STATSLOG_TIMELINE");
  State->Timeline = !(Timeline && Timeline[0] == '0');
  const auto Switches = fextl::fmt::format("# switches:{}\n", SwitchSummary());
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

void StatsLogNameNative(uint64_t Address, std::string_view Name) {
  const size_t Index = NamedNativeCount.fetch_add(1, std::memory_order_relaxed);
  if (Index >= MaxNamedNatives) {
    return;
  }
  auto& N = NamedNatives[Index];
  N.Address = Address;
  const size_t Length = std::min(Name.size(), sizeof(N.Name) - 1);
  memcpy(N.Name, Name.data(), Length);
  N.Name[Length] = 0;
  N.Ready.store(true, std::memory_order_release);
}

void StatsLogNote(std::string_view Line) {
#ifdef FEX_DISABLE_STATSLOG
  return;
#endif
  const char* Dir = Switch("FEX_STATSLOG");
  if (!Dir || (Dir[0] == '0' && !Dir[1])) {
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
