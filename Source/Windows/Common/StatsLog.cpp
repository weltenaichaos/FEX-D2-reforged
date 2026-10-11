// SPDX-License-Identifier: MIT
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/SHMStats.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SHMStats.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

// Exported by ntdll (Defs/ntdll.def) but not declared by the bundled winternl.h.
extern "C" NTSTATUS WINAPI NtOpenThread(HANDLE* Handle, ACCESS_MASK Access, const OBJECT_ATTRIBUTES* Attr, const CLIENT_ID* Id);

namespace FEX::Windows {
namespace {
  constexpr size_t MaxTracked = 512;
  constexpr size_t MaxPrinted = 8;

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
  };

  struct Delta {
    Sample D;
    double CPUPercent;
  };

  // All state is static and fixed-size: a tick runs inside FEX's exception handler on whatever guest thread happens
  // to be there, where allocating could deadlock against a lock the interrupted code holds.
  struct LogState {
    const StatAlloc* Stats;
    FILE* File;
    uint64_t CounterFreq;
    uint64_t IntervalTicks;
    uint64_t StartTicks;
    uint64_t LastTicks;
    std::atomic<uint64_t> NextDue;
    std::atomic<bool> Busy;
    Sample Prev[MaxTracked];
    size_t PrevCount;
    Sample Current[MaxTracked];
    Delta Deltas[MaxTracked];
    char Line[8192];
  };

  LogState* State;

  uint64_t GetCounterFrequency() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Result));
    return Result ? Result : 1;
  }

  uint64_t GetCounter() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Result));
    return Result;
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

  // Writes the thread's name (ASCII, spaces replaced) into Out, which must hold at least 64 bytes.
  void QueryThreadName(uint32_t TID, char* Out, size_t OutSize) {
    Out[0] = '-';
    Out[1] = 0;
    HANDLE Handle = OpenThreadForQuery(TID);
    if (!Handle) {
      return;
    }
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 128 * sizeof(WCHAR)] {};
    ULONG Length {};
    if (NtQueryInformationThread(Handle, static_cast<THREADINFOCLASS>(38) /* ThreadNameInformation */, Buffer, sizeof(Buffer), &Length) ==
        STATUS_SUCCESS) {
      const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
      size_t o = 0;
      for (USHORT i = 0; Name->Buffer && i < Name->Length / sizeof(WCHAR) && o + 1 < OutSize; ++i) {
        const WCHAR c = Name->Buffer[i];
        Out[o++] = c > 0x20 && c < 0x7f ? static_cast<char>(c) : '_';
      }
      if (o) {
        Out[o] = 0;
      }
    }
    NtClose(Handle);
  }

  template<typename... Args>
  size_t Append(size_t Pos, fmt::format_string<Args...> Format, Args&&... args) {
    if (Pos >= sizeof(State->Line)) {
      return Pos;
    }
    auto Result = fmt::format_to_n(State->Line + Pos, sizeof(State->Line) - Pos, Format, std::forward<Args>(args)...);
    return std::min(Pos + Result.size, sizeof(State->Line));
  }

  void WriteSample(uint64_t Now) {
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    const double Elapsed100ns = static_cast<double>(Now - State->LastTicks) * 10'000'000.0 / static_cast<double>(State->CounterFreq);
    const double Seconds = static_cast<double>(Now - State->StartTicks) / static_cast<double>(State->CounterFreq);
    State->LastTicks = Now;

    size_t CurrentCount = 0;
    State->Stats->ForEachSlot([&](const FEXCore::SHMStats::ThreadStats& Slot) {
      if (CurrentCount == MaxTracked) {
        return;
      }
      const uint32_t TID = Slot.TID.load(std::memory_order_relaxed);
      State->Current[CurrentCount++] = {
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
      };
    });

    Sample Total {};
    double TotalCPU {};
    for (size_t c = 0; c < CurrentCount; ++c) {
      const Sample& Cur = State->Current[c];
      static const Sample Zero {};
      const Sample* Old = &Zero;
      for (size_t i = 0; i < State->PrevCount; ++i) {
        if (State->Prev[i].TID == Cur.TID) {
          Old = &State->Prev[i];
          break;
        }
      }
      // A reused TID can make counters go backwards; treat that as a fresh start.
      auto Diff = [](uint64_t New, uint64_t OldV) {
        return New >= OldV ? New - OldV : New;
      };
      Delta& Entry = State->Deltas[c];
      Entry.D = {
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
      };
      Entry.CPUPercent = Elapsed100ns > 0 ? static_cast<double>(Entry.D.CPUTime100ns) * 100.0 / Elapsed100ns : 0.0;
      TotalCPU += Entry.CPUPercent;
      Total.JITTime += Entry.D.JITTime;
      Total.JITCount += Entry.D.JITCount;
      Total.SignalTime += Entry.D.SignalTime;
      Total.SIGBUSCount += Entry.D.SIGBUSCount;
      Total.SMCCount += Entry.D.SMCCount;
      Total.CacheMissCount += Entry.D.CacheMissCount;
      Total.FloatFallbackCount += Entry.D.FloatFallbackCount;
    }

    std::copy_n(State->Current, CurrentCount, State->Prev);
    State->PrevCount = CurrentCount;

    std::sort(State->Deltas, State->Deltas + CurrentCount, [](const Delta& a, const Delta& b) { return a.CPUPercent > b.CPUPercent; });

    const double IntervalMs = Elapsed100ns / 10'000.0;
    size_t Pos = Append(0, "t={:.1f}s span={:.0f}ms threads={} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} x87fb={}\n",
                        Seconds, IntervalMs, CurrentCount, TotalCPU, Total.JITTime * MsPerTick, Total.JITCount, Total.SMCCount,
                        Total.SIGBUSCount, Total.SignalTime * MsPerTick, Total.CacheMissCount, Total.FloatFallbackCount);

    size_t Printed {};
    for (size_t c = 0; c < CurrentCount && Printed < MaxPrinted; ++c) {
      const auto& Entry = State->Deltas[c];
      const auto& D = Entry.D;
      if (Entry.CPUPercent < 3.0 && !D.JITCount && !D.SMCCount && !D.SIGBUSCount) {
        continue;
      }
      ++Printed;
      char Name[64];
      QueryThreadName(D.TID, Name, sizeof(Name));
      Pos = Append(Pos, "  tid={} {} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} lock={:.1f}ms x87fb={} dc={}\n",
                   D.TID, static_cast<const char*>(Name), Entry.CPUPercent, D.JITTime * MsPerTick, D.JITCount, D.SMCCount, D.SIGBUSCount,
                   D.SignalTime * MsPerTick, D.CacheMissCount, D.CacheLockTime * MsPerTick, D.FloatFallbackCount, D.DiskCacheHitCount);
    }

    fwrite(State->Line, 1, Pos, State->File);
  }

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

  const auto Path = fextl::fmt::format("{}\\fexstats-{}-{}.txt", Dir, AppName, GetCurrentProcessId());
  FILE* File = fopen(Path.c_str(), "w");
  if (!File) {
    LogMan::Msg::EFmt("FEX_STATSLOG: can't open {}", Path);
    return;
  }

  auto Header = fextl::fmt::format("# FEX stats for {} (pid {}), roughly one block per second. span = length of the block; cpu = thread "
                                   "CPU time; jit = time/blocks compiled; smc = writes to translated code; sigbus = unaligned atomic "
                                   "fixups; sig = time in exception handling; miss = lookup cache misses; lock = lookup cache lock "
                                   "time; x87fb = x87 fallbacks; dc = disk cache hits\n",
                                   AppName, GetCurrentProcessId());
  // No fflush anywhere in here: FEX's CRT fwrite goes straight to WriteFile, and its fflush is an UNIMPLEMENTED() stub
  // that terminates the process.
  fwrite(Header.data(), 1, Header.size(), File);

  // Lives for the rest of the process. No background thread: a thread of our own never reported anything in D2R
  // (most likely stopped by its protector), so samples are taken from StatsLogTick() on guest threads instead.
  State = new LogState {};
  State->Stats = Stats;
  State->File = File;
  State->CounterFreq = GetCounterFrequency();
  State->IntervalTicks = State->CounterFreq;
  State->StartTicks = GetCounter();
  State->LastTicks = State->StartTicks;
  State->NextDue.store(State->StartTicks + State->IntervalTicks, std::memory_order_relaxed);
}

void StatsLogTick() {
  if (!State) {
    return;
  }

  const uint64_t Now = GetCounter();
  uint64_t Due = State->NextDue.load(std::memory_order_relaxed);
  if (Now < Due) {
    return;
  }

  // One writer at a time; anyone else just carries on.
  if (State->Busy.exchange(true, std::memory_order_acquire)) {
    return;
  }
  if (State->NextDue.compare_exchange_strong(Due, Now + State->IntervalTicks, std::memory_order_relaxed)) {
    WriteSample(Now);
  }
  State->Busy.store(false, std::memory_order_release);
}
} // namespace FEX::Windows
