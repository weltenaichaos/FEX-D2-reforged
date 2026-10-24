// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Counts the event calls of the game's loader threads (see LoaderWatch.h)
$end_info$
*/

#include "LoaderWatch.h"
#include "FrameLatency.h"
#include "Windows/Common/PriorityGuard.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

extern "C" {
// Module.S ExitFunctionEC: while [0] is non-zero, a call from x64 code on a thread whose TEB is one of [1..4] into one of
// the entries [5..8] (CreateEventW), [9..12] (CloseHandle) or [13..16] (WaitForMultipleObjects) runs FEX's wrapper instead
// (NativeLoaderCreateEventTarget and so on), after storing the entry it called in [17], [18] or [19]. Entries not found
// stay 0, which never matches.
alignas(64) uint64_t FEXLoaderWatch[20];
HANDLE FEXNativeLoaderCreateEventW(void* Attributes, BOOL ManualReset, BOOL InitialState, const wchar_t* Name, uint64_t Lr, uint64_t Sp);
BOOL FEXNativeLoaderCloseHandle(HANDLE Handle, uint64_t Lr, uint64_t Sp);
DWORD FEXNativeLoaderWaitForMultiple(DWORD Count, const HANDLE* Handles, BOOL WaitAll, DWORD Milliseconds, uint64_t Lr, uint64_t Sp);
extern void* X64ReturnInstr; // Module.cpp
}

namespace LoaderWatch {
namespace {
  constexpr size_t TebSlots = 4;
  constexpr size_t EntriesPerFunction = 4;
  constexpr size_t CreateEntries = 5, CloseEntries = 9, WaitEntries = 13, CalledEntries = 17;
  enum Function { Create, Close, Wait, FunctionCount };
  constexpr const char* FunctionNames[FunctionCount] = {"CreateEventW", "CloseHandle", "WaitForMultipleObjects"};

  using CreateEventFn = HANDLE (*)(void*, BOOL, BOOL, const wchar_t*);
  using CloseHandleFn = BOOL (*)(HANDLE);
  using WaitMultipleFn = DWORD (*)(DWORD, const HANDLE*, BOOL, DWORD);

  constexpr DWORD Infinite = 0xffffffff;
  constexpr DWORD WaitObject0 = 0, WaitAbandoned0 = 0x80, WaitTimeout = 0x102, WaitFailed = 0xffffffff;
  constexpr size_t RecentEvents = 16; // events a thread created, kept until closed or pushed out
  constexpr size_t MaxCallers = 8;    // per function
  constexpr size_t HandleBins = 5;    // 1, 2, 3, 4, 5 or more
  constexpr size_t TimeoutBins = 6;   // 0, 1-15, 16-100, 101-1000, more, INFINITE

  uint64_t Freq {1};

  uint64_t Now() {
    uint64_t Value {};
    __asm volatile("isb; mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Value));
    return Value;
  }

  // Events a watched thread created, so its waits and closes can tell its own new events from others.
  struct Created {
    uint64_t Handle;
    uint64_t At;
    uint32_t Waits;
  };
  struct ThreadData {
    uint32_t TID;
    Created Recent[RecentEvents];
    uint32_t Next;
  };
  ThreadData PerThread[TebSlots];

  struct Caller {
    uint64_t Address;
    uint32_t Calls;
  };

  // One second's counts, under Lock; the log thread takes them every second.
  struct Counts {
    uint64_t Calls[FunctionCount];
    uint64_t Ticks[FunctionCount];
    uint64_t Longest[FunctionCount];
    uint64_t Failed[FunctionCount];
    // CreateEventW
    uint64_t Manual, InitiallySet, Named, WithAttributes;
    // WaitForMultipleObjects
    uint64_t Handles[HandleBins];
    uint64_t Timeouts[TimeoutBins];
    uint64_t AllOf;
    uint64_t SignaledFirst, SignaledOther, Abandoned, TimedOut;
    uint64_t OwnEvent, OwnFirst, OwnSignaled;
    uint64_t Long; // 5 ms or longer
    // CloseHandle
    uint64_t CloseOwn, CloseOwnLifeTicks, CloseOwnWaits, CloseOwnUnwaited;
    Caller Callers[FunctionCount][MaxCallers];
    uint64_t OtherCallers[FunctionCount];
  };
  std::mutex Lock;
  Counts Current {};
  bool Logging {};

  int SlotOf(uint64_t Teb) {
    for (size_t i = 0; i < TebSlots; ++i) {
      if (__atomic_load_n(&FEXLoaderWatch[1 + i], __ATOMIC_RELAXED) == Teb) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // The x64 return address: in lr, unless the caller left the stack misaligned, in which case Module.S left it on the stack
  // and lr points at an x64 ret instruction.
  uint64_t CallerOf(uint64_t Lr, uint64_t Sp) {
    return Lr == reinterpret_cast<uint64_t>(X64ReturnInstr) ? *reinterpret_cast<const uint64_t*>(Sp) : Lr;
  }

  void NoteCall(Counts& C, Function F, uint64_t Ticks, uint64_t From, bool Failed) {
    ++C.Calls[F];
    C.Ticks[F] += Ticks;
    C.Longest[F] = std::max(C.Longest[F], Ticks);
    C.Failed[F] += Failed ? 1 : 0;
    for (auto& K : C.Callers[F]) {
      if (K.Address == From || !K.Address) {
        K.Address = From;
        ++K.Calls;
        return;
      }
    }
    ++C.OtherCallers[F];
  }

  Created* FindOwn(int Slot, uint64_t Handle) {
    if (Slot < 0 || !Handle) {
      return nullptr;
    }
    for (auto& R : PerThread[Slot].Recent) {
      if (R.Handle == Handle) {
        return &R;
      }
    }
    return nullptr;
  }

  // Called by the priority guard for each loader thread it finds (Teb != 0) or that ended (Teb == 0).
  void Watch(uint32_t TID, uint64_t Teb) {
    if (Teb) {
      bool Watching = false;
      {
        std::scoped_lock Guard {Lock};
        for (size_t i = 0; i < TebSlots && !Watching; ++i) {
          if (!FEXLoaderWatch[1 + i]) {
            PerThread[i] = {.TID = TID};
            __atomic_store_n(&FEXLoaderWatch[1 + i], Teb, __ATOMIC_RELEASE);
            __atomic_store_n(&FEXLoaderWatch[0], 1, __ATOMIC_RELEASE);
            Watching = true;
          }
        }
      }
      // Not under Lock: the log thread holds its own lock while it asks for the line.
      if (Watching) {
        FEX::Windows::StatsLogNote(fextl::fmt::format("loader calls: watching tid={}'s CreateEventW, CloseHandle and WaitForMultipleObjects "
                                                      "(FEX_LOADER_WATCH=0 turns it off)",
                                                      TID));
      }
      return;
    }
    std::scoped_lock Guard {Lock};
    bool Any = false;
    for (size_t i = 0; i < TebSlots; ++i) {
      if (FEXLoaderWatch[1 + i] && PerThread[i].TID == TID) {
        __atomic_store_n(&FEXLoaderWatch[1 + i], 0, __ATOMIC_RELEASE);
      }
      Any |= FEXLoaderWatch[1 + i] != 0;
    }
    __atomic_store_n(&FEXLoaderWatch[0], Any ? 1 : 0, __ATOMIC_RELEASE);
  }

  fextl::string CallerText(uint64_t Address) {
    MEMORY_BASIC_INFORMATION Info {};
    if (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), MemoryBasicInformation, &Info, sizeof(Info), nullptr) ==
          STATUS_SUCCESS &&
        Info.AllocationBase) {
      return fextl::fmt::format("{:X}+{:X}", reinterpret_cast<uint64_t>(Info.AllocationBase), Address - reinterpret_cast<uint64_t>(Info.AllocationBase));
    }
    return fextl::fmt::format("{:X}", Address);
  }

  fextl::string StatsLine() {
    Counts C;
    fextl::string Threads;
    {
      std::scoped_lock Guard {Lock};
      C = Current;
      Current = {};
      for (size_t i = 0; i < TebSlots; ++i) {
        if (FEXLoaderWatch[1 + i]) {
          Threads += fextl::fmt::format("{}{}", Threads.empty() ? "" : ",", PerThread[i].TID);
        }
      }
    }
    if (!C.Calls[Create] && !C.Calls[Close] && !C.Calls[Wait]) {
      return {};
    }
    const auto Ms = [](uint64_t Ticks) {
      return static_cast<double>(Ticks) * 1000.0 / static_cast<double>(Freq);
    };
    const auto Head = [&](Function F) {
      return fextl::fmt::format("{} {}/s {:.1f} ms (longest {:.2f}){}", FunctionNames[F], C.Calls[F], Ms(C.Ticks[F]), Ms(C.Longest[F]),
                                C.Failed[F] ? fextl::fmt::format(" failed {}", C.Failed[F]) : fextl::string {});
    };
    fextl::string Out = fextl::fmt::format("  loader calls (tid {}): ", Threads.empty() ? "-" : Threads);
    Out += Head(Create) + fextl::fmt::format(" manual-reset {} initially set {} named {} with attributes {}", C.Manual, C.InitiallySet,
                                             C.Named, C.WithAttributes);
    Out += " | " + Head(Wait) +
           fextl::fmt::format(" handles 1/2/3/4/5+ {}/{}/{}/{}/{} all-of {} | timeout 0/1-15/16-100/101-1000/more/infinite {}/{}/{}/{}/{}/{} | "
                              "result first {} other {} abandoned {} timeout {} | 5 ms+ {} | with a new event of its own {} (as the first "
                              "handle {}, that one signaled {})",
                              C.Handles[0], C.Handles[1], C.Handles[2], C.Handles[3], C.Handles[4], C.AllOf, C.Timeouts[0], C.Timeouts[1],
                              C.Timeouts[2], C.Timeouts[3], C.Timeouts[4], C.Timeouts[5], C.SignaledFirst, C.SignaledOther, C.Abandoned,
                              C.TimedOut, C.Long, C.OwnEvent, C.OwnFirst, C.OwnSignaled);
    Out += " | " + Head(Close) +
           fextl::fmt::format(" its own new events {} (lived {:.2f} ms avg, waited on {:.1f} times avg, never {})", C.CloseOwn,
                              C.CloseOwn ? Ms(C.CloseOwnLifeTicks) / static_cast<double>(C.CloseOwn) : 0.0,
                              C.CloseOwn ? static_cast<double>(C.CloseOwnWaits) / static_cast<double>(C.CloseOwn) : 0.0, C.CloseOwnUnwaited);
    Out += " | from";
    for (size_t F = 0; F < FunctionCount; ++F) {
      Caller Sorted[MaxCallers];
      std::copy(std::begin(C.Callers[F]), std::end(C.Callers[F]), Sorted);
      std::sort(std::begin(Sorted), std::end(Sorted), [](const Caller& A, const Caller& B) { return A.Calls > B.Calls; });
      fextl::string List;
      for (size_t i = 0; i < 3 && Sorted[i].Address; ++i) {
        List += fextl::fmt::format(" {} x{}", CallerText(Sorted[i].Address), Sorted[i].Calls);
      }
      if (C.OtherCallers[F]) {
        List += fextl::fmt::format(" others x{}", C.OtherCallers[F]);
      }
      if (!List.empty()) {
        Out += fextl::fmt::format(" {}:{};", FunctionNames[F], List);
      }
    }
    Out += "\n";
    return Out;
  }
} // namespace

void Init(bool StatsLog) {
  const char* Value = FEX::Windows::Switch("FEX_LOADER_WATCH");
  const char* Cores = FEX::Windows::Switch("FEX_LOADER_CORES");
  const bool Finding = Cores && Cores[0] != 'o' && Cores[0] != '0';
  if (!StatsLog || !Value || Value[0] != '1' || !Finding) {
    if (StatsLog) {
      FEX::Windows::StatsLogNote(Finding ? "loader calls: not watched (FEX_LOADER_WATCH=1 watches them)" :
                                           "loader calls: not watched (loader threads aren't looked for, FEX_LOADER_CORES=off)");
    }
    return;
  }
  uint64_t F {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(F));
  Freq = F ? F : 1;

  // Each function's entries in kernelbase, and kernel32's own ones if it has any (Wine's kernel32 forwards these to
  // kernelbase, and a forwarded export has no entry of its own).
  const wchar_t* Modules[] = {L"kernelbase.dll", L"kernel32.dll"};
  const char* Names[FunctionCount] = {"CreateEventW", "CloseHandle", "WaitForMultipleObjects"};
  const size_t First[FunctionCount] = {CreateEntries, CloseEntries, WaitEntries};
  fextl::string Found;
  bool Complete = true;
  for (size_t Fn = 0; Fn < FunctionCount; ++Fn) {
    size_t Count {};
    for (const wchar_t* Module : Modules) {
      const uint64_t Base = FrameLatency::FindModule(Module);
      uint64_t Entries[3] {};
      const size_t N = Base ? FrameLatency::FindExportEntries(Base, Names[Fn], Entries) : 0;
      for (size_t i = 0; i < N && Count < EntriesPerFunction; ++i) {
        if (std::find(FEXLoaderWatch + First[Fn], FEXLoaderWatch + First[Fn] + Count, Entries[i]) == FEXLoaderWatch + First[Fn] + Count) {
          FEXLoaderWatch[First[Fn] + Count++] = Entries[i];
        }
      }
    }
    Complete &= Count != 0;
    Found += fextl::fmt::format(" {} {}", Names[Fn], Count);
  }
  if (!Complete) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("loader calls: not watched, entries not all found (found{})", Found));
    std::fill(std::begin(FEXLoaderWatch), std::end(FEXLoaderWatch), 0);
    return;
  }
  Logging = true;
  FEX::Windows::SetLoaderThreadHook(Watch);
  FEX::Windows::StatsLogSetLineProvider(StatsLine);
  FEX::Windows::StatsLogNote(fextl::fmt::format("loader calls: ready to watch loader threads' calls (entries found{}; FEX_LOADER_WATCH=0 turns "
                                                "it off)",
                                                Found));
}
} // namespace LoaderWatch

using namespace LoaderWatch;

extern "C" HANDLE FEXNativeLoaderCreateEventW(void* Attributes, BOOL ManualReset, BOOL InitialState, const wchar_t* Name, uint64_t Lr, uint64_t Sp) {
  const auto Real = reinterpret_cast<CreateEventFn>(__atomic_load_n(&FEXLoaderWatch[CalledEntries + Create], __ATOMIC_ACQUIRE));
  const uint64_t Start = Now();
  const HANDLE Result = Real(Attributes, ManualReset, InitialState, Name);
  const uint64_t End = Now();
  const int Slot = SlotOf(reinterpret_cast<uint64_t>(NtCurrentTeb()));
  if (Slot < 0) {
    return Result;
  }
  if (Result) {
    // Only this thread writes its slot's ring.
    auto& T = PerThread[Slot];
    T.Recent[T.Next] = {.Handle = reinterpret_cast<uint64_t>(Result), .At = End};
    T.Next = (T.Next + 1) % RecentEvents;
  }
  std::scoped_lock Guard {Lock};
  NoteCall(Current, Create, End - Start, CallerOf(Lr, Sp), !Result);
  Current.Manual += ManualReset ? 1 : 0;
  Current.InitiallySet += InitialState ? 1 : 0;
  Current.Named += Name ? 1 : 0;
  Current.WithAttributes += Attributes ? 1 : 0;
  return Result;
}

extern "C" BOOL FEXNativeLoaderCloseHandle(HANDLE Handle, uint64_t Lr, uint64_t Sp) {
  const auto Real = reinterpret_cast<CloseHandleFn>(__atomic_load_n(&FEXLoaderWatch[CalledEntries + Close], __ATOMIC_ACQUIRE));
  const uint64_t Start = Now();
  const BOOL Result = Real(Handle);
  const uint64_t End = Now();
  const int Slot = SlotOf(reinterpret_cast<uint64_t>(NtCurrentTeb()));
  if (Slot < 0) {
    return Result;
  }
  Created Own {};
  if (Created* R = FindOwn(Slot, reinterpret_cast<uint64_t>(Handle))) {
    Own = *R;
    *R = {};
  }
  std::scoped_lock Guard {Lock};
  NoteCall(Current, Close, End - Start, CallerOf(Lr, Sp), !Result);
  if (Own.Handle) {
    ++Current.CloseOwn;
    Current.CloseOwnLifeTicks += Start - Own.At;
    Current.CloseOwnWaits += Own.Waits;
    Current.CloseOwnUnwaited += Own.Waits ? 0 : 1;
  }
  return Result;
}

extern "C" DWORD FEXNativeLoaderWaitForMultiple(DWORD Count, const HANDLE* Handles, BOOL WaitAll, DWORD Milliseconds, uint64_t Lr, uint64_t Sp) {
  const auto Real = reinterpret_cast<WaitMultipleFn>(__atomic_load_n(&FEXLoaderWatch[CalledEntries + Wait], __ATOMIC_ACQUIRE));
  const int Slot = SlotOf(reinterpret_cast<uint64_t>(NtCurrentTeb()));
  // Which of the handles are events this thread created (the first 8 handles are looked at).
  int OwnIndex = -1;
  bool OwnFirst = false;
  if (Slot >= 0 && Handles) {
    for (DWORD i = 0; i < std::min<DWORD>(Count, 8); ++i) {
      if (Created* R = FindOwn(Slot, reinterpret_cast<uint64_t>(Handles[i]))) {
        ++R->Waits;
        OwnIndex = OwnIndex < 0 ? static_cast<int>(i) : OwnIndex;
        OwnFirst |= i == 0;
      }
    }
  }
  const uint64_t Start = Now();
  const DWORD Result = Real(Count, Handles, WaitAll, Milliseconds);
  const uint64_t End = Now();
  if (Slot < 0) {
    return Result;
  }
  std::scoped_lock Guard {Lock};
  NoteCall(Current, Wait, End - Start, CallerOf(Lr, Sp), Result == WaitFailed);
  ++Current.Handles[std::clamp<DWORD>(Count, 1, HandleBins) - 1];
  Current.AllOf += WaitAll ? 1 : 0;
  ++Current.Timeouts[Milliseconds == Infinite ? 5 : Milliseconds == 0 ? 0 : Milliseconds <= 15 ? 1 : Milliseconds <= 100 ? 2 : Milliseconds <= 1000 ? 3 : 4];
  if (Result == WaitObject0) {
    ++Current.SignaledFirst;
  } else if (Result > WaitObject0 && Result < WaitObject0 + Count) {
    ++Current.SignaledOther;
  } else if (Result >= WaitAbandoned0 && Result < WaitAbandoned0 + Count) {
    ++Current.Abandoned;
  } else if (Result == WaitTimeout) {
    ++Current.TimedOut;
  }
  Current.Long += End - Start >= Freq / 200 ? 1 : 0;
  if (OwnIndex >= 0) {
    ++Current.OwnEvent;
    Current.OwnFirst += OwnFirst ? 1 : 0;
    Current.OwnSignaled += Result == WaitObject0 + static_cast<DWORD>(OwnIndex) ? 1 : 0;
  }
  return Result;
}
