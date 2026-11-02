// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Follows the game's pipe reads and writes (Steam's per-frame round trip) for the stats log
$end_info$
*/

#include "PipeWatch.h"
#include "FrameLatency.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>

extern "C" {
// Module.S ExitFunctionEC: while [0] is non-zero, a call from x64 code into one of the WriteFile entries [0..2] runs
// FEXNativeWriteFile (NativeWriteFileTarget) instead, after storing the entry it called in [3]. Entries not found repeat
// [0].
alignas(64) uint64_t FEXWriteWatch[4];
BOOL FEXNativeWriteFile(HANDLE File, const void* Buffer, DWORD Bytes, DWORD* Written, uint64_t Lr, uint64_t Sp);
extern void* X64ReturnInstr; // Module.cpp
}

namespace PipeWatch {
namespace {
  namespace Linux = FEX::Windows::Linux;

  constexpr size_t SlotCount = 8;  // handles the main thread read or wrote that the game files watch doesn't follow
  constexpr size_t KindCount = 8;  // message kinds (size and first 8 bytes) kept per handle and direction each second
  constexpr size_t TebLastError = 0x68;
  constexpr uint32_t ErrorIoPending = 997;
  constexpr size_t MaxPeerThreads = 256;
  constexpr uint32_t SystemProcessInformationClass = 5;

  enum : uint32_t { StateUnknown = 0, StatePipe = 1, StateNotPipe = 2 };

  struct X64Overlapped {
    uint64_t Internal;
    uint64_t InternalHigh;
    uint32_t Offset;
    uint32_t OffsetHigh;
    uint64_t Event;
  };

  struct Direction {
    std::atomic<uint64_t> MainCalls {};
    std::atomic<uint64_t> MainBytes {};
    std::atomic<uint64_t> MainTicks {};
    std::atomic<uint64_t> MainLongest {};
    std::atomic<uint64_t> OtherCalls {};
    std::atomic<uint64_t> OtherBytes {};
    std::atomic<uint64_t> OtherTicks {};
  };

  struct Kind {
    uint64_t Head; // the first 8 bytes (fewer for a shorter message, the rest 0)
    uint32_t Size;
    uint32_t Count;
  };

  struct KindTable {
    Kind Kinds[KindCount];
    uint32_t Other; // messages of further kinds
  };

  struct Slot {
    std::atomic<uint64_t> Handle {}; // 0 = free; set by the main thread only
    std::atomic<uint32_t> State {};
    std::atomic<uint64_t> LastUse {}; // counter value
    Direction Read;
    Direction Write;
    // The main thread's messages, under KindLock.
    KindTable ReadKinds;
    KindTable WriteKinds;
    // The log thread's own.
    char Name[128];
    uint64_t NamedAt;
    uint32_t CreatorPid; // from an anonymous pipe's name, 0 if none
  };

  struct PeerThread {
    int Tid;
    uint64_t RunNs;
    uint64_t WaitNs;
    uint64_t Slices;
    bool Seen;
  };

  bool Active {};
  bool Armed {};
  uint32_t ArmTries {};
  uint64_t MainTeb {};
  uint64_t CounterFreq {1};
  Slot Slots[SlotCount];
  std::atomic<uint64_t> Unslotted {}; // main thread calls on handles with no free slot
  std::atomic_flag KindLock = ATOMIC_FLAG_INIT;

  // The log thread's.
  bool ProcessesListed {};
  uint32_t PeerWindowsPid {};
  char PeerName[64] {};
  int PeerLinuxPid {};
  uint32_t PeerLookups {};
  PeerThread PeerThreads[MaxPeerThreads] {};
  size_t PeerThreadCount {};

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

  void LockKinds() {
    while (KindLock.test_and_set(std::memory_order_acquire)) {
      __asm volatile("yield");
    }
  }

  void UnlockKinds() {
    KindLock.clear(std::memory_order_release);
  }

  Slot* FindSlot(uint64_t Handle) {
    for (auto& S : Slots) {
      if (S.Handle.load(std::memory_order_acquire) == Handle) {
        return &S;
      }
    }
    return nullptr;
  }

  // Main thread only: a free slot for a handle not seen yet (the log thread names it).
  Slot* ClaimSlot(uint64_t Handle) {
    for (auto& S : Slots) {
      if (S.Handle.load(std::memory_order_acquire) == 0) {
        S.State.store(StateUnknown, std::memory_order_relaxed);
        S.LastUse.store(GetCounter(), std::memory_order_relaxed); // before the handle: the log thread frees idle slots
        S.Handle.store(Handle, std::memory_order_release);
        return &S;
      }
    }
    return nullptr;
  }

  void AddKind(KindTable& T, const void* Buffer, uint64_t Bytes) {
    uint64_t Head {};
    if (Buffer && Bytes) {
      memcpy(&Head, Buffer, std::min<uint64_t>(Bytes, sizeof(Head)));
    }
    const uint32_t Size = static_cast<uint32_t>(std::min<uint64_t>(Bytes, 0xffffffffu));
    LockKinds();
    bool Done = false;
    for (auto& K : T.Kinds) {
      if (K.Count && K.Head == Head && K.Size == Size) {
        ++K.Count;
        Done = true;
        break;
      }
      if (!K.Count) {
        K = {Head, Size, 1};
        Done = true;
        break;
      }
    }
    if (!Done) {
      ++T.Other;
    }
    UnlockKinds();
  }

  void Note(bool IsWrite, uint64_t Handle, const void* Buffer, uint64_t Bytes, bool HaveData, uint64_t Ticks, bool Main) {
    Slot* S = FindSlot(Handle);
    if (!S) {
      if (!Main) {
        return; // other threads only add to handles the main thread uses
      }
      S = ClaimSlot(Handle);
      if (!S) {
        Unslotted.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    S->LastUse.store(GetCounter(), std::memory_order_relaxed);
    auto& D = IsWrite ? S->Write : S->Read;
    if (Main) {
      D.MainCalls.fetch_add(1, std::memory_order_relaxed);
      D.MainBytes.fetch_add(Bytes, std::memory_order_relaxed);
      D.MainTicks.fetch_add(Ticks, std::memory_order_relaxed);
      uint64_t Longest = D.MainLongest.load(std::memory_order_relaxed);
      while (Ticks > Longest && !D.MainLongest.compare_exchange_weak(Longest, Ticks, std::memory_order_relaxed)) {
      }
      if (S->State.load(std::memory_order_relaxed) != StateNotPipe) {
        AddKind(IsWrite ? S->WriteKinds : S->ReadKinds, HaveData ? Buffer : nullptr, Bytes);
      }
    } else {
      D.OtherCalls.fetch_add(1, std::memory_order_relaxed);
      D.OtherBytes.fetch_add(Bytes, std::memory_order_relaxed);
      D.OtherTicks.fetch_add(Ticks, std::memory_order_relaxed);
    }
  }

  // The end of a handle's object name, for the log (as GameFiles.cpp's HandleName).
  void HandleName(HANDLE File, char* Out, size_t Size) {
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 520 * sizeof(WCHAR)] {};
    ULONG Length {};
    if (NtQueryObject(File, ObjectNameInformation, Buffer, sizeof(Buffer), &Length) != STATUS_SUCCESS) {
      snprintf(Out, Size, "-");
      return;
    }
    const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
    const size_t Chars = Name->Buffer ? Name->Length / sizeof(WCHAR) : 0;
    const size_t From = Chars > Size - 1 ? Chars - (Size - 1) : 0;
    size_t n = 0;
    for (size_t i = From; i < Chars && n + 1 < Size; ++i) {
      const WCHAR Ch = Name->Buffer[i];
      Out[n++] = Ch >= 0x20 && Ch < 0x80 ? static_cast<char>(Ch) : '?';
    }
    Out[n] = 0;
    if (!n) {
      snprintf(Out, Size, "?");
    }
  }

  // Wine's CreatePipe names a pipe "Win32.Pipes.%08lu.%08u" (the id of the process that made it, in decimal, and a count).
  uint32_t CreatorOf(const char* Name) {
    const char* P = strstr(Name, "Win32.Pipes.");
    if (!P) {
      return 0;
    }
    return static_cast<uint32_t>(strtoul(P + 12, nullptr, 10));
  }

  const char* ShortName(const char* Name) {
    const char* P = strstr(Name, "\\Device\\NamedPipe\\");
    return P ? P + 18 : Name;
  }

  bool Arm() {
    const wchar_t* Modules[] = {L"kernelbase.dll", L"kernel32.dll"};
    uint64_t Found[3] {};
    size_t Count {};
    for (const auto* Name : Modules) {
      const uint64_t Base = FrameLatency::FindModule(Name);
      uint64_t Entries[3] {};
      const size_t N = Base ? FrameLatency::FindExportEntries(Base, "WriteFile", Entries) : 0;
      for (size_t i = 0; i < N && Count < std::size(Found); ++i) {
        if (std::find(Found, Found + Count, Entries[i]) == Found + Count) {
          Found[Count++] = Entries[i];
        }
      }
    }
    if (!Count) {
      return false;
    }
    // [0] last: it switches the check on.
    __atomic_store_n(&FEXWriteWatch[2], Found[Count > 2 ? 2 : 0], __ATOMIC_RELAXED);
    __atomic_store_n(&FEXWriteWatch[1], Found[Count > 1 ? 1 : 0], __ATOMIC_RELAXED);
    __atomic_store_n(&FEXWriteWatch[0], Found[0], __ATOMIC_RELEASE);
    fextl::string Text;
    for (size_t i = 0; i < Count; ++i) {
      Text += fextl::fmt::format(" {:X}", Found[i]);
    }
    FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: WriteFile found (entries{}); the game's writes are now followed too", Text));
    return true;
  }

  // The Windows processes (NtQuerySystemInformation), written once as a note; finds the pipe's maker and its Linux pid.
  void ListProcesses(uint32_t Wanted) {
    fextl::vector<uint8_t> Buffer(256 * 1024);
    ULONG Needed {};
    NTSTATUS Status = NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(SystemProcessInformationClass), Buffer.data(),
                                               static_cast<ULONG>(Buffer.size()), &Needed);
    if (Status == STATUS_INFO_LENGTH_MISMATCH && Needed > Buffer.size() && Needed < (16u << 20)) {
      Buffer.resize(Needed + 64 * 1024);
      Status = NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(SystemProcessInformationClass), Buffer.data(),
                                        static_cast<ULONG>(Buffer.size()), &Needed);
    }
    if (Status != STATUS_SUCCESS) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: the list of Windows processes couldn't be read (status {:#x})",
                                                    static_cast<uint32_t>(Status)));
      return;
    }
    // x64 SYSTEM_PROCESS_INFORMATION: NextEntryOffset +0, thread count +4, ImageName +0x38 (UNICODE_STRING), process
    // id +0x50, parent id +0x58.
    fextl::string List;
    size_t Processes {};
    char WantedName[64] {};
    uint32_t WantedParent {};
    uint32_t WantedThreads {};
    for (size_t Offset = 0; Offset + 0x60 <= Buffer.size();) {
      const uint8_t* E = Buffer.data() + Offset;
      uint32_t Next {}, Threads {};
      uint64_t Pid {}, Parent {};
      UNICODE_STRING Image {};
      memcpy(&Next, E, 4);
      memcpy(&Threads, E + 4, 4);
      memcpy(&Image, E + 0x38, sizeof(Image));
      memcpy(&Pid, E + 0x50, 8);
      memcpy(&Parent, E + 0x58, 8);
      char Name[64] {};
      const size_t Chars = Image.Buffer ? std::min<size_t>(Image.Length / sizeof(WCHAR), sizeof(Name) - 1) : 0;
      for (size_t i = 0; i < Chars; ++i) {
        const WCHAR Ch = Image.Buffer[i];
        Name[i] = Ch >= 0x20 && Ch < 0x80 ? static_cast<char>(Ch) : '?';
      }
      if (!Chars) {
        snprintf(Name, sizeof(Name), "%s", Pid ? "?" : "idle");
      }
      List += fextl::fmt::format("{}{} {} (parent {}, {} threads)", Processes ? ", " : "", Pid, Name, Parent, Threads);
      ++Processes;
      if (Wanted && Pid == Wanted) {
        memcpy(WantedName, Name, sizeof(WantedName));
        WantedParent = static_cast<uint32_t>(Parent);
        WantedThreads = Threads;
      }
      if (!Next) {
        break;
      }
      Offset += Next;
    }
    FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: Windows processes (id name): {}", List));
    if (!Wanted) {
      return;
    }
    if (!WantedName[0]) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: Windows process {} (the pipe's maker) isn't running any more", Wanted));
      return;
    }
    PeerWindowsPid = Wanted;
    memcpy(PeerName, WantedName, sizeof(PeerName));
    PeerLinuxPid = Linux::FindProcess(PeerName);
    FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: the pipe's maker is Windows process {} {} (parent {}, {} threads), Linux pid {}; "
                                                  "its threads are on the 'pipe peer' line",
                                                  Wanted, WantedName, WantedParent, WantedThreads, PeerLinuxPid));
  }

  void NameSlots(uint64_t Now) {
    for (auto& S : Slots) {
      const uint64_t Handle = S.Handle.load(std::memory_order_acquire);
      if (!Handle) {
        continue;
      }
      const uint32_t State = S.State.load(std::memory_order_relaxed);
      // A handle unused for 10 s may have been closed (its number can come back for another file): free it.
      if (Now - S.LastUse.load(std::memory_order_relaxed) > 10 * CounterFreq) {
        S.Handle.store(0, std::memory_order_release);
        continue;
      }
      if (State != StateUnknown && Now - S.NamedAt < 10 * CounterFreq) {
        continue;
      }
      char Name[sizeof(S.Name)];
      HandleName(reinterpret_cast<HANDLE>(Handle), Name, sizeof(Name));
      S.NamedAt = Now;
      if (State != StateUnknown && strcmp(Name, S.Name) == 0) {
        continue;
      }
      memcpy(S.Name, Name, sizeof(Name));
      const bool Pipe = strstr(Name, "\\Device\\NamedPipe\\") != nullptr;
      S.CreatorPid = Pipe ? CreatorOf(Name) : 0;
      S.State.store(Pipe ? StatePipe : StateNotPipe, std::memory_order_relaxed);
      if (!Pipe) {
        continue;
      }
      FEX::Windows::StatsLogNote(fextl::fmt::format("pipe watch: handle {:#x} the main thread uses is {}{}", Handle, Name,
                                                    S.CreatorPid ? fextl::fmt::format(" (an anonymous pipe made by Windows process {})", S.CreatorPid) :
                                                                   fextl::string()));
      if (S.CreatorPid && !ProcessesListed) {
        ProcessesListed = true;
        ListProcesses(S.CreatorPid);
      }
    }
    // steam.exe may not be found by name at once (a wrapper's comm): look again now and then.
    if (PeerName[0] && !PeerLinuxPid && PeerLookups < 10 && (++PeerLookups % 2) == 0) {
      PeerLinuxPid = Linux::FindProcess(PeerName);
    }
  }

  fextl::string KindsText(KindTable& T) {
    Kind Copy[KindCount];
    LockKinds();
    memcpy(Copy, T.Kinds, sizeof(Copy));
    const uint32_t Other = T.Other;
    memset(T.Kinds, 0, sizeof(T.Kinds));
    T.Other = 0;
    UnlockKinds();
    std::sort(std::begin(Copy), std::end(Copy), [](const Kind& a, const Kind& b) { return a.Count > b.Count; });
    fextl::string Out;
    for (const auto& K : Copy) {
      if (!K.Count) {
        break;
      }
      Out += fextl::fmt::format("{}{} B", Out.empty() ? "" : ", ", K.Size);
      if (K.Size) {
        Out += ' ';
        for (uint32_t i = 0; i < std::min<uint32_t>(K.Size, 8); ++i) {
          Out += fextl::fmt::format("{:02x}", static_cast<uint32_t>((K.Head >> (8 * i)) & 0xff));
        }
      }
      Out += fextl::fmt::format(" x{}", K.Count);
    }
    if (Other) {
      Out += fextl::fmt::format(", {} more of other kinds", Other);
    }
    return Out.empty() ? fextl::string("-") : Out;
  }

  struct DirTotals {
    uint64_t MainCalls, MainBytes, MainTicks, MainLongest, OtherCalls, OtherBytes, OtherTicks;
  };

  DirTotals Take(Direction& D) {
    return {D.MainCalls.exchange(0, std::memory_order_relaxed),   D.MainBytes.exchange(0, std::memory_order_relaxed),
            D.MainTicks.exchange(0, std::memory_order_relaxed),   D.MainLongest.exchange(0, std::memory_order_relaxed),
            D.OtherCalls.exchange(0, std::memory_order_relaxed),  D.OtherBytes.exchange(0, std::memory_order_relaxed),
            D.OtherTicks.exchange(0, std::memory_order_relaxed)};
  }

  fextl::string PeerLine() {
    if (!PeerLinuxPid) {
      return {};
    }
    struct Gather {
      int Tids[MaxPeerThreads];
      size_t Count;
    } G {};
    const auto TaskDir = fextl::fmt::format("/proc/{}/task", PeerLinuxPid);
    if (!Linux::ForEachEntry(
          TaskDir.c_str(),
          [](const char* Name, void* Arg) {
            auto* G = static_cast<Gather*>(Arg);
            if (G->Count < MaxPeerThreads) {
              G->Tids[G->Count++] = atoi(Name);
            }
          },
          &G)) {
      PeerLinuxPid = 0; // it ended
      return fextl::fmt::format("  pipe peer {} (Windows pid {}): ended\n", PeerName, PeerWindowsPid);
    }
    for (size_t i = 0; i < PeerThreadCount; ++i) {
      PeerThreads[i].Seen = false;
    }
    struct Busy {
      int Tid;
      uint64_t RunNs, WaitNs;
      char Comm[17];
      int Processor, Nice;
    } Top[3] {};
    uint64_t RunNs {}, WaitNs {}, Slices {};
    size_t New {};
    for (size_t i = 0; i < G.Count; ++i) {
      const int Tid = G.Tids[i];
      Linux::SchedStat Sched {};
      if (Tid <= 0 || !Linux::ReadSchedStat(fextl::fmt::format("/proc/{}/task/{}/schedstat", PeerLinuxPid, Tid).c_str(), Sched)) {
        continue;
      }
      PeerThread* P = nullptr;
      for (size_t j = 0; j < PeerThreadCount; ++j) {
        if (PeerThreads[j].Tid == Tid) {
          P = &PeerThreads[j];
          break;
        }
      }
      if (!P) {
        // New this second (or the first look): no time before to count from.
        if (PeerThreadCount < MaxPeerThreads) {
          PeerThreads[PeerThreadCount++] = {Tid, Sched.RunNs, Sched.WaitNs, Sched.Slices, true};
        }
        ++New;
        continue;
      }
      const uint64_t Run = Sched.RunNs - P->RunNs, Wait = Sched.WaitNs - P->WaitNs;
      RunNs += Run;
      WaitNs += Wait;
      Slices += Sched.Slices - P->Slices;
      *P = {Tid, Sched.RunNs, Sched.WaitNs, Sched.Slices, true};
      // The three busiest (run + wait) get their name, CPU and nice.
      size_t Pos = 3;
      for (size_t k = 0; k < 3; ++k) {
        if (Run + Wait > Top[k].RunNs + Top[k].WaitNs) {
          Pos = k;
          break;
        }
      }
      if (Pos < 3 && Run + Wait > 0) {
        for (size_t k = 2; k > Pos; --k) {
          Top[k] = Top[k - 1];
        }
        Top[Pos] = {Tid, Run, Wait, {}, -1, 0};
      }
    }
    // Drop threads that ended.
    size_t Kept {};
    for (size_t i = 0; i < PeerThreadCount; ++i) {
      if (PeerThreads[i].Seen) {
        PeerThreads[Kept++] = PeerThreads[i];
      }
    }
    PeerThreadCount = Kept;
    fextl::string TopText;
    for (auto& T : Top) {
      if (!T.Tid) {
        break;
      }
      Linux::TaskStat Stat {};
      if (Linux::ReadTaskStat(fextl::fmt::format("/proc/{}/task/{}/stat", PeerLinuxPid, T.Tid).c_str(), Stat)) {
        memcpy(T.Comm, Stat.Comm, sizeof(T.Comm));
        T.Processor = Stat.Processor;
        T.Nice = Stat.Nice;
      }
      TopText += fextl::fmt::format(" {}/{} {:.1f}+{:.1f}@{} n{}", T.Comm[0] ? T.Comm : "?", T.Tid, static_cast<double>(T.RunNs) / 1e6,
                                    static_cast<double>(T.WaitNs) / 1e6, T.Processor, T.Nice);
    }
    return fextl::fmt::format("  pipe peer {} (Windows pid {}, Linux {}): {} threads ran {:.1f} ms, waited {:.1f} ms for a CPU, {} runs "
                              "({:.3f} ms waited a run){} | busiest (name/tid run+wait ms @last cpu, nice):{}\n",
                              PeerName, PeerWindowsPid, PeerLinuxPid, G.Count, static_cast<double>(RunNs) / 1e6,
                              static_cast<double>(WaitNs) / 1e6, Slices, Slices ? static_cast<double>(WaitNs) / 1e6 / static_cast<double>(Slices) : 0.0,
                              New ? fextl::fmt::format(", {} new", New) : fextl::string(), TopText.empty() ? fextl::string(" -") : TopText);
  }

  fextl::string StatsLine() {
    const uint64_t Start = GetCounter();
    if (!Armed && ArmTries < 120) {
      ++ArmTries;
      Armed = Arm();
    }
    NameSlots(Start);
    const double MsPerTick = 1000.0 / static_cast<double>(CounterFreq);
    fextl::string Out;
    size_t OtherHandles {};
    uint64_t OtherHandleCalls {};
    for (auto& S : Slots) {
      const uint64_t Handle = S.Handle.load(std::memory_order_acquire);
      const DirTotals R = Take(S.Read);
      const DirTotals W = Take(S.Write);
      if (!Handle) {
        continue;
      }
      const uint32_t State = S.State.load(std::memory_order_relaxed);
      if (State != StatePipe) {
        // Not a pipe (or not named yet): only counted.
        if (R.MainCalls + W.MainCalls) {
          ++OtherHandles;
          OtherHandleCalls += R.MainCalls + W.MainCalls;
        }
        if (State == StateNotPipe) {
          LockKinds();
          memset(&S.ReadKinds, 0, sizeof(S.ReadKinds));
          memset(&S.WriteKinds, 0, sizeof(S.WriteKinds));
          UnlockKinds();
        }
        continue;
      }
      const fextl::string ReadKinds = KindsText(S.ReadKinds);
      const fextl::string WriteKinds = KindsText(S.WriteKinds);
      if (!R.MainCalls && !W.MainCalls && !R.OtherCalls && !W.OtherCalls) {
        continue;
      }
      Out += fextl::fmt::format("  pipe watch {}{}: main reads {} {:.1f} KB {:.1f} ms (longest {:.2f}), writes {} {:.1f} KB {:.1f} ms (longest "
                                "{:.2f}) | other threads reads {} {:.1f} KB {:.1f} ms, writes {} {:.1f} KB {:.1f} ms | main reads by size "
                                "and first bytes: {} | main writes: {}\n",
                                ShortName(S.Name), S.CreatorPid == PeerWindowsPid && PeerName[0] ? fextl::fmt::format(" ({}'s)", PeerName) : fextl::string(),
                                R.MainCalls, static_cast<double>(R.MainBytes) / 1024.0, static_cast<double>(R.MainTicks) * MsPerTick,
                                static_cast<double>(R.MainLongest) * MsPerTick, W.MainCalls, static_cast<double>(W.MainBytes) / 1024.0,
                                static_cast<double>(W.MainTicks) * MsPerTick, static_cast<double>(W.MainLongest) * MsPerTick, R.OtherCalls,
                                static_cast<double>(R.OtherBytes) / 1024.0, static_cast<double>(R.OtherTicks) * MsPerTick, W.OtherCalls,
                                static_cast<double>(W.OtherBytes) / 1024.0, static_cast<double>(W.OtherTicks) * MsPerTick, ReadKinds, WriteKinds);
    }
    const uint64_t NoSlot = Unslotted.exchange(0, std::memory_order_relaxed);
    if (Out.empty() && !OtherHandleCalls && !NoSlot) {
      return {};
    }
    Out += PeerLine();
    if (OtherHandleCalls || NoSlot) {
      Out += fextl::fmt::format("  pipe watch: main thread's other reads/writes (not pipes, or not named yet) {} on {} handles, {} on handles "
                                "with no slot\n",
                                OtherHandleCalls, OtherHandles, NoSlot);
    }
    Out += fextl::fmt::format("  pipe watch work {:.2f} ms\n", static_cast<double>(GetCounter() - Start) * MsPerTick);
    return Out;
  }
} // namespace

void Init(bool StatsLog) {
  if (!StatsLog) {
    return;
  }
  const char* Value = FEX::Windows::Switch("FEX_PIPE_WATCH");
  if (!Value || Value[0] != '1') {
    return;
  }
  MainTeb = reinterpret_cast<uint64_t>(NtCurrentTeb());
  CounterFreq = GetCounterFrequency();
  Active = true;
  FEX::Windows::StatsLogNote(
    "pipe watch (build 146): the main thread's pipe reads and writes are counted by pipe, with each message's size and first 8 "
    "bytes ('pipe watch' line), the process that made the pipe is named from the list of Windows processes, and its threads' "
    "time on a CPU and waiting for one is on the 'pipe peer' line (build 143: the main thread read a Steam pipe twice a frame "
    "and waited ~0.84 ms a frame on Steam; FEX_PIPE_WATCH=0 turns this off)");
  FEX::Windows::StatsLogSetLineProvider(StatsLine);
}

void NoteRead(void* File, const void* Buffer, uint64_t Bytes, bool Queued, uint64_t Ticks, bool Main) {
  if (!Active) {
    return;
  }
  Note(false, reinterpret_cast<uint64_t>(File), Buffer, Bytes, !Queued, Ticks, Main);
}
} // namespace PipeWatch

// The game's WriteFile, through Module.S (see FEXWriteWatch). Lr and Sp locate the x64 caller's stack: the fifth argument
// (the OVERLAPPED) is above the 32 bytes of home space, past the return address when Module.S left it on the stack (lr is
// then X64ReturnInstr).
extern "C" BOOL FEXNativeWriteFile(HANDLE File, const void* Buffer, DWORD Bytes, DWORD* Written, uint64_t Lr, uint64_t Sp) {
  using namespace PipeWatch;
  using WriteFileFn = BOOL (*)(HANDLE, const void*, DWORD, DWORD*, void*);
  const auto Real = reinterpret_cast<WriteFileFn>(__atomic_load_n(&FEXWriteWatch[3], __ATOMIC_ACQUIRE));
  const bool ReturnOnStack = Lr == reinterpret_cast<uint64_t>(X64ReturnInstr);
  auto* Overlapped = *reinterpret_cast<X64Overlapped* const*>(Sp + (ReturnOnStack ? 0x28 : 0x20));
  const uint64_t Start = GetCounter();
  const BOOL Result = Real(File, Buffer, Bytes, Written, Overlapped);
  const uint64_t End = GetCounter();
  // Nothing below may change what the game sees: the last error stays as WriteFile left it.
  auto* LastError = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + TebLastError);
  const uint32_t Error = *LastError;
  const bool Queued = !Result && Error == ErrorIoPending;
  if (Active && (Result || Queued) && Bytes) {
    const bool Main = reinterpret_cast<uint64_t>(NtCurrentTeb()) == MainTeb;
    Note(true, reinterpret_cast<uint64_t>(File), Buffer, Bytes, true, End - Start, Main);
  }
  *LastError = Error;
  return Result;
}
