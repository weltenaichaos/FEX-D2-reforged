// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Drops the game's big files from Android's file cache, so its reads don't push other apps out of memory
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
  constexpr uint64_t MinBytes = 32ull << 20; // smaller files don't matter for the file cache
  constexpr int MaxDepth = 4;                // <game>/Data/data/data.000 is depth 3
  constexpr size_t MaxWalkEntries = 20'000;
  constexpr size_t PathSize = 1024;
  constexpr uint32_t FileOpen = 1; // FILE_OPEN

  struct Counters {
    std::atomic<uint64_t> Passes;
    std::atomic<uint64_t> Drops;     // files dropped (one fadvise each)
    std::atomic<uint64_t> Failed;    // fadvise calls that failed
    std::atomic<uint64_t> ShrankKB;  // how much the file cache shrank across the drops
    std::atomic<uint64_t> LongestNs; // longest pass
    std::atomic<uint32_t> Files;     // files dropped at the last pass
  };

  struct File {
    int Fd;
    uint64_t Hash;
  };

  Counters Stats {};
  uint64_t IntervalMs {500};
  uint64_t CounterFreq {1};

  // FEXFileCache thread only.
  File Files[MaxFiles];
  size_t FileCount {};
  uint64_t FileBytes {};
  fextl::string FirstNames; // a few of the files, for the log
  size_t Named {};
  size_t WalkEntries {};
  char WalkPath[PathSize];
  char LinkTarget[PathSize];

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

  uint64_t Hash(const char* Text, size_t Len) {
    uint64_t H = 1469598103934665603ull;
    for (size_t i = 0; i < Len; ++i) {
      H = (H ^ static_cast<uint8_t>(Text[i])) * 1099511628211ull;
    }
    return H;
  }

  // Files left alone: FEX's own caches (the disk cache is mapped, the unpack cache drops itself), the driver's shader
  // caches (small reads that are wanted again), and what isn't a file on storage.
  bool Excluded(const char* Path) {
    return Path[0] != '/' || !strncmp(Path, "/proc/", 6) || !strncmp(Path, "/dev/", 5) || !strncmp(Path, "/sys/", 5) ||
           strstr(Path, "/fex-emu/") || strstr(Path, "shader_cache") || strstr(Path, "(deleted)");
  }

  bool Known(uint64_t H) {
    for (size_t i = 0; i < FileCount; ++i) {
      if (Files[i].Hash == H) {
        return true;
      }
    }
    return false;
  }

  // Opens Path (a regular file of Size bytes) for dropping. False when it was known, excluded or can't be opened.
  bool Add(const char* Path, uint64_t Size) {
    const size_t Len = strlen(Path);
    const uint64_t H = Hash(Path, Len);
    if (FileCount >= MaxFiles || Excluded(Path) || Known(H)) {
      return false;
    }
    const int Fd = FEX::Windows::Linux::OpenRead(Path, false);
    if (Fd < 0) {
      return false;
    }
    // Known by the path the kernel names it with (symbolic links resolved), which is what the scan of the game's open
    // files sees.
    uint64_t Canonical = H;
    char Link[48];
    snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Fd);
    char Resolved[PathSize];
    const int64_t ResolvedLen = FEX::Windows::Linux::ReadLink(Link, Resolved, sizeof(Resolved));
    if (ResolvedLen > 0) {
      Canonical = Hash(Resolved, static_cast<size_t>(ResolvedLen));
      if (Canonical != H && Known(Canonical)) {
        FEX::Windows::Linux::Close(Fd);
        return false;
      }
    }
    Files[FileCount++] = {Fd, Canonical};
    FileBytes += Size;
    if (Named < 6) {
      const char* Slash = strrchr(Path, '/');
      FirstNames += fextl::fmt::format("{}{} {} MB", Named ? ", " : "", Slash ? Slash + 1 : Path, Size >> 20);
      ++Named;
    }
    return true;
  }

  void WalkDir(size_t PathLen, int Depth);

  struct WalkArgs {
    size_t PathLen;
    int Depth;
  };

  void WalkEntry(const char* Name, void* Arg) {
    const auto* A = static_cast<WalkArgs*>(Arg);
    if (++WalkEntries > MaxWalkEntries || FileCount >= MaxFiles) {
      return;
    }
    const size_t NameLen = strlen(Name);
    if (A->PathLen + 1 + NameLen + 1 > PathSize) {
      return;
    }
    WalkPath[A->PathLen] = '/';
    memcpy(WalkPath + A->PathLen + 1, Name, NameLen + 1);
    FEX::Windows::Linux::FileInfo Info {};
    if (FEX::Windows::Linux::Stat(WalkPath, Info)) {
      if (Info.Directory && A->Depth + 1 < MaxDepth) {
        WalkDir(A->PathLen + 1 + NameLen, A->Depth + 1);
      } else if (Info.Regular && Info.Size >= MinBytes) {
        Add(WalkPath, Info.Size);
      }
    }
    WalkPath[A->PathLen] = 0;
  }

  // Walks WalkPath[0..PathLen) (NUL-terminated there).
  void WalkDir(size_t PathLen, int Depth) {
    WalkArgs Args {PathLen, Depth};
    WalkPath[PathLen] = 0;
    FEX::Windows::Linux::ForEachEntry(WalkPath, WalkEntry, &Args);
    WalkPath[PathLen] = 0;
  }

  // The Linux path of the folder D2R.exe is in, through Wine's wine_nt_to_unix_file_name. Empty with the reason in Why.
  fextl::string GameFolder(fextl::string& Why) {
    const auto* Params = reinterpret_cast<const RTL_USER_PROCESS_PARAMETERS64*>(NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters);
    if (!Params || !Params->ImagePathName.Buffer || !Params->ImagePathName.Length) {
      Why = "no image path";
      return {};
    }
    wchar_t DosPath[PathSize];
    const size_t Chars = std::min<size_t>(Params->ImagePathName.Length / sizeof(wchar_t), PathSize - 1);
    memcpy(DosPath, reinterpret_cast<const wchar_t*>(Params->ImagePathName.Buffer), Chars * sizeof(wchar_t));
    DosPath[Chars] = 0;
    // The folder: up to the last backslash.
    size_t Cut = Chars;
    while (Cut && DosPath[Cut - 1] != L'\\' && DosPath[Cut - 1] != L'/') {
      --Cut;
    }
    if (Cut <= 1) {
      Why = "image path has no folder";
      return {};
    }
    DosPath[Cut - 1] = 0;

    UNICODE_STRING NtName {};
    if (!RtlDosPathNameToNtPathName_U(DosPath, &NtName, nullptr, nullptr)) {
      Why = "RtlDosPathNameToNtPathName_U failed";
      return {};
    }
    UNICODE_STRING NtDll {};
    RtlInitUnicodeString(&NtDll, L"ntdll.dll");
    HMODULE Module {};
    void* Proc {};
    ANSI_STRING ProcName {};
    RtlInitAnsiString(&ProcName, "wine_nt_to_unix_file_name");
    using ToUnixFn = NTSTATUS(WINAPI*)(const OBJECT_ATTRIBUTES*, char*, ULONG*, UINT);
    fextl::string Result;
    if (LdrGetDllHandle(nullptr, 0, &NtDll, &Module) != STATUS_SUCCESS ||
        LdrGetProcedureAddress(Module, &ProcName, 0, &Proc) != STATUS_SUCCESS || !Proc) {
      Why = "ntdll has no wine_nt_to_unix_file_name";
    } else {
      OBJECT_ATTRIBUTES Attr {};
      InitializeObjectAttributes(&Attr, &NtName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
      char Unix[PathSize];
      ULONG Size = sizeof(Unix);
      const NTSTATUS Status = reinterpret_cast<ToUnixFn>(Proc)(&Attr, Unix, &Size, FileOpen);
      if (Status != STATUS_SUCCESS) {
        Why = fextl::fmt::format("wine_nt_to_unix_file_name failed ({:X})", static_cast<uint32_t>(Status));
      } else {
        Unix[sizeof(Unix) - 1] = 0;
        Result = Unix;
        while (Result.size() > 1 && Result.back() == '/') {
          Result.pop_back();
        }
      }
    }
    RtlFreeUnicodeString(&NtName);
    return Result;
  }

  // Big files the game has open that the walk didn't find (it may keep data elsewhere): opened too, from then on.
  struct ScanArgs {
    size_t Added;
  };

  void ScanEntry(const char* Name, void* Arg) {
    auto* A = static_cast<ScanArgs*>(Arg);
    if (FileCount >= MaxFiles) {
      return;
    }
    char Link[48];
    snprintf(Link, sizeof(Link), "/proc/self/fd/%s", Name);
    const int64_t Len = FEX::Windows::Linux::ReadLink(Link, LinkTarget, sizeof(LinkTarget));
    if (Len <= 0 || Excluded(LinkTarget) || Known(Hash(LinkTarget, static_cast<size_t>(Len)))) {
      return;
    }
    FEX::Windows::Linux::FileInfo Info {};
    if (!FEX::Windows::Linux::Stat(Link, Info) || !Info.Regular || Info.Size < MinBytes) {
      return;
    }
    if (Add(LinkTarget, Info.Size)) {
      ++A->Added;
      FEX::Windows::StatsLogNote(fextl::fmt::format("game files: also dropping {} ({} MB), which the game has open", LinkTarget, Info.Size >> 20));
    }
  }

  // The kind of storage the game's folder is on, for the log. Through FUSE (an SD card, or Android's shared storage)
  // reads are passed on to the files under it, whose cache a drop through FUSE may not reach.
  fextl::string Storage(const char* Path) {
    const uint64_t Type = FEX::Windows::Linux::FileSystemType(Path);
    switch (Type) {
    case 0xF2F52010: return "f2fs";
    case 0xEF53: return "ext4";
    case 0xE0F5E1E2: return "erofs";
    case 0x794C7630: return "overlayfs";
    case 0x01021994: return "tmpfs";
    case 0x2011BAB0: return "exFAT";
    case 0x4D44: return "FAT";
    case 0x65735546: return "FUSE, the drops may not reach the cache of the files under it";
    case 0x5DCA2DF5: return "sdcardfs, the drops may not reach the cache of the files under it";
    case 0: return "unknown storage";
    default: return fextl::fmt::format("file system {:X}", Type);
    }
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
    fextl::string Why;
    const fextl::string Folder = GameFolder(Why);
    if (!Folder.empty() && Folder.size() < PathSize) {
      memcpy(WalkPath, Folder.c_str(), Folder.size() + 1);
      WalkDir(Folder.size(), 0);
      FEX::Windows::StatsLogNote(fextl::fmt::format(
        "game files: {} files of {} MB or more in {} ({:.1f} GB on {}{}), dropped from the file cache "
        "every {} ms{}{}",
        FileCount, MinBytes >> 20, Folder, static_cast<double>(FileBytes) / (1024.0 * 1024.0 * 1024.0), Storage(Folder.c_str()),
        WalkEntries > MaxWalkEntries ? ", folder too big to look at all of it" : "", IntervalMs, FirstNames.empty() ? "" : ": ", FirstNames));
    } else {
      FEX::Windows::StatsLogNote(fextl::fmt::format("game files: can't find the game's folder ({}); only big files the game has open are "
                                                    "dropped",
                                                    Folder.empty() ? Why : fextl::string("path too long")));
    }

    for (uint64_t Pass = 0;; ++Pass) {
      const uint64_t Begin = GetCounter();
      // Every 2 s, look for big files the game has open that aren't in the list.
      if (Pass % 4 == 0) {
        ScanArgs Args {};
        FEX::Windows::Linux::ForEachEntry("/proc/self/fd", ScanEntry, &Args);
      }
      uint64_t Failed = 0;
      if (FileCount) {
        const uint64_t Before = FileCacheKB();
        for (size_t i = 0; i < FileCount; ++i) {
          if (FEX::Windows::Linux::DropFromFileCache(Files[i].Fd, 0, 0) < 0) {
            ++Failed;
          }
        }
        const uint64_t After = FileCacheKB();
        if (Before && After && After < Before) {
          Stats.ShrankKB.fetch_add(Before - After, std::memory_order_relaxed);
        }
      }
      Stats.Files.store(static_cast<uint32_t>(FileCount), std::memory_order_relaxed);
      Stats.Drops.fetch_add(FileCount - Failed, std::memory_order_relaxed);
      Stats.Failed.fetch_add(Failed, std::memory_order_relaxed);
      Stats.Passes.fetch_add(1, std::memory_order_relaxed);
      const uint64_t Ns = (GetCounter() - Begin) * 1'000'000'000 / CounterFreq;
      uint64_t Longest = Stats.LongestNs.load(std::memory_order_relaxed);
      while (Ns > Longest && !Stats.LongestNs.compare_exchange_weak(Longest, Ns, std::memory_order_relaxed)) {}
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
    const uint32_t Count = Stats.Files.load(std::memory_order_relaxed);
    if (!Passes) {
      return {};
    }
    return fextl::fmt::format("  game files: {} files, file cache {:.0f} MB smaller after the drops ({} drops in {} passes, longest {:.1f} "
                              "ms{})\n",
                              Count, static_cast<double>(ShrankKB) / 1024.0, Drops, Passes, static_cast<double>(LongestNs) / 1e6,
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
  FEX::Windows::StatsLogNote(fextl::fmt::format("game files: the game's files of {} MB or more are dropped from the file cache every {} ms "
                                                "by "
                                                "FEXFileCache (FEX_GAME_FILES_UNCACHED=0 turns it off, FEX_GAME_FILES_MS sets the time)",
                                                MinBytes >> 20, IntervalMs));
  if (StatsLog) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(FileCacheThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXFileCache");
}
} // namespace GameFiles
