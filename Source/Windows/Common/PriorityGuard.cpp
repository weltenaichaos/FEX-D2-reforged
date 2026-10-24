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
    int MainLinuxTid;    // its Linux tid
    uint32_t EaseWaitMs; // FEX_MAINCORE_WAIT, 0 = never let the main thread off its core
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
    Keeper.StatusPath = fextl::fmt::format("/proc/self/task/{}/status", Keeper.Tid);
    uint64_t KeepFast {}, KeepWide {};

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
          // Only the stats log (its 2ms sampler and per-second work), this guard, the rumble sender and the file cache dropper move: the stall
          // probe measures what game threads see, and the disk cache writer, prefetcher and unpack cache threads hold locks
          // or feed loads the game waits on.
          T.KeepCores = !(T.Name == "FEXStatsLog" || T.Name == "FEXPrioGuard" || T.Name == "FEXThreadPin" || T.Name == "FEXFrameLatency" ||
                          T.Name == "FEXRumble" || T.Name == "FEXFileCache");
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
  }
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
