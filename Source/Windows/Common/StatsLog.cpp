// SPDX-License-Identifier: MIT
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/SHMStats.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
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

  struct LogState {
    const StatAlloc* Stats;
    FILE* File;
    uint64_t CounterFreq;
    Sample Prev[MaxTracked];
    size_t PrevCount;
  };

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

  void Sleep1s() {
    LARGE_INTEGER Time {.QuadPart = -10'000'000LL};
    NtDelayExecution(FALSE, &Time);
  }

  void* LogThread(void* Arg) {
    auto* State = static_cast<LogState*>(Arg);
    const double MsPerTick = 1000.0 / static_cast<double>(State->CounterFreq);
    uint64_t Seconds {};

    fextl::vector<Sample> Current;
    Current.reserve(MaxTracked);

    for (;;) {
      Sleep1s();
      ++Seconds;

      Current.clear();
      State->Stats->ForEachSlot([&](const FEXCore::SHMStats::ThreadStats& Slot) {
        if (Current.size() == MaxTracked) {
          return;
        }
        const uint32_t TID = Slot.TID.load(std::memory_order_relaxed);
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
        });
      });

      struct Delta {
        Sample D;
        double CPUPercent;
      };
      fextl::vector<Delta> Deltas;
      Deltas.reserve(Current.size());
      Sample Total {};
      double TotalCPU {};

      for (const auto& Cur : Current) {
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
            },
        };
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
        Deltas.push_back(Entry);
      }

      State->PrevCount = std::min(Current.size(), MaxTracked);
      std::copy_n(Current.begin(), State->PrevCount, State->Prev);

      std::sort(Deltas.begin(), Deltas.end(), [](const Delta& a, const Delta& b) { return a.CPUPercent > b.CPUPercent; });

      auto Line = fextl::fmt::format("t={} threads={} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} x87fb={}\n",
                                     Seconds, Current.size(), TotalCPU, Total.JITTime * MsPerTick, Total.JITCount, Total.SMCCount,
                                     Total.SIGBUSCount, Total.SignalTime * MsPerTick, Total.CacheMissCount, Total.FloatFallbackCount);

      size_t Printed {};
      for (const auto& Entry : Deltas) {
        const auto& D = Entry.D;
        const bool Interesting = Entry.CPUPercent >= 3.0 || D.JITCount || D.SMCCount || D.SIGBUSCount;
        if (!Interesting || Printed == MaxPrinted) {
          continue;
        }
        ++Printed;
        Line += fextl::fmt::format("  tid={} {} cpu={:.0f}% jit={:.1f}ms/{} smc={} sigbus={} sig={:.1f}ms miss={} lock={:.1f}ms x87fb={} "
                                   "dc={}\n",
                                   D.TID, QueryThreadName(D.TID), Entry.CPUPercent, D.JITTime * MsPerTick, D.JITCount, D.SMCCount,
                                   D.SIGBUSCount, D.SignalTime * MsPerTick, D.CacheMissCount, D.CacheLockTime * MsPerTick,
                                   D.FloatFallbackCount, D.DiskCacheHitCount);
      }

      fwrite(Line.data(), 1, Line.size(), State->File);
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
                                   "cache hits\n",
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
  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(LogThread, State, {.LowPriority = false, .Internal = true}, "FEXStatsLog");
}

void StatsLogNote(std::string_view Line) {
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
