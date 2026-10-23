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
#include <cstdlib>
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

  struct Config {
    PrioMode Prio;
    CoreMode Cores;
    bool HelpersLittle;
    bool ServerNiceOn;
    int ServerNice;
    uint64_t FastMask;   // the CPU(s) with the highest maximum clock, 0 when all are alike or unknown
    uint64_t LittleMask; // the CPU(s) with the lowest maximum clock, 0 when all are alike or unknown
    uint32_t MainTID;    // the thread that started the process (FEX's process init runs on it)
  };

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
      C.FastMask = C.LittleMask = 0;
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
          // Only the stats log (its 2ms sampler and per-second work) and this guard move: the stall probe measures what
          // game threads see, and the disk cache writer, prefetcher and unpack cache threads hold locks or feed loads the
          // game waits on.
          T.KeepCores = !(T.Name == "FEXStatsLog" || T.Name == "FEXPrioGuard" || T.Name == "FEXThreadPin" || T.Name == "FEXFrameLatency");
        }
        Entries.push_back({Next, TID, Basic, &T, TID == C->MainTID || T.Name == "Main"});
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

      LARGE_INTEGER Interval {.QuadPart = -20'000'000}; // 2s
      NtDelayExecution(FALSE, &Interval);
    }
    return nullptr;
  }
} // namespace

void StartPriorityGuard(std::string_view AppName) {
  auto* C = new Config {};
  C->MainTID = GetCurrentThreadId();

  const char* Prio = Switch("FEX_PRIO");
  C->Prio = !Prio || Prio[0] == 'k' || Prio[0] == '0' ? PrioMode::Keep : Prio[0] == 'n' ? PrioMode::Normal : PrioMode::Clamp;

  const char* Cores = Switch("FEX_MAINCORE");
  C->Cores = !Cores || Cores[0] == 'o' || Cores[0] == '0' ? CoreMode::Off : Cores[0] == 'b' ? CoreMode::Big : CoreMode::Fast;

  const char* Helpers = Switch("FEX_HELPER_CORES");
  C->HelpersLittle = Helpers && Helpers[0] == 'l';

  const char* ServerNice = Switch("FEX_SERVER_NICE");
  if (ServerNice && ServerNice[0] != 'o' && (ServerNice[0] == '-' || (ServerNice[0] >= '0' && ServerNice[0] <= '9'))) {
    C->ServerNiceOn = true;
    C->ServerNice = std::clamp(atoi(ServerNice), -20, 19);
  }

  StatsLogNote(C->Prio == PrioMode::Keep ? fextl::fmt::format("prio: off, Windows thread priorities pass through to Linux nice "
                                                               "(FEX_PRIO=normal keeps every thread at nice 0)") :
                                           fextl::fmt::format("prio: {} in {}, checked every 2s (FEX_PRIO=keep turns it off)",
                                                              C->Prio == PrioMode::Normal ? "every thread at NORMAL" :
                                                                                            "base priorities kept to BELOW_NORMAL..ABOVE_NORMAL",
                                                              AppName));
  StatsLogNote(fextl::fmt::format("cores: main thread (tid {}) {}; FEX helpers {} (FEX_MAINCORE=off, FEX_HELPER_CORES=off turn them off)",
                                  C->MainTID,
                                  C->Cores == CoreMode::Fast ? "alone on the fastest core" :
                                  C->Cores == CoreMode::Big  ? "on the game's cores" :
                                                               "left alone",
                                  C->HelpersLittle ? "on the slowest cores" : "left alone"));
  StatsLogNote(C->ServerNiceOn ? fextl::fmt::format("server: wineserver to nice {} (FEX_SERVER_NICE=off leaves it alone)", C->ServerNice) :
                                 fextl::string("server: wineserver nice left alone"));

  if (C->Prio == PrioMode::Keep && C->Cores == CoreMode::Off && !C->HelpersLittle && !C->ServerNiceOn) {
    delete C;
    return;
  }
  if (C->Cores == CoreMode::Fast || C->HelpersLittle) {
    DetectCores(*C);
  }
  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(GuardThread, C, {.LowPriority = false, .Internal = true}, "FEXPrioGuard");
}
} // namespace FEX::Windows
