// SPDX-License-Identifier: MIT
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/LinuxSys.h"
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
#include <FEXCore/Utils/WritePriorityMutex.h>

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
extern "C" NTSTATUS WINAPI NtCreateEvent(HANDLE* Handle, ACCESS_MASK Access, const OBJECT_ATTRIBUTES* Attr, int Type, BOOLEAN State);
extern "C" NTSTATUS WINAPI NtSetEvent(HANDLE Handle, LONG* PreviousState);
extern "C" NTSTATUS WINAPI NtResetEvent(HANDLE Handle, LONG* PreviousState);

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

  // Frame times (see StatsLogSetFrameMarks): a "frames" line every second, a "frames10s" line every FrameSummarySeconds,
  // and with FEX_STATSLOG_PROFILE=1 a "# frame" line for every frame of SlowFrameMs or more with what Main was sampled
  // doing during it, read from this and the previous second's samples.
  constexpr double SlowFrameMs = 50.0;
  constexpr size_t MaxSlowFrameLines = 1500;
  constexpr uint64_t FrameSummarySeconds = 10;

  // Native-call sampler (with FEX_STATSLOG_PROFILE=1): ExitFunctionEC (ARM64EC/Module.S) stores the native function it
  // enters in the thread's CHPE CPU area (EmulatorDataInline). Samples of a thread outside emulated code count against the
  // last native function it entered from x86 code: a Wine/Windows API (named by the module's exports), one of FEX's own
  // native redirects, or the native code an x86 callback returned to. Waiting shows up as the wait function.
  constexpr size_t CPUAreaNativeTargetOffset = 0x50;
  constexpr size_t MaxNativeTargets = 32;
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
    uint64_t SuspendCount;
    // Longest single events of the last second (cycles) and when they ended (cycle counter), see ThreadStats.
    uint64_t MaxJITTime;
    uint64_t MaxJITAt;
    uint64_t MaxSignalTime;
    uint64_t MaxSignalAt;
    uint64_t MaxDiskCacheLookupTime;
    uint64_t MaxDiskCacheLookupAt;
    uint64_t MaxCodeLockWaitTime;
    uint64_t MaxCodeLockWaitAt;
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

  // Stall probe, see StatsLogSetLockProbe.
  enum ProbeKind : uint8_t { ProbeSleep, ProbeVQ, ProbeFault, ProbeLock, ProbeServer, ProbeKinds };
  constexpr const char* ProbeNames[ProbeKinds] = {"sleep", "vq", "fault", "fexlock", "server"};
  constexpr int64_t ProbeSleep100ns = 40'000; // 4ms between rounds
  constexpr uint64_t ProbeServerEvery = 4;    // wineserver round trip every 4th round
  constexpr size_t ProbeFaultPages = 4096;    // 16MB, one fresh page per round, then decommitted and committed again
  constexpr double ProbeKeepMs = 2.0;         // events at least this long are kept for "# frame" lines
  constexpr size_t ProbeRingSize = 4096;
  struct ProbeEvent {
    uint64_t End;   // counter value when it ended
    uint64_t Ticks; // how long it took; for sleep, how much later than asked the thread woke up
    uint8_t Kind;
  };
  // Written by the probe thread only.
  struct ProbeShared {
    ProbeEvent Ring[ProbeRingSize];
    std::atomic<uint64_t> Written;
    std::atomic<uint64_t> Max[ProbeKinds]; // longest since the log thread last took them
    std::atomic<uint64_t> MaxAt[ProbeKinds];
    std::atomic<uint64_t> Rounds;
  };
  ProbeShared* Probe {};
  std::atomic<void (*)()> LockProbe {};

  // Scheduler view (on with the stats log; FEX_SCHEDLOG=0 turns it off), read straight from Linux /proc so it costs no
  // wineserver round trips: where the main thread and the wineserver run (sampled every 10ms while runnable), how long
  // each of this process's threads ran and waited for a CPU in the last second (schedstat), and every 5 seconds which
  // other processes of this user used the CPU. "wait" is time a thread was ready to run but had no CPU.
  struct SchedPrev {
    uint64_t RunNs;
    uint64_t WaitNs;
    uint64_t Ticks;
    uint64_t Seen;
  };
  struct SchedState {
    bool On;
    int Pid;           // Linux pid = the main thread's Linux tid
    int MainFd = -1;   // /proc/self/task/<pid>/stat
    int ServerPid;
    int ServerFd = -1; // /proc/<wineserver>/stat
    uint32_t MainSamples, MainRunnable, MainCores[Linux::MaxCPUs];
    uint32_t ServerSamples, ServerRunnable, ServerCores[Linux::MaxCPUs];
    uint32_t Tick;
    uint64_t Second;
    SchedPrev Main, Server;
    // vkd3d-proton's busiest queue thread: it hands the game's GPU work and each frame's present to the driver, so what it
    // waits in tells the GPU apart from the present path. Its /proc syscall file is read every 10ms: "running", or the
    // Linux call it is blocked in with that call's arguments.
    int QueueTid;
    int QueueFd = -1;
    bool QueueUnreadable;
    bool QueueEverRead;
    uint32_t QueueSamples;
    struct CallCount {
      uint64_t Key; // syscall number << 32, plus the ioctl request or futex operation; RunningKey, OutsideKey
      uint32_t N;
    };
    CallCount QueueCalls[10];
    uint32_t QueueOther;
    fextl::unordered_map<int, SchedPrev> Tasks;
    fextl::unordered_map<int, SchedPrev> Procs;
    fextl::unordered_map<int, fextl::string> ProcNames;
  };
  SchedState Sched;

  void SchedOpenServer() {
    Linux::Close(Sched.ServerFd);
    Sched.ServerFd = -1;
    Sched.ServerPid = Linux::FindProcess("wineserver");
    if (Sched.ServerPid) {
      Sched.ServerFd = Linux::Open(fextl::fmt::format("/proc/{}/stat", Sched.ServerPid).c_str());
    }
    Sched.Server = {};
  }

  uint64_t GetCounter();
  uint64_t GetCounterFrequency();

  // Per-core load (with the scheduler view), for load from outside the game, which /proc doesn't show by process (other
  // users' processes are hidden from apps): how busy each CPU was in the last second, whoever ran there, from /proc/stat
  // when Android lets apps read it, otherwise from each CPU's idle time in its cpuidle states (sysfs stateN/time, us). A
  // CPU's time in an idle state is only added when it wakes up, so a CPU that slept through the whole second looks busy:
  // with no idle state entered in the second and not paused, it shows as "?" and is left out of the sums. Busy on the
  // game's CPUs minus this process's and the wineserver's run time is what other processes and the kernel used there
  // (Wine's other processes included, see "sched procs"). Paused: the share of 10 ms samples in which the kernel had a
  // CPU out of use (core control or heat), from sched_getaffinity of the main and log threads against their own masks.
  // Also the number of runnable tasks of the whole system (/proc/loadavg) and the share of the second some task waited
  // for a CPU (/proc/pressure/cpu), when readable.
  constexpr int MaxIdleStates = 10;
  struct CoreLoadState {
    bool On;
    int Cpus;
    bool ProcStat;
    int StatFd = -1;
    int TimeFds[Linux::MaxCPUs][MaxIdleStates];
    int UsageFds[Linux::MaxCPUs][MaxIdleStates];
    int States[Linux::MaxCPUs];
    uint64_t PrevIdle[Linux::MaxCPUs];  // /proc/stat: idle+iowait ticks; cpuidle: us
    uint64_t PrevTotal[Linux::MaxCPUs]; // /proc/stat: all ticks
    uint64_t PrevUsage[Linux::MaxCPUs]; // cpuidle: idle states entered
    bool PrevOk[Linux::MaxCPUs];
    uint64_t PrevCounter;
    bool Have;
    int LogTid;
    fextl::string MainStatus, LogStatus;
    uint64_t Allowed; // the main and log threads' own masks
    uint32_t PauseSamples;
    uint32_t Paused[Linux::MaxCPUs];
    int LoadavgFd = -1;
    int PsiFd = -1;
    uint64_t PrevPsiUs;
    bool HavePsi;
    fextl::string MainCgroup, ServerCgroup;
    uint32_t CgroupNotes;
  };
  CoreLoadState Core;

  // Reads one CPU's counters; false when they can't be read (offline CPU, or no access).
  bool CoreRead(int Cpu, const char* ProcStat, uint64_t& Idle, uint64_t& Total, uint64_t& Usage) {
    if (Core.ProcStat) {
      // "cpuN user nice system idle iowait irq softirq steal ..." in ticks.
      const auto Name = fextl::fmt::format("\ncpu{} ", Cpu);
      const char* Line = ProcStat ? strstr(ProcStat, Name.c_str()) : nullptr;
      if (!Line) {
        return false;
      }
      char* P = const_cast<char*>(Line + Name.size());
      uint64_t Fields[8] {};
      for (auto& F : Fields) {
        F = strtoull(P, &P, 10);
      }
      Idle = Fields[3] + Fields[4];
      Total = 0;
      for (const auto F : Fields) {
        Total += F;
      }
      return true;
    }
    if (!Core.States[Cpu]) {
      return false;
    }
    Idle = Usage = Total = 0;
    for (int i = 0; i < Core.States[Cpu]; ++i) {
      char Buffer[32];
      int64_t Len = Linux::Pread(Core.TimeFds[Cpu][i], Buffer, sizeof(Buffer) - 1, 0);
      if (Len <= 0) {
        return false;
      }
      Buffer[Len] = 0;
      Idle += strtoull(Buffer, nullptr, 10);
      Len = Linux::Pread(Core.UsageFds[Cpu][i], Buffer, sizeof(Buffer) - 1, 0);
      if (Len > 0) {
        Buffer[Len] = 0;
        Usage += strtoull(Buffer, nullptr, 10);
      }
    }
    return true;
  }

  void CoreLoadInit() {
    Core.On = true;
    Core.LogTid = Linux::GetTid();
    Core.MainStatus = fextl::fmt::format("/proc/self/task/{}/status", Sched.Pid);
    Core.LogStatus = fextl::fmt::format("/proc/self/task/{}/status", Core.LogTid);
    uint32_t KHz[Linux::MaxCPUs];
    Linux::ReadMaxClocks(KHz);
    while (Core.Cpus < Linux::MaxCPUs && KHz[Core.Cpus]) {
      ++Core.Cpus;
    }
    char Probe[256];
    Core.StatFd = Linux::Open("/proc/stat");
    const int64_t StatLen = Core.StatFd >= 0 ? Linux::Pread(Core.StatFd, Probe, sizeof(Probe) - 1, 0) : Core.StatFd;
    Core.ProcStat = StatLen > 0;
    if (!Core.ProcStat) {
      Linux::Close(Core.StatFd);
      Core.StatFd = -1;
    }
    int StateTotal {};
    if (!Core.ProcStat) {
      for (int Cpu = 0; Cpu < Core.Cpus; ++Cpu) {
        for (int i = 0; i < MaxIdleStates; ++i) {
          const int TimeFd = Linux::Open(fextl::fmt::format("/sys/devices/system/cpu/cpu{}/cpuidle/state{}/time", Cpu, i).c_str());
          if (TimeFd < 0) {
            break;
          }
          Core.TimeFds[Cpu][i] = TimeFd;
          Core.UsageFds[Cpu][i] = Linux::Open(fextl::fmt::format("/sys/devices/system/cpu/cpu{}/cpuidle/state{}/usage", Cpu, i).c_str());
          ++Core.States[Cpu];
          ++StateTotal;
        }
      }
    }
    Core.LoadavgFd = Linux::Open("/proc/loadavg");
    if (Core.LoadavgFd >= 0 && Linux::Pread(Core.LoadavgFd, Probe, sizeof(Probe) - 1, 0) <= 0) {
      Linux::Close(Core.LoadavgFd);
      Core.LoadavgFd = -1;
    }
    Core.PsiFd = Linux::Open("/proc/pressure/cpu");
    if (Core.PsiFd >= 0 && Linux::Pread(Core.PsiFd, Probe, sizeof(Probe) - 1, 0) <= 0) {
      Linux::Close(Core.PsiFd);
      Core.PsiFd = -1;
    }
    StatsLogNote(fextl::fmt::format("cores: per-core load of {} CPUs from {}; system runnable tasks {}; cpu pressure {}", Core.Cpus,
                                    Core.ProcStat  ? fextl::string("/proc/stat") :
                                    StateTotal     ? fextl::fmt::format("cpuidle ({} idle states; /proc/stat not readable, error {})", StateTotal, StatLen) :
                                                     fextl::fmt::format("nothing: /proc/stat (error {}) and cpuidle not readable", StatLen),
                                    Core.LoadavgFd >= 0 ? "from /proc/loadavg" : "not readable",
                                    Core.PsiFd >= 0 ? "from /proc/pressure/cpu" : "not readable"));
  }

  // Every 10ms: which of the game's CPUs the kernel has out of use.
  void CorePauseSample() {
    if (!Core.On || !Core.Allowed) {
      return;
    }
    const uint64_t Usable = Linux::GetAffinity(Sched.Pid) | Linux::GetAffinity(Core.LogTid);
    const uint64_t Paused = Core.Allowed & ~Usable;
    ++Core.PauseSamples;
    for (int i = 0; Paused && i < Linux::MaxCPUs; ++i) {
      if (Paused & (1ull << i)) {
        ++Core.Paused[i];
      }
    }
  }

  fextl::string CoreLine(double GameRunMs, double ServerRunMs) {
    if (!Core.On || !Core.Cpus) {
      return {};
    }
    const uint64_t Counter = GetCounter();
    char StatBuffer[4096];
    const char* StatText {};
    if (Core.ProcStat) {
      const int64_t Len = Linux::Pread(Core.StatFd, StatBuffer + 1, sizeof(StatBuffer) - 2, 0);
      StatBuffer[0] = '\n';
      StatBuffer[Len > 0 ? Len + 1 : 1] = 0;
      StatText = StatBuffer;
    }
    uint64_t Idle[Linux::MaxCPUs] {}, Total[Linux::MaxCPUs] {}, Usage[Linux::MaxCPUs] {};
    bool Ok[Linux::MaxCPUs] {};
    for (int Cpu = 0; Cpu < Core.Cpus; ++Cpu) {
      Ok[Cpu] = CoreRead(Cpu, StatText, Idle[Cpu], Total[Cpu], Usage[Cpu]);
    }
    fextl::string Out;
    if (Core.Have) {
      const double Wall = static_cast<double>(Counter - Core.PrevCounter) / static_cast<double>(GetCounterFrequency());
      fextl::string Text;
      double GameCpusBusy {};
      int Known {}, InGame {};
      for (int Cpu = 0; Cpu < Core.Cpus; ++Cpu) {
        const bool Game = Core.Allowed & (1ull << Cpu);
        InGame += Game;
        const uint32_t PausedPct = Core.PauseSamples ? (Core.Paused[Cpu] * 100 + Core.PauseSamples / 2) / Core.PauseSamples : 0;
        double Busy = -1.0;
        if (Ok[Cpu] && Core.PrevOk[Cpu]) {
          if (Core.ProcStat) {
            const uint64_t Dt = Total[Cpu] - Core.PrevTotal[Cpu];
            Busy = Dt ? 1.0 - static_cast<double>(Idle[Cpu] - Core.PrevIdle[Cpu]) / static_cast<double>(Dt) : -1.0;
          } else if (Usage[Cpu] != Core.PrevUsage[Cpu] || Idle[Cpu] != Core.PrevIdle[Cpu]) {
            Busy = 1.0 - static_cast<double>(Idle[Cpu] - Core.PrevIdle[Cpu]) / (Wall * 1e6);
          } else if (PausedPct >= 90) {
            Busy = 0.0; // asleep the whole second because it was paused
          }
        }
        if (Busy >= 0.0) {
          Busy = std::clamp(Busy, 0.0, 1.0);
          Text += fextl::fmt::format(" {}:{:.0f}", Cpu, Busy * 100.0);
          if (Game) {
            GameCpusBusy += Busy;
            ++Known;
          }
        } else {
          Text += fextl::fmt::format(" {}:?", Cpu);
        }
      }
      Out = fextl::fmt::format("  cores busy%:{}", Text);
      if (Core.Allowed) {
        const double Game = GameRunMs / 1000.0 / Wall;
        const double Server = ServerRunMs / 1000.0 / Wall;
        Out += fextl::fmt::format(" | game cpus {:#x} busy {:.2f}{} = game {:.2f} + server {:.2f} + others ~{:.2f}", Core.Allowed,
                                  GameCpusBusy, Known < InGame ? fextl::fmt::format(" ({} of {} known)", Known, InGame) : fextl::string {},
                                  Game, Server, std::max(GameCpusBusy - Game - Server, 0.0));
      }
      fextl::string PausedText;
      for (int Cpu = 0; Cpu < Core.Cpus; ++Cpu) {
        if (Core.Paused[Cpu]) {
          PausedText += fextl::fmt::format(" {}:{}%", Cpu, (Core.Paused[Cpu] * 100 + Core.PauseSamples / 2) / std::max<uint32_t>(Core.PauseSamples, 1));
        }
      }
      if (!PausedText.empty()) {
        Out += fextl::fmt::format(" | paused{}", PausedText);
      }
    }
    if (Core.LoadavgFd >= 0) {
      char Buffer[128];
      const int64_t Len = Linux::Pread(Core.LoadavgFd, Buffer, sizeof(Buffer) - 1, 0);
      if (Len > 0) {
        Buffer[Len] = 0;
        // "0.50 0.60 0.70 3/1234 5678": the fourth field is runnable/all.
        const char* P = Buffer;
        for (int Field = 0; Field < 3 && P; ++Field) {
          P = strchr(P, ' ');
          P = P ? P + 1 : nullptr;
        }
        if (P && Core.Have) {
          Out += fextl::fmt::format(" | runnable {}", strtoull(P, nullptr, 10));
        }
      }
    }
    if (Core.PsiFd >= 0) {
      char Buffer[256];
      const int64_t Len = Linux::Pread(Core.PsiFd, Buffer, sizeof(Buffer) - 1, 0);
      if (Len > 0) {
        Buffer[Len] = 0;
        // "some avg10=1.23 avg60=... avg300=... total=<us>"
        if (const char* T = strstr(Buffer, "total=")) {
          const uint64_t TotalUs = strtoull(T + 6, nullptr, 10);
          if (Core.HavePsi && Core.Have) {
            const double Wall = static_cast<double>(Counter - Core.PrevCounter) / static_cast<double>(GetCounterFrequency());
            Out += fextl::fmt::format(" | cpu pressure {:.0f}%", static_cast<double>(TotalUs - Core.PrevPsiUs) / (Wall * 1e4));
          }
          Core.PrevPsiUs = TotalUs;
          Core.HavePsi = true;
        }
      }
    }
    for (int Cpu = 0; Cpu < Core.Cpus; ++Cpu) {
      Core.PrevIdle[Cpu] = Idle[Cpu];
      Core.PrevTotal[Cpu] = Total[Cpu];
      Core.PrevUsage[Cpu] = Usage[Cpu];
      Core.PrevOk[Cpu] = Ok[Cpu];
    }
    Core.PrevCounter = Counter;
    Core.Have = true;
    Core.Allowed = Linux::ReadAllowedMask(Core.MainStatus.c_str()) | Linux::ReadAllowedMask(Core.LogStatus.c_str());
    Core.PauseSamples = 0;
    memset(Core.Paused, 0, sizeof(Core.Paused));
    return Out.empty() ? Out : Out + "\n";
  }

  // The first line of a small file, or "-".
  fextl::string FirstLine(const fextl::string& Path) {
    char Buffer[128];
    if (Linux::ReadSmallFile(Path.c_str(), Buffer, sizeof(Buffer)) <= 0) {
      return "-";
    }
    char* End = strchr(Buffer, '\n');
    if (End) {
      *End = 0;
    }
    return Buffer;
  }

  // Which cgroups a task is in ("ctrl:/path ..."), with the CPUs of its cpuset and the limits of its cpu group when
  // readable. Android sorts apps into groups (top-app, foreground, background, ...) with their own CPUs and CPU share; a
  // launcher's child processes stay in the group they started in.
  fextl::string CgroupText(const fextl::string& Path, bool Details) {
    char Buffer[1024];
    if (Linux::ReadSmallFile(Path.c_str(), Buffer, sizeof(Buffer)) <= 0) {
      return {};
    }
    fextl::string Out;
    for (char* Line = Buffer; Line && *Line;) {
      char* Next = strchr(Line, '\n');
      if (Next) {
        *Next++ = 0;
      }
      // "N:controllers:/path"
      const char* Ctrl = strchr(Line, ':');
      const char* GroupPath = Ctrl ? strchr(Ctrl + 1, ':') : nullptr;
      if (Ctrl && GroupPath) {
        const fextl::string Controllers(Ctrl + 1, GroupPath - Ctrl - 1);
        const fextl::string Group(GroupPath + 1);
        Out += fextl::fmt::format(" {}:{}", Controllers.empty() ? "v2" : Controllers.c_str(), Group);
        if (Details) {
          const fextl::string Sub = Group == "/" ? fextl::string {} : Group;
          if (Controllers == "cpuset") {
            Out += fextl::fmt::format("(cpus {})", FirstLine(fextl::fmt::format("/dev/cpuset{}/cpus", Sub)));
          } else if (Controllers == "cpu" || Controllers == "cpu,cpuacct" || Controllers == "cpuctl") {
            Out += fextl::fmt::format("(uclamp.min {} max {} shares {})", FirstLine(fextl::fmt::format("/dev/cpuctl{}/cpu.uclamp.min", Sub)),
                                      FirstLine(fextl::fmt::format("/dev/cpuctl{}/cpu.uclamp.max", Sub)),
                                      FirstLine(fextl::fmt::format("/dev/cpuctl{}/cpu.shares", Sub)));
          }
        }
      }
      Line = Next;
    }
    return Out;
  }

  // Every second: notes when the main thread's or the wineserver's cgroups change (and once at the start).
  void CgroupCheck() {
    if (!Core.On) {
      return;
    }
    auto Check = [](const char* Who, const fextl::string& Path, fextl::string& Last) {
      fextl::string Now = CgroupText(Path, false);
      if (Now.empty() || Now == Last) {
        return;
      }
      Last = Now;
      if (++Core.CgroupNotes <= 20 || Core.CgroupNotes % 50 == 0) {
        StatsLogNote(fextl::fmt::format("cgroup: {}{}", Who, CgroupText(Path, true)));
      }
    };
    Check("main thread", fextl::fmt::format("/proc/self/task/{}/cgroup", Sched.Pid), Core.MainCgroup);
    if (Sched.ServerPid) {
      Check("wineserver", fextl::fmt::format("/proc/{}/cgroup", Sched.ServerPid), Core.ServerCgroup);
    }
  }

  // Every second, the whole device: what kind of CPU time the busy CPUs spent (user, low priority user, kernel,
  // interrupts), how much of it this app's Android group and its user had (cgroup v2 cpu.stat, which also counts the
  // launcher's own process and Wine's other processes, hidden from /proc or not), and how short memory is (free memory,
  // swap, the kernel's reclaim and pressure), with this process's resident and GPU memory. For load from outside the
  // game: build 56 saw 5 of 8 cores busy outside it for 90 s after a load, with no visible process using them.
  struct SystemLoadState {
    bool On;
    bool Have;
    uint64_t PrevCounter;
    uint64_t PrevCpu[8];           // /proc/stat "cpu" line: user nice system idle iowait irq softirq steal (ticks)
    uint64_t PrevUser, PrevSystem; // this process (ticks)
    fextl::string AppCpuStat, UidCpuStat;
    uint64_t PrevAppUs, PrevUidUs;
    bool HaveApp, HaveUid;
    uint64_t PrevVm[7];  // pswpin pswpout pgscan_kswapd pgscan_direct workingset refaults allocstall pgmajfault (pages/count)
    uint64_t PrevPsi[3]; // memory some, memory full, io some (us)
    fextl::string GpuMemMapped, GpuMemUnmapped;
  };
  SystemLoadState System;

  char SystemBuffer[16384];

  // The number after Key ("Key value" or "Key=value" form) in Text, at a line start (Key begins with '\n') or anywhere.
  bool SystemField(const char* Text, const char* Key, uint64_t& Out) {
    const char* P = strstr(Text, Key);
    if (!P) {
      return false;
    }
    P += strlen(Key);
    while (*P == ' ' || *P == ':' || *P == '=') {
      ++P;
    }
    Out = strtoull(P, nullptr, 10);
    return true;
  }

  uint64_t SystemCgroupUs(const fextl::string& Path, bool& Ok) {
    uint64_t Us {};
    Ok = Linux::ReadSmallFile(Path.c_str(), SystemBuffer, sizeof(SystemBuffer)) > 0 && SystemField(SystemBuffer, "usage_usec", Us);
    return Us;
  }

  // Every second, the CPU use of each Android user (cgroup v2: Android puts every app and every system service in
  // /sys/fs/cgroup/uid_<its user>/pid_<its process>), the busy time in no user's group (kernel threads and interrupts),
  // and the processes each user started and ended. Build 57 saw 5-6 of 8 cores busy outside this app's user for 60-80 s
  // after area loads; Android hides other users' processes in /proc, but not their groups.
  struct UserCpu {
    uint32_t Uid;
    uint64_t UsageUs, SystemUs;
    fextl::vector<uint32_t> Pids; // sorted
  };
  struct UsersState {
    bool On;
    bool Listable; // /sys/fs/cgroup can be listed; else Probed holds the users found once at the start
    bool Have;
    uint32_t OwnUid;
    fextl::vector<UserCpu> Prev; // sorted by Uid
    fextl::vector<uint32_t> Probed;
  };
  UsersState Users;

  struct NumberedNames {
    const char* Prefix;
    size_t PrefixLen;
    fextl::vector<uint32_t>* Out;
  };

  // Collects N from directory entries named <Prefix>N.
  void CollectNumbered(const char* Name, void* Arg) {
    auto* A = static_cast<NumberedNames*>(Arg);
    if (strncmp(Name, A->Prefix, A->PrefixLen) != 0) {
      return;
    }
    const char* P = Name + A->PrefixLen;
    if (*P < '0' || *P > '9') {
      return;
    }
    char* End {};
    const unsigned long long Value = strtoull(P, &End, 10);
    if (*End == 0) {
      A->Out->push_back(static_cast<uint32_t>(Value));
    }
  }

  bool ListNumbered(const char* Path, const char* Prefix, fextl::vector<uint32_t>& Out) {
    Out.clear();
    NumberedNames Args {Prefix, strlen(Prefix), &Out};
    if (!Linux::ForEachEntry(Path, CollectNumbered, &Args)) {
      return false;
    }
    std::sort(Out.begin(), Out.end());
    return true;
  }

  fextl::string UserName(uint32_t Uid) {
    const uint32_t AppId = Uid % 100000;
    const char* Known {};
    switch (AppId) {
    case 0: Known = "root"; break;
    case 1000: Known = "system"; break;
    case 1001: Known = "phone"; break;
    case 1002: Known = "bluetooth"; break;
    case 1010: Known = "wifi"; break;
    case 1013: Known = "media"; break;
    case 1019: Known = "drm"; break;
    case 1021: Known = "gps"; break;
    case 1027: Known = "nfc"; break;
    case 1036: Known = "logd"; break;
    case 1041: Known = "audioserver"; break;
    case 1046: Known = "mediacodec"; break;
    case 1047: Known = "cameraserver"; break;
    case 1066: Known = "statsd"; break;
    case 1069: Known = "lmkd"; break;
    case 1073: Known = "network stack"; break;
    case 2000: Known = "shell"; break;
    case 9999: Known = "nobody"; break;
    default: break;
    }
    if (Known) {
      return Uid >= 100000 ? fextl::fmt::format("{} (user {})", Known, Uid / 100000) : fextl::string(Known);
    }
    if (AppId >= 10000 && AppId < 20000) {
      return fextl::fmt::format("app {}", Uid);
    }
    if (AppId >= 90000 && AppId < 99000) {
      return fextl::fmt::format("app zygote child {}", Uid);
    }
    if (AppId >= 99000) {
      return fextl::fmt::format("isolated {}", Uid);
    }
    return fextl::fmt::format("uid {}", Uid);
  }

  bool ReadUserCpu(uint32_t Uid, UserCpu& Out) {
    char Path[64];
    snprintf(Path, sizeof(Path), "/sys/fs/cgroup/uid_%u/cpu.stat", Uid);
    if (Linux::ReadSmallFile(Path, SystemBuffer, sizeof(SystemBuffer)) <= 0 || !SystemField(SystemBuffer, "usage_usec", Out.UsageUs)) {
      return false;
    }
    SystemField(SystemBuffer, "system_usec", Out.SystemUs);
    Out.Uid = Uid;
    return true;
  }

  void UsersInit(uint32_t OwnUid) {
    Users.OwnUid = OwnUid;
    fextl::vector<uint32_t> Uids;
    Users.Listable = ListNumbered("/sys/fs/cgroup", "uid_", Uids) && !Uids.empty();
    if (!Users.Listable) {
      // Can't list: look for the usual users once (system services, the first apps).
      const auto Probe = [&](uint32_t First, uint32_t Last) {
        UserCpu U {};
        for (uint32_t Uid = First; Uid <= Last; ++Uid) {
          if (ReadUserCpu(Uid, U)) {
            Users.Probed.push_back(Uid);
          }
        }
      };
      Probe(0, 0);
      Probe(1000, 1099);
      Probe(2000, 2000);
      Probe(10000, 10999);
      Uids = Users.Probed;
    }
    fextl::vector<uint32_t> Pids;
    char Path[64];
    snprintf(Path, sizeof(Path), "/sys/fs/cgroup/uid_%u", OwnUid);
    const bool PidsOk = ListNumbered(Path, "pid_", Pids) && !Pids.empty();
    Users.On = !Uids.empty();
    StatsLogNote(fextl::fmt::format("users: CPU use of {} Android users {}, {}", Uids.size(),
                                    Users.Listable ? "(all, listed every second)" : "(not listable: the usual ones, found once)",
                                    PidsOk ? "their process starts and ends counted" : "process starts and ends not readable"));
  }

  // The 'users busy' line. BusyCores: the whole device's busy CPU time this second, in cores (< 0 when unknown).
  fextl::string UsersLine(bool Show, double Wall, double BusyCores) {
    if (!Users.On) {
      return {};
    }
    fextl::vector<uint32_t> Uids;
    if (Users.Listable) {
      ListNumbered("/sys/fs/cgroup", "uid_", Uids);
    } else {
      Uids = Users.Probed;
    }
    fextl::vector<UserCpu> Now;
    Now.reserve(Uids.size());
    char Path[64];
    for (const uint32_t Uid : Uids) {
      UserCpu U {};
      if (!ReadUserCpu(Uid, U)) {
        continue;
      }
      snprintf(Path, sizeof(Path), "/sys/fs/cgroup/uid_%u", Uid);
      ListNumbered(Path, "pid_", U.Pids);
      Now.push_back(std::move(U));
    }
    fextl::string Out;
    if (Show && Users.Have) {
      struct Row {
        uint32_t Uid;
        double Cores, Kernel;
        uint32_t Started, Ended;
        size_t Procs;
      };
      fextl::vector<Row> Rows;
      uint64_t SumUs {};
      uint32_t Started {}, Ended {};
      const auto Count = [](const fextl::vector<uint32_t>& A, const fextl::vector<uint32_t>& B) {
        // Entries of A not in B (both sorted).
        uint32_t N {};
        size_t j {};
        for (const uint32_t V : A) {
          while (j < B.size() && B[j] < V) {
            ++j;
          }
          N += j == B.size() || B[j] != V;
        }
        return N;
      };
      static const fextl::vector<uint32_t> NoPids;
      size_t i {}, j {};
      while (i < Users.Prev.size() || j < Now.size()) {
        const UserCpu* P = i < Users.Prev.size() ? &Users.Prev[i] : nullptr;
        const UserCpu* N = j < Now.size() ? &Now[j] : nullptr;
        if (P && N && P->Uid == N->Uid) {
          ++i, ++j;
        } else if (N && (!P || N->Uid < P->Uid)) {
          P = nullptr; // a new user: all of its use and processes are new
          ++j;
        } else {
          N = nullptr; // a user whose group is gone
          ++i;
        }
        const uint64_t Usage = N ? N->UsageUs - std::min(N->UsageUs, P ? P->UsageUs : 0) : 0;
        const uint64_t Kernel = N ? N->SystemUs - std::min(N->SystemUs, P ? P->SystemUs : 0) : 0;
        const uint32_t Start = Count(N ? N->Pids : NoPids, P ? P->Pids : NoPids);
        const uint32_t End = Count(P ? P->Pids : NoPids, N ? N->Pids : NoPids);
        SumUs += Usage;
        const uint32_t Uid = N ? N->Uid : P->Uid;
        if (Uid == Users.OwnUid) {
          continue;
        }
        Started += Start;
        Ended += End;
        Rows.push_back(
          {Uid, static_cast<double>(Usage) / 1e6 / Wall, static_cast<double>(Kernel) / 1e6 / Wall, Start, End, N ? N->Pids.size() : 0});
      }
      std::sort(Rows.begin(), Rows.end(), [](const Row& a, const Row& b) { return a.Cores > b.Cores; });
      Out = "  users busy:";
      if (BusyCores >= 0) {
        Out += fextl::fmt::format(" kernel threads+irq {:.1f}", std::max(0.0, BusyCores - static_cast<double>(SumUs) / 1e6 / Wall));
      }
      size_t Listed {};
      for (const auto& R : Rows) {
        if (Listed == 6 || R.Cores < 0.05) {
          break;
        }
        Out += fextl::fmt::format(" | {} {:.1f} (kernel {:.1f}, {} procs", UserName(R.Uid), R.Cores, R.Kernel, R.Procs);
        if (R.Started) {
          Out += fextl::fmt::format(", +{}", R.Started);
        }
        if (R.Ended) {
          Out += fextl::fmt::format(", -{}", R.Ended);
        }
        Out += ")";
        ++Listed;
      }
      Out += fextl::fmt::format(" | outside this app: {} processes started, {} ended\n", Started, Ended);
    }
    Users.Prev = std::move(Now);
    Users.Have = true;
    return Out;
  }

  void SystemInit() {
    System.On = true;
    // "0::/uid_10121/pid_22319": this process's cgroup v2 group (the app's process tree) and its parent (the app's user).
    char Buffer[1024];
    fextl::string Group;
    if (Linux::ReadSmallFile("/proc/self/cgroup", Buffer, sizeof(Buffer)) > 0) {
      if (const char* P = strstr(Buffer, "0::")) {
        const char* End = strchr(P, '\n');
        Group.assign(P + 3, End ? End : P + strlen(P));
      }
    }
    fextl::string AppNote = "no cgroup v2 group";
    if (!Group.empty() && Group != "/") {
      System.AppCpuStat = fextl::fmt::format("/sys/fs/cgroup{}/cpu.stat", Group);
      const size_t Slash = Group.rfind('/');
      if (Slash != fextl::string::npos && Slash > 0) {
        System.UidCpuStat = fextl::fmt::format("/sys/fs/cgroup{}/cpu.stat", Group.substr(0, Slash));
      }
      bool AppOk {}, UidOk {};
      SystemCgroupUs(System.AppCpuStat, AppOk);
      if (!System.UidCpuStat.empty()) {
        SystemCgroupUs(System.UidCpuStat, UidOk);
      }
      AppNote = fextl::fmt::format("group {} {}, its parent {}", Group, AppOk ? "readable" : "not readable",
                                   System.UidCpuStat.empty() ? "-" :
                                   UidOk                     ? "readable" :
                                                               "not readable");
      if (!AppOk) {
        System.AppCpuStat.clear();
      }
      if (!UidOk) {
        System.UidCpuStat.clear();
      }
    }
    uint64_t TotalKB {}, SwapKB {};
    const bool Mem =
      Linux::ReadSmallFile("/proc/meminfo", SystemBuffer, sizeof(SystemBuffer)) > 0 && SystemField(SystemBuffer, "MemTotal", TotalKB);
    SystemField(SystemBuffer, "SwapTotal", SwapKB);
    const bool Vm = Linux::ReadSmallFile("/proc/vmstat", SystemBuffer, sizeof(SystemBuffer)) > 0;
    const bool Psi = Linux::ReadSmallFile("/proc/pressure/memory", SystemBuffer, sizeof(SystemBuffer)) > 0;
    // Adreno's per-process GPU memory (kgsl), when readable.
    const int Pid = Linux::GetPid();
    System.GpuMemMapped = fextl::fmt::format("/sys/class/kgsl/kgsl/proc/{}/gpumem_mapped", Pid);
    System.GpuMemUnmapped = fextl::fmt::format("/sys/class/kgsl/kgsl/proc/{}/gpumem_unmapped", Pid);
    StatsLogNote(fextl::fmt::format("system: memory {} MB, swap {} MB; cgroup cpu use: {}; /proc/vmstat {}; memory pressure {}; "
                                    "GPU memory per process from kgsl (checked once the game uses the GPU)",
                                    Mem ? fextl::fmt::format("{}", TotalKB / 1024) : fextl::string("?"), SwapKB / 1024, AppNote,
                                    Vm ? "readable" : "not readable", Psi ? "readable" : "not readable"));
    uint32_t OwnUid = ~0u;
    if (const size_t At = Group.find("/uid_"); At != fextl::string::npos) {
      OwnUid = static_cast<uint32_t>(strtoull(Group.c_str() + At + 5, nullptr, 10));
    }
    UsersInit(OwnUid);
  }

  fextl::string SystemLine(double ServerRunMs) {
    if (!System.On) {
      return {};
    }
    const uint64_t Counter = GetCounter();
    const double Wall = System.Have ? static_cast<double>(Counter - System.PrevCounter) / static_cast<double>(GetCounterFrequency()) : 0.0;
    const bool Show = System.Have && Wall > 0.2;
    auto Cores = [&](uint64_t Ticks) {
      return static_cast<double>(Ticks) / 100.0 / Wall;
    };
    auto MBs = [&](uint64_t Pages) {
      return static_cast<double>(Pages) * 4096.0 / 1048576.0 / Wall;
    };
    fextl::string Out;

    // CPU time of the whole device by kind.
    uint64_t Cpu[8] {};
    if (Linux::ReadSmallFile("/proc/stat", SystemBuffer, sizeof(SystemBuffer)) > 0 && !strncmp(SystemBuffer, "cpu ", 4)) {
      char* P = SystemBuffer + 4;
      for (auto& F : Cpu) {
        F = strtoull(P, &P, 10);
      }
    }
    Linux::TaskStat Self {};
    const bool HaveSelf = Linux::ReadTaskStat("/proc/self/stat", Self);
    double BusyCores = -1.0;
    if (Show && Cpu[0]) {
      uint64_t D[8];
      for (int i = 0; i < 8; ++i) {
        D[i] = Cpu[i] - System.PrevCpu[i];
      }
      const uint64_t Busy = D[0] + D[1] + D[2] + D[5] + D[6] + D[7];
      BusyCores = Cores(Busy);
      Out += fextl::fmt::format("  system busy {:.1f} cores = user {:.1f} + low prio {:.1f} + kernel {:.1f} + irq {:.1f}, io wait {:.1f}",
                                Cores(Busy), Cores(D[0]), Cores(D[1]), Cores(D[2]), Cores(D[5] + D[6] + D[7]), Cores(D[4]));
      if (HaveSelf) {
        Out += fextl::fmt::format(" | this process {:.1f} (kernel {:.1f}), wineserver {:.1f}",
                                  Cores(Self.UserTicks + Self.SystemTicks - System.PrevUser - System.PrevSystem),
                                  Cores(Self.SystemTicks - System.PrevSystem), ServerRunMs / 1000.0 / Wall);
      }
    }
    memcpy(System.PrevCpu, Cpu, sizeof(Cpu));
    if (HaveSelf) {
      System.PrevUser = Self.UserTicks;
      System.PrevSystem = Self.SystemTicks;
    }

    // This app's group and its user.
    if (!System.AppCpuStat.empty() || !System.UidCpuStat.empty()) {
      bool AppOk {}, UidOk {};
      const uint64_t AppUs = System.AppCpuStat.empty() ? 0 : SystemCgroupUs(System.AppCpuStat, AppOk);
      const uint64_t UidUs = System.UidCpuStat.empty() ? 0 : SystemCgroupUs(System.UidCpuStat, UidOk);
      if (Show && !Out.empty()) {
        Out += " | app group ";
        Out += AppOk && System.HaveApp ? fextl::fmt::format("{:.1f}", static_cast<double>(AppUs - System.PrevAppUs) / 1e6 / Wall) :
                                         fextl::string("?");
        Out += ", app user ";
        Out += UidOk && System.HaveUid ? fextl::fmt::format("{:.1f}", static_cast<double>(UidUs - System.PrevUidUs) / 1e6 / Wall) :
                                         fextl::string("?");
      }
      System.PrevAppUs = AppUs;
      System.PrevUidUs = UidUs;
      System.HaveApp = AppOk;
      System.HaveUid = UidOk;
    }
    if (!Out.empty()) {
      Out += "\n";
    }
    Out += UsersLine(Show, Wall, BusyCores);

    // Memory.
    fextl::string Mem;
    uint64_t AvailKB {}, SwapTotalKB {}, SwapFreeKB {};
    if (Linux::ReadSmallFile("/proc/meminfo", SystemBuffer, sizeof(SystemBuffer)) > 0 && SystemField(SystemBuffer, "MemAvailable", AvailKB)) {
      SystemField(SystemBuffer, "SwapTotal", SwapTotalKB);
      SystemField(SystemBuffer, "SwapFree", SwapFreeKB);
      uint64_t CachedKB {};
      SystemField(SystemBuffer, "\nCached", CachedKB);
      Mem += fextl::fmt::format("  memory free {} MB, file cache {} MB, swap used {} MB", AvailKB / 1024, CachedKB / 1024,
                                (SwapTotalKB - std::min(SwapFreeKB, SwapTotalKB)) / 1024);
    }
    uint64_t Vm[7] {};
    bool HaveVm = Linux::ReadSmallFile("/proc/vmstat", SystemBuffer, sizeof(SystemBuffer)) > 0;
    if (HaveVm) {
      SystemField(SystemBuffer, "\npswpin ", Vm[0]);
      SystemField(SystemBuffer, "\npswpout ", Vm[1]);
      SystemField(SystemBuffer, "\npgscan_kswapd ", Vm[2]);
      SystemField(SystemBuffer, "\npgscan_direct ", Vm[3]);
      if (!SystemField(SystemBuffer, "\nworkingset_refault_file ", Vm[4])) {
        SystemField(SystemBuffer, "\nworkingset_refault ", Vm[4]);
      }
      for (const char* P = SystemBuffer; (P = strstr(P, "\nallocstall")) != nullptr; ++P) {
        const char* Space = strchr(P + 1, ' ');
        if (Space) {
          Vm[5] += strtoull(Space + 1, nullptr, 10);
        }
      }
      SystemField(SystemBuffer, "\npgmajfault ", Vm[6]);
      if (Show) {
        uint64_t D[7];
        for (int i = 0; i < 7; ++i) {
          D[i] = Vm[i] - System.PrevVm[i];
        }
        Mem += fextl::fmt::format("{}swap out {:.1f} in {:.1f} MB/s, reclaim scan kswapd {:.0f} direct {:.0f} MB/s, stalls {}, "
                                  "refaults {:.1f} MB/s, major faults {}",
                                  Mem.empty() ? "  memory " : ", ", MBs(D[1]), MBs(D[0]), MBs(D[2]), MBs(D[3]), D[5], MBs(D[4]), D[6]);
      }
      memcpy(System.PrevVm, Vm, sizeof(Vm));
    }
    uint64_t Psi[3] {};
    bool HavePsi = false;
    if (Linux::ReadSmallFile("/proc/pressure/memory", SystemBuffer, sizeof(SystemBuffer)) > 0) {
      // "some avg10=... total=N\nfull avg10=... total=N"
      const char* Full = strstr(SystemBuffer, "full");
      HavePsi = SystemField(SystemBuffer, "total", Psi[0]);
      if (Full) {
        SystemField(Full, "total", Psi[1]);
      }
    }
    if (Linux::ReadSmallFile("/proc/pressure/io", SystemBuffer, sizeof(SystemBuffer)) > 0) {
      SystemField(SystemBuffer, "total", Psi[2]);
    }
    if (Show && HavePsi) {
      Mem += fextl::fmt::format(
        " | pressure memory {:.0f}% (all stuck {:.0f}%), io {:.0f}%", static_cast<double>(Psi[0] - System.PrevPsi[0]) / (Wall * 1e4),
        static_cast<double>(Psi[1] - System.PrevPsi[1]) / (Wall * 1e4), static_cast<double>(Psi[2] - System.PrevPsi[2]) / (Wall * 1e4));
    }
    memcpy(System.PrevPsi, Psi, sizeof(Psi));

    // This process: resident memory, swapped out, GPU memory.
    if (Linux::ReadSmallFile("/proc/self/status", SystemBuffer, sizeof(SystemBuffer)) > 0) {
      uint64_t RssKB {}, SwapKB {}, AnonKB {}, FileKB {}, ShmemKB {};
      if (SystemField(SystemBuffer, "\nVmRSS", RssKB)) {
        SystemField(SystemBuffer, "\nVmSwap", SwapKB);
        if (SystemField(SystemBuffer, "\nRssAnon", AnonKB)) {
          SystemField(SystemBuffer, "\nRssFile", FileKB);
          SystemField(SystemBuffer, "\nRssShmem", ShmemKB);
          Mem += fextl::fmt::format(" | this process {} MB (own {}, files {}, shared {}; swapped {} MB)", RssKB / 1024, AnonKB / 1024,
                                    FileKB / 1024, ShmemKB / 1024, SwapKB / 1024);
        } else {
          Mem += fextl::fmt::format(" | this process {} MB (swapped {} MB)", RssKB / 1024, SwapKB / 1024);
        }
      }
    }
    if (!System.GpuMemMapped.empty()) {
      char Buffer[64];
      uint64_t Gpu {};
      bool Ok = false;
      for (const auto* Path : {&System.GpuMemMapped, &System.GpuMemUnmapped}) {
        if (Linux::ReadSmallFile(Path->c_str(), Buffer, sizeof(Buffer)) > 0) {
          Gpu += strtoull(Buffer, nullptr, 10);
          Ok = true;
        }
      }
      if (Ok) {
        Mem += fextl::fmt::format(", GPU {} MB", Gpu >> 20);
      }
    }
    if (Show && !Mem.empty()) {
      Out += Mem;
      Out += "\n";
    }

    System.PrevCounter = Counter;
    System.Have = true;
    return Out;
  }

  void SchedInit() {
    const char* Value = Switch("FEX_SCHEDLOG");
    Sched.On = !(Value && Value[0] == '0');
    if (!Sched.On) {
      return;
    }
    Sched.Pid = Linux::GetPid();
    Sched.MainFd = Linux::Open(fextl::fmt::format("/proc/self/task/{}/stat", Sched.Pid).c_str());
    SchedOpenServer();
    CoreLoadInit();
    SystemInit();
  }

  void SchedSampleOne(int Fd, uint32_t& Samples, uint32_t& Runnable, uint32_t (&Cores)[Linux::MaxCPUs]) {
    if (Fd < 0) {
      return;
    }
    char Buffer[1024];
    const int64_t Len = Linux::Pread(Fd, Buffer, sizeof(Buffer) - 1, 0);
    if (Len <= 0) {
      return;
    }
    Buffer[Len] = 0;
    Linux::TaskStat S {};
    if (!Linux::ParseTaskStat(Buffer, S)) {
      return;
    }
    ++Samples;
    if (S.State == 'R') {
      ++Runnable;
      if (S.Processor >= 0 && S.Processor < Linux::MaxCPUs) {
        ++Cores[S.Processor];
      }
    }
  }

  constexpr uint64_t RunningKey = ~0ull;
  constexpr uint64_t OutsideKey = ~1ull; // blocked, but not in a system call

  // One sample of what the queue thread is doing, from /proc/self/task/<tid>/syscall.
  void SchedSampleQueue() {
    if (Sched.QueueFd < 0) {
      return;
    }
    char Buffer[256];
    const int64_t Len = Linux::Pread(Sched.QueueFd, Buffer, sizeof(Buffer) - 1, 0);
    if (Len <= 0) {
      // Never readable: say so once and stop. Readable before: the thread is gone, find the next busiest one.
      if (!Sched.QueueEverRead) {
        Sched.QueueUnreadable = true;
        StatsLogNote(fextl::fmt::format("sched: can't read what vkd3d's queue thread {} is doing (/proc syscall file, error {})", Sched.QueueTid, Len));
      }
      Linux::Close(Sched.QueueFd);
      Sched.QueueFd = -1;
      Sched.QueueTid = 0;
      return;
    }
    Sched.QueueEverRead = true;
    Buffer[Len] = 0;
    uint64_t Key;
    if (Buffer[0] == 'r') {
      Key = RunningKey;
    } else if (Buffer[0] == '-') {
      Key = OutsideKey;
    } else {
      char* P = Buffer;
      const uint64_t Nr = strtoull(P, &P, 10);
      uint64_t Args[2] {};
      for (auto& A : Args) {
        A = strtoull(P, &P, 16);
      }
      Key = Nr << 32;
      if (Nr == 29) { // ioctl: the request says which driver call
        Key |= Args[1] & 0xffffffff;
      } else if (Nr == 98) { // futex: the operation, without the private flag
        Key |= Args[1] & 0x7f;
      }
    }
    ++Sched.QueueSamples;
    for (auto& C : Sched.QueueCalls) {
      if (C.N && C.Key == Key) {
        ++C.N;
        return;
      }
    }
    for (auto& C : Sched.QueueCalls) {
      if (!C.N) {
        C = {Key, 1};
        return;
      }
    }
    ++Sched.QueueOther;
  }

  const char* SyscallName(uint64_t Nr) {
    switch (Nr) {
    case 22: return "epoll_pwait";
    case 29: return "ioctl";
    case 63: return "read";
    case 64: return "write";
    case 67: return "pread";
    case 73: return "ppoll";
    case 98: return "futex";
    case 101: return "nanosleep";
    case 115: return "clock_nanosleep";
    case 124: return "sched_yield";
    case 206: return "sendto";
    case 207: return "recvfrom";
    case 211: return "sendmsg";
    case 212: return "recvmsg";
    case 215: return "munmap";
    case 222: return "mmap";
    case 226: return "mprotect";
    case 441: return "epoll_pwait2";
    case 449: return "futex_waitv";
    default: return nullptr;
    }
  }

  fextl::string QueueCallName(uint64_t Key) {
    if (Key == RunningKey) {
      return "running";
    }
    if (Key == OutsideKey) {
      return "blocked";
    }
    const uint64_t Nr = Key >> 32;
    const uint32_t Arg = static_cast<uint32_t>(Key);
    const char* Name = SyscallName(Nr);
    fextl::string Out = Name ? fextl::string(Name) : fextl::fmt::format("nr{}", Nr);
    if (Nr == 29) {
      Out += fextl::fmt::format("({:#x})", Arg);
    } else if (Nr == 98 && Arg == 0) {
      Out += "(wait)";
    } else if (Nr == 98 && Arg == 1) {
      Out += "(wake)";
    } else if (Nr == 98 && Arg == 9) {
      Out += "(wait_bitset)";
    } else if (Nr == 98) {
      Out += fextl::fmt::format("({})", Arg);
    }
    return Out;
  }

  // From the 2ms sampler: every 10ms, the CPU the main thread and the wineserver are on, and what vkd3d's queue thread is
  // doing.
  void SchedSampleTick() {
    if (!Sched.On || ++Sched.Tick % 5) {
      return;
    }
    SchedSampleOne(Sched.MainFd, Sched.MainSamples, Sched.MainRunnable, Sched.MainCores);
    SchedSampleOne(Sched.ServerFd, Sched.ServerSamples, Sched.ServerRunnable, Sched.ServerCores);
    SchedSampleQueue();
    CorePauseSample();
  }

  fextl::string SchedCores(uint32_t Runnable, const uint32_t (&Cores)[Linux::MaxCPUs]) {
    fextl::string Out;
    for (int i = 0; i < Linux::MaxCPUs; ++i) {
      if (Cores[i]) {
        Out += fextl::fmt::format(" {}:{}%", i, (Cores[i] * 100 + Runnable / 2) / std::max<uint32_t>(Runnable, 1));
      }
    }
    return Out.empty() ? fextl::string(" -") : Out;
  }

  // Run and wait time of a task since the last call, in ms; false when its schedstat can't be read.
  bool SchedDelta(const fextl::string& Dir, SchedPrev& Prev, double& RunMs, double& WaitMs, uint64_t Second) {
    Linux::SchedStat S {};
    if (!Linux::ReadSchedStat((Dir + "/schedstat").c_str(), S)) {
      return false;
    }
    const bool Fresh = Prev.Seen == 0 || Prev.Seen + 1 != Second;
    RunMs = Fresh ? 0.0 : static_cast<double>(S.RunNs - Prev.RunNs) / 1e6;
    WaitMs = Fresh ? 0.0 : static_cast<double>(S.WaitNs - Prev.WaitNs) / 1e6;
    Prev.RunNs = S.RunNs;
    Prev.WaitNs = S.WaitNs;
    Prev.Seen = Second;
    return !Fresh;
  }

  // GPU view (on with the stats log; FEX_GPULOG=0 turns it off), from the Adreno driver's sysfs files, read with direct
  // Linux calls: the GPU clock every 50ms (average, lowest and highest each second), the share of the last driver window
  // the GPU was busy, the highest clock currently allowed and the power levels behind it (a launcher's power control or
  // the thermal limit lowers them), and the GPU temperature. Build 50 showed heavy scenes limited after the CPU at 37-42
  // ms a frame; busy near 100% means the GPU itself, lower means the frames wait in the present path.
  struct GpuState {
    bool On;
    bool Unreadable;
    int ClkFd = -1;
    uint32_t Tick;
    uint64_t SumMHz;
    uint32_t N, MinMHz, MaxMHz;
    int Levels = -1;
  };
  GpuState Gpu;
  constexpr const char* KgslDir = "/sys/class/kgsl/kgsl-3d0/";

  // A number from a small sysfs file (its first field), or -1.
  int64_t ReadKgslNumber(const char* Name, int64_t* Second = nullptr) {
    char Buffer[64];
    if (Linux::ReadSmallFile(fextl::fmt::format("{}{}", KgslDir, Name).c_str(), Buffer, sizeof(Buffer)) <= 0) {
      return -1;
    }
    char* P = Buffer;
    while (*P == ' ') {
      ++P;
    }
    if (*P < '0' || *P > '9') {
      return -1;
    }
    const int64_t First = static_cast<int64_t>(strtoull(P, &P, 10));
    if (Second) {
      while (*P == ' ' || *P == '\t') {
        ++P;
      }
      *Second = *P >= '0' && *P <= '9' ? static_cast<int64_t>(strtoull(P, nullptr, 10)) : -1;
    }
    return First;
  }

  void GpuInit() {
    const char* Value = Switch("FEX_GPULOG");
    Gpu.On = !(Value && Value[0] == '0');
    if (!Gpu.On) {
      return;
    }
    Gpu.ClkFd = Linux::Open(fextl::fmt::format("{}gpuclk", KgslDir).c_str());
    if (Gpu.ClkFd < 0) {
      Gpu.ClkFd = Linux::Open(fextl::fmt::format("{}devfreq/cur_freq", KgslDir).c_str());
    }
    if (Gpu.ClkFd < 0) {
      Gpu.Unreadable = true;
      StatsLogNote(fextl::fmt::format("gpu: {} can't be read (error {}), no gpu lines", KgslDir, Gpu.ClkFd));
      return;
    }
    Gpu.Levels = static_cast<int>(ReadKgslNumber("num_pwrlevels"));
    StatsLogNote(fextl::fmt::format("gpu: clock read every 50ms, {} power levels (0 = fastest), max {} MHz (FEX_GPULOG=0 turns it off)", Gpu.Levels,
                                    ReadKgslNumber("max_gpuclk") / 1000000));
  }

  // From the 2ms sampler.
  void GpuSampleTick() {
    if (Gpu.ClkFd < 0 || ++Gpu.Tick % 25) {
      return;
    }
    char Buffer[32];
    const int64_t Len = Linux::Pread(Gpu.ClkFd, Buffer, sizeof(Buffer) - 1, 0);
    if (Len <= 0) {
      return;
    }
    Buffer[Len] = 0;
    const uint64_t Hz = strtoull(Buffer, nullptr, 10);
    const uint32_t MHz = static_cast<uint32_t>(Hz > 100000 ? Hz / 1000000 : Hz); // Hz, or already MHz
    Gpu.SumMHz += MHz;
    Gpu.MinMHz = Gpu.N ? std::min(Gpu.MinMHz, MHz) : MHz;
    Gpu.MaxMHz = Gpu.N ? std::max(Gpu.MaxMHz, MHz) : MHz;
    ++Gpu.N;
  }

  fextl::string GpuLine() {
    if (Gpu.ClkFd < 0) {
      return {};
    }
    fextl::string Out = "  gpu";
    if (Gpu.N) {
      Out += fextl::fmt::format(" MHz={} (min {} max {})", Gpu.SumMHz / Gpu.N, Gpu.MinMHz, Gpu.MaxMHz);
    }
    Gpu.SumMHz = Gpu.N = 0;
    int64_t Total {};
    const int64_t Busy = ReadKgslNumber("gpubusy", &Total);
    if (Busy >= 0 && Total > 0) {
      Out += fextl::fmt::format(" busy={}%", std::min<int64_t>(Busy * 100 / Total, 100));
    } else if (const int64_t Percent = ReadKgslNumber("gpu_busy_percentage"); Percent >= 0) {
      Out += fextl::fmt::format(" busy={}%", Percent);
    }
    if (const int64_t Cap = ReadKgslNumber("max_gpuclk"); Cap > 0) {
      Out += fextl::fmt::format(" cap={}MHz", Cap / 1000000);
    }
    const int64_t MaxLevel = ReadKgslNumber("max_pwrlevel");
    const int64_t MinLevel = ReadKgslNumber("min_pwrlevel");
    if (MaxLevel >= 0 && MinLevel >= 0) {
      Out += fextl::fmt::format(" levels={}..{}", MaxLevel, MinLevel);
    }
    if (const int64_t Thermal = ReadKgslNumber("thermal_pwrlevel"); Thermal >= 0) {
      Out += fextl::fmt::format(" thermal={}", Thermal);
    }
    if (const int64_t Temp = ReadKgslNumber("temp"); Temp > 0) {
      Out += fextl::fmt::format(" temp={}C", Temp > 1000 ? Temp / 1000 : Temp);
    }
    return Out + "\n";
  }

  fextl::string SchedLines(uint64_t Seconds) {
    if (!Sched.On) {
      return {};
    }
    Sched.Second = Seconds;
    fextl::string Out;
    double GameRunMs {}, ServerRunMs {};

    // The main thread.
    {
      const auto Dir = fextl::fmt::format("/proc/self/task/{}", Sched.Pid);
      double RunMs {}, WaitMs {};
      const bool Have = SchedDelta(Dir, Sched.Main, RunMs, WaitMs, Seconds);
      Linux::TaskStat S {};
      const bool HaveStat = Linux::ReadTaskStat((Dir + "/stat").c_str(), S);
      Out += fextl::fmt::format("  sched main {} run={} wait={} nice={} allowed={:#x} on{} (runnable {}/{} samples)\n",
                                HaveStat ? S.Comm : "?", Have ? fextl::fmt::format("{:.0f}ms", RunMs) : fextl::string("-"),
                                Have ? fextl::fmt::format("{:.1f}ms", WaitMs) : fextl::string("-"), HaveStat ? S.Nice : 0,
                                Linux::GetAffinity(Sched.Pid), SchedCores(Sched.MainRunnable, Sched.MainCores), Sched.MainRunnable,
                                Sched.MainSamples);
    }

    // The wineserver.
    if (!Sched.ServerPid && Seconds % 10 == 1) {
      SchedOpenServer();
    }
    if (Sched.ServerPid) {
      const auto Dir = fextl::fmt::format("/proc/{}", Sched.ServerPid);
      double RunMs {}, WaitMs {};
      const bool Have = SchedDelta(Dir, Sched.Server, RunMs, WaitMs, Seconds);
      ServerRunMs = RunMs;
      Linux::TaskStat S {};
      if (!Linux::ReadTaskStat((Dir + "/stat").c_str(), S)) {
        Out += fextl::fmt::format("  sched server pid {} gone\n", Sched.ServerPid);
        SchedOpenServer();
      } else {
        Out += fextl::fmt::format("  sched server pid={} run={} wait={} nice={} allowed={:#x} on{} (runnable {}/{} samples)\n",
                                  Sched.ServerPid, Have ? fextl::fmt::format("{:.0f}ms", RunMs) : fextl::string("-"),
                                  Have ? fextl::fmt::format("{:.1f}ms", WaitMs) : fextl::string("-"), S.Nice,
                                  Linux::GetAffinity(Sched.ServerPid), SchedCores(Sched.ServerRunnable, Sched.ServerCores),
                                  Sched.ServerRunnable, Sched.ServerSamples);
      }
    } else if (Seconds == 1) {
      Out += "  sched server not found\n";
    }

    // This process's threads: the busiest by CPU time, with how long each waited for a CPU.
    {
      struct Row {
        int Tid;
        double RunMs;
        double WaitMs;
        int Processor;
        int Nice;
        char Comm[17];
      };
      struct Args {
        fextl::vector<Row>* Rows;
        uint64_t Second;
      };
      fextl::vector<Row> Rows;
      Args A {&Rows, Seconds};
      Linux::ForEachEntry(
        "/proc/self/task",
        [](const char* Name, void* Arg) {
          auto* A = static_cast<Args*>(Arg);
          const int Tid = atoi(Name);
          if (Tid <= 0) {
            return;
          }
          const auto Dir = fextl::fmt::format("/proc/self/task/{}", Tid);
          Row R {.Tid = Tid};
          auto& Prev = Sched.Tasks[Tid];
          if (!SchedDelta(Dir, Prev, R.RunMs, R.WaitMs, A->Second)) {
            return;
          }
          Linux::TaskStat S {};
          if (!Linux::ReadTaskStat((Dir + "/stat").c_str(), S)) {
            return;
          }
          memcpy(R.Comm, S.Comm, sizeof(R.Comm));
          R.Processor = S.Processor;
          R.Nice = S.Nice;
          A->Rows->push_back(R);
        },
        &A);
      for (auto It = Sched.Tasks.begin(); It != Sched.Tasks.end();) {
        It = It->second.Seen + 1 < Seconds ? Sched.Tasks.erase(It) : std::next(It);
      }
      double TotalRun {}, TotalWait {};
      for (const auto& R : Rows) {
        TotalRun += R.RunMs;
        TotalWait += R.WaitMs;
      }
      GameRunMs = TotalRun;
      std::sort(Rows.begin(), Rows.end(), [](const Row& a, const Row& b) { return a.RunMs + a.WaitMs > b.RunMs + b.WaitMs; });
      fextl::string Text;
      for (size_t i = 0; i < std::min<size_t>(Rows.size(), 10); ++i) {
        const auto& R = Rows[i];
        if (R.RunMs + R.WaitMs < 5.0) {
          break;
        }
        Text += fextl::fmt::format(" {}/{} {:.0f}+{:.0f}@{}{}", R.Comm, R.Tid, R.RunMs, R.WaitMs, R.Processor,
                                   R.Nice ? fextl::fmt::format("n{}", R.Nice) : fextl::string {});
      }
      if (!Rows.empty()) {
        Out += fextl::fmt::format("  sched threads run={:.0f}ms wait={:.0f}ms (name/tid run+wait ms @last cpu):{}\n", TotalRun, TotalWait, Text);
      }

      // What vkd3d's queue thread did this second, then follow the busiest one (a game can have several queues).
      if (Sched.QueueFd >= 0 && Sched.QueueSamples) {
        SchedState::CallCount Calls[std::size(Sched.QueueCalls)];
        memcpy(Calls, Sched.QueueCalls, sizeof(Calls));
        std::sort(std::begin(Calls), std::end(Calls), [](const auto& a, const auto& b) { return a.N > b.N; });
        fextl::string Text;
        for (const auto& C : Calls) {
          if (C.N) {
            Text += fextl::fmt::format(" {} {}%", QueueCallName(C.Key), (C.N * 100 + Sched.QueueSamples / 2) / Sched.QueueSamples);
          }
        }
        if (Sched.QueueOther) {
          Text += fextl::fmt::format(" other {}%", (Sched.QueueOther * 100 + Sched.QueueSamples / 2) / Sched.QueueSamples);
        }
        Out += fextl::fmt::format("  sched queue vkd3d_queue/{} ({} samples):{}\n", Sched.QueueTid, Sched.QueueSamples, Text);
      }
      memset(Sched.QueueCalls, 0, sizeof(Sched.QueueCalls));
      Sched.QueueSamples = Sched.QueueOther = 0;
      int Busiest {};
      double BusiestRun = -1.0;
      for (const auto& R : Rows) {
        if (strcmp(R.Comm, "vkd3d_queue") == 0 && R.RunMs > BusiestRun) {
          Busiest = R.Tid;
          BusiestRun = R.RunMs;
        }
      }
      if (Busiest && Busiest != Sched.QueueTid && !Sched.QueueUnreadable) {
        Linux::Close(Sched.QueueFd);
        Sched.QueueTid = Busiest;
        Sched.QueueFd = Linux::Open(fextl::fmt::format("/proc/self/task/{}/syscall", Busiest).c_str());
      }
    }

    // Every 5 seconds: the other processes of this user that used the CPU (Wine's other processes, the wineserver, the
    // launcher's own).
    if (Seconds % 5 == 0) {
      struct Row {
        int Pid;
        uint64_t Ticks;
      };
      struct Args {
        fextl::vector<Row>* Rows;
        uint64_t Second;
      };
      fextl::vector<Row> Rows;
      Args A {&Rows, Seconds};
      Linux::ForEachEntry(
        "/proc",
        [](const char* Name, void* Arg) {
          auto* A = static_cast<Args*>(Arg);
          if (Name[0] < '1' || Name[0] > '9') {
            return;
          }
          const int Pid = atoi(Name);
          if (Pid <= 0 || Pid == Sched.Pid) {
            return;
          }
          Linux::TaskStat S {};
          if (!Linux::ReadTaskStat(fextl::fmt::format("/proc/{}/stat", Pid).c_str(), S)) {
            return;
          }
          auto& Prev = Sched.Procs[Pid];
          const uint64_t Ticks = S.UserTicks + S.SystemTicks;
          const bool Fresh = Prev.Seen == 0 || Prev.Seen + 5 != A->Second;
          const uint64_t Delta = Fresh ? 0 : Ticks - Prev.Ticks;
          Prev.Ticks = Ticks;
          Prev.Seen = A->Second;
          Sched.ProcNames[Pid] = S.Comm;
          if (Delta) {
            A->Rows->push_back({Pid, Delta});
          }
        },
        &A);
      for (auto It = Sched.Procs.begin(); It != Sched.Procs.end();) {
        if (It->second.Seen != Seconds) {
          Sched.ProcNames.erase(It->first);
          It = Sched.Procs.erase(It);
        } else {
          It = std::next(It);
        }
      }
      std::sort(Rows.begin(), Rows.end(), [](const Row& a, const Row& b) { return a.Ticks > b.Ticks; });
      fextl::string Text;
      for (size_t i = 0; i < std::min<size_t>(Rows.size(), 8); ++i) {
        // Clock ticks are 10ms; over 5 seconds one tick is 0.2% of a core.
        Text += fextl::fmt::format(" {}/{} {}%", Sched.ProcNames[Rows[i].Pid], Rows[i].Pid, Rows[i].Ticks * 10 / 50);
      }
      Out += fextl::fmt::format("  sched procs 5s (name/pid % of a core):{}\n", Text.empty() ? " -" : Text.c_str());
    }

    Out += CoreLine(GameRunMs, ServerRunMs);
    Out += SystemLine(ServerRunMs);
    CgroupCheck();

    Sched.MainSamples = Sched.MainRunnable = Sched.ServerSamples = Sched.ServerRunnable = 0;
    memset(Sched.MainCores, 0, sizeof(Sched.MainCores));
    memset(Sched.ServerCores, 0, sizeof(Sched.ServerCores));
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
    bool ScanImage;       // FEX_STATSLOG_SCAN=1: the preload probe's per-second scan of the executable
    uint64_t WorkTicks;   // how long the log thread's work between two sampling seconds took last time
    uint64_t SecondStart; // cycle counter when the current second's sampling began
    fextl::unordered_map<uint64_t, ProfileCount> ProfileCounts;
    uint32_t ProfileSamples[2];
    fextl::vector<uint64_t> DumpedRIPs;

    // Main thread hitch recorder, see HitchMinBins. Samples of the current second in bin order, and the run in progress,
    // which can go on into the next second.
    struct MainSample {
      uint8_t Bin;
      uint32_t At; // counter ticks since SecondStart
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
      uint32_t At; // counter ticks since SecondStart
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

    // Frame times, see SlowFrameMs. The previous second's samples, for slow frames that began in it.
    MainSample PrevMainSamples[MaxMainSamples];
    size_t PrevMainSampleCount;
    OtherSample PrevOtherSamples[MaxOtherSamples];
    size_t PrevOtherSampleCount;
    uint64_t PrevSecondStart;
    uint64_t PrevSeconds;
    uint64_t FramesRead;                // frame marks processed
    uint64_t LastFrameStart;            // counter value of the last frame start, 0 before the first
    fextl::vector<float> SummaryFrames; // frame times (ms) since the last frames10s line
    size_t SlowFrameLines;

    // Names of native functions seen by the native-call sampler, and the export tables of the modules they are in.
    struct ModuleExports {
      fextl::string Name;
      fextl::vector<std::pair<uint32_t, uint32_t>> Functions; // RVA, name RVA; sorted by RVA
    };
    fextl::unordered_map<uint64_t, fextl::string> NativeNames;
    fextl::unordered_map<uint64_t, ModuleExports> Modules;

    // Stall probe events of 2ms or more from this and the previous second, for "# frame" lines.
    uint64_t ProbeRead;
    fextl::vector<ProbeEvent> ProbeEvents;
  };

  // Up to six, see StatsLogSetLineProvider.
  constexpr size_t MaxLineProviders = 6;
  std::atomic<fextl::string (*)()> LineProviders[MaxLineProviders] {};
  // Called with every sample of the main thread, see StatsLogSetMainSampleHook.
  std::atomic<void (*)(uint64_t)> MainSampleHook {};

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

  void* ProbeThread(void*) {
    auto* P = Probe;
    const uint64_t Freq = GetCounterFrequency();
    const uint64_t SleepTicks = Freq * static_cast<uint64_t>(ProbeSleep100ns) / 10'000'000;
    const uint64_t KeepTicks = static_cast<uint64_t>(ProbeKeepMs * static_cast<double>(Freq) / 1000.0);
    auto Record = [&](ProbeKind Kind, uint64_t End, uint64_t Ticks) {
      if (Ticks > P->Max[Kind].load(std::memory_order_relaxed)) {
        P->Max[Kind].store(Ticks, std::memory_order_relaxed);
        P->MaxAt[Kind].store(End, std::memory_order_relaxed);
      }
      if (Ticks >= KeepTicks) {
        const uint64_t W = P->Written.load(std::memory_order_relaxed);
        P->Ring[W % ProbeRingSize] = {End, Ticks, Kind};
        P->Written.store(W + 1, std::memory_order_release);
      }
    };

    void* Region {};
    SIZE_T RegionSize = ProbeFaultPages * 4096;
    if (NtAllocateVirtualMemory(NtCurrentProcess(), &Region, 0, &RegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) {
      Region = nullptr;
    }
    size_t Page {};
    for (uint64_t Round = 0;; ++Round) {
      // Sleep: how much later than asked a normal priority thread gets the CPU back.
      LARGE_INTEGER Interval {.QuadPart = -ProbeSleep100ns};
      uint64_t T0 = GetCounter();
      NtDelayExecution(FALSE, &Interval);
      uint64_t T1 = GetCounter();
      Record(ProbeSleep, T1, T1 - T0 > SleepTicks ? T1 - T0 - SleepTicks : 0);

      // VirtualQuery: Wine's process-wide memory lock (virtual_mutex), taken by every memory call and Wine fault.
      MEMORY_BASIC_INFORMATION Info {};
      T0 = GetCounter();
      NtQueryVirtualMemory(NtCurrentProcess(), Region ? Region : static_cast<void*>(&Info), MemoryBasicInformation, &Info, sizeof(Info), nullptr);
      T1 = GetCounter();
      Record(ProbeVQ, T1, T1 - T0);

      // First touch of a page: the kernel's per-process memory map lock, held for writing by every mprotect/mmap/munmap.
      if (Region) {
        if (Page == ProbeFaultPages) {
          void* Base = Region;
          SIZE_T Size = RegionSize;
          NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_DECOMMIT);
          Base = Region;
          Size = RegionSize;
          if (NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) {
            Region = nullptr;
          }
          Page = 0;
        }
        if (Region) {
          auto* Touch = static_cast<volatile uint8_t*>(Region) + Page++ * 4096;
          T0 = GetCounter();
          *Touch = 1;
          T1 = GetCounter();
          Record(ProbeFault, T1, T1 - T0);
        }
      }

      // FEX's global memory/SMC lock.
      if (auto* Lock = LockProbe.load(std::memory_order_acquire)) {
        T0 = GetCounter();
        Lock();
        T1 = GetCounter();
        Record(ProbeLock, T1, T1 - T0);
      }

      // A wineserver round trip (this thread's suspend count).
      if (Round % ProbeServerEvery == 0) {
        ULONG SuspendCount {};
        T0 = GetCounter();
        NtQueryInformationThread(NtCurrentThread(), static_cast<THREADINFOCLASS>(35) /* ThreadSuspendCount */, &SuspendCount,
                                 sizeof(SuspendCount), nullptr);
        T1 = GetCounter();
        Record(ProbeServer, T1, T1 - T0);
      }
      P->Rounds.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
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

  // The log thread's Wine calls go through the wineserver, which also serves the game's own event, wait and file calls,
  // so it keeps one handle per thread instead of opening and closing one every second, and caches thread names. Log
  // thread only. A held handle keeps its thread ID from being reused; handles of threads that have ended are closed
  // by CloseEndedThreadHandles.
  fextl::unordered_map<uint32_t, HANDLE> ThreadHandles;

  uint64_t QueryThreadCPUTime(uint32_t TID) {
    auto [It, Inserted] = ThreadHandles.try_emplace(TID, nullptr);
    if (Inserted) {
      It->second = OpenThreadForQuery(TID);
    }
    if (!It->second) {
      ThreadHandles.erase(It);
      return 0;
    }
    KERNEL_USER_TIMES Times {};
    uint64_t Result {};
    if (NtQueryInformationThread(It->second, ThreadTimes, &Times, sizeof(Times), nullptr) == STATUS_SUCCESS) {
      Result = Times.KernelTime.QuadPart + Times.UserTime.QuadPart;
    }
    return Result;
  }

  template<typename IsLive>
  void CloseEndedThreadHandles(IsLive&& Live) {
    for (auto It = ThreadHandles.begin(); It != ThreadHandles.end();) {
      if (Live(It->first)) {
        ++It;
      } else {
        NtClose(It->second);
        It = ThreadHandles.erase(It);
      }
    }
  }

  struct CachedThreadName {
    fextl::string Name;
    uint64_t Second;
  };
  fextl::unordered_map<uint32_t, CachedThreadName> ThreadNames;
  uint64_t LogSecond {}; // seconds logged so far, for the name cache

  fextl::string QueryThreadNameUncached(uint32_t TID) {
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

  // Names are asked again after 30 seconds, or after 5 while a thread has none (the game names some threads late).
  fextl::string QueryThreadName(uint32_t TID) {
    auto [It, Inserted] = ThreadNames.try_emplace(TID);
    auto& Cached = It->second;
    if (Inserted || LogSecond - Cached.Second >= (Cached.Name.empty() ? 5 : 30)) {
      Cached.Name = QueryThreadNameUncached(TID);
      Cached.Second = LogSecond;
    }
    return Cached.Name;
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

    // ARM64EC modules: the CHPE redirection table maps an export's x64 entry (Source) to the ARM64EC code behind it
    // (Destination). x86 code that calls an ntdll syscall stub ends up there through Wine's syscall dispatch, so give the
    // destination the export's name too.
    size_t Redirected = 0;
    const auto LoadConfigDir = Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    IMAGE_LOAD_CONFIG_DIRECTORY64 LoadConfig {};
    constexpr size_t CHPEFieldEnd = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + sizeof(LoadConfig.CHPEMetadataPointer);
    IMAGE_ARM64EC_METADATA Metadata {};
    if (LoadConfigDir.VirtualAddress && LoadConfigDir.Size >= CHPEFieldEnd &&
        ReadMemory(Base + LoadConfigDir.VirtualAddress, &LoadConfig, CHPEFieldEnd) && LoadConfig.Size >= CHPEFieldEnd &&
        LoadConfig.CHPEMetadataPointer && ReadMemory(LoadConfig.CHPEMetadataPointer, &Metadata, sizeof(Metadata)) &&
        Metadata.RedirectionMetadata && Metadata.RedirectionMetadataCount && Metadata.RedirectionMetadataCount <= 65536) {
      fextl::vector<IMAGE_ARM64EC_REDIRECTION_ENTRY> Entries(Metadata.RedirectionMetadataCount);
      if (ReadMemoryChunked(Base + Metadata.RedirectionMetadata, Entries.data(), Entries.size() * sizeof(Entries[0]))) {
        for (const auto& E : Entries) {
          auto It = std::lower_bound(M.Functions.begin(), M.Functions.begin() + Exported, std::pair<uint32_t, uint32_t> {E.Source, 0});
          if (It != M.Functions.begin() + Exported && It->first == E.Source && E.Destination && E.Destination < Nt.OptionalHeader.SizeOfImage) {
            M.Functions.push_back({E.Destination, It->second});
            ++Redirected;
          }
        }
      }
    }
    std::sort(M.Functions.begin(), M.Functions.end());
    StatsLogNote(fextl::fmt::format("native names: {} at {:X}: {} exports, {} patchable, {} redirected", M.Name, Base, Exported, Thunks, Redirected));
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

  // The blocks and native functions of a run or slow frame, and the other threads busy in emulated code meanwhile.
  fextl::string RunBody(LogState* State, const LogState::Run& R, const fextl::vector<std::pair<uint32_t, uint64_t>>& Blocks,
                        const fextl::vector<std::pair<uint32_t, uint64_t>>& Natives) {
    fextl::string Text;
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

  fextl::string FormatRun(LogState* State, const LogState::Run& R, const char* Kind) {
    fextl::vector<std::pair<uint32_t, uint64_t>> Blocks, Natives;
    SplitCounts(R.Counts, Blocks, Natives);
    auto Text = fextl::fmt::format("# {} t={}.{:02} {}ms full={}ms samples={} blocks={}:", Kind, R.StartSecond, R.StartBin, R.Bins * 10,
                                   R.FullBins * 10, R.Samples, Blocks.size());
    return Text + RunBody(State, R, Blocks, Natives);
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

  std::atomic<const uint64_t*> FrameMarks {};
  size_t FrameMarkRingSize {};

  // Where a counter value falls in the log's seconds, as "S.BB" (second, 10ms bin), for this or the previous second.
  fextl::string FrameTime(const LogState* State, uint64_t Seconds, uint64_t At) {
    const bool Current = At >= State->SecondStart;
    const uint64_t Start = Current ? State->SecondStart : State->PrevSecondStart;
    const uint64_t Bin = At > Start ? std::min<uint64_t>((At - Start) * TimelineBins / State->CounterFreq, TimelineBins - 1) : 0;
    return fextl::fmt::format("{}.{:02}", Current ? Seconds : State->PrevSeconds, Bin);
  }

  // The stall probe's events of 2ms or more that overlapped Begin..End, longest of each kind: " stalls[sleep=40ms server=38ms]",
  // " stalls[-]" when there were none, nothing when the probe is off.
  fextl::string ProbeDuring(const LogState* State, uint64_t Begin, uint64_t End) {
    if (!Probe) {
      return {};
    }
    uint64_t Max[ProbeKinds] {};
    for (const auto& E : State->ProbeEvents) {
      if (E.End > Begin && E.End - E.Ticks < End) {
        Max[E.Kind] = std::max(Max[E.Kind], E.Ticks);
      }
    }
    fextl::string Out;
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    for (size_t k = 0; k < ProbeKinds; ++k) {
      if (Max[k]) {
        Out += fextl::fmt::format("{}{}={:.0f}ms", Out.empty() ? "" : " ", ProbeNames[k], static_cast<double>(Max[k]) * MsPerTick);
      }
    }
    return fextl::fmt::format(" stalls[{}]", Out.empty() ? "-" : Out);
  }

  // Takes the probe's new events (dropping those that ended before the previous second) and returns the second's
  // "probe" line: rounds done and the longest of each kind with the 10ms bin it ended in.
  fextl::string ProbeLine(LogState* State, double MsPerTick) {
    if (!Probe) {
      return {};
    }
    const uint64_t Written = Probe->Written.load(std::memory_order_acquire);
    if (Written - State->ProbeRead > ProbeRingSize) {
      State->ProbeRead = Written - ProbeRingSize;
    }
    const uint64_t KeepFrom = State->PrevSecondStart ? State->PrevSecondStart : State->SecondStart;
    State->ProbeEvents.erase(std::remove_if(State->ProbeEvents.begin(), State->ProbeEvents.end(), [&](const ProbeEvent& E) { return E.End < KeepFrom; }),
                             State->ProbeEvents.end());
    for (; State->ProbeRead < Written; ++State->ProbeRead) {
      State->ProbeEvents.push_back(Probe->Ring[State->ProbeRead % ProbeRingSize]);
    }
    auto Line = fextl::fmt::format("  probe rounds={} max", Probe->Rounds.exchange(0, std::memory_order_relaxed));
    for (size_t k = 0; k < ProbeKinds; ++k) {
      const uint64_t Ticks = Probe->Max[k].exchange(0, std::memory_order_relaxed);
      const uint64_t At = Probe->MaxAt[k].exchange(0, std::memory_order_relaxed);
      const int64_t Bin = At > State->SecondStart ? static_cast<int64_t>((At - State->SecondStart) * TimelineBins / State->CounterFreq) : -1;
      Line += fextl::fmt::format(" {}={:.1f}ms@{}", ProbeNames[k], static_cast<double>(Ticks) * MsPerTick, Bin);
    }
    return Line + "\n";
  }

  // A "# frame" line: what Main was sampled doing from frame start Begin to the next frame start End, from this and the
  // previous second's samples (an older start is noted as such).
  fextl::string FormatSlowFrame(LogState* State, uint64_t Seconds, uint64_t Begin, uint64_t End, double Ms) {
    LogState::Run R {};
    auto Add = [&](const LogState::MainSample* Main, size_t MainCount, const LogState::OtherSample* Other, size_t OtherCount, uint64_t Start) {
      for (size_t i = 0; i < MainCount; ++i) {
        const uint64_t At = Start + Main[i].At;
        if (At >= Begin && At < End) {
          ++R.Counts[Main[i].Header];
          ++R.Samples;
        }
      }
      for (size_t i = 0; i < OtherCount; ++i) {
        const uint64_t At = Start + Other[i].At;
        if (At >= Begin && At < End) {
          auto& T = R.Others[Other[i].TID];
          ++T.Samples;
          T.InSim += Other[i].In ? 1 : 0;
          if (Other[i].Value) {
            ++T.Counts[Other[i].Value];
          }
        }
      }
    };
    if (State->PrevSecondStart) {
      Add(State->PrevMainSamples, State->PrevMainSampleCount, State->PrevOtherSamples, State->PrevOtherSampleCount, State->PrevSecondStart);
    }
    Add(State->MainSamples, State->MainSampleCount, State->OtherSamples, State->OtherSampleCount, State->SecondStart);

    fextl::vector<std::pair<uint32_t, uint64_t>> Blocks, Natives;
    SplitCounts(R.Counts, Blocks, Natives);
    uint32_t Emulated {};
    for (const auto& B : Blocks) {
      Emulated += B.first;
    }
    const bool Older = State->PrevSecondStart ? Begin < State->PrevSecondStart : Begin < State->SecondStart;
    auto Text =
      fextl::fmt::format("# frame t={} {:.0f}ms{} main samples={} emu={}%{}:", Older ? fextl::string("<") : FrameTime(State, Seconds, Begin),
                         Ms, Older ? " (began before the samples kept)" : "", R.Samples, R.Samples ? Emulated * 100 / R.Samples : 0,
                         ProbeDuring(State, Begin, End));
    return Text + RunBody(State, R, Blocks, Natives);
  }

  // Frame times from the frame starts written up to the end of this second's sampling window, see SlowFrameMs.
  fextl::string FrameLines(LogState* State, uint64_t Seconds) {
    const uint64_t* Marks = FrameMarks.load(std::memory_order_acquire);
    if (!Marks) {
      return {};
    }
    const uint64_t Count = __atomic_load_n(&Marks[0], __ATOMIC_ACQUIRE);
    const uint64_t WindowEnd = State->SecondStart + State->CounterFreq;
    if (Count - State->FramesRead > FrameMarkRingSize) {
      State->FramesRead = Count - FrameMarkRingSize;
    }
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    fextl::vector<float> Frames;
    fextl::string Slow;
    while (State->FramesRead < Count) {
      const uint64_t Start = __atomic_load_n(&Marks[1 + (State->FramesRead & (FrameMarkRingSize - 1))], __ATOMIC_RELAXED);
      if (Start > WindowEnd) {
        break; // after this second's samples, next second
      }
      ++State->FramesRead;
      if (State->LastFrameStart && Start > State->LastFrameStart) {
        const double Ms = static_cast<double>(Start - State->LastFrameStart) * MsPerTick;
        Frames.push_back(static_cast<float>(Ms));
        if (State->Profile && Ms >= SlowFrameMs && State->SlowFrameLines < MaxSlowFrameLines) {
          ++State->SlowFrameLines;
          Slow += FormatSlowFrame(State, Seconds, State->LastFrameStart, Start, Ms);
        }
      }
      State->LastFrameStart = Start;
    }

    // Keep this second's samples for frames that end in the next one.
    std::copy_n(State->MainSamples, State->MainSampleCount, State->PrevMainSamples);
    State->PrevMainSampleCount = State->MainSampleCount;
    std::copy_n(State->OtherSamples, State->OtherSampleCount, State->PrevOtherSamples);
    State->PrevOtherSampleCount = State->OtherSampleCount;
    State->PrevSecondStart = State->SecondStart;
    State->PrevSeconds = Seconds;

    auto Summary = [](fextl::vector<float>& F) {
      std::sort(F.begin(), F.end());
      double Sum {};
      size_t Over[4] {};
      for (const float Ms : F) {
        Sum += Ms;
        Over[0] += Ms >= 40.0f;
        Over[1] += Ms >= 50.0f;
        Over[2] += Ms >= 67.0f;
        Over[3] += Ms >= 100.0f;
      }
      const auto At = [&F](double Fraction) {
        return F[std::min(F.size() - 1, static_cast<size_t>(Fraction * static_cast<double>(F.size())))];
      };
      return fextl::fmt::format("n={} avg={:.1f} p50={:.1f} p90={:.1f} p99={:.1f} max={:.1f}ms >=40ms:{} >=50ms:{} >=67ms:{} >=100ms:{}",
                                F.size(), Sum / static_cast<double>(F.size()), At(0.5), At(0.9), At(0.99), F.back(), Over[0], Over[1],
                                Over[2], Over[3]);
    };
    fextl::string Out;
    if (!Frames.empty()) {
      State->SummaryFrames.insert(State->SummaryFrames.end(), Frames.begin(), Frames.end());
      Out += fextl::fmt::format("  frames {}\n", Summary(Frames));
    }
    if (Seconds % FrameSummarySeconds == 0 && !State->SummaryFrames.empty()) {
      Out += fextl::fmt::format("  frames{}s {}\n", FrameSummarySeconds, Summary(State->SummaryFrames));
      State->SummaryFrames.clear();
    }
    return Out + Slow;
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
            State->MainSamples[State->MainSampleCount++] = {static_cast<uint8_t>(Bin), static_cast<uint32_t>(Now - Start), Header};
          }
        }
        uint64_t NativeTarget {};
        if (!In && T.NativeTarget) {
          if (const uint64_t Target = *T.NativeTarget) {
            NativeTarget = Target;
            CountNative(T, Target);
            if (T.IsMain && State->MainSampleCount < MaxMainSamples) {
              State->MainSamples[State->MainSampleCount++] = {static_cast<uint8_t>(Bin), static_cast<uint32_t>(Now - Start),
                                                              Target | NativeSampleFlag};
            }
          }
        }
        if (T.IsMain) {
          if (const auto Hook = MainSampleHook.load(std::memory_order_relaxed)) {
            Hook(NativeTarget);
          }
        }
        if (State->Profile && !T.IsMain && State->OtherSampleCount < MaxOtherSamples) {
          const uint64_t Value = In ? (T.BlockHeader ? *T.BlockHeader : 0) : (NativeTarget ? NativeTarget | NativeSampleFlag : 0);
          State->OtherSamples[State->OtherSampleCount++] = {static_cast<uint8_t>(Bin), In, T.TID, static_cast<uint32_t>(Now - Start), Value};
        }
        if (T.BinSamples[Bin] < 255) {
          ++T.BinSamples[Bin];
          T.BinInSimulation[Bin] += In ? 1 : 0;
        }
      }
      if (State->Profile) {
        ProfileTick(State);
      }
      SchedSampleTick();
      GpuSampleTick();
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

  // Exclusive locks of FEX's code invalidation lock (see FEX_CODELOCK_POLITE in ARM64EC/Module.cpp): taken, how many
  // waited 1ms or more, polite waits that ran out, the longest wait and hold with the 10ms bin each ended in.
  FEXCore::Utils::WritePriorityMutex::ContentionStats CodeLockStats;

  fextl::string CodeLockLine(LogState* State, double MsPerTick) {
    auto Take = [](std::atomic<uint64_t>& Field) {
      return Field.exchange(0, std::memory_order_relaxed);
    };
    const uint64_t Locks = Take(CodeLockStats.Locks);
    const uint64_t Slow = Take(CodeLockStats.SlowLocks);
    const uint64_t GiveUps = Take(CodeLockStats.PoliteGiveUps);
    const uint64_t MaxWait = Take(CodeLockStats.MaxWait);
    const uint64_t MaxWaitAt = Take(CodeLockStats.MaxWaitAt);
    const uint64_t MaxHold = Take(CodeLockStats.MaxHold);
    const uint64_t MaxHoldAt = Take(CodeLockStats.MaxHoldAt);
    if (!Locks && !GiveUps) {
      return {};
    }
    auto Bin = [State](uint64_t At) {
      return At > State->SecondStart ? static_cast<int64_t>((At - State->SecondStart) * TimelineBins / State->CounterFreq) : -1;
    };
    return fextl::fmt::format("  codelock writes={} slow={} giveup={} wait={:.1f}ms@{} hold={:.1f}ms@{}\n", Locks, Slow, GiveUps,
                              MaxWait * MsPerTick, Bin(MaxWaitAt), MaxHold * MsPerTick, Bin(MaxHoldAt));
  }

  // Whether Wine's fast event paths (esync, fsync or ntsync) are on. Without them every event and wait call (SetEvent,
  // ResetEvent, WaitForSingleObject, ...) is a round trip to the wineserver, which serves every thread and process in
  // turn. Times setting and resetting an event: a few microseconds when the fast path works, tens or more otherwise.
  fextl::string WineSyncLine() {
    fextl::string Line = "# wine sync:";
    for (const char* Name : {"WINEESYNC", "WINEFSYNC", "WINE_DISABLE_FAST_SYNC", "PROTON_NO_ESYNC", "PROTON_NO_FSYNC"}) {
      const char* Value = getenv(Name);
      Line += fextl::fmt::format(" {}={}", Name, Value && Value[0] ? Value : "-");
    }
    HANDLE Event {};
    OBJECT_ATTRIBUTES Attr {.Length = sizeof(Attr)};
    if (NtCreateEvent(&Event, EVENT_ALL_ACCESS, &Attr, 0 /* NotificationEvent */, FALSE) != STATUS_SUCCESS) {
      return Line + "; event test failed\n";
    }
    constexpr int Rounds = 200;
    const uint64_t Freq = GetCounterFrequency();
    uint64_t Total {}, Longest {};
    for (int i = 0; i < Rounds; ++i) {
      const uint64_t Begin = GetCounter();
      NtSetEvent(Event, nullptr);
      NtResetEvent(Event, nullptr);
      const uint64_t Took = GetCounter() - Begin;
      Total += Took;
      Longest = std::max(Longest, Took);
    }
    NtClose(Event);
    const double AvgUs = static_cast<double>(Total) * 1'000'000.0 / static_cast<double>(Freq) / Rounds;
    return Line + fextl::fmt::format("; event set+reset avg={:.1f}us max={:.1f}us ({})\n", AvgUs,
                                     static_cast<double>(Longest) * 1'000'000.0 / static_cast<double>(Freq),
                                     AvgUs < 10.0 ? "fast path on" : "goes through the wineserver");
  }

  void* LogThread(void* Arg) {
    auto* State = static_cast<LogState*>(Arg);
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    uint64_t Seconds {};

    fextl::vector<Sample> Current;
    Current.reserve(MaxTracked);
    fextl::vector<const FEXCore::SHMStats::ThreadStats*> Slots;
    Slots.reserve(MaxTracked);
    SchedInit();
    GpuInit();

    for (;;) {
      SampleFor1s(State);
      ++Seconds;
      LogSecond = Seconds;
      const uint64_t WorkStart = GetCounter();

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
          .SuspendCount = Slot.AccumulatedSuspendCount,
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
        Cur.MaxCodeLockWaitTime = Take(Mutable->MaxCodeLockWaitTime);
        Cur.MaxCodeLockWaitAt = Take(Mutable->MaxCodeLockWaitAt);
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
              .SuspendCount = Diff(Cur.SuspendCount, Old->SuspendCount),
              .MaxJITTime = Cur.MaxJITTime,
              .MaxJITAt = Cur.MaxJITAt,
              .MaxSignalTime = Cur.MaxSignalTime,
              .MaxSignalAt = Cur.MaxSignalAt,
              .MaxDiskCacheLookupTime = Cur.MaxDiskCacheLookupTime,
              .MaxDiskCacheLookupAt = Cur.MaxDiskCacheLookupAt,
              .MaxCodeLockWaitTime = Cur.MaxCodeLockWaitTime,
              .MaxCodeLockWaitAt = Cur.MaxCodeLockWaitAt,
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
        Total.SuspendCount += Entry.D.SuspendCount;
        Deltas.push_back(Entry);
      }

      State->PrevCount = std::min(Current.size(), MaxTracked);
      std::copy_n(Current.begin(), State->PrevCount, State->Prev);

      std::sort(Deltas.begin(), Deltas.end(), [](const Delta& a, const Delta& b) { return a.CPUPercent > b.CPUPercent; });

      auto Line = fextl::fmt::format("t={} threads={} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} x87fb={} "
                                     "dc={}/{} dcl={:.1f}ms susp={}\n",
                                     Seconds, Current.size(), TotalCPU, Total.JITTime * MsPerTick, Total.JITCount, Total.SMCCount,
                                     Total.SIGBUSCount, Total.SignalTime * MsPerTick, Total.CacheMissCount, Total.FloatFallbackCount,
                                     Total.DiskCacheHitCount, Total.DiskCacheMissCount, Total.DiskCacheLookupTime * MsPerTick,
                                     Total.SuspendCount);

      size_t Printed {};
      for (const auto& Entry : Deltas) {
        const auto& D = Entry.D;
        const bool Interesting = Entry.CPUPercent >= 3.0 || D.JITCount || D.SMCCount || D.SIGBUSCount || D.DiskCacheHitCount || D.SuspendCount;
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
                                   "x87fb={} dc={}{}\n",
                                   D.TID, QueryThreadName(D.TID), Entry.CPUPercent, Emu, D.JITTime * MsPerTick, D.JITCount, D.SMCCount,
                                   D.SIGBUSCount, D.SignalTime * MsPerTick, D.CacheMissCount, D.CacheLockTime * MsPerTick,
                                   D.FloatFallbackCount, D.DiskCacheHitCount,
                                   D.SuspendCount ? fextl::fmt::format(" susp={}", D.SuspendCount) : fextl::string {});
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
        // A compile waiting for the code invalidation lock (included in jit).
        AddMax("codelock", D.MaxCodeLockWaitTime, D.MaxCodeLockWaitAt);
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

      Line += SchedLines(Seconds);
      Line += GpuLine();
      Line += ProbeLine(State, MsPerTick);
      Line += FrameLines(State, Seconds);
      if (State->Profile) {
        Line += MainHitches(State, Seconds);
      }

      // Preload probe, see ImageWatch. boot = hits before the first scan (game start). Off unless FEX_STATSLOG_SCAN=1:
      // the scan asks Wine about every region of the executable each second, which competes with the game's own memory calls.
      if (State->ScanImage) {
        uint64_t TotalPages {};
        const uint64_t ReadablePages = ScanImage(static_cast<uint32_t>(Seconds), TotalPages);
        Line += fextl::fmt::format("  pre exe early={} late={} other={} boot={} readable={}/{}\n", HitsEarly.exchange(0),
                                   HitsLate.exchange(0), HitsOther.exchange(0), HitsBoot.exchange(0), ReadablePages, TotalPages);
      }

      // CPU clocks in MHz per core 0-7 (0-2 little, 3-6 middle, 7 the X3 on a Snapdragon 8 Gen 2), every 10 seconds.
      if (Seconds % 10 == 1) {
        if (const auto Clocks = ReadCoreClocks(); !Clocks.empty()) {
          Line += fextl::fmt::format("  clk={}\n", Clocks);
        } else if (ClocksUnreadable && Seconds == 1) {
          Line += "  clk=unreadable\n";
        }
      }

      Line += CodeLockLine(State, MsPerTick);
      // How long the log thread's own work between the last two sampling seconds took (nothing is sampled meanwhile).
      Line += fextl::fmt::format("  logger work={:.1f}ms\n", State->WorkTicks * MsPerTick);

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

      for (const auto& Slot : LineProviders) {
        if (const auto Provider = Slot.load(std::memory_order_acquire)) {
          Line += Provider();
        }
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

      CloseEndedThreadHandles(
        [&](uint32_t TID) { return std::any_of(Current.begin(), Current.end(), [TID](const Sample& S) { return S.TID == TID; }); });
      State->WorkTicks = GetCounter() - WorkStart;
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

  auto Header =
    fextl::fmt::format("# FEX stats for {} (pid {}), one block per second. cpu = thread CPU time; jit = time/blocks compiled; "
                       "smc = writes to translated code; sigbus = unaligned atomic fixups; sig = time in exception "
                       "handling; miss = lookup cache misses; lock = lookup cache lock time; x87fb = x87 fallbacks; dc = disk "
                       "cache hits; emu = share of the second spent in emulated x86 code (the rest of cpu is native: "
                       "Wine, VKD3D, driver); dco = why compiles missed the disk cache and what happened to the code "
                       "compiled instead; io = page faults that read storage, and storage reads; with FEX_STATSLOG_PROFILE=1, "
                       "'# hitch' / '# stall' = Main ran emulated code / no emulated code for 100ms or more, with the other "
                       "threads that were busy in emulated code meanwhile; jobwait = the game's frame wait loop: tries that "
                       "found "
                       "nothing to run (idle) or found jobs, time paused, longest uninterrupted idle wait; frames = time from "
                       "one game frame "
                       "start to the next (count, average, median, 90th/99th percentile, longest, how many took 40/50/67/100ms "
                       "or more), frames10s = the same over 10 seconds; '# frame' = a frame of 50ms or more with what Main and "
                       "the busy threads were doing in it; codelock = FEX's code invalidation lock: exclusive locks taken, how "
                       "many waited 1ms "
                       "or more, polite waits that ran out, longest wait and hold (max codelock = a compile's longest wait "
                       "for it); logger work = the log thread's own work between two sampled seconds; susp = times threads stopped "
                       "for a suspend request in emulated code (time suspended shows as fex:suspended); probe = the stall "
                       "probe's longest waits this second, stalls[...] on a '# frame' line = its waits of 2ms or more during "
                       "that frame; '# prio' = thread priority changes (FEX_PRIO); '# cores' = thread core changes (FEX_MAINCORE, "
                       "FEX_HELPER_CORES); '# server' = wineserver nice (FEX_SERVER_NICE); sched main/server = Linux view of "
                       "the main thread and the wineserver: CPU time (run), time ready but without a CPU (wait), nice, allowed "
                       "CPUs, and which CPU it was on while runnable (sampled every 10ms); sched threads = the busiest threads "
                       "(Linux name/tid run+wait ms @last CPU); sched procs 5s = other processes of this user by CPU use; waits main = "
                       "handles the "
                       "main thread waited on this second (kind, /latency = a swapchain frame latency handle, +N = released N more "
                       "times, calls, ms sampled waiting @caller, timeout ms); '# latency' = what FEX_FRAMELATENCY found and did; '# smc' "
                       "= a page whose own code kept writing to it, "
                       "so it stays writable and its code checks itself (FEX_SMC_HOTPAGE); sched queue = what "
                       "vkd3d's queue thread (it hands the game's GPU work and each present to the driver) was doing, sampled every "
                       "10ms: running, or the Linux call it waited in (ioctl(request) = a GPU driver call, ppoll/futex = waiting "
                       "for another thread, a fence or the display server); gpu = GPU clock (average, lowest and highest of 50ms "
                       "samples), busy = the GPU's busy share in the driver's last window, cap = highest clock allowed now, levels = "
                       "the power levels allowed (0 = fastest; a launcher's power control or heat raises the first), temperature; "
                       "cores busy% = how busy each CPU was, whoever ran there (? = can't tell), the game's CPUs' busy time split "
                       "into this process, the wineserver and others (other apps, Android, the launcher, the kernel), paused = "
                       "share of the second the kernel had a CPU out of use, runnable = runnable tasks of the whole system, cpu "
                       "pressure = share of the second some task waited for a CPU; system busy = the whole device's busy CPU "
                       "time in cores by kind (user, low prio = user at lowered priority, kernel, irq), this process, the "
                       "wineserver, app group = this app's Android group (the launcher's own process and Wine's processes), "
                       "app user = all processes of the app's user; users busy = the busy time of the kernel's own threads and "
                       "interrupts (in no Android user's group), then the busiest Android users (system services and apps; app "
                       "N = an app's user id) with their kernel share, processes, and processes started (+) and ended (-) "
                       "since the last second; memory = free memory, file cache, swap, the kernel's reclaim (swap "
                       "out/in, pages scanned to free memory by kswapd or by allocating tasks, stalls = allocations that had to "
                       "reclaim, refaults = evicted file pages read again), pressure = share of the second tasks waited for "
                       "memory or storage, this process's resident memory (own = its private memory, files = mapped files, shared "
                       "= shared memory), swapped and GPU memory; '# cgroup' = the Android groups the main "
                       "thread and the wineserver are in; rumble = the game's XInputSetState calls (FEX_RUMBLE): calls, changes "
                       "of strength, changes sent to the controller and how long Wine took for each\n",
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
  const char* Scan = Switch("FEX_STATSLOG_SCAN");
  State->ScanImage = Scan && Scan[0] == '1';
  const auto Switches = fextl::fmt::format("# switches:{}\n", SwitchSummary());
  fwrite(Switches.data(), 1, Switches.size(), File);
  const auto WineSync = WineSyncLine();
  fwrite(WineSync.data(), 1, WineSync.size(), File);
  const char* StallProbe = Switch("FEX_STALLPROBE");
  static fextl::unique_ptr<FEXCore::Threads::Thread> ProbeThreadObject;
  if (State->Profile && !(StallProbe && StallProbe[0] == '0')) {
    Probe = new ProbeShared {};
    ProbeThreadObject = FEXCore::Threads::Thread::Create(ProbeThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXStallProbe");
    const char* ProbeNote = "# stall probe: on, every 4ms: sleep (late wakeup), vq (Wine memory lock), fault (kernel memory map lock), "
                            "fexlock (FEX memory/SMC lock), every 16ms server (wineserver round trip); FEX_STALLPROBE=0 turns it off\n";
    fwrite(ProbeNote, 1, strlen(ProbeNote), File);
  }
  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(LogThread, State, {.LowPriority = false, .Internal = true}, "FEXStatsLog");
}

FEXCore::Utils::WritePriorityMutex::ContentionStats* StatsLogCodeLockStats() {
  return &CodeLockStats;
}

void StatsLogSetLockProbe(void (*ProbeFunction)()) {
  LockProbe.store(ProbeFunction, std::memory_order_release);
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
  for (auto& Slot : LineProviders) {
    fextl::string (*Empty)() = nullptr;
    if (Slot.compare_exchange_strong(Empty, Provider, std::memory_order_release)) {
      return;
    }
  }
}

void StatsLogSetMainSampleHook(void (*Hook)(uint64_t NativeTarget)) {
  MainSampleHook.store(Hook, std::memory_order_release);
}

void StatsLogSetFrameMarks(const uint64_t* Marks, size_t RingSize) {
  FrameMarkRingSize = RingSize;
  FrameMarks.store(Marks, std::memory_order_release);
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
