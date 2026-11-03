// SPDX-License-Identifier: MIT
#include "Windows/Common/LockBoost.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/Utils/WritePriorityMutex.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace FEX::Windows::LockBoost {
namespace {
  constexpr int RaisedNice = -20; // the main thread's
  constexpr uint32_t Kinds = 3;   // index = lock kind

  thread_local int CachedTid;

  // "/proc/self/task/<Tid>/<File>" into Out, without the C library (it can run inside an exception handler).
  void TaskPath(char (&Out)[64], int Tid, const char* File) {
    char* P = Out;
    for (const char* S = "/proc/self/task/"; *S;) {
      *P++ = *S++;
    }
    char Digits[12];
    int N = 0;
    unsigned Value = Tid > 0 ? static_cast<unsigned>(Tid) : 0;
    do {
      Digits[N++] = static_cast<char>('0' + Value % 10);
      Value /= 10;
    } while (Value && N < 11);
    while (N) {
      *P++ = Digits[--N];
    }
    *P++ = '/';
    while (*File && P < Out + 63) {
      *P++ = *File++;
    }
    *P = 0;
  }

  int SelfTid() {
    int Tid = CachedTid;
    if (!Tid) {
      Tid = Linux::GetTid();
      CachedTid = Tid;
    }
    return Tid;
  }

  // A thread's state letter and CPU from its stat file ('?' and -1 when it can't be read). Only raw Linux calls and the
  // stack: it runs in a thread that waits for a lock, which may be inside an exception handler.
  void ReadTaskState(int Tid, char& State, int& Cpu) {
    State = '?';
    Cpu = -1;
    char Path[64];
    TaskPath(Path, Tid, "stat");
    char Buffer[512];
    if (Linux::ReadSmallFile(Path, Buffer, sizeof(Buffer)) <= 0) {
      return;
    }
    // After the name in parentheses (which may hold spaces): field 3 is the state, field 39 the CPU it last ran on.
    const char* P = strrchr(Buffer, ')');
    if (!P || P[1] != ' ') {
      return;
    }
    P += 2;
    State = *P;
    for (int Field = 3; Field < 39 && P; ++Field) {
      P = strchr(P, ' ');
      if (P) {
        ++P;
      }
    }
    if (P && *P >= '0' && *P <= '9') {
      Cpu = 0;
      for (; *P >= '0' && *P <= '9'; ++P) {
        Cpu = Cpu * 10 + (*P - '0');
      }
    }
  }

  // What the waiter saw when it raised an owner, kept until that hold ends (one raise per lock at a time).
  struct Pending {
    std::atomic<uint64_t> Raised;
    std::atomic<uint64_t> HeldNs;
    std::atomic<int> Cpu;
    std::atomic<char> State;
  };

  struct KindStats {
    std::atomic<uint64_t> Raises;
    std::atomic<uint64_t> Failures;
    std::atomic<int> Error;
    std::atomic<uint64_t> HeldNs; // raised holds, from taking the lock to letting go
    // The longest raised hold since the stats log last took it.
    std::atomic<uint64_t> LongestNs;
    std::atomic<uint64_t> LongestRaised;
    std::atomic<uint64_t> LongestAtRaiseNs;
    std::atomic<int> LongestCpu;
    std::atomic<char> LongestState;
  };

  Pending Pendings[Kinds];
  KindStats Stats[Kinds];
  uint64_t AfterMs {};
  uint64_t ShownRaises[Kinds] {};
  uint64_t ShownFailures[Kinds] {};
  uint64_t ShownHeldNs[Kinds] {};

  int RaisedTid(uint64_t Raised) {
    return static_cast<int>(Raised >> 32);
  }
  int RaisedFromNice(uint64_t Raised) {
    return static_cast<int>(static_cast<uint32_t>(Raised)) - 64;
  }

  uint64_t Prepare(int Tid) {
    const int Nice = Linux::GetNice(Tid);
    if (Nice < -100 || Nice <= RaisedNice) {
      return 0;
    }
    return static_cast<uint64_t>(static_cast<uint32_t>(Tid)) << 32 | static_cast<uint32_t>(Nice + 64);
  }

  void Apply(uint64_t Raised, uint32_t Kind, uint64_t HeldNs) {
    const int Tid = RaisedTid(Raised);
    const int Result = Linux::SetNice(Tid, RaisedNice);
    auto& S = Stats[Kind < Kinds ? Kind : 0];
    if (Result != 0) {
      S.Failures.fetch_add(1, std::memory_order_relaxed);
      S.Error.store(Result, std::memory_order_relaxed);
      return;
    }
    S.Raises.fetch_add(1, std::memory_order_relaxed);
    char State;
    int Cpu;
    ReadTaskState(Tid, State, Cpu);
    auto& P = Pendings[Kind < Kinds ? Kind : 0];
    P.HeldNs.store(HeldNs, std::memory_order_relaxed);
    P.Cpu.store(Cpu, std::memory_order_relaxed);
    P.State.store(State, std::memory_order_relaxed);
    P.Raised.store(Raised, std::memory_order_release);
  }

  void Restore(uint64_t Raised, uint32_t Kind, uint64_t HeldNs) {
    Linux::SetNice(RaisedTid(Raised), RaisedFromNice(Raised));
    if (!HeldNs) {
      return;
    }
    auto& S = Stats[Kind < Kinds ? Kind : 0];
    auto& P = Pendings[Kind < Kinds ? Kind : 0];
    S.HeldNs.fetch_add(HeldNs, std::memory_order_relaxed);
    if (HeldNs > S.LongestNs.load(std::memory_order_relaxed) && P.Raised.load(std::memory_order_acquire) == Raised) {
      S.LongestNs.store(HeldNs, std::memory_order_relaxed);
      S.LongestRaised.store(Raised, std::memory_order_relaxed);
      S.LongestAtRaiseNs.store(P.HeldNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
      S.LongestCpu.store(P.Cpu.load(std::memory_order_relaxed), std::memory_order_relaxed);
      S.LongestState.store(P.State.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }

  FEXCore::Utils::WritePriorityMutex::LockBoostHooks Hooks {
    .SelfTid = SelfTid,
    .Prepare = Prepare,
    .Apply = Apply,
    .Restore = Restore,
  };
} // namespace

bool Init() {
  const char* Value = Switch("FEX_LOCK_BOOST");
  AfterMs = Value ? std::min<uint64_t>(strtoull(Value, nullptr, 10), 1000) : 0;
  if (!AfterMs || !FEXCore::Utils::WritePriorityMutex::KernelFutex) {
    return false;
  }
  Hooks.AfterNs = AfterMs * 1'000'000;
  FEXCore::Utils::WritePriorityMutex::LockBoost = &Hooks;
  return true;
}

fextl::string Note() {
  if (!FEXCore::Utils::WritePriorityMutex::LockBoost) {
    return fextl::fmt::format("lock boost: off (FEX_LOCK_BOOST={}{})", AfterMs,
                              FEXCore::Utils::WritePriorityMutex::KernelFutex ? "" : ", needs FEX_KERNEL_FUTEX=1");
  }
  return fextl::fmt::format("lock boost: a thread that holds FEX's code lock or memory lock {} ms or more while another thread "
                            "waits for it runs at nice {} until it lets go, then at its own nice again (build 156; "
                            "'lock boost' line in seconds with a raise: holder, how long it held the lock, and its state and CPU "
                            "when raised, R = ready or running, S/D = blocked; FEX_LOCK_BOOST=0 turns it off)",
                            AfterMs, RaisedNice);
}

fextl::string ThreadName(int Tid) {
  char Path[64];
  TaskPath(Path, Tid, "comm");
  char Buffer[64];
  if (Linux::ReadSmallFile(Path, Buffer, sizeof(Buffer)) <= 0) {
    return "?";
  }
  if (char* End = strchr(Buffer, '\n')) {
    *End = 0;
  }
  return Buffer[0] ? fextl::string(Buffer) : fextl::string("?");
}

fextl::string Line() {
  if (!FEXCore::Utils::WritePriorityMutex::LockBoost) {
    return {};
  }
  constexpr const char* Names[Kinds] = {"?", "code lock", "memory lock"};
  fextl::string Out;
  for (uint32_t K = 1; K < Kinds; ++K) {
    auto& S = Stats[K];
    const uint64_t Raises = S.Raises.load(std::memory_order_relaxed);
    const uint64_t Failures = S.Failures.load(std::memory_order_relaxed);
    const uint64_t HeldNs = S.HeldNs.load(std::memory_order_relaxed);
    const uint64_t NewRaises = Raises - ShownRaises[K];
    const uint64_t NewFailures = Failures - ShownFailures[K];
    const uint64_t NewHeldNs = HeldNs - ShownHeldNs[K];
    ShownRaises[K] = Raises;
    ShownFailures[K] = Failures;
    ShownHeldNs[K] = HeldNs;
    const uint64_t LongestNs = S.LongestNs.exchange(0, std::memory_order_relaxed);
    if (!NewRaises && !NewFailures && !LongestNs) {
      continue;
    }
    Out += Out.empty() ? fextl::fmt::format("  lock boost (after {} ms):", AfterMs) : fextl::string(" |");
    Out += fextl::fmt::format(" {} raised {} holders ({} times in all), their holds {:.1f} ms", Names[K], NewRaises, Raises,
                              static_cast<double>(NewHeldNs) / 1e6);
    if (LongestNs) {
      const uint64_t R = S.LongestRaised.load(std::memory_order_relaxed);
      const int Tid = RaisedTid(R);
      const int Cpu = S.LongestCpu.load(std::memory_order_relaxed);
      Out += fextl::fmt::format(", longest {:.1f} ms by {} {} (when raised after {:.1f} ms: {} on cpu {}, nice {})",
                                static_cast<double>(LongestNs) / 1e6, Tid, ThreadName(Tid),
                                static_cast<double>(S.LongestAtRaiseNs.load(std::memory_order_relaxed)) / 1e6,
                                S.LongestState.load(std::memory_order_relaxed), Cpu, RaisedFromNice(R));
    }
    if (NewFailures) {
      Out += fextl::fmt::format(", {} raises failed (error {})", NewFailures, -S.Error.load(std::memory_order_relaxed));
    }
  }
  return Out.empty() ? Out : Out + "\n";
}
} // namespace FEX::Windows::LockBoost
