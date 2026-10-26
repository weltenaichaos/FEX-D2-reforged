// SPDX-License-Identifier: MIT
#include "Windows/Common/PriorityGuard.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

// Exported by ntdll (Defs/ntdll.def) but not declared by the bundled headers.
extern "C" NTSTATUS WINAPI NtGetNextThread(HANDLE Process, HANDLE Thread, ACCESS_MASK Access, ULONG Attributes, ULONG Flags, HANDLE* Next);

namespace FEX::Windows {
namespace {
  const THREADINFOCLASS ThreadAffinityMaskClass = static_cast<THREADINFOCLASS>(4);
  const THREADINFOCLASS ThreadNameInformationClass = static_cast<THREADINFOCLASS>(38);
  const PROCESSINFOCLASS ProcessPriorityClassInfo = static_cast<PROCESSINFOCLASS>(18);

  struct PriorityClassInfo {
    BOOLEAN Foreground;
    UCHAR PriorityClass;
  };
  // PROCESS_PRIOCLASS_* as Wine and Windows number them.
  constexpr UCHAR ClassNormal = 2;
  // The base priority Wine gives each class (server/process.c set_process_priority).
  LONG ClassBase(UCHAR Class) {
    switch (Class) {
    case 1: return 4;
    case 3: return 13;
    case 4: return 24;
    case 5: return 6;
    case 6: return 10;
    default: return 8;
    }
  }
  const char* ClassName(UCHAR Class) {
    switch (Class) {
    case 1: return "idle";
    case 2: return "normal";
    case 3: return "high";
    case 4: return "realtime";
    case 5: return "below_normal";
    case 6: return "above_normal";
    default: return "unknown";
    }
  }

  // Same layout as Windows' THREAD_BASIC_INFORMATION; spelled out because the bundled headers differ in field names.
  struct ThreadBasicInfo {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    LONG Priority;     // effective, 1..31 (Wine adds 1 for the process's first thread)
    LONG BasePriority; // relative to the class: -2..2, or -15 (IDLE) / 15 (TIME_CRITICAL)
  };

  // Same layout as Windows' PROCESS_BASIC_INFORMATION (the bundled headers hide the affinity in a reserved field).
  struct ProcessBasicInfo {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
  };

  // The CPUs Wine lets this process's threads use (GameNative narrows it after start), 0 if unknown.
  uint64_t ProcessAffinity() {
    ProcessBasicInfo Info {};
    return NtQueryInformationProcess(NtCurrentProcess(), ProcessBasicInformation, &Info, sizeof(Info), nullptr) == STATUS_SUCCESS ?
             Info.AffinityMask :
             0;
  }

  enum class PrioMode { Keep, Clamp, Normal };
  enum class CoreMode { Off, Big, Fast };
  enum class LoaderMode { Off, Find, Little };
  enum class OthersMode { Off, Process, User };

  struct Config {
    PrioMode Prio;
    CoreMode Cores;
    bool HelpersLittle;
    bool ServerNiceOn;
    int ServerNice;
    bool MainNiceOn;
    int MainNice;
    uint64_t FastMask;  // the CPU(s) with the highest maximum clock, 0 when all are alike or unknown
    uint64_t LittleMask; // the CPU(s) with the lowest maximum clock, 0 when all are alike or unknown
    uint64_t AllMask;    // every CPU with a known clock, 0 when Windows numbers them differently
    uint32_t MainTID;    // the thread that started the process (FEX's process init runs on it)
    int MainLinuxTid;    // its Linux tid
    uint32_t EaseWaitMs; // FEX_MAINCORE_WAIT, 0 = never let the main thread off its core
    LoaderMode Loader;   // FEX_LOADER_CORES
    uint64_t ExeBase;    // the main exe's image range as loaded, before any protection remaps it
    uint64_t ExeEnd;
    uint32_t GameExcPerRound; // FEX_LOADER_GAME_EXCEPTIONS, 0 = off
    OthersMode Others;        // FEX_MAINCORE_OTHERS
  };

  // Loader threads, see FEX_LOADER_CORES in PriorityGuard.h.
  constexpr uint32_t LoaderSamples = 8;              // per 2 s round, one every 250 ms
  constexpr uint32_t LoaderMinProtection = 6;        // samples in protection code, over a thread's busy rounds; and 3 times its game samples
  constexpr uint32_t LoaderMaxRounds = 4;            // busy rounds counted before a thread that didn't qualify isn't sampled for a while
  constexpr uint64_t LoaderBusyNs = 500'000'000;     // run time in a 2 s round: 25% of a core
  constexpr uint32_t LoaderSkipRounds = 30;          // 1 minute
  // Build 81: D2R's loader thread spends most of its samples in Wine calls, and over 4 busy rounds gave 3 to 7 samples in
  // protection code in every log from build 73 to 80, never one in the game's code; the game's workers gave 0-1 in
  // protection code and 6-25 in the game's. With 6 needed it was a coin toss: in build 80 (4 at 0:17) it stayed on the
  // game's big cores until 1:29, in build 76 (3, then 5) until 2:51.
  constexpr uint32_t LoaderMinProtectionNoGame = 3;  // samples in protection code over all LoaderMaxRounds busy rounds, with no game sample
  constexpr uint32_t LoaderSoonRounds = 5;           // 10 s: a thread with protection samples and none of the game's is checked again soon
  constexpr size_t MaxModuleKinds = 64;              // cached image kinds, see ModuleKinds
  constexpr size_t MaxLoaderThreads = 4;
  constexpr size_t MaxGameThreads = 4; // game threads found by their exceptions, on the "loader" line
  constexpr uint32_t MaxGameNotes = 16;
  constexpr uint32_t MaxCheckNotes = 24;             // "# loader: checked ..." lines, for busy threads found not to be loader threads

  // The emulated threads, see GuardThreadStarted.
  struct Registered {
    uint32_t TID;
    int LinuxTid;
    uint64_t Teb;
    const volatile uint64_t* BlockHeader;
    const volatile uint64_t* GameExceptions;
  };
  constexpr size_t MaxRegistered = 4096;
  std::mutex RegistryLock;
  Registered Registry[MaxRegistered];
  size_t RegistryCount {};
  std::atomic<void (*)(uint32_t, uint64_t)> LoaderHook {};

  // The per-second "loader" line: written by the guard once a second, read by the stats log thread.
  struct LoaderLineEntry {
    uint32_t TID;
    int LinuxTid;
    uint64_t Mask;
    int Processor;
    double RunMs;
    double WaitMs;
    bool Game;          // a game thread found by its exceptions in the game's code, not a loader thread
    double GameExcPerS; // those exceptions a second
  };
  constexpr size_t MaxLineEntries = MaxLoaderThreads + MaxGameThreads;
  std::mutex LoaderLineLock;
  LoaderLineEntry LoaderLine[MaxLineEntries];
  size_t LoaderLineCount {};

  fextl::string LoaderStatsLine() {
    std::scoped_lock Lock {LoaderLineLock};
    if (!LoaderLineCount) {
      return {};
    }
    fextl::string Out = "  loader";
    for (size_t i = 0; i < LoaderLineCount; ++i) {
      const auto& E = LoaderLine[i];
      Out += fextl::fmt::format("{}{} tid={} (Linux {}) cores {:#x}, last on CPU {}: ran {:.0f} ms/s, waited {:.0f} ms/s for a CPU",
                                i ? ";" : "", E.Game ? " game thread" : "", E.TID, E.LinuxTid, E.Mask, E.Processor, E.RunMs, E.WaitMs);
      if (E.Game) {
        Out += fextl::fmt::format(", {:.0f} exceptions/s in the game's code", E.GameExcPerS);
      }
    }
    Out += "\n";
    return Out;
  }

  // Mirrors of FEXCore's CPUBackend::JITCodeHeader / JITCodeTail (Interface/Core/CPUBackend.h), as in StatsLog.cpp.
  struct JITCodeTailMirror {
    size_t Size;
    uint64_t RIP;
    size_t GuestSize;
  };

  // Where a sampled JIT block's guest code is (build 63; build 62 counted everything that wasn't an image view as outside
  // the game, and D2R's protection remaps D2R.exe into a non-image view, so it moved three of the game's worker threads):
  //  - Game: in the main exe's range as loaded, or in an image of x64 code (another DLL of the game's).
  //  - Native: in an ARM64EC image (Wine's DLLs, VKD3D, DXVK). A thread that sits in a Wine call (a wait, CreateEventW)
  //    was last in the x64 entry stub of that DLL's function, so these samples say nothing about the thread's own code.
  //  - Protection: in committed memory that is neither: D2R_loader.dll's remapped code and the code it decrypts.
  //  - Unread: no block yet, or the block or its memory is gone.
  enum class CodeKind : uint8_t { Unread, Native, Game, Protection };
  struct CodeClass {
    uint64_t Header;
    CodeKind Kind;
    uint64_t RIP;
    uint64_t Base; // allocation base
  };

  // Image kinds by allocation base, so each image's header is read once.
  struct ModuleKinds {
    uint64_t Base[MaxModuleKinds];
    bool Native[MaxModuleKinds];
    size_t Count;
  };

  // Whether the image at Base is ARM64EC (its load config has CHPE metadata), read through /proc/self/mem.
  bool IsArm64ECImage(uint64_t Base) {
    IMAGE_DOS_HEADER Dos {};
    if (!Linux::ReadOwnMemory(Base, &Dos, sizeof(Dos)) || Dos.e_magic != IMAGE_DOS_SIGNATURE || Dos.e_lfanew <= 0 || Dos.e_lfanew > 0x10000) {
      return false;
    }
    IMAGE_NT_HEADERS64 Nt {};
    if (!Linux::ReadOwnMemory(Base + Dos.e_lfanew, &Nt, sizeof(Nt)) || Nt.Signature != IMAGE_NT_SIGNATURE ||
        Nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return false;
    }
    const auto& Dir = Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr size_t CHPEFieldEnd =
      offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64::CHPEMetadataPointer);
    if (!Dir.VirtualAddress || Dir.Size < CHPEFieldEnd) {
      return false;
    }
    IMAGE_LOAD_CONFIG_DIRECTORY64 LoadConfig {};
    if (!Linux::ReadOwnMemory(Base + Dir.VirtualAddress, &LoadConfig, CHPEFieldEnd)) {
      return false;
    }
    return LoadConfig.Size >= CHPEFieldEnd && LoadConfig.CHPEMetadataPointer;
  }

  CodeClass ClassifyBlock(const Config& C, ModuleKinds& Kinds, uint64_t Header) {
    CodeClass R {.Header = Header, .Kind = CodeKind::Unread};
    uint32_t OffsetToTail {};
    JITCodeTailMirror Tail {};
    // The block may have been freed since it was sampled: read through /proc/self/mem, which fails safely.
    if (!Header || !Linux::ReadOwnMemory(Header, &OffsetToTail, sizeof(OffsetToTail)) || !OffsetToTail ||
        !Linux::ReadOwnMemory(Header + OffsetToTail, &Tail, sizeof(Tail)) || !Tail.RIP) {
      return R;
    }
    R.RIP = Tail.RIP;
    if (Tail.RIP >= C.ExeBase && Tail.RIP < C.ExeEnd) {
      R.Kind = CodeKind::Game;
      R.Base = C.ExeBase;
      return R;
    }
    MEMORY_BASIC_INFORMATION Info {};
    if (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Tail.RIP), MemoryBasicInformation, &Info, sizeof(Info), nullptr) !=
          STATUS_SUCCESS ||
        Info.State != MEM_COMMIT) {
      return R;
    }
    R.Base = reinterpret_cast<uintptr_t>(Info.AllocationBase);
    if (R.Base == C.ExeBase) {
      // The exe's remapped view, should its size be unknown.
      R.Kind = CodeKind::Game;
      return R;
    }
    if (Info.Type != MEM_IMAGE) {
      R.Kind = CodeKind::Protection;
      return R;
    }
    for (size_t i = 0; i < Kinds.Count; ++i) {
      if (Kinds.Base[i] == R.Base) {
        R.Kind = Kinds.Native[i] ? CodeKind::Native : CodeKind::Game;
        return R;
      }
    }
    const bool Native = IsArm64ECImage(R.Base);
    if (Kinds.Count == MaxModuleKinds) {
      Kinds.Count = 0;
    }
    Kinds.Base[Kinds.Count] = R.Base;
    Kinds.Native[Kinds.Count++] = Native;
    R.Kind = Native ? CodeKind::Native : CodeKind::Game;
    return R;
  }

  struct Tracked {
    fextl::string Name;
    bool Named;
    bool FEX;
    bool KeepCores; // a FEX thread that stays where it is with FEX_HELPER_CORES=little
    LONG FirstBase;
    LONG FirstPriority;
    uint64_t FirstAffinity;
    uint64_t Orig;  // the last affinity seen that this guard didn't set
    uint64_t SetTo; // the last affinity this guard set
    uint32_t Fixes;
    uint32_t CoreFixes;
    uint32_t SeenAt;
    bool Main;
    // Loader thread detection, see FEX_LOADER_CORES.
    int LinuxTid; // 0 = not an emulated thread, or not registered yet
    uint64_t Teb;
    uint64_t Samples[LoaderSamples]; // JIT block headers sampled this round
    uint32_t SampleCount;
    uint64_t LastRunNs; // its Linux run time at the last round, 0 = not read yet
    // Code samples of its busy rounds so far, by CodeKind, and an example of protection and of game code.
    uint32_t InProtection, InGame, InNative, Unread, BusyRounds;
    CodeClass ProtectionExample, GameExample;
    uint32_t SkipUntil;
    uint32_t Checks;
    bool Loader;
    // Exceptions it raised in the main exe's code (see FEX_LOADER_GAME_EXCEPTIONS): the count read every 250 ms, the count
    // at the last round, and whether it is a game thread for good.
    uint64_t GameExc;
    uint64_t GameExcAtRound;
    bool GameThread;
  };

  // nice value Wine on Linux gives an effective priority (server/thread.c apply_thread_priority, nice_limit -19).
  int WineNice(LONG Priority) {
    Priority = std::clamp<LONG>(Priority, 1, 15);
    return 19 + (Priority - 1) * -38 / 14;
  }

  fextl::string QueryName(HANDLE Thread) {
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 128 * sizeof(WCHAR)] {};
    fextl::string Result;
    ULONG Length {};
    if (NtQueryInformationThread(Thread, ThreadNameInformationClass, Buffer, sizeof(Buffer), &Length) == STATUS_SUCCESS) {
      const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
      for (USHORT i = 0; Name->Buffer && i < Name->Length / sizeof(WCHAR); ++i) {
        const WCHAR c = Name->Buffer[i];
        Result.push_back(c >= 0x20 && c < 0x7f && c != ' ' ? static_cast<char>(c) : '_');
      }
    }
    return Result;
  }

  // Puts the process back in the normal priority class. Returns the class base priority in effect afterwards.
  LONG CheckClass(bool First) {
    PriorityClassInfo Info {};
    const NTSTATUS Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessPriorityClassInfo, &Info, sizeof(Info), nullptr);
    if (Status != STATUS_SUCCESS) {
      if (First) {
        StatsLogNote(fextl::fmt::format("prio: can't read the priority class, status={:#x}", static_cast<uint32_t>(Status)));
      }
      return 8;
    }
    if (Info.PriorityClass == ClassNormal) {
      if (First) {
        StatsLogNote("prio: priority class normal");
      }
      return 8;
    }
    PriorityClassInfo New {.Foreground = Info.Foreground, .PriorityClass = ClassNormal};
    const NTSTATUS SetStatus = NtSetInformationProcess(NtCurrentProcess(), ProcessPriorityClassInfo, &New, sizeof(New));
    StatsLogNote(fextl::fmt::format("prio: priority class {} -> normal, status={:#x}", ClassName(Info.PriorityClass),
                                    static_cast<uint32_t>(SetStatus)));
    return SetStatus == STATUS_SUCCESS ? 8 : ClassBase(Info.PriorityClass);
  }

  // Detects the fastest and slowest cores from their maximum clocks.
  void DetectCores(Config& C) {
    uint32_t KHz[Linux::MaxCPUs];
    Linux::ReadMaxClocks(KHz);
    uint32_t Max {}, Min {~0u};
    for (uint32_t K : KHz) {
      if (K) {
        Max = std::max(Max, K);
        Min = std::min(Min, K);
      }
    }
    fextl::string Text;
    for (int i = 0; i < Linux::MaxCPUs; ++i) {
      if (KHz[i]) {
        C.AllMask |= 1ull << i;
        Text += fextl::fmt::format(" {}:{}", i, KHz[i] / 1000);
        if (KHz[i] == Max && Max != Min) {
          C.FastMask |= 1ull << i;
        }
        if (KHz[i] == Min && Max != Min) {
          C.LittleMask |= 1ull << i;
        }
      }
    }
    // Windows CPU numbers match Linux ones unless WINE_CPU_TOPOLOGY renumbers them, which shows as a different count.
    SYSTEM_BASIC_INFORMATION Info {};
    int LinuxCount {};
    for (uint32_t K : KHz) {
      LinuxCount += K ? 1 : 0;
    }
    const bool SameNumbers = NtQuerySystemInformation(SystemBasicInformation, &Info, sizeof(Info), nullptr) == STATUS_SUCCESS &&
                             Info.NumberOfProcessors == LinuxCount;
    if (!SameNumbers) {
      C.FastMask = C.LittleMask = C.AllMask = 0;
    }
    StatsLogNote(fextl::fmt::format("cores: max MHz{}; Windows sees {} CPUs; fastest {:#x}, slowest {:#x}{}",
                                    Text.empty() ? " unreadable" : Text.c_str(), static_cast<int>(Info.NumberOfProcessors), C.FastMask,
                                    C.LittleMask, SameNumbers ? "" : " (CPU numbers differ from Linux, core changes off)"));
  }

  // The affinity a thread should have, or its current one. T.Orig is what the game or Wine last gave it.
  uint64_t WantedAffinity(const Config& C, const Tracked& T, bool IsMain, uint64_t Current, uint64_t GameMask, bool FastUsable,
                          uint64_t ProcessMask) {
    const bool MainAlone = C.Cores == CoreMode::Fast && FastUsable;
    if (T.FEX) {
      // The slowest cores only when Wine allows them (with GameNative's "cores 3-7" it doesn't; build 49 then retried and
      // failed every 2 seconds while the stats log thread kept running next to the main thread).
      if (C.HelpersLittle && C.LittleMask && !T.KeepCores && (!ProcessMask || (C.LittleMask & ProcessMask) == C.LittleMask)) {
        return C.LittleMask;
      }
      // Off the main thread's core like the game's own threads: in build 49 FEX's threads still ran there.
      if (MainAlone && (T.Orig & ~C.FastMask)) {
        return T.Orig & ~C.FastMask;
      }
      return Current;
    }
    if (C.Cores == CoreMode::Off || !GameMask) {
      return Current;
    }
    if (IsMain) {
      return C.Cores == CoreMode::Fast && FastUsable ? C.FastMask : GameMask;
    }
    if (C.Cores == CoreMode::Fast && FastUsable && (T.Orig & ~C.FastMask)) {
      return T.Orig & ~C.FastMask;
    }
    return T.Orig;
  }

  // Keeps every thread of the wineserver off the main thread's core. In build 49 the wineserver (at nice -10, above the main
  // thread's -2) still ran on that core 10-20% of the time while loading, and the main thread then waited 150-270 ms a
  // second for its only core.
  void KeepServerOff(int Pid, uint64_t Mask, uint32_t& Reports) {
    struct Args {
      uint64_t Mask;
      uint32_t* Reports;
      int Pid;
    } A {.Mask = Mask, .Reports = &Reports, .Pid = Pid};
    const auto Path = fextl::fmt::format("/proc/{}/task", Pid);
    Linux::ForEachEntry(
      Path.c_str(),
      [](const char* Name, void* Arg) {
        auto* A = static_cast<Args*>(Arg);
        const int Tid = atoi(Name);
        if (Tid <= 0) {
          return;
        }
        const uint64_t Before = Linux::GetAffinity(Tid);
        if (Before == A->Mask) {
          return;
        }
        const int Result = Linux::SetAffinity(Tid, A->Mask);
        if (++*A->Reports <= 3 || *A->Reports % 50 == 0) {
          StatsLogNote(fextl::fmt::format("server: wineserver pid {} thread {} cores {:#x} -> {:#x} (off the main thread's core), result {}, "
                                          "change #{}",
                                          A->Pid, Tid, Before, A->Mask, Result, *A->Reports));
        }
      },
      &A);
  }

  // Keeps every other thread of this Linux user off the main thread's core (FEX_MAINCORE_OTHERS, see PriorityGuard.h):
  // threads of this process that Wine doesn't list (the guard's round above only reaches the threads Wine knows) and, with
  // "user", every thread of the user's other processes (Wine's other programs such as steam.exe, the launcher's own
  // process). The wineserver has KeepServerOff. A thread loses only the main thread's core; one that could run nowhere else
  // gets the game's other cores.
  constexpr uint32_t MaxOthersNotes = 80;

  struct OthersKeeper {
    struct Proc {
      fextl::string Name;
      uint32_t SeenAt;
      uint32_t Moved;  // threads moved off the core, all rounds
      uint32_t Rounds; // rounds in which some had to be moved
      bool FailNoted;
    };
    // Processes by pid; threads hidden from /proc (see OthersHidden) by the negated number of their cgroup (pid_N).
    fextl::unordered_map<int, Proc> Procs;
    int SelfPid;
    uint32_t Uid;
    uint32_t Notes;
    uint32_t MovedMinute; // threads moved since the last summary
    uint32_t TotalMoved;
    // Build 77: the user's cgroup v2 directory ("/sys/fs/cgroup/uid_<user>"), empty when unknown, and a buffer for its
    // groups' thread lists.
    fextl::string UidCgroup;
    fextl::vector<char> Buffer;
    bool HiddenOff;    // the groups can't be listed
    bool HiddenNoted;  // a group's thread list couldn't be read (noted once)
  };

  struct OthersSweep {
    OthersKeeper* K;
    int MainTid;
    uint64_t Fast;
    uint64_t Fallback;
    uint64_t All; // every CPU, 0 if unknown
    int ServerPid;
    uint32_t Round;
  };

  struct OthersTasks {
    const OthersSweep* S;
    int Pid;
    bool Self;
    uint32_t Threads;
    uint32_t Moved;
    uint32_t Failed;
    int Error;
    uint64_t Before;
    uint64_t After;
  };

  void OthersTask(const char* Name, void* Arg) {
    auto* T = static_cast<OthersTasks*>(Arg);
    const int Tid = atoi(Name);
    if (Tid <= 0 || (T->Self && Tid == T->S->MainTid)) {
      return;
    }
    ++T->Threads;
    const uint64_t Usable = Linux::GetAffinity(Tid);
    if (!(Usable & T->S->Fast)) {
      return;
    }
    // GetAffinity leaves out CPUs the kernel has paused at the moment; the status file has the mask as set, so a thread
    // keeps those.
    char Path[96];
    snprintf(Path, sizeof(Path), "/proc/%d/task/%d/status", T->Pid, Tid);
    uint64_t Mask = Linux::ReadAllowedMask(Path);
    if (!(Mask & T->S->Fast)) {
      Mask = Usable;
    }
    uint64_t Want = Mask & ~T->S->Fast;
    if (!Want) {
      Want = T->S->Fallback;
    }
    if (!Want || Want == Mask) {
      return;
    }
    const int Result = Linux::SetAffinity(Tid, Want);
    if (Result == 0) {
      if (!T->Moved) {
        T->Before = Mask;
        T->After = Want;
      }
      ++T->Moved;
    } else {
      ++T->Failed;
      T->Error = Result;
    }
  }

  // "pid N", or for threads hidden from /proc (Key < 0) "group pid_N".
  fextl::string OthersKeyText(int Key) {
    return Key < 0 ? fextl::fmt::format("group pid_{}", -Key) : fextl::fmt::format("pid {}", Key);
  }

  // Counts and notes what one sweep of a process (or of a group's hidden threads) moved.
  void OthersReport(const OthersSweep& S, int Key, OthersKeeper::Proc& P, const OthersTasks& T) {
    auto& K = *S.K;
    if (T.Moved) {
      ++P.Rounds;
      P.Moved += T.Moved;
      K.MovedMinute += T.Moved;
      K.TotalMoved += T.Moved;
      // The first two rounds that moved any of a process's threads, then every 50th: a program that keeps putting its
      // threads back shows as a count.
      if ((P.Rounds <= 2 || P.Rounds % 50 == 0) && K.Notes < MaxOthersNotes) {
        ++K.Notes;
        StatsLogNote(fextl::fmt::format("cores: {} ({}): {} of {} threads off the main thread's core, {:#x} -> {:#x}{} "
                                        "(FEX_MAINCORE_OTHERS)",
                                        P.Name, OthersKeyText(Key), T.Moved, T.Threads, T.Before, T.After,
                                        P.Rounds > 1 ? fextl::fmt::format(", again (#{}, {} threads in all)", P.Rounds, P.Moved) : fextl::string {}));
      }
    }
    if (T.Failed && !P.FailNoted && K.Notes < MaxOthersNotes) {
      P.FailNoted = true;
      ++K.Notes;
      StatsLogNote(fextl::fmt::format("cores: {} ({}): {} threads can't be moved off the main thread's core, errno {}", P.Name,
                                      OthersKeyText(Key), T.Failed, -T.Error));
    }
  }

  void OthersProcess(const OthersSweep& S, int Pid, bool Self) {
    char Path[64];
    snprintf(Path, sizeof(Path), "/proc/%d/task", Pid);
    OthersTasks T {.S = &S, .Pid = Pid, .Self = Self};
    if (!Linux::ForEachEntry(Path, OthersTask, &T)) {
      return;
    }
    auto& K = *S.K;
    auto [It, New] = K.Procs.try_emplace(Pid);
    auto& P = It->second;
    P.SeenAt = S.Round;
    if (New) {
      if (Self) {
        P.Name = "this process";
      } else {
        char Comm[64];
        snprintf(Path, sizeof(Path), "/proc/%d/comm", Pid);
        const int64_t Len = Linux::ReadSmallFile(Path, Comm, sizeof(Comm));
        P.Name = Len > 0 ? fextl::string(Comm, static_cast<size_t>(Comm[Len - 1] == '\n' ? Len - 1 : Len)) : fextl::string("?");
      }
    }
    OthersReport(S, Pid, P, T);
  }

  // Reads a whole cgroup file (its lines come in pieces) into K.Buffer, NUL-terminated. Its length or a negative errno.
  int64_t ReadGroupFile(OthersKeeper& K, const char* Path) {
    const int Fd = Linux::Open(Path);
    if (Fd < 0) {
      K.Buffer[0] = 0;
      return Fd;
    }
    size_t Len = 0;
    while (Len + 1 < K.Buffer.size()) {
      const int64_t N = Linux::Pread(Fd, K.Buffer.data() + Len, K.Buffer.size() - 1 - Len, Len);
      if (N <= 0) {
        if (N < 0 && !Len) {
          Linux::Close(Fd);
          K.Buffer[0] = 0;
          return N;
        }
        break;
      }
      Len += static_cast<size_t>(N);
    }
    Linux::Close(Fd);
    K.Buffer[Len] = 0;
    return static_cast<int64_t>(Len);
  }

  // A thread or process /proc doesn't show this user (Android's /proc hides processes that aren't dumpable from everyone
  // without the right to trace them, their own user included), or shows as someone else's.
  bool HiddenFromProc(const OthersKeeper& K, long Id) {
    char Path[32];
    snprintf(Path, sizeof(Path), "/proc/%ld", Id);
    Linux::FileInfo Info {};
    return !Linux::Stat(Path, Info) || Info.Uid != K.Uid;
  }

  void CollectGroups(const char* Name, void* Arg) {
    if (strncmp(Name, "pid_", 4) != 0 || Name[4] < '0' || Name[4] > '9') {
      return;
    }
    static_cast<fextl::vector<int>*>(Arg)->push_back(atoi(Name + 4));
  }

  // What a group's hidden threads belong to, for the notes: its processes /proc doesn't show.
  fextl::string HiddenName(OthersKeeper& K, int Group) {
    char Path[128];
    snprintf(Path, sizeof(Path), "%s/pid_%d/cgroup.procs", K.UidCgroup.c_str(), Group);
    fextl::string Pids;
    uint32_t Count {};
    if (ReadGroupFile(K, Path) > 0) {
      for (char* P = K.Buffer.data(); *P;) {
        char* End {};
        const long Pid = strtol(P, &End, 10);
        if (End == P) {
          break;
        }
        P = End;
        while (*P == '\n' || *P == ' ') {
          ++P;
        }
        if (Pid > 0 && HiddenFromProc(K, Pid) && Count++ < 4) {
          Pids += fextl::fmt::format("{}{}", Pids.empty() ? "" : ", ", Pid);
        }
      }
    }
    return Count == 1 ? fextl::fmt::format("a process hidden from /proc (pid {}; in the launcher's group: its own app process)", Pids) :
           Count     ? fextl::fmt::format("{} processes hidden from /proc (pids {}{})", Count, Pids, Count > 4 ? ", ..." : "") :
                       fextl::string("threads hidden from /proc");
  }

  // Build 77: threads of this user that /proc doesn't show, above all the launcher's own app process (GameNative's Java
  // process: it renders the game's frames to the screen and reads the controller). Android groups every process of an
  // app's user in cgroup v2 (/sys/fs/cgroup/uid_<user>/pid_<process>, Wine's processes in the launcher's group), and a
  // group's cgroup.threads lists every thread in it. Build 76 moved every thread /proc shows off the main thread's core,
  // yet ~17% of that core stayed busy with other threads while the main thread waited ~40 ms a second for it, and the
  // user's CPU use that no /proc process accounts for was ~0.14 of a core. Such a thread's own mask can't be read either,
  // so it gets every core but the main thread's.
  void OthersHidden(const OthersSweep& S) {
    auto& K = *S.K;
    if (K.UidCgroup.empty() || K.HiddenOff) {
      return;
    }
    fextl::vector<int> Groups;
    if (!Linux::ForEachEntry(K.UidCgroup.c_str(), CollectGroups, &Groups)) {
      K.HiddenOff = true;
      if (K.Notes < MaxOthersNotes) {
        ++K.Notes;
        StatsLogNote(fextl::fmt::format("cores: {} can't be listed, threads hidden from /proc (the launcher's own process) stay where "
                                        "they are",
                                        K.UidCgroup));
      }
      return;
    }
    if (K.Buffer.empty()) {
      K.Buffer.resize(128 * 1024);
    }
    for (const int G : Groups) {
      char Path[128];
      snprintf(Path, sizeof(Path), "%s/pid_%d/cgroup.threads", K.UidCgroup.c_str(), G);
      const int64_t Len = ReadGroupFile(K, Path);
      if (Len < 0) {
        if (!K.HiddenNoted && K.Notes < MaxOthersNotes) {
          K.HiddenNoted = true;
          ++K.Notes;
          StatsLogNote(fextl::fmt::format("cores: {} can't be read (errno {}): threads hidden from /proc there stay where they are",
                                          Path, -Len));
        }
        continue;
      }
      OthersTasks T {.S = &S};
      for (char* P = K.Buffer.data(); *P;) {
        char* End {};
        const long Tid = strtol(P, &End, 10);
        if (End == P) {
          break;
        }
        P = End;
        while (*P == '\n' || *P == ' ') {
          ++P;
        }
        // Threads /proc shows were handled above (the wineserver's by KeepServerOff).
        if (Tid <= 0 || Tid == S.MainTid || !HiddenFromProc(K, Tid)) {
          continue;
        }
        ++T.Threads;
        const uint64_t Usable = Linux::GetAffinity(static_cast<int>(Tid));
        if (!(Usable & S.Fast)) {
          continue;
        }
        const uint64_t Want = (S.All ? S.All : Usable) & ~S.Fast;
        if (!Want) {
          continue;
        }
        const int Result = Linux::SetAffinity(static_cast<int>(Tid), Want);
        if (Result == 0) {
          if (!T.Moved) {
            T.Before = Usable;
            T.After = Want;
          }
          ++T.Moved;
        } else {
          ++T.Failed;
          T.Error = Result;
        }
      }
      if (!T.Threads) {
        continue;
      }
      auto [It, New] = K.Procs.try_emplace(-G);
      auto& Proc = It->second;
      Proc.SeenAt = S.Round;
      if (New) {
        Proc.Name = HiddenName(K, G);
      }
      OthersReport(S, -G, Proc, T);
    }
  }

  void OthersEntry(const char* Name, void* Arg) {
    const auto* S = static_cast<const OthersSweep*>(Arg);
    if (Name[0] < '1' || Name[0] > '9') {
      return;
    }
    const int Pid = atoi(Name);
    if (Pid <= 0 || Pid == S->K->SelfPid || Pid == S->ServerPid) {
      return;
    }
    char Path[64];
    snprintf(Path, sizeof(Path), "/proc/%d", Pid);
    Linux::FileInfo Info {};
    if (!Linux::Stat(Path, Info) || Info.Uid != S->K->Uid) {
      return;
    }
    OthersProcess(*S, Pid, false);
  }

  void KeepOthersOff(OthersKeeper& K, const Config& C, uint64_t Fallback, int ServerPid, uint32_t Round) {
    OthersSweep S {
      .K = &K, .MainTid = C.MainLinuxTid, .Fast = C.FastMask, .Fallback = Fallback, .All = C.AllMask, .ServerPid = ServerPid, .Round = Round};
    OthersProcess(S, K.SelfPid, true);
    if (C.Others == OthersMode::User) {
      Linux::ForEachEntry("/proc", OthersEntry, &S);
      OthersHidden(S);
    }
    for (auto It = K.Procs.begin(); It != K.Procs.end();) {
      It = It->second.SeenAt == Round ? std::next(It) : K.Procs.erase(It);
    }
    // Once a minute, when anything had to be moved: which programs keep starting threads there.
    if (Round % 30 == 0 && K.MovedMinute) {
      fextl::string Text;
      for (const auto& [Key, P] : K.Procs) {
        if (P.Moved) {
          Text += Key < 0 ? fextl::fmt::format(" [{}]/pid_{} {}", P.Name, -Key, P.Moved) : fextl::fmt::format(" {}/{} {}", P.Name, Key, P.Moved);
        }
      }
      StatsLogNote(fextl::fmt::format("cores: s={} other threads moved off the main thread's core: {} this minute, {} in all; by process "
                                      "(name/pid threads in all):{}",
                                      Round * 2, K.MovedMinute, K.TotalMoved, Text.empty() ? " -" : Text));
      K.MovedMinute = 0;
    }
  }

  // Lifts every thread of the wineserver to nice C.ServerNice. Returns the pid (0 if not found).
  int BoostServer(const Config& C, int Pid, bool Report) {
    if (!Pid) {
      Pid = Linux::FindProcess("wineserver");
      if (!Pid) {
        if (Report) {
          StatsLogNote("server: no wineserver process found, nice unchanged");
        }
        return 0;
      }
    }
    struct Args {
      const Config* C;
      int Pid;
      bool Report;
      int Threads;
      int Changed;
      int Failed;
      int Before;
      int After;
      int Error;
    } A {.C = &C, .Pid = Pid, .Report = Report, .Before = -1000, .After = -1000};
    const auto Path = fextl::fmt::format("/proc/{}/task", Pid);
    const bool Listed = Linux::ForEachEntry(
      Path.c_str(),
      [](const char* Name, void* Arg) {
        auto* A = static_cast<Args*>(Arg);
        const int Tid = atoi(Name);
        if (Tid <= 0) {
          return;
        }
        ++A->Threads;
        const int Nice = Linux::GetNice(Tid);
        if (A->Before == -1000) {
          A->Before = Nice;
        }
        if (Nice > A->C->ServerNice) {
          const int Result = Linux::SetNice(Tid, A->C->ServerNice);
          if (Result == 0) {
            ++A->Changed;
          } else {
            ++A->Failed;
            A->Error = Result;
          }
        }
        if (A->After == -1000) {
          A->After = Linux::GetNice(Tid);
        }
      },
      &A);
    if (!Listed) {
      if (Report) {
        StatsLogNote(fextl::fmt::format("server: wineserver pid {} gone", Pid));
      }
      return 0;
    }
    if (Report || A.Changed || A.Failed) {
      StatsLogNote(fextl::fmt::format("server: wineserver pid {} threads={} nice {} -> {} (changed {}, failed {}{})", Pid, A.Threads,
                                      A.Before, A.After, A.Changed, A.Failed,
                                      A.Failed ? fextl::fmt::format(", errno {}", -A.Error) : fextl::string {}));
    }
    return Pid;
  }

  // Keeps the main thread on its core at the Linux level. Wine only knows the affinity it set itself: in the build 50 log
  // the main thread's Linux cores changed to all 8 at 34 s (and to 0-6 for a second now and then) while Wine still
  // reported the X3, so the 2-second round above never noticed, and the main thread ran 60-80% of the time on the slower
  // cores. Only the main thread changed. It is the process's first Linux thread, the one a `taskset -p <pid>` or a
  // per-process cpuset move reaches (Android, or the launcher's power control). Checked every 250 ms with
  // sched_getaffinity, which costs no wineserver round trip.
  //
  // At login and while loading, the main thread alone on the X3 waited up to 650 ms a second for it (build 50; 150-270 ms
  // in build 49): something outside the game runs there then. When it waits FEX_MAINCORE_WAIT ms (default 40) or more in
  // a second, it may also use the game's other big cores until it has waited under 10 ms a second for a while (5 s, longer
  // when it had to leave again soon after coming back).
  // Keeps the main thread at Linux nice FEX_MAIN_NICE (see PriorityGuard.h). Wine sets a thread's nice from its Windows
  // priority only when that priority changes (the round above does that once at start, from D2R's TIME_CRITICAL), so
  // this mostly sets it once; it is checked every 250 ms all the same, like the core.
  struct MainNiceKeeper {
    int Tid; // 0: off
    int Nice;
    uint32_t Sets;
    uint32_t Failures;
    uint32_t FailuresInRow;
  };

  void KeepMainNice(MainNiceKeeper& K) {
    if (!K.Tid) {
      return;
    }
    const int Now = Linux::GetNice(K.Tid);
    if (Now == -1000 || Now == K.Nice) {
      if (Now == -1000) {
        StatsLogNote(fextl::fmt::format("prio: main thread (Linux {}) gone, its nice no longer kept", K.Tid));
        K.Tid = 0;
      }
      return;
    }
    const int Result = Linux::SetNice(K.Tid, K.Nice);
    const int After = Linux::GetNice(K.Tid);
    if (Result == 0) {
      ++K.Sets;
      K.FailuresInRow = 0;
    } else {
      ++K.Failures;
      ++K.FailuresInRow;
    }
    const uint32_t Times = K.Sets + K.Failures;
    if (Times <= 4 || (Times & (Times - 1)) == 0) {
      StatsLogNote(fextl::fmt::format("prio: main thread (Linux {}) nice {} -> {} ({}; set {} times, failed {} times; FEX_MAIN_NICE=off leaves it)",
                                      K.Tid, Now, After, Result == 0 ? fextl::string("set") : fextl::fmt::format("errno {}", -Result),
                                      K.Sets, K.Failures));
    }
    if (K.FailuresInRow >= 8) {
      StatsLogNote(fextl::fmt::format("prio: main thread's nice can't be set (errno {}), no longer tried", -Result));
      K.Tid = 0;
    }
  }

  struct MainKeeper {
    int Tid;
    uint32_t EaseMs;
    uint32_t Tick; // 250 ms ticks
    bool Widened;
    uint32_t CalmSeconds;
    uint32_t CalmNeeded = 5;
    uint32_t RepinnedAt; // tick of the last return to the fast core, 0 = none yet
    bool HaveWait;
    uint64_t LastWaitNs;
    uint64_t Applied;  // the mask this keeper or the round last put the thread on
    uint64_t LastSeen; // the last mask found that neither of them set
    uint32_t Resets;
    uint32_t Widens;
    fextl::string StatusPath; // /proc/self/task/<tid>/status
    uint64_t PausedSeen;      // CPUs of its mask the kernel had paused at the last check
    uint32_t Pauses;
  };

  void KeepMainCore(MainKeeper& K, uint64_t FastMask, uint64_t WideMask) {
    ++K.Tick;
    if (!K.Tid || !FastMask) {
      K.HaveWait = false;
      return;
    }

    // Once a second: how long the main thread was ready to run but had no CPU.
    if (K.Tick % 4 == 0 && K.EaseMs && (WideMask & ~FastMask)) {
      Linux::SchedStat S {};
      if (Linux::ReadSchedStat(fextl::fmt::format("/proc/self/task/{}/schedstat", K.Tid).c_str(), S)) {
        if (K.HaveWait) {
          const double WaitMs = static_cast<double>(S.WaitNs - K.LastWaitNs) / 1e6;
          if (!K.Widened && WaitMs >= K.EaseMs) {
            K.Widened = true;
            K.CalmSeconds = 0;
            ++K.Widens;
            if (K.RepinnedAt && K.Tick - K.RepinnedAt <= 40) {
              K.CalmNeeded = std::min<uint32_t>(K.CalmNeeded * 2, 60);
            }
            if (K.Widens <= 5 || K.Widens % 20 == 0) {
              StatsLogNote(fextl::fmt::format("cores: main thread waited {:.0f} ms in the last second for its core (something outside the game "
                                              "runs there, as at login and while loading); it may use {:#x} until it waits under 10 ms a "
                                              "second for {} s, #{} (FEX_MAINCORE_WAIT=off keeps it on its core)",
                                              WaitMs, WideMask, K.CalmNeeded, K.Widens));
            }
          } else if (K.Widened) {
            K.CalmSeconds = WaitMs < 10.0 ? K.CalmSeconds + 1 : 0;
            if (K.CalmSeconds >= K.CalmNeeded) {
              K.Widened = false;
              K.RepinnedAt = K.Tick;
              if (K.Widens <= 5 || K.Widens % 20 == 0) {
                StatsLogNote(fextl::fmt::format("cores: main thread back alone on {:#x} after {} s waiting under 10 ms a second", FastMask,
                                                K.CalmSeconds));
              }
            }
          } else if (K.RepinnedAt && K.Tick - K.RepinnedAt > 240) {
            K.CalmNeeded = 5;
          }
        }
        K.LastWaitNs = S.WaitNs;
        K.HaveWait = true;
      }
    }

    const uint64_t Want = K.Widened ? WideMask : FastMask;
    const uint64_t Now = Linux::GetAffinity(K.Tid);
    if (Now == Want) {
      K.Applied = Want;
      K.PausedSeen = 0;
      return;
    }
    // sched_getaffinity leaves out CPUs the kernel has paused (core control or heat take a CPU out of use without
    // switching it off). When the thread's own mask is still what was set, that is all it is: nothing to set back. The
    // "changed to 0xb8" and "0x78" of the build 52 log (cores 6 and 7 missing for a moment) look like this.
    const uint64_t Own = Linux::ReadAllowedMask(K.StatusPath.c_str());
    if (Own == Want) {
      K.Applied = Want;
      const uint64_t Paused = Want & ~Now;
      if (Paused != K.PausedSeen) {
        K.PausedSeen = Paused;
        if (Paused && (++K.Pauses <= 5 || K.Pauses % 50 == 0)) {
          StatsLogNote(fextl::fmt::format("cores: CPUs {:#x} of the main thread's {:#x} are paused by the kernel for now (core control or heat), "
                                          "its own mask is unchanged, #{}",
                                          Paused, Want, K.Pauses));
        }
      }
      return;
    }
    if (!Now && !Own) {
      return;
    }
    // Its own mask changed: Android or the launcher outside Wine, or the kernel moving it elsewhere because every CPU it was
    // allowed was paused (the build 50 log's "changed to all 8").
    const uint64_t Seen = Own ? Own : Now;
    const bool Outside = Seen != K.Applied && Seen != K.LastSeen;
    if (Seen != K.Applied) {
      K.LastSeen = Seen;
    }
    const int Result = Linux::SetAffinity(K.Tid, Want);
    if (Result == 0) {
      K.Applied = Want;
    }
    if (Outside && (++K.Resets <= 3 || K.Resets % 50 == 0)) {
      StatsLogNote(fextl::fmt::format("cores: main thread's Linux cores were changed to {:#x} (usable now {:#x}) outside Wine (Android, the "
                                      "launcher, or the kernel when all of its cores were paused); set back to {:#x}, result {}, change #{}",
                                      Seen, Now, Want, Result, K.Resets));
    }
  }

  uint64_t Counter() {
    uint64_t Value {};
    __asm volatile("isb; mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Value));
    return Value;
  }

  uint64_t CounterFrequency() {
    uint64_t Value {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Value));
    return Value ? Value : 1;
  }

  // Finds loader threads and keeps them on the slowest cores, see FEX_LOADER_CORES in PriorityGuard.h.
  struct LoaderThread {
    uint32_t TID;
    int LinuxTid;
    fextl::string StatusPath; // /proc/self/task/<tid>/status
    fextl::string SchedPath;  // .../schedstat
    fextl::string StatPath;   // .../stat
    uint32_t Resets;
    uint32_t Failures;
    uint32_t RetryAt; // tick before which a failed core change isn't tried again
    bool HaveSched;
    uint64_t LastRunNs;
    uint64_t LastWaitNs;
    uint64_t Before;      // its Linux cores before it was moved
    uint64_t LastGameExc; // game threads: the exception count at the last "loader" line
  };

  struct LoaderFinder {
    const Config* C;
    ModuleKinds Kinds;
    fextl::vector<LoaderThread> Threads;
    fextl::vector<LoaderThread> GameThreads; // found by their exceptions in the game's code, see FEX_LOADER_GAME_EXCEPTIONS
    uint32_t Tick;
    uint32_t CheckNotes;
    uint32_t GameNotes;
    uint64_t LastSecond; // counter value at the last per-second line
  };

  // Every 250 ms: one code sample of each emulated thread that could still be a loader thread, and the loader threads'
  // Linux cores; once a second the "loader" line's figures.
  void LoaderTick(LoaderFinder& F, fextl::unordered_map<uint32_t, Tracked>& Threads) {
    ++F.Tick;
    {
      // Held while reading BlockHeader: ThreadTerm unregisters a thread (taking this lock) before freeing its state.
      std::scoped_lock Lock {RegistryLock};
      for (size_t i = 0; i < RegistryCount; ++i) {
        const auto& R = Registry[i];
        const auto It = Threads.find(R.TID);
        if (It == Threads.end()) {
          continue;
        }
        auto& T = It->second;
        T.GameExc = R.GameExceptions ? *R.GameExceptions : 0;
        if (T.LinuxTid != R.LinuxTid) {
          // New, or a new thread with an old one's TID.
          T.LinuxTid = R.LinuxTid;
          T.SampleCount = T.SkipUntil = T.Checks = 0;
          T.InProtection = T.InGame = T.InNative = T.Unread = T.BusyRounds = 0;
          T.LastRunNs = 0;
          T.Loader = false;
          T.GameThread = false;
          T.GameExcAtRound = T.GameExc;
        }
        if (!T.Named || T.FEX || T.Main || T.Loader || T.GameThread || T.SampleCount >= LoaderSamples) {
          continue;
        }
        T.Samples[T.SampleCount++] = *R.BlockHeader;
      }
    }

    const uint64_t Want = F.C->Loader == LoaderMode::Little ? F.C->LittleMask : 0;
    const bool Second = F.Tick % 4 == 0;
    const uint64_t Now = Second ? Counter() : 0;
    const double Elapsed = Second && F.LastSecond ? static_cast<double>(Now - F.LastSecond) / static_cast<double>(CounterFrequency()) : 0.0;
    LoaderLineEntry Line[MaxLineEntries] {};
    size_t LineCount {};
    const auto Sched = [&](LoaderThread& L, LoaderLineEntry& E) {
      Linux::SchedStat S {};
      if (!Linux::ReadSchedStat(L.SchedPath.c_str(), S)) {
        return false;
      }
      Linux::TaskStat Stat {};
      E.TID = L.TID;
      E.LinuxTid = L.LinuxTid;
      E.Mask = Linux::GetAffinity(L.LinuxTid);
      E.Processor = Linux::ReadTaskStat(L.StatPath.c_str(), Stat) ? Stat.Processor : -1;
      if (L.HaveSched && Elapsed > 0.1) {
        E.RunMs = static_cast<double>(S.RunNs - L.LastRunNs) / 1e6 / Elapsed;
        E.WaitMs = static_cast<double>(S.WaitNs - L.LastWaitNs) / 1e6 / Elapsed;
      }
      L.LastRunNs = S.RunNs;
      L.LastWaitNs = S.WaitNs;
      L.HaveSched = true;
      return true;
    };
    for (auto& L : F.Threads) {
      if (Want && F.Tick >= L.RetryAt) {
        // The thread's own mask: sched_getaffinity would leave out CPUs the kernel has paused for a moment.
        const uint64_t Own = Linux::ReadAllowedMask(L.StatusPath.c_str());
        if (Own && Own != Want) {
          const int Result = Linux::SetAffinity(L.LinuxTid, Want);
          if (Result) {
            ++L.Failures;
            L.RetryAt = F.Tick + 40; // 10 s
          }
          if (++L.Resets <= 3 || L.Resets % 50 == 0) {
            StatsLogNote(fextl::fmt::format("loader: tid={} (Linux {}) Linux cores were {:#x}; set back to {:#x}, result {}, change #{}", L.TID,
                                            L.LinuxTid, Own, Want, Result, L.Resets));
          }
        }
      }
      if (Second && LineCount < MaxLoaderThreads && Sched(L, Line[LineCount])) {
        ++LineCount;
      }
    }
    for (auto& L : F.GameThreads) {
      if (!Second || LineCount >= MaxLineEntries) {
        break;
      }
      const auto Found = Threads.find(L.TID);
      if (Found == Threads.end() || Found->second.LinuxTid != L.LinuxTid) {
        continue;
      }
      auto& E = Line[LineCount];
      const bool HadSched = L.HaveSched;
      if (!Sched(L, E)) {
        continue;
      }
      E.Game = true;
      const uint64_t Exc = Found->second.GameExc;
      E.GameExcPerS = HadSched && Elapsed > 0.1 ? static_cast<double>(Exc - L.LastGameExc) / Elapsed : 0.0;
      L.LastGameExc = Exc;
      ++LineCount;
    }
    if (Second) {
      F.LastSecond = Now;
      std::scoped_lock Lock {LoaderLineLock};
      std::copy(Line, Line + LineCount, LoaderLine);
      LoaderLineCount = LineCount;
    }
  }

  // A thread that raised GameExc exceptions in the main exe's code this round runs the game's own code, whatever its
  // samples say (see FEX_LOADER_GAME_EXCEPTIONS): it is never moved, and goes back to its cores if it was moved.
  void MarkGameThread(LoaderFinder& F, uint32_t TID, Tracked& T, uint64_t GameExc) {
    T.GameThread = true;
    fextl::string Moved;
    if (T.Loader) {
      T.Loader = false;
      const auto It =
        std::find_if(F.Threads.begin(), F.Threads.end(), [&](const LoaderThread& L) { return L.TID == TID && L.LinuxTid == T.LinuxTid; });
      if (It != F.Threads.end()) {
        const uint64_t Now = Linux::GetAffinity(T.LinuxTid);
        const int Result = It->Before ? Linux::SetAffinity(T.LinuxTid, It->Before) : 0;
        Moved = It->Before ?
                  fextl::fmt::format("; it had been moved as a loader thread: Linux cores {:#x} -> {:#x}, result {}", Now, It->Before, Result) :
                  fextl::string("; it had been found as a loader thread, its cores weren't changed");
        if (const auto Hook = LoaderHook.load(std::memory_order_acquire)) {
          Hook(TID, 0);
        }
        F.Threads.erase(It);
      }
    }
    if (F.GameNotes < MaxGameNotes) {
      ++F.GameNotes;
      StatsLogNote(fextl::fmt::format("loader: tid={} {} (Linux {}) raised {} exceptions in the game's own code (main exe) in 2 s: a game "
                                      "thread, never moved to the slowest cores{} (FEX_LOADER_GAME_EXCEPTIONS=0 turns this off)",
                                      TID, T.Name.empty() ? "-" : T.Name, T.LinuxTid, GameExc, Moved));
    }
    if (F.GameThreads.size() < MaxGameThreads) {
      LoaderThread L {.TID = TID, .LinuxTid = T.LinuxTid};
      L.StatusPath = fextl::fmt::format("/proc/self/task/{}/status", T.LinuxTid);
      L.SchedPath = fextl::fmt::format("/proc/self/task/{}/schedstat", T.LinuxTid);
      L.StatPath = fextl::fmt::format("/proc/self/task/{}/stat", T.LinuxTid);
      L.LastGameExc = T.GameExc;
      F.GameThreads.push_back(std::move(L));
    }
  }

  // Every 2 s round: which busy threads ran loader code in this round's samples.
  void LoaderRound(LoaderFinder& F, fextl::unordered_map<uint32_t, Tracked>& Threads, uint32_t Round) {
    const uint64_t Want = F.C->Loader == LoaderMode::Little ? F.C->LittleMask : 0;
    for (auto& [TID, T] : Threads) {
      const uint32_t N = T.SampleCount;
      T.SampleCount = 0;
      if (!T.LinuxTid || T.FEX || T.Main) {
        continue;
      }
      const uint64_t GameExc = T.GameExc - T.GameExcAtRound;
      T.GameExcAtRound = T.GameExc;
      if (F.C->GameExcPerRound && !T.GameThread && GameExc >= F.C->GameExcPerRound) {
        MarkGameThread(F, TID, T, GameExc);
      }
      if (T.Loader || T.GameThread) {
        continue;
      }
      Linux::SchedStat S {};
      if (!Linux::ReadSchedStat(fextl::fmt::format("/proc/self/task/{}/schedstat", T.LinuxTid).c_str(), S)) {
        continue;
      }
      const uint64_t RunNs = T.LastRunNs ? S.RunNs - T.LastRunNs : 0;
      T.LastRunNs = std::max<uint64_t>(S.RunNs, 1);
      if (Round < T.SkipUntil || RunNs < LoaderBusyNs || !N) {
        continue;
      }

      // Each distinct block resolved once; the counts add up over the thread's busy rounds, since a thread that spends
      // much of its time in Wine calls gives only a few samples of its own code per round.
      CodeClass Seen[LoaderSamples];
      size_t SeenCount {};
      for (uint32_t i = 0; i < N; ++i) {
        const CodeClass* K {};
        for (size_t j = 0; j < SeenCount && !K; ++j) {
          K = Seen[j].Header == T.Samples[i] ? &Seen[j] : nullptr;
        }
        if (!K) {
          Seen[SeenCount] = ClassifyBlock(*F.C, F.Kinds, T.Samples[i]);
          K = &Seen[SeenCount++];
        }
        switch (K->Kind) {
        case CodeKind::Protection:
          if (++T.InProtection == 1) {
            T.ProtectionExample = *K;
          }
          break;
        case CodeKind::Game:
          if (++T.InGame == 1) {
            T.GameExample = *K;
          }
          break;
        case CodeKind::Native: ++T.InNative; break;
        case CodeKind::Unread: ++T.Unread; break;
        }
      }
      ++T.BusyRounds;
      ++T.Checks;
      const double Busy = static_cast<double>(RunNs) / 2e7; // % of a core over the 2 s round
      const bool ByShare = T.InProtection >= LoaderMinProtection && T.InProtection >= 3 * T.InGame;
      const bool ByNoGame = T.BusyRounds >= LoaderMaxRounds && T.InGame == 0 && T.InProtection >= LoaderMinProtectionNoGame;
      const bool IsLoader = ByShare || ByNoGame;
      if (!IsLoader && T.BusyRounds < LoaderMaxRounds) {
        continue;
      }
      fextl::string Where;
      if (T.InProtection) {
        Where += fextl::fmt::format(", e.g. {:X} in the allocation at {:X}", T.ProtectionExample.RIP, T.ProtectionExample.Base);
      }
      if (T.InGame) {
        Where += fextl::fmt::format("{} game code e.g. {:X} in the {} at {:X}", T.InProtection ? ";" : ",", T.GameExample.RIP,
                                    T.GameExample.Base == F.C->ExeBase ? "main exe" : "module", T.GameExample.Base);
      }
      const fextl::string Counts =
        fextl::fmt::format("over {} busy rounds, of {} code samples {} in protection code (outside the exe and the game's DLLs), {} in the game's "
                           "code, {} in Wine's or other native DLLs (waiting or in a Wine call), {} unreadable{}",
                           T.BusyRounds, T.InProtection + T.InGame + T.InNative + T.Unread, T.InProtection, T.InGame, T.InNative, T.Unread, Where);
      if (!IsLoader) {
        const bool Soon = T.InProtection && !T.InGame;
        if (F.CheckNotes < MaxCheckNotes) {
          ++F.CheckNotes;
          StatsLogNote(fextl::fmt::format("loader: checked tid={} {} (Linux {}) at {:.0f}% of a core {}; not a loader thread, checked again in {}",
                                          TID, T.Name.empty() ? "-" : T.Name, T.LinuxTid, Busy, Counts, Soon ? "10 s" : "a minute"));
        }
        T.InProtection = T.InGame = T.InNative = T.Unread = T.BusyRounds = 0;
        T.SkipUntil = Round + (Soon ? LoaderSoonRounds : LoaderSkipRounds);
        continue;
      }

      T.Loader = true;
      if (F.Threads.size() >= MaxLoaderThreads) {
        StatsLogNote(fextl::fmt::format("loader: tid={} (Linux {}) at {:.0f}% of a core {}: a loader thread, but {} loader threads are kept "
                                        "already; left where it is",
                                        TID, T.LinuxTid, Busy, Counts, F.Threads.size()));
        continue;
      }
      LoaderThread L {.TID = TID, .LinuxTid = T.LinuxTid};
      L.StatusPath = fextl::fmt::format("/proc/self/task/{}/status", T.LinuxTid);
      L.SchedPath = fextl::fmt::format("/proc/self/task/{}/schedstat", T.LinuxTid);
      L.StatPath = fextl::fmt::format("/proc/self/task/{}/stat", T.LinuxTid);
      const uint64_t Before = Linux::GetAffinity(T.LinuxTid);
      L.Before = Before;
      const int Result = Want ? Linux::SetAffinity(T.LinuxTid, Want) : 0;
      if (Result) {
        ++L.Failures;
        L.RetryAt = F.Tick + 40;
      }
      StatsLogNote(fextl::fmt::format(
        "loader: tid={} {} (Linux {}) at {:.0f}% of a core {}: a loader thread{}; {}", TID, T.Name.empty() ? "-" : T.Name, T.LinuxTid, Busy, Counts,
        ByShare ? "" : " (no game code sample in its 4 busy rounds: build 81)",
        Want                                     ? fextl::fmt::format("Linux cores {:#x} -> {:#x} (the slowest), result {} (FEX_LOADER_CORES=off leaves it)",
                                                                      Before, Want, Result) :
        F.C->Loader == LoaderMode::Little ? fextl::string("left where it is: the slowest cores aren't known") :
                                            fextl::string("left where it is (FEX_LOADER_CORES=find)")));
      F.Threads.push_back(std::move(L));
      if (const auto Hook = LoaderHook.load(std::memory_order_acquire)) {
        Hook(TID, T.Teb);
      }
    }

    // Loader threads that ended (or whose TID a new thread has now).
    for (auto It = F.Threads.begin(); It != F.Threads.end();) {
      const auto Found = Threads.find(It->TID);
      if (Found != Threads.end() && Found->second.Loader && Found->second.LinuxTid == It->LinuxTid) {
        ++It;
        continue;
      }
      StatsLogNote(fextl::fmt::format("loader: tid={} (Linux {}) ended", It->TID, It->LinuxTid));
      if (const auto Hook = LoaderHook.load(std::memory_order_acquire)) {
        Hook(It->TID, 0);
      }
      It = F.Threads.erase(It);
    }
    for (auto It = F.GameThreads.begin(); It != F.GameThreads.end();) {
      const auto Found = Threads.find(It->TID);
      if (Found != Threads.end() && Found->second.GameThread && Found->second.LinuxTid == It->LinuxTid) {
        ++It;
        continue;
      }
      StatsLogNote(fextl::fmt::format("loader: game thread tid={} (Linux {}) ended", It->TID, It->LinuxTid));
      It = F.GameThreads.erase(It);
    }
  }


  // Build 87: the game's job workers, see FEX_WORKER_NICE in PriorityGuard.h.
  enum class WorkerMode { Off, Fixed, Alternate };
  constexpr size_t MaxWorkers = 16;
  constexpr uint64_t WorkerWindowSeconds = 10;    // as the stats log's frames10s lines, so each window has one
  constexpr uint32_t WorkerWindowTicks = 40;      // without the stats log: 250 ms ticks

  struct WorkerThread {
    uint32_t TID;
    int LinuxTid;
    uint64_t Teb;
    int Before; // its nice when found
    int SchedFd;
    char StatusPath[48];
    bool Gone;
    bool HavePrev;
    Linux::SchedStat Prev;
    uint64_t PrevVoluntary, PrevInvoluntary, PrevCalls;
    uint32_t Sets, Failures;
  };

  // Per-second figures added up over a window, and over all whole windows of each half.
  struct WorkerSums {
    uint32_t Seconds;
    uint32_t ThreadSeconds;
    double RunMs, WaitMs, SpinMs;
    uint64_t Runs, Slept, Preempted, Calls;
  };

  struct WorkerKeeper {
    std::mutex Lock;
    WorkerMode Mode;
    int Nice;        // the raised half's nice
    bool LeftAtZero; // the other half: nice 0 with FEX_PRIO=normal (what it gives every game thread), else each thread's own
    WorkerThread Threads[MaxWorkers];
    size_t Count;
    uint32_t Tick;   // the guard's 250 ms ticks, for the halves without the stats log
    bool Applied;
    bool LastRaised;
    uint32_t Changes; // nice changes made, all threads
    uint32_t Failures;
    // Stats log thread only.
    uint64_t PrevSpinNs;
    bool HaveSpin;
    WorkerSums Window;
    WorkerSums Totals[2]; // [0] the other half, [1] raised
    uint32_t Windows[2];
  };
  WorkerKeeper* Workers {};
  std::atomic<JobWorkerSource> WorkerSource {};

  // Alternating halves follow the stats log's seconds: seconds 1-10 raised, 11-20 not, and so on.
  bool RaisedInSecond(uint64_t Second) {
    return ((Second - 1) / WorkerWindowSeconds) % 2 == 0;
  }

  // The half the current moment belongs to.
  bool WorkersRaisedNow(const WorkerKeeper& K) {
    if (K.Mode == WorkerMode::Fixed) {
      return true;
    }
    if (const uint64_t Logged = StatsLogSecond()) {
      return RaisedInSecond(Logged + 1);
    }
    return (K.Tick / WorkerWindowTicks) % 2 == 0;
  }

  int WantedNice(const WorkerKeeper& K, const WorkerThread& T, bool Raised) {
    return Raised ? K.Nice : K.LeftAtZero ? 0 : T.Before;
  }

  // Under K.Lock.
  void ApplyWorkerNice(WorkerKeeper& K, bool Raised) {
    for (size_t i = 0; i < K.Count; ++i) {
      auto& T = K.Threads[i];
      if (T.Gone) {
        continue;
      }
      const int Now = Linux::GetNice(T.LinuxTid);
      if (Now < -100) {
        T.Gone = true;
        continue;
      }
      const int Want = WantedNice(K, T, Raised);
      if (Now == Want) {
        continue;
      }
      if (Linux::SetNice(T.LinuxTid, Want) == 0) {
        ++T.Sets;
        ++K.Changes;
      } else {
        ++T.Failures;
        ++K.Failures;
      }
    }
    K.Applied = true;
    K.LastRaised = Raised;
  }

  // Every 250 ms on the guard's thread: takes in the threads the job system found, and keeps the nice of the half.
  void KeepWorkers(WorkerKeeper& K, const Config& C, const LoaderFinder* Finder) {
    ++K.Tick;
    JobWorker Found[MaxWorkers];
    uint64_t SpinNs {};
    const auto Source = WorkerSource.load(std::memory_order_acquire);
    const size_t N = Source ? Source(Found, MaxWorkers, &SpinNs) : 0;
    for (size_t i = 0; i < N; ++i) {
      {
        std::scoped_lock Lock {K.Lock};
        bool Known = false;
        for (size_t j = 0; j < K.Count && !Known; ++j) {
          Known = K.Threads[j].Teb == Found[i].Teb && !K.Threads[j].Gone;
        }
        if (Known) {
          continue;
        }
      }
      Registered R {};
      {
        std::scoped_lock Lock {RegistryLock};
        for (size_t j = 0; j < RegistryCount; ++j) {
          if (Registry[j].Teb == Found[i].Teb) {
            R = Registry[j];
            break;
          }
        }
      }
      if (!R.TID || R.TID == C.MainTID || R.LinuxTid <= 0) {
        continue;
      }
      if (Finder && std::any_of(Finder->Threads.begin(), Finder->Threads.end(), [&](const LoaderThread& L) { return L.TID == R.TID; })) {
        continue;
      }
      std::scoped_lock Lock {K.Lock};
      // A slot of a thread that ended, or a new one.
      size_t Slot = K.Count;
      for (size_t j = 0; j < K.Count; ++j) {
        if (K.Threads[j].Gone || K.Threads[j].Teb == Found[i].Teb) {
          Slot = j;
          break;
        }
      }
      if (Slot == MaxWorkers) {
        continue;
      }
      auto& T = K.Threads[Slot];
      if (Slot < K.Count && T.SchedFd >= 0) {
        Linux::Close(T.SchedFd);
      }
      T = {};
      T.TID = R.TID;
      T.LinuxTid = R.LinuxTid;
      T.Teb = Found[i].Teb;
      T.Before = Linux::GetNice(R.LinuxTid);
      char Path[64];
      snprintf(Path, sizeof(Path), "/proc/self/task/%d/schedstat", R.LinuxTid);
      T.SchedFd = Linux::Open(Path);
      snprintf(T.StatusPath, sizeof(T.StatusPath), "/proc/self/task/%d/status", R.LinuxTid);
      T.PrevCalls = Found[i].Calls;
      if (Slot == K.Count) {
        ++K.Count;
      }
      const bool Raised = WorkersRaisedNow(K);
      StatsLogNote(fextl::fmt::format("workers: tid={} (Linux {}) uses the game's job system: a job worker (#{}), nice {} when found, now {}",
                                      T.TID, T.LinuxTid, K.Count, T.Before, WantedNice(K, T, Raised)));
    }
    if (K.Count) {
      std::scoped_lock Lock {K.Lock};
      ApplyWorkerNice(K, WorkersRaisedNow(K));
    }
  }

  fextl::string WorkerHalfText(const WorkerKeeper& K, bool Raised) {
    if (K.Mode == WorkerMode::Fixed) {
      return fextl::fmt::format("nice {}", K.Nice);
    }
    if (Raised) {
      return fextl::fmt::format("nice {} (raised half)", K.Nice);
    }
    return K.LeftAtZero ? fextl::string("nice 0 (other half)") : fextl::string("their own nice (other half)");
  }

  fextl::string WorkerSumsText(const WorkerSums& S) {
    const double Secs = S.Seconds ? static_cast<double>(S.Seconds) : 1.0;
    return fextl::fmt::format("{:.1f} threads ran {:.0f} ms/s, waited {:.0f} ms/s for a CPU, {:.0f} runs/s ({:.0f} us waited a run), "
                              "slept {:.0f}/s, preempted {:.0f}/s, {:.0f}k job system calls/s; main thread spun {:.0f} ms/s in the frame "
                              "wait loop",
                              S.ThreadSeconds / Secs, S.RunMs / Secs, S.WaitMs / Secs, S.Runs / Secs,
                              S.Runs ? S.WaitMs * 1000.0 / static_cast<double>(S.Runs) : 0.0, S.Slept / Secs, S.Preempted / Secs,
                              S.Calls / Secs / 1000.0, S.SpinMs / Secs);
  }

  void AddSums(WorkerSums& To, const WorkerSums& From) {
    To.Seconds += From.Seconds;
    To.ThreadSeconds += From.ThreadSeconds;
    To.RunMs += From.RunMs;
    To.WaitMs += From.WaitMs;
    To.SpinMs += From.SpinMs;
    To.Runs += From.Runs;
    To.Slept += From.Slept;
    To.Preempted += From.Preempted;
    To.Calls += From.Calls;
  }

  // Every second on the stats log thread: the "workers" line, a "# workers" line at the end of each 10 s window, and the
  // switch to the next half right at the window's end.
  fextl::string WorkersStatsLine() {
    auto* KP = Workers;
    if (!KP) {
      return {};
    }
    auto& K = *KP;
    const uint64_t Second = StatsLogSecond();
    JobWorker Found[MaxWorkers];
    uint64_t SpinNs {};
    const auto Source = WorkerSource.load(std::memory_order_acquire);
    const size_t N = Source ? Source(Found, MaxWorkers, &SpinNs) : 0;
    const bool Raised = K.Mode == WorkerMode::Fixed || RaisedInSecond(Second);

    std::scoped_lock Lock {K.Lock};
    const double SpinMs = K.HaveSpin && SpinNs >= K.PrevSpinNs ? static_cast<double>(SpinNs - K.PrevSpinNs) / 1e6 : 0.0;
    K.PrevSpinNs = SpinNs;
    K.HaveSpin = Source != nullptr;
    WorkerSums S {};
    fextl::string Each;
    for (size_t i = 0; i < K.Count; ++i) {
      auto& T = K.Threads[i];
      if (T.Gone) {
        continue;
      }
      Linux::SchedStat Now {};
      if (!Linux::PreadSchedStat(T.SchedFd, Now)) {
        T.Gone = true;
        Linux::Close(T.SchedFd);
        T.SchedFd = -1;
        Each += fextl::fmt::format(" {}:ended", T.TID);
        continue;
      }
      uint64_t Voluntary {}, Involuntary {};
      const bool HaveSwitches = Linux::ReadContextSwitches(T.StatusPath, Voluntary, Involuntary);
      uint64_t Calls = T.PrevCalls;
      for (size_t j = 0; j < N; ++j) {
        if (Found[j].Teb == T.Teb) {
          Calls = Found[j].Calls;
        }
      }
      if (T.HavePrev) {
        const double RunMs = static_cast<double>(Now.RunNs - T.Prev.RunNs) / 1e6;
        const double WaitMs = static_cast<double>(Now.WaitNs - T.Prev.WaitNs) / 1e6;
        const uint64_t Runs = Now.Slices - T.Prev.Slices;
        const uint64_t Slept = HaveSwitches && Voluntary >= T.PrevVoluntary ? Voluntary - T.PrevVoluntary : 0;
        const uint64_t Preempted = HaveSwitches && Involuntary >= T.PrevInvoluntary ? Involuntary - T.PrevInvoluntary : 0;
        const uint64_t CallsNow = Calls >= T.PrevCalls ? Calls - T.PrevCalls : 0;
        ++S.ThreadSeconds;
        S.RunMs += RunMs;
        S.WaitMs += WaitMs;
        S.Runs += Runs;
        S.Slept += Slept;
        S.Preempted += Preempted;
        S.Calls += CallsNow;
        Each += fextl::fmt::format(" {}:{:.0f}+{:.0f}/{}/{}k", T.TID, RunMs, WaitMs, Runs, CallsNow / 1000);
      }
      T.Prev = Now;
      if (HaveSwitches) {
        T.PrevVoluntary = Voluntary;
        T.PrevInvoluntary = Involuntary;
      }
      T.PrevCalls = Calls;
      T.HavePrev = true;
    }
    S.SpinMs = SpinMs;

    fextl::string Out;
    if (S.ThreadSeconds) {
      S.Seconds = 1;
      Out = fextl::fmt::format("  workers {}: {} threads ran {:.0f} ms, waited {:.0f} ms for a CPU, {} runs ({:.0f} us waited a run), slept {}, "
                               "preempted {}, {}k job system calls | tid:run+wait ms/runs/calls{}\n",
                               WorkerHalfText(K, Raised), S.ThreadSeconds, S.RunMs, S.WaitMs, S.Runs, S.Runs ? S.WaitMs * 1000.0 / static_cast<double>(S.Runs) : 0.0,
                               S.Slept, S.Preempted, S.Calls / 1000, Each);
      AddSums(K.Window, S);
    }

    if (Second % WorkerWindowSeconds == 0) {
      if (K.Window.Seconds) {
        const bool Whole = K.Window.Seconds == WorkerWindowSeconds;
        fextl::string Text = fextl::fmt::format("# workers: seconds {}-{} at {}: {}", Second - WorkerWindowSeconds + 1, Second,
                                                WorkerHalfText(K, Raised), WorkerSumsText(K.Window));
        if (Whole) {
          AddSums(K.Totals[Raised ? 1 : 0], K.Window);
          ++K.Windows[Raised ? 1 : 0];
        } else {
          Text += fextl::fmt::format(" (only {} s of it counted)", K.Window.Seconds);
        }
        if (K.Mode == WorkerMode::Alternate && K.Windows[0] && K.Windows[1]) {
          Text += fextl::fmt::format(" | whole windows so far: raised {} windows: {} | other half {} windows: {}", K.Windows[1],
                                     WorkerSumsText(K.Totals[1]), K.Windows[0], WorkerSumsText(K.Totals[0]));
        }
        Out += Text + "\n";
      }
      K.Window = {};
      // The next window's half starts now, rather than at the guard's next tick.
      if (K.Mode == WorkerMode::Alternate && K.Count) {
        ApplyWorkerNice(K, RaisedInSecond(Second + 1));
      }
    }
    return Out;
  }

  void* GuardThread(void* Arg) {
    const auto* C = static_cast<Config*>(Arg);
    const uint32_t SelfTID = GetCurrentThreadId();
    const LONG Low = C->Prio == PrioMode::Normal ? 0 : -1;
    const LONG High = C->Prio == PrioMode::Normal ? 0 : 1;

    struct Entry {
      HANDLE Handle;
      uint32_t TID;
      ThreadBasicInfo Basic;
      Tracked* T;
      bool IsMain;
    };

    fextl::unordered_map<uint32_t, Tracked> Threads;
    fextl::vector<HANDLE> Handles;
    fextl::vector<Entry> Entries;
    uint32_t Round {};
    bool ReportedWalkFailure {};
    uint64_t ReportedGameMask {};
    int ServerPid {};
    uint32_t ServerCoreReports {};
    MainKeeper Keeper {.Tid = C->Cores == CoreMode::Fast ? C->MainLinuxTid : 0, .EaseMs = C->EaseWaitMs};
    MainNiceKeeper NiceKeeper {.Tid = C->MainNiceOn ? C->MainLinuxTid : 0, .Nice = C->MainNice};
    Keeper.StatusPath = fextl::fmt::format("/proc/self/task/{}/status", Keeper.Tid);
    uint64_t KeepFast {}, KeepWide {};
    LoaderFinder Finder {.C = C};
    OthersKeeper Others {};
    Others.SelfPid = Linux::GetPid();
    Others.Uid = Linux::GetUid();
    {
      // "0::/uid_10121/pid_1716": this process's cgroup v2 group; its parent holds the user's groups.
      char Group[512];
      if (Linux::ReadSmallFile("/proc/self/cgroup", Group, sizeof(Group)) > 0) {
        if (const char* P = strstr(Group, "0::/uid_")) {
          const char* Start = P + 3;
          const char* End = Start + 5;
          while (*End >= '0' && *End <= '9') {
            ++End;
          }
          Others.UidCgroup = fextl::fmt::format("/sys/fs/cgroup{}", fextl::string(Start, static_cast<size_t>(End - Start)));
        }
      }
    }

    for (;;) {
      const LONG Base = C->Prio != PrioMode::Keep ? CheckClass(Round == 0) : 8;
      ++Round;

      if (C->ServerNiceOn && (Round == 1 || Round % 30 == 0)) {
        ServerPid = BoostServer(*C, ServerPid, Round == 1);
      } else if (!C->ServerNiceOn && C->Cores == CoreMode::Fast && !ServerPid && (Round == 1 || Round % 30 == 0)) {
        ServerPid = Linux::FindProcess("wineserver");
      }
      const uint64_t ProcessMask = ProcessAffinity();

      // NtGetNextThread continues from the previous handle, so all handles stay open until the end of this round.
      Handles.clear();
      Entries.clear();
      HANDLE Next {};
      NTSTATUS WalkStatus;
      uint32_t Count {}, Fixed {}, CoreFixed {};
      while ((WalkStatus = NtGetNextThread(NtCurrentProcess(), Handles.empty() ? nullptr : Handles.back(),
                                           THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, 0, 0, &Next)) ==
             STATUS_SUCCESS) {
        Handles.push_back(Next);
        ThreadBasicInfo Basic {};
        if (NtQueryInformationThread(Next, ThreadBasicInformation, &Basic, sizeof(Basic), nullptr) != STATUS_SUCCESS ||
            Basic.ExitStatus != STATUS_PENDING) {
          continue;
        }
        const auto TID = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Basic.ClientId.UniqueThread));
        if (TID == SelfTID) {
          continue;
        }
        ++Count;
        auto [It, New] = Threads.try_emplace(TID);
        auto& T = It->second;
        T.SeenAt = Round;
        T.Teb = reinterpret_cast<uintptr_t>(Basic.TebBaseAddress);
        if (New) {
          T.FirstBase = Basic.BasePriority;
          T.FirstPriority = Basic.Priority;
          T.FirstAffinity = Basic.AffinityMask;
        }
        if (New || Basic.AffinityMask != T.SetTo) {
          T.Orig = Basic.AffinityMask;
        }
        if (!T.Named || (T.Name.empty() && Round % 5 == 0)) {
          T.Name = QueryName(Next);
          T.Named = true;
          T.FEX = T.Name.size() >= 3 && T.Name.compare(0, 3, "FEX") == 0;
          // Only the stats log (its 2ms sampler and per-second work), this guard, the rumble sender and the file cache dropper move: the stall
          // probe measures what game threads see, and the disk cache writer, prefetcher and unpack cache threads hold locks
          // or feed loads the game waits on.
          T.KeepCores = !(T.Name == "FEXStatsLog" || T.Name == "FEXPrioGuard" || T.Name == "FEXThreadPin" || T.Name == "FEXFrameLatency" ||
                          T.Name == "FEXRumble" || T.Name == "FEXFileCache");
        }
        T.Main = TID == C->MainTID || T.Name == "Main";
        Entries.push_back({Next, TID, Basic, &T, T.Main});
      }

      // The cores the game gave its own threads: the most common affinity among threads other than FEX's and the main
      // thread, as the game or Wine set it (not as this guard changed it).
      uint64_t GameMask {};
      if (C->Cores != CoreMode::Off) {
        fextl::unordered_map<uint64_t, uint32_t> Votes;
        uint32_t Best {};
        for (const auto& E : Entries) {
          if (!E.T->FEX && !E.IsMain && E.T->Orig) {
            const uint32_t N = ++Votes[E.T->Orig];
            if (N > Best || (N == Best && E.T->Orig < GameMask)) {
              Best = N;
              GameMask = E.T->Orig;
            }
          }
        }
      }
      const bool FastUsable = C->FastMask && GameMask && (GameMask & C->FastMask) == C->FastMask && GameMask != C->FastMask;
      if (ServerPid && C->Cores == CoreMode::Fast && FastUsable) {
        KeepServerOff(ServerPid, GameMask & ~C->FastMask, ServerCoreReports);
      }
      if (C->Others != OthersMode::Off && C->Cores == CoreMode::Fast && FastUsable) {
        KeepOthersOff(Others, *C, GameMask & ~C->FastMask, ServerPid, Round);
      }
      // What the main thread's Linux cores should be until the next round: the fast core, or with it the game's big cores
      // (never the slowest ones) while it waits too long for the fast one.
      KeepFast = C->Cores == CoreMode::Fast && FastUsable ? C->FastMask : 0;
      KeepWide = KeepFast ? ((GameMask | C->FastMask) & ~C->LittleMask) | C->FastMask : 0;
      if (GameMask && GameMask != ReportedGameMask && C->Cores != CoreMode::Off) {
        ReportedGameMask = GameMask;
        StatsLogNote(fextl::fmt::format(
          "cores: game threads on {:#x}; main thread -> {:#x}{}, other game threads {}", GameMask,
          C->Cores == CoreMode::Fast && FastUsable ? C->FastMask : GameMask,
          C->Cores == CoreMode::Fast && !FastUsable ? " (fastest core not usable, same as the others)" : "",
          C->Cores == CoreMode::Fast && FastUsable ? fextl::fmt::format("lose {:#x}", C->FastMask) : fextl::string("unchanged")));
      }

      for (auto& E : Entries) {
        auto& T = *E.T;
        const auto& Basic = E.Basic;

        if (!T.FEX && C->Prio != PrioMode::Keep) {
          // Effective priority = class base + base priority, +1 for the process's first thread (up to 15).
          const LONG Want = std::clamp<LONG>(Basic.BasePriority, Low, High);
          const LONG Expected = std::min<LONG>(Base + Want, 15);
          const bool PriorityOff = Basic.Priority < Expected || Basic.Priority > std::min<LONG>(Expected + 1, 15);
          if (Basic.BasePriority != Want || PriorityOff) {
            LONG Value = Want;
            const NTSTATUS Status = NtSetInformationThread(E.Handle, ThreadBasePriority, &Value, sizeof(Value));
            ++T.Fixes;
            ++Fixed;
            // The first three fixes of a thread, then every 50th: a game that sets priorities every frame shows as a count.
            if (T.Fixes <= 3 || T.Fixes % 50 == 0) {
              StatsLogNote(fextl::fmt::format("prio: tid={} {} base={} prio={} (nice {}) affinity={:#x} -> base {} (nice {}), status={:#x}, "
                                              "fix #{}",
                                              E.TID, T.Name.empty() ? "-" : T.Name, Basic.BasePriority, Basic.Priority,
                                              WineNice(Basic.Priority), static_cast<uint64_t>(Basic.AffinityMask), Want, WineNice(8 + Want),
                                              static_cast<uint32_t>(Status), T.Fixes));
            }
          }
        }

        const uint64_t Current = Basic.AffinityMask;
        const uint64_t Want = WantedAffinity(*C, T, E.IsMain, Current, GameMask, FastUsable, ProcessMask);
        if (Want && Want != Current) {
          KAFFINITY Value = Want;
          const NTSTATUS Status = NtSetInformationThread(E.Handle, ThreadAffinityMaskClass, &Value, sizeof(Value));
          if (Status == STATUS_SUCCESS) {
            T.SetTo = Want;
          }
          ++T.CoreFixes;
          ++CoreFixed;
          if (T.CoreFixes <= 3 || T.CoreFixes % 50 == 0) {
            StatsLogNote(fextl::fmt::format("cores: tid={} {}{} affinity {:#x} -> {:#x}, status={:#x}, fix #{}", E.TID,
                                            T.Name.empty() ? "-" : T.Name, E.IsMain ? " (main thread)" : "", Current, Want,
                                            static_cast<uint32_t>(Status), T.CoreFixes));
          }
        }
      }

      for (HANDLE H : Handles) {
        NtClose(H);
      }

      if (C->Loader != LoaderMode::Off) {
        LoaderRound(Finder, Threads, Round);
      }

      if (Handles.empty() && !ReportedWalkFailure) {
        ReportedWalkFailure = true;
        StatsLogNote(fextl::fmt::format("prio: can't list the process's threads, NtGetNextThread status={:#x}", static_cast<uint32_t>(WalkStatus)));
      }

      // Once a minute: every thread's first seen base priority and affinity, so a log shows what the game asked for.
      if (Round % 30 == 0) {
        fextl::unordered_map<uint64_t, uint32_t> Masks, NowMasks;
        fextl::string Odd;
        for (const auto& E : Entries) {
          const auto& T = *E.T;
          ++Masks[T.FirstAffinity];
          ++NowMasks[E.Basic.AffinityMask];
          if (!T.FEX && (T.FirstBase != 0 || T.Fixes)) {
            Odd += fextl::fmt::format(" {}:{}:{}/{}x{}", E.TID, T.Name.empty() ? "-" : T.Name.substr(0, 16), T.FirstBase, T.FirstPriority, T.Fixes);
          }
        }
        fextl::string MaskText, NowText;
        for (const auto& [Mask, N] : Masks) {
          MaskText += fextl::fmt::format(" {:#x}x{}", Mask, N);
        }
        for (const auto& [Mask, N] : NowMasks) {
          NowText += fextl::fmt::format(" {:#x}x{}", Mask, N);
        }
        StatsLogNote(fextl::fmt::format("prio: s={} threads={} fixed={} cores fixed={} | affinity at start{} | now{} | not normal at start "
                                        "(tid:name:base/prio x fixes):{}",
                                        Round * 2, Count, Fixed, CoreFixed, MaskText, NowText, Odd.empty() ? " none" : Odd));
      }

      for (auto It = Threads.begin(); It != Threads.end();) {
        It = It->second.SeenAt == Round ? std::next(It) : Threads.erase(It);
      }

      // 2 s to the next round, checking the main thread's Linux cores every 250 ms meanwhile.
      for (int i = 0; i < 8; ++i) {
        LARGE_INTEGER Interval {.QuadPart = -2'500'000}; // 250ms
        NtDelayExecution(FALSE, &Interval);
        KeepMainCore(Keeper, KeepFast, KeepWide);
        KeepMainNice(NiceKeeper);
        if (C->Loader != LoaderMode::Off) {
          LoaderTick(Finder, Threads);
        }
        if (Workers) {
          KeepWorkers(*Workers, *C, C->Loader != LoaderMode::Off ? &Finder : nullptr);
        }
      }
    }
    return nullptr;
  }
} // namespace

void StartPriorityGuard(std::string_view AppName) {
  auto* C = new Config {};
  C->MainTID = GetCurrentThreadId();
  C->MainLinuxTid = Linux::GetTid();

  const char* Prio = Switch("FEX_PRIO");
  C->Prio = !Prio || Prio[0] == 'k' || Prio[0] == '0' ? PrioMode::Keep : Prio[0] == 'n' ? PrioMode::Normal : PrioMode::Clamp;

  const char* Cores = Switch("FEX_MAINCORE");
  C->Cores = !Cores || Cores[0] == 'o' || Cores[0] == '0' ? CoreMode::Off : Cores[0] == 'b' ? CoreMode::Big : CoreMode::Fast;

  const char* Wait = Switch("FEX_MAINCORE_WAIT");
  C->EaseWaitMs = !Wait ? 40 : (Wait[0] >= '0' && Wait[0] <= '9') ? static_cast<uint32_t>(std::clamp(atoi(Wait), 0, 1000)) : 0;

  const char* Others = Switch("FEX_MAINCORE_OTHERS");
  C->Others = !Others || Others[0] == 'o' || Others[0] == '0' ? OthersMode::Off : Others[0] == 'p' ? OthersMode::Process : OthersMode::User;

  const char* Helpers = Switch("FEX_HELPER_CORES");
  C->HelpersLittle = Helpers && Helpers[0] == 'l';

  const char* Loader = Switch("FEX_LOADER_CORES");
  C->Loader = !Loader || Loader[0] == 'o' || Loader[0] == '0' ? LoaderMode::Off : Loader[0] == 'f' ? LoaderMode::Find : LoaderMode::Little;
  // Read now, in process init: D2R's protection later remaps the exe into a view that is no longer an image.
  C->ExeBase = reinterpret_cast<uint64_t>(reinterpret_cast<__TEB*>(NtCurrentTeb())->Peb->ImageBaseAddress);
  if (const auto* Nt = C->ExeBase ? RtlImageNtHeader(reinterpret_cast<HMODULE>(C->ExeBase)) : nullptr) {
    C->ExeEnd = C->ExeBase + Nt->OptionalHeader.SizeOfImage;
  }
  const char* GameExc = Switch("FEX_LOADER_GAME_EXCEPTIONS");
  C->GameExcPerRound = GameExc && GameExc[0] >= '0' && GameExc[0] <= '9' ? static_cast<uint32_t>(std::clamp(atoi(GameExc), 0, 1'000'000)) : 0;

  const char* ServerNice = Switch("FEX_SERVER_NICE");
  if (ServerNice && ServerNice[0] != 'o' && (ServerNice[0] == '-' || (ServerNice[0] >= '0' && ServerNice[0] <= '9'))) {
    C->ServerNiceOn = true;
    C->ServerNice = std::clamp(atoi(ServerNice), -20, 19);
  }
  const char* MainNice = Switch("FEX_MAIN_NICE");
  if (MainNice && MainNice[0] != 'o' && (MainNice[0] == '-' || (MainNice[0] >= '0' && MainNice[0] <= '9'))) {
    C->MainNiceOn = true;
    C->MainNice = std::clamp(atoi(MainNice), -20, 19);
  }

  WorkerMode WorkerNiceMode = WorkerMode::Off;
  int WorkerNice = -10;
  if (const char* W = Switch("FEX_WORKER_NICE"); W && W[0] && W[0] != 'o') {
    const char* Number = W;
    if (W[0] == 'a') {
      WorkerNiceMode = WorkerMode::Alternate;
      Number = W + 1;
      while (*Number && *Number != '-' && (*Number < '0' || *Number > '9')) {
        ++Number;
      }
    } else if (W[0] == '-' || (W[0] >= '0' && W[0] <= '9')) {
      WorkerNiceMode = WorkerMode::Fixed;
    }
    if (*Number == '-' || (*Number >= '0' && *Number <= '9')) {
      WorkerNice = std::clamp(atoi(Number), -20, 19);
    }
  }

  StatsLogNote(C->Prio == PrioMode::Keep ? fextl::fmt::format("prio: off, Windows thread priorities pass through to Linux nice "
                                                               "(FEX_PRIO=normal keeps every thread at nice 0)") :
                                           fextl::fmt::format("prio: {} in {}, checked every 2s (FEX_PRIO=keep turns it off)",
                                                              C->Prio == PrioMode::Normal ? "every thread at NORMAL" :
                                                                                            "base priorities kept to BELOW_NORMAL..ABOVE_NORMAL",
                                                              AppName));
  StatsLogNote(fextl::fmt::format("cores: main thread (tid {}, Linux {}) {}; FEX helpers {} (FEX_MAINCORE=off, FEX_HELPER_CORES=off turn them "
                                  "off)",
                                  C->MainTID, C->MainLinuxTid,
                                  C->Cores == CoreMode::Fast ? "alone on the fastest core" :
                                  C->Cores == CoreMode::Big  ? "on the game's cores" :
                                                               "left alone",
                                  C->HelpersLittle ? "on the slowest cores" : "left alone"));
  if (C->Cores == CoreMode::Fast) {
    StatsLogNote(C->EaseWaitMs ? fextl::fmt::format("cores: main thread's Linux cores checked every 250 ms; it may use the other big cores while it "
                                                    "waits {} ms or more a second for its own (FEX_MAINCORE_WAIT=off keeps it there)",
                                                    C->EaseWaitMs) :
                                 fextl::string("cores: main thread's Linux cores checked every 250 ms; it stays on its core however long it waits"));
    StatsLogNote(C->Others == OthersMode::User ?
                   "cores: every other thread of this Linux user (Wine's other programs such as steam.exe, this process's threads Wine "
                   "doesn't list, and from build 77 the threads /proc hides, found through the user's cgroup groups: the launcher's "
                   "own app process) kept off the main thread's core, checked every 2 s (FEX_MAINCORE_OTHERS=process only this "
                   "process's, =off leaves them)" :
                 C->Others == OthersMode::Process ?
                   "cores: this process's threads Wine doesn't list kept off the main thread's core, other programs left alone "
                   "(FEX_MAINCORE_OTHERS=process; =user moves steam.exe and the rest of this user's threads too)" :
                   "cores: other programs' threads left alone, they may run on the main thread's core (FEX_MAINCORE_OTHERS=off)");
  }
  StatsLogNote(C->ServerNiceOn ? fextl::fmt::format("server: wineserver to nice {} (FEX_SERVER_NICE=off leaves it alone)", C->ServerNice) :
                                 fextl::string("server: wineserver nice left alone"));
  StatsLogNote(C->MainNiceOn ? fextl::fmt::format("prio: main thread (Linux {}) kept at nice {}, checked every 250 ms (FEX_MAIN_NICE=off leaves "
                                                  "it to Wine)",
                                                  C->MainLinuxTid, C->MainNice) :
                               fextl::string("prio: main thread's nice left to Wine"));

  StatsLogNote(WorkerNiceMode == WorkerMode::Alternate ?
                 fextl::fmt::format("workers: the game's job workers (threads other than the main thread that use its job system) at nice {} "
                                    "in every other 10 s window (seconds 1-10, 21-30, ...), {} in the others, checked every 250 ms; a "
                                    "'workers' line every second and a '# workers' line per window compare the halves "
                                    "(FEX_WORKER_NICE=-10 keeps them at -10, =off leaves them)",
                                    WorkerNice, C->Prio == PrioMode::Normal ? "nice 0 (as FEX_PRIO=normal leaves them)" : "their own nice") :
               WorkerNiceMode == WorkerMode::Fixed ?
                 fextl::fmt::format("workers: the game's job workers (threads other than the main thread that use its job system) kept at "
                                    "nice {}, checked every 250 ms (FEX_WORKER_NICE=off leaves them, =alt switches every 10 s)",
                                    WorkerNice) :
                 fextl::string("workers: the game's job workers' nice left alone (FEX_WORKER_NICE=alt raises it in every other 10 s window)"));

  const fextl::string Exe = fextl::fmt::format("main exe {:X}-{:X}", C->ExeBase, C->ExeEnd);
  StatsLogNote(C->Loader == LoaderMode::Little ?
                 fextl::fmt::format("loader: threads running at 25% of a core or more whose own code is mostly protection code (not in the {} "
                                    "or another x64 DLL; Wine's DLLs don't count either way), or from build 81 that ran 3 or more protection "
                                    "code samples and none of the game's code in 4 busy rounds, go to the slowest cores at the Linux level, "
                                    "checked every 250 ms (FEX_LOADER_CORES=off leaves them, =find only finds them)",
                                    Exe) :
               C->Loader == LoaderMode::Find ?
                 fextl::fmt::format("loader: threads running mostly protection code (not in the {} or another x64 DLL) are found and logged, "
                                    "left where they are (FEX_LOADER_CORES=little moves them to the slowest cores)",
                                    Exe) :
                 fextl::string("loader: off (FEX_LOADER_CORES=little moves threads running mostly the game's protection code to the slowest "
                               "cores)"));
  if (C->Loader != LoaderMode::Off) {
    StatsLogNote(C->GameExcPerRound ? fextl::fmt::format("loader: a thread that raises {} or more exceptions within 2 s in the game's own "
                                                         "code (the {}) is a game "
                                                         "thread: never moved to the slowest cores, moved back if it was "
                                                         "(FEX_LOADER_GAME_EXCEPTIONS=0 turns this off)",
                                                         C->GameExcPerRound, Exe) :
                                      fextl::string("loader: threads aren't told apart by their exceptions in the game's own code "
                                                    "(FEX_LOADER_GAME_EXCEPTIONS=400 "
                                                    "keeps threads raising 400 or more within 2 s off the slowest cores)"));
  }

  if (C->Prio == PrioMode::Keep && C->Cores == CoreMode::Off && !C->HelpersLittle && !C->ServerNiceOn && !C->MainNiceOn &&
      C->Loader == LoaderMode::Off && WorkerNiceMode == WorkerMode::Off) {
    delete C;
    return;
  }
  if (WorkerNiceMode != WorkerMode::Off) {
    Workers = new WorkerKeeper {};
    Workers->Mode = WorkerNiceMode;
    Workers->Nice = WorkerNice;
    Workers->LeftAtZero = C->Prio == PrioMode::Normal;
    StatsLogSetLineProvider(WorkersStatsLine);
  }
  if (C->Cores == CoreMode::Fast || C->HelpersLittle || C->Loader == LoaderMode::Little) {
    DetectCores(*C);
  }
  if (C->Loader != LoaderMode::Off) {
    StatsLogSetLineProvider(LoaderStatsLine);
  }
  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(GuardThread, C, {.LowPriority = false, .Internal = true}, "FEXPrioGuard");
}

void GuardThreadStarted(uint32_t TID, int LinuxTid, uint64_t Teb, const volatile uint64_t* BlockHeader, const volatile uint64_t* GameExceptions) {
  std::scoped_lock Lock {RegistryLock};
  const Registered R {.TID = TID, .LinuxTid = LinuxTid, .Teb = Teb, .BlockHeader = BlockHeader, .GameExceptions = GameExceptions};
  for (size_t i = 0; i < RegistryCount; ++i) {
    if (Registry[i].TID == TID) {
      Registry[i] = R;
      return;
    }
  }
  if (RegistryCount < MaxRegistered) {
    Registry[RegistryCount++] = R;
  }
}

void GuardThreadEnded(uint32_t TID) {
  std::scoped_lock Lock {RegistryLock};
  for (size_t i = 0; i < RegistryCount; ++i) {
    if (Registry[i].TID == TID) {
      Registry[i] = Registry[--RegistryCount];
      return;
    }
  }
}

void SetLoaderThreadHook(void (*Hook)(uint32_t TID, uint64_t Teb)) {
  LoaderHook.store(Hook, std::memory_order_release);
}

void SetJobWorkerSource(JobWorkerSource Source) {
  WorkerSource.store(Source, std::memory_order_release);
}
} // namespace FEX::Windows
