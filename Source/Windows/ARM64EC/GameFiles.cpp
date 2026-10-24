// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Drops the game's data files from Android's file cache, so its reads don't push other apps out of memory
$end_info$
*/

#include "GameFiles.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>

namespace GameFiles {
namespace {
  constexpr size_t MaxFiles = 256;
  constexpr size_t MaxOtherDirs = 6;
  constexpr int64_t NotFoundNoteMs = 120'000;

  struct Counters {
    std::atomic<uint64_t> Passes;
    std::atomic<uint64_t> Drops;     // files dropped (one fadvise each)
    std::atomic<uint64_t> Failed;    // fadvise calls that failed
    std::atomic<uint64_t> ShrankKB;  // how much the file cache shrank across the drops
    std::atomic<uint64_t> LongestNs; // longest pass
    std::atomic<uint32_t> Open;      // data files open at the last pass
  };

  Counters Stats {};
  uint64_t IntervalMs {500};
  uint64_t CounterFreq {1};

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

  void SleepMs(uint64_t Ms) {
    LARGE_INTEGER Interval {.QuadPart = -static_cast<int64_t>(Ms * 10'000)};
    NtDelayExecution(FALSE, &Interval);
  }

  char Lower(char Ch) {
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
  }

  // The last '/' in Text[0..Len), or nullptr.
  const char* LastSlash(const char* Text, size_t Len) {
    while (Len) {
      if (Text[--Len] == '/') {
        return Text + Len;
      }
    }
    return nullptr;
  }

  bool EqualsLower(const char* Text, size_t Len, const char* Lowercase) {
    if (strlen(Lowercase) != Len) {
      return false;
    }
    for (size_t i = 0; i < Len; ++i) {
      if (Lower(Text[i]) != Lowercase[i]) {
        return false;
      }
    }
    return true;
  }

  // A CASC data archive: ".../data/data.<digits>" (any case).
  bool IsDataFile(const char* Path, size_t Len) {
    const char* Slash = LastSlash(Path, Len);
    if (!Slash || Slash == Path) {
      return false;
    }
    const char* Name = Slash + 1;
    const size_t NameLen = Len - static_cast<size_t>(Name - Path);
    if (NameLen < 6 || !EqualsLower(Name, 5, "data.")) {
      return false;
    }
    for (size_t i = 5; i < NameLen; ++i) {
      if (Name[i] < '0' || Name[i] > '9') {
        return false;
      }
    }
    const char* DirSlash = LastSlash(Path, static_cast<size_t>(Slash - Path));
    if (!DirSlash) {
      return false;
    }
    return EqualsLower(DirSlash + 1, static_cast<size_t>(Slash - DirSlash - 1), "data");
  }

  uint64_t Hash(const char* Text, size_t Len) {
    uint64_t H = 1469598103934665603ull;
    for (size_t i = 0; i < Len; ++i) {
      H = (H ^ static_cast<uint8_t>(Text[i])) * 1099511628211ull;
    }
    return H;
  }

  struct PassState {
    int Fds[MaxFiles];
    uint64_t Hashes[MaxFiles];
    size_t Count;
    bool Full;
    char Path[512];
    char FirstPath[512];
    // For the note when no data file shows up: folders of the other open files.
    bool CollectOthers;
    size_t OtherCount;
    uint64_t OtherHashes[MaxOtherDirs];
    char OtherDirs[MaxOtherDirs][96];
  };

  void NoteOther(PassState* S, const char* Path, size_t Len) {
    if (S->OtherCount >= MaxOtherDirs || Path[0] != '/') {
      return;
    }
    if (!strncmp(Path, "/dev/", 5) || !strncmp(Path, "/proc/", 6) || !strncmp(Path, "/sys/", 5) || !strncmp(Path, "/memfd:", 7) ||
        strstr(Path, "(deleted)")) {
      return;
    }
    const char* Slash = LastSlash(Path, Len);
    const size_t DirLen = Slash ? static_cast<size_t>(Slash - Path) : Len;
    const uint64_t H = Hash(Path, DirLen);
    for (size_t i = 0; i < S->OtherCount; ++i) {
      if (S->OtherHashes[i] == H) {
        return;
      }
    }
    // The end of the folder's path says the most.
    char* Out = S->OtherDirs[S->OtherCount];
    constexpr size_t Room = sizeof(S->OtherDirs[0]) - 1;
    if (DirLen <= Room) {
      memcpy(Out, Path, DirLen);
      Out[DirLen] = 0;
    } else {
      memcpy(Out, "...", 3);
      memcpy(Out + 3, Path + DirLen - (Room - 3), Room - 3);
      Out[Room] = 0;
    }
    S->OtherHashes[S->OtherCount++] = H;
  }

  void Visit(const char* Name, void* Arg) {
    auto* S = static_cast<PassState*>(Arg);
    if (S->Count >= MaxFiles) {
      S->Full = true;
      return;
    }
    char Link[48];
    snprintf(Link, sizeof(Link), "/proc/self/fd/%s", Name);
    const int64_t Len = FEX::Windows::Linux::ReadLink(Link, S->Path, sizeof(S->Path));
    if (Len <= 0) {
      return;
    }
    if (!IsDataFile(S->Path, static_cast<size_t>(Len))) {
      if (S->CollectOthers) {
        NoteOther(S, S->Path, static_cast<size_t>(Len));
      }
      return;
    }
    // Wine may hold several fds for one file (the wineserver's and each handle's); dropping it once is enough.
    const uint64_t H = Hash(S->Path, static_cast<size_t>(Len));
    for (size_t i = 0; i < S->Count; ++i) {
      if (S->Hashes[i] == H) {
        return;
      }
    }
    if (!S->Count) {
      memcpy(S->FirstPath, S->Path, static_cast<size_t>(Len) + 1);
    }
    S->Fds[S->Count] = static_cast<int>(strtoull(Name, nullptr, 10));
    S->Hashes[S->Count++] = H;
  }

  // The kernel's file cache ("Cached" in /proc/meminfo) in KB, 0 when unreadable.
  uint64_t FileCacheKB() {
    char Buffer[4096];
    if (FEX::Windows::Linux::ReadSmallFile("/proc/meminfo", Buffer, sizeof(Buffer)) <= 0) {
      return 0;
    }
    const char* P = strstr(Buffer, "\nCached:");
    return P ? strtoull(P + 8, nullptr, 10) : 0;
  }

  void* FileCacheThread(void*) {
    // About 11 KB: off the stack of a thread that also lists directories.
    static PassState S;
    bool Found = false;
    bool NotFoundNoted = false;
    const uint64_t Start = GetCounter();
    for (;;) {
      const uint64_t Begin = GetCounter();
      const int64_t SinceStartMs = static_cast<int64_t>((Begin - Start) * 1000 / CounterFreq);
      S.Count = 0;
      S.Full = false;
      S.OtherCount = 0;
      S.CollectOthers = !Found && !NotFoundNoted && SinceStartMs >= NotFoundNoteMs;
      const bool Listed = FEX::Windows::Linux::ForEachEntry("/proc/self/fd", Visit, &S);

      uint64_t Before = 0;
      uint64_t Failed = 0;
      if (S.Count) {
        Before = FileCacheKB();
        for (size_t i = 0; i < S.Count; ++i) {
          if (FEX::Windows::Linux::DropFromFileCache(S.Fds[i], 0, 0) < 0) {
            ++Failed;
          }
        }
        const uint64_t After = FileCacheKB();
        if (Before && After && After < Before) {
          Stats.ShrankKB.fetch_add(Before - After, std::memory_order_relaxed);
        }
      }
      Stats.Open.store(static_cast<uint32_t>(S.Count), std::memory_order_relaxed);
      Stats.Drops.fetch_add(S.Count - Failed, std::memory_order_relaxed);
      Stats.Failed.fetch_add(Failed, std::memory_order_relaxed);
      Stats.Passes.fetch_add(1, std::memory_order_relaxed);
      const uint64_t Ns = (GetCounter() - Begin) * 1'000'000'000 / CounterFreq;
      uint64_t Longest = Stats.LongestNs.load(std::memory_order_relaxed);
      while (Ns > Longest && !Stats.LongestNs.compare_exchange_weak(Longest, Ns, std::memory_order_relaxed)) {}

      if (!Found && S.Count) {
        Found = true;
        FEX::Windows::StatsLogNote(
          fextl::fmt::format("game files: {} data file{} open (first {}){}; dropped from the file cache every {} ms", S.Count,
                             S.Count == 1 ? "" : "s", S.FirstPath, S.Full ? ", more open files than are looked at" : "", IntervalMs));
      } else if (S.CollectOthers) {
        NotFoundNoted = true;
        fextl::string Dirs;
        for (size_t i = 0; i < S.OtherCount; ++i) {
          Dirs += fextl::fmt::format("{}{}", i ? ", " : " ", S.OtherDirs[i]);
        }
        FEX::Windows::StatsLogNote(Listed ? fextl::fmt::format("game files: no data.NNN file open after 2 minutes, nothing dropped; "
                                                               "folders of "
                                                               "other open files:{}",
                                                               Dirs.empty() ? fextl::string(" none") : Dirs) :
                                            fextl::string("game files: /proc/self/fd can't be listed, nothing dropped"));
      }
      SleepMs(IntervalMs);
    }
    return nullptr;
  }

  fextl::string StatsLine() {
    const uint64_t Passes = Stats.Passes.exchange(0, std::memory_order_relaxed);
    const uint64_t Drops = Stats.Drops.exchange(0, std::memory_order_relaxed);
    const uint64_t Failed = Stats.Failed.exchange(0, std::memory_order_relaxed);
    const uint64_t ShrankKB = Stats.ShrankKB.exchange(0, std::memory_order_relaxed);
    const uint64_t LongestNs = Stats.LongestNs.exchange(0, std::memory_order_relaxed);
    const uint32_t Open = Stats.Open.load(std::memory_order_relaxed);
    if (!Passes) {
      return {};
    }
    return fextl::fmt::format("  game files: {} data files open, file cache {:.0f} MB smaller after the drops ({} drops in {} passes, "
                              "longest {:.1f} ms{})\n",
                              Open, static_cast<double>(ShrankKB) / 1024.0, Drops, Passes, static_cast<double>(LongestNs) / 1e6,
                              Failed ? fextl::fmt::format(", {} failed", Failed) : fextl::string());
  }
} // namespace

void Init(bool StatsLog) {
  const char* Value = FEX::Windows::Switch("FEX_GAME_FILES_UNCACHED");
  if (!Value || Value[0] != '1') {
    return;
  }
  const char* Ms = FEX::Windows::Switch("FEX_GAME_FILES_MS");
  IntervalMs = std::clamp<uint64_t>(Ms && Ms[0] >= '0' && Ms[0] <= '9' ? strtoull(Ms, nullptr, 10) : 500, 100, 10'000);
  CounterFreq = GetCounterFrequency();
  FEX::Windows::StatsLogNote(fextl::fmt::format("game files: the game's data files (.../data/data.NNN) are dropped from the file cache "
                                                "every {} ms by FEXFileCache (FEX_GAME_FILES_UNCACHED=0 turns it off, FEX_GAME_FILES_MS "
                                                "sets the time)",
                                                IntervalMs));
  if (StatsLog) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(FileCacheThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXFileCache");
}
} // namespace GameFiles
