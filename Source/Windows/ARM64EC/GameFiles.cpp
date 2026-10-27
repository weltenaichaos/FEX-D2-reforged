// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Drops the game's big files from Android's file cache, so its reads don't push other apps out of memory
$end_info$
*/

#include "GameFiles.h"
#include "FrameLatency.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
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

extern "C" {
// Module.S ExitFunctionEC: while [0] is non-zero, a call from x64 code into one of the ReadFile entries [0..2] runs
// FEXNativeReadFile (NativeReadFileTarget) instead, after storing the entry it called in [3]. Entries not found repeat
// [0].
alignas(64) uint64_t FEXReadWatch[4];
BOOL FEXNativeReadFile(HANDLE File, void* Buffer, DWORD Bytes, DWORD* Read, uint64_t Lr, uint64_t Sp);
extern void* X64ReturnInstr; // Module.cpp
// NativeRedirect.cpp: [0] = frame starts counted so far (0 until the frame clock is found), then a ring of 1024
// CNTVCT_EL0 times.
extern uint64_t FEXFrameMarks[];
}

namespace GameFiles {
namespace {
  constexpr size_t MaxFiles = 256;
  constexpr uint64_t MinBytes = 32ull << 20; // smaller files don't matter for the file cache
  constexpr int MaxDepth = 4;                // <game>/Data/data/data.000 is depth 3
  constexpr size_t MaxWalkEntries = 20'000;
  constexpr size_t PathSize = 1024;
  constexpr uint32_t FileOpen = 1; // FILE_OPEN

  // Build 82, the read watch: the watched files are cut into 1 MB pieces, each with the time of its last read on a clock
  // that counts the bytes read.
  constexpr unsigned PieceShift = 20;
  constexpr size_t MaxPieces = 64 * 1024; // 64 GB of files; files past that are dropped whole
  constexpr size_t HandleSlots = 32;      // handles recently looked up
  constexpr uint64_t HandleKeepMs = 500;  // how long a looked up handle is trusted (a closed handle's number is reused)
  // How much was read between two reads of the same piece: < 128 MB, 512 MB, 1, 2, 4, 8 GB, more, never read before.
  constexpr size_t Buckets = 8;
  constexpr uint64_t BucketLimitMB[Buckets - 2] = {128, 512, 1024, 2048, 4096, 8192};
  constexpr const char* BucketNames[Buckets] = {"<128M", "<512M", "<1G", "<2G", "<4G", "<8G", "more", "first"};
  constexpr size_t MaxSlowReads = 6; // per second
  constexpr double SlowReadMs = 8.0;
  constexpr uint32_t ErrorIoPending = 997;
  constexpr uint32_t ClassInternal = 6, ClassPosition = 14; // FileInternalInformation, FilePositionInformation
  constexpr size_t TebLastError = 0x68;

  struct Counters {
    std::atomic<uint64_t> Passes;
    std::atomic<uint64_t> Drops;     // drop calls (one fadvise each)
    std::atomic<uint64_t> Failed;    // fadvise calls that failed
    std::atomic<uint64_t> ShrankKB;  // how much the file cache shrank across the drops
    std::atomic<uint64_t> LongestNs; // longest pass
    std::atomic<uint32_t> Files;     // files dropped at the last pass
    // Build 82, at the last pass: pieces kept because anyone read them lately, because the main thread did, or because a
    // read ended in the piece before (the kernel's readahead).
    std::atomic<uint32_t> KeptRecent, KeptMain, KeptAhead;
    std::atomic<uint32_t> KeptLearned; // build 89: kept only because they are learned
    std::atomic<uint64_t> GuardPasses; // passes that dropped everything for low memory
    std::atomic<uint64_t> AvailableMB; // the device's available memory at the last pass
  };

  struct File {
    int Fd;
    uint64_t Hash;
    int Watch; // index into Watched, -1 when its reads aren't followed
  };

  // A file the read watch follows. Written by FEXFileCache before WatchedCount counts it, read by the game's threads.
  struct WatchedFile {
    uint64_t Inode;
    uint32_t FirstPiece; // its pieces in the stamp arrays
    uint32_t Pieces;
    int Fd;              // FEXFileCache's own descriptor of it (open for good), for asking storage ahead
    uint64_t Size;
    char Name[32];
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
  uint32_t PiecesUsed {};

  WatchedFile Watched[MaxFiles];
  std::atomic<uint32_t> WatchedCount {};
  // The all-reads clock at the end of each piece's last read (0 = never read), the main thread's own clock at the end of
  // its last read of it, and the all-reads clock when a read last ended in the piece before.
  std::atomic<uint64_t> AllStamps[MaxPieces];
  std::atomic<uint64_t> MainStamps[MaxPieces];
  std::atomic<uint64_t> AheadStamps[MaxPieces];
  std::atomic<uint64_t> AllClock {}, MainClock {}; // bytes read from the watched files by anyone / by the main thread

  // The keep budget (bytes): pieces read lately by anyone, pieces the main thread read lately. 0 = drop everything.
  uint64_t KeepRecent {}, KeepMain {};
  uint64_t KeepMB {512};
  uint64_t GuardMB {2048};
  bool ReadWatchOn {};
  std::atomic<bool> Armed {};
  uint64_t MainTeb {};

  // Build 87, FEX_GAME_FILES_AHEAD: a read of AheadMin bytes or more by the main thread is asked of storage whole before
  // ReadFile runs, in AheadPart pieces (one WILLNEED each; the kernel takes at most its read-ahead size per call). In the
  // build 82-86 logs the main thread's cold reads went at ~180 MB/s: a 3968 KB read at an area's entry took 18-23 ms in
  // every run. A buffered read the kernel doesn't have reads ahead one read-ahead window at a time, mostly with one
  // request at storage, while UFS storage reads several GB/s with many requests queued.
  uint64_t AheadMin {};
  std::atomic<uint64_t> AheadPart {128 << 10}; // set once by FEXFileCache from the device's settings

  // Build 89, FEX_GAME_FILES_LEARN (see GameFiles.h): ranges of the watched files the main thread waited for storage for
  // during play, remembered across sessions, asked of storage at the start and at loading screens, and kept.
  constexpr size_t MaxLearned = 1024;       // ranges in the list
  constexpr uint64_t LearnBlock = 64 << 10; // ranges cover whole 64 KB blocks
  constexpr uint32_t ForgetSessions = 30;   // a range the main thread didn't read in this many sessions is dropped
  constexpr size_t MaxCandidates = 128;     // the main thread's storage reads waiting for their frame to end
  constexpr uint64_t PlayFrameMs = 250;     // a frame this long or longer is a loading screen's (FEX_CODE_BUFFER_LOAD_MS)
  constexpr size_t MarkRing = 1024;         // NativeRedirect.cpp FrameMarkRing
  constexpr uint64_t SaveEveryMs = 3'000;   // the list is written at most this often, when it changed
  constexpr size_t MaxListBytes = 1 << 20;
  constexpr uint8_t PieceLearned = 1, PieceUsed = 2; // LearnedPieces bits; used = read by the main thread this session

  struct LearnedRange {
    char Name[32];     // the file's name
    uint64_t FileSize; // its size when learned: a game update that changes it drops the range
    uint64_t Offset;
    uint64_t Bytes;
    uint32_t Seen;  // sessions in which the main thread read it
    uint32_t Last;  // the last of them
    int32_t Watch;  // the watched file in this session, -1 when it isn't here
    bool New;       // learned in this session
    bool Counted;   // this session is counted in Seen
  };

  struct Candidate {
    int32_t Watch;
    uint64_t Offset;
    uint64_t Bytes;
    uint64_t Frame; // frame marks written when the read ended: its frame began at mark Frame - 1
  };

  struct LearnCounters {
    // Per second.
    std::atomic<uint64_t> NewRanges, NewBytes; // ranges added, bytes added (new ranges and grown ones)
    std::atomic<uint64_t> Skipped;             // storage reads in a loading screen's frame, not learned
    std::atomic<uint64_t> Lost;                // storage reads not noted (no room)
    std::atomic<uint64_t> Asks, AskBytes, AskStorageKB, AskNs, AskCalls;
    std::atomic<uint64_t> Evicted;             // ranges dropped for the budget
    std::atomic<uint64_t> SaveFailed;
    // Now: ranges in the list, of them in files here, read by the main thread this session, learned this session; the
    // bytes of the ranges here and the bytes learned this session.
    std::atomic<uint32_t> ListRanges, HereRanges, UsedRanges, SessionRanges;
    std::atomic<uint64_t> HereBytes, SessionBytes;
  };
  LearnCounters Learning {};
  uint64_t LearnBudget {}; // bytes, 0 = off
  fextl::string LearnFile;
  uint32_t Session {1};
  LearnedRange Ranges[MaxLearned]; // FEXFileCache only
  size_t RangeCount {};
  bool LearnDirty {};
  uint64_t LastSave {};
  std::atomic<uint8_t> LearnedPieces[MaxPieces];
  std::mutex CandidateLock;
  Candidate Candidates[MaxCandidates];
  size_t CandidateCount {};

  struct HandleSlot {
    uint64_t Handle;
    int32_t Watch;
    uint64_t At; // counter ticks
  };

  // Build 88: what the main thread's thread did during one of its reads (getrusage RUSAGE_THREAD before and after), and
  // the storage's clock before and after (MHz, 0 = unknown). The build 87 log had the main thread's big reads of game data
  // at ~230 MB/s whether or not storage was asked for the whole range first.
  struct ReadUsage {
    bool Have;
    float CpuMs; // user + kernel
    float SystemMs;
    uint32_t StorageKB; // read from storage by this thread (its reads and the readahead they started)
    uint32_t MinorFaults, MajorFaults;
    uint32_t Slept, Preempted;
    uint32_t ClockBeforeMHz, ClockAfterMHz;
  };

  struct SlowRead {
    int32_t Watch;
    uint32_t OffsetMB;
    uint32_t KB;
    float Ms;
    uint8_t Bucket;
    bool Kept;
    bool Learned; // build 89: it read learned data
    ReadUsage U;
  };

  // Per second, under ReadLock.
  struct ReadCounts {
    uint64_t Reads, Bytes;            // all reads of watched files
    uint64_t Queued, QueuedBytes;     // ... that went on in the background (overlapped, pending)
    uint64_t MainReads, MainBytes, MainTicks, MainLongest;
    uint64_t MainKeptReads, MainKeptBytes, MainKeptTicks; // ... whose pieces were all kept
    uint64_t OtherReads, OtherBytes, OtherTicks, OtherLongest; // other threads' reads that waited
    uint64_t OtherKeptReads, OtherKeptBytes, OtherKeptTicks;
    uint64_t ReuseMain[Buckets];    // MB*2^20 of the main thread's reads by how much anyone read since the piece's last read
    uint64_t ReuseMainOwn[Buckets]; // ... by how much the main thread read since
    uint64_t ReuseOther[Buckets];   // other threads' reads by how much anyone read since
    uint64_t OtherFiles, OtherFilesBytes; // reads of files that aren't watched
    uint64_t NoOffset;                    // reads of watched files whose offset couldn't be told
    // Build 87: the main thread's reads asked ahead (FEX_GAME_FILES_AHEAD), their bytes, the time the asking took, its
    // calls, and the reads' own wait.
    uint64_t AheadReads, AheadBytes, AheadAskTicks, AheadCalls, AheadReadTicks, AheadLongest;
    // Build 88: the main thread's reads of watched files measured: CPU time during them, KB it read from storage.
    uint64_t MainMeasured, MainCpuUs, MainStorageKB;
    // Build 89: the main thread's reads of learned data (FEX_GAME_FILES_LEARN), their wait and what came from storage.
    uint64_t LearnedReads, LearnedBytes, LearnedTicks, LearnedLongest, LearnedStorageKB;
    SlowRead Slow[MaxSlowReads];
    size_t SlowCount;
    uint64_t SlowMore;
  };
  std::mutex ReadLock;
  ReadCounts Reads {};
  HandleSlot Handles[HandleSlots] {};
  size_t NextHandle {};

  // x64 layouts of what the game passes.
  struct X64Overlapped {
    uint64_t Internal;
    uint64_t InternalHigh;
    uint32_t Offset;
    uint32_t OffsetHigh;
    uint64_t Event;
  };
  struct InternalInformation {
    int64_t IndexNumber;
  };
  struct PositionInformation {
    int64_t CurrentByteOffset;
  };

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

  // Follows the reads of a file of Size bytes from now on, if there is room. Its index, or -1.
  int Watch(const char* Path, uint64_t Size, uint64_t Inode, int Fd) {
    const uint32_t Count = WatchedCount.load(std::memory_order_relaxed);
    const uint64_t Pieces = (Size >> PieceShift) + 1;
    if (!ReadWatchOn || !Inode || Count >= MaxFiles || PiecesUsed + Pieces > MaxPieces) {
      return -1;
    }
    auto& W = Watched[Count];
    W.Inode = Inode;
    W.FirstPiece = PiecesUsed;
    W.Pieces = static_cast<uint32_t>(Pieces);
    W.Fd = Fd;
    W.Size = Size;
    const char* Slash = strrchr(Path, '/');
    snprintf(W.Name, sizeof(W.Name), "%s", Slash ? Slash + 1 : Path);
    PiecesUsed += static_cast<uint32_t>(Pieces);
    WatchedCount.store(Count + 1, std::memory_order_release);
    return static_cast<int>(Count);
  }

  // Opens Path (a regular file of Size bytes) for dropping. False when it was known, excluded or can't be opened.
  bool Add(const char* Path, uint64_t Size, uint64_t Inode) {
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
    Files[FileCount++] = {Fd, Canonical, Watch(Path, Size, Inode, Fd)};
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
        Add(WalkPath, Info.Size, Info.Inode);
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
    if (Add(LinkTarget, Info.Size, Info.Inode)) {
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

  fextl::string StorageReadAhead(const char* Path);
  void FindStorageClock();

  // The kernel's file cache ("Cached") and the device's available memory ("MemAvailable") from /proc/meminfo, in KB; 0
  // when unreadable.
  struct MemInfo {
    uint64_t CachedKB;
    uint64_t AvailableKB;
  };

  MemInfo ReadMemInfo() {
    char Buffer[4096];
    if (FEX::Windows::Linux::ReadSmallFile("/proc/meminfo", Buffer, sizeof(Buffer)) <= 0) {
      return {};
    }
    const char* Cached = strstr(Buffer, "\nCached:");
    const char* Available = strstr(Buffer, "\nMemAvailable:");
    return {Cached ? strtoull(Cached + 8, nullptr, 10) : 0, Available ? strtoull(Available + 14, nullptr, 10) : 0};
  }

  // Whether a piece last read at Stamp is among the last Budget bytes read on a clock now at Now.
  bool Within(uint64_t Stamp, uint64_t Now, uint64_t Budget) {
    return Stamp && Budget && (Stamp >= Now || Now - Stamp <= Budget);
  }

  size_t BucketOf(uint64_t Stamp, uint64_t Now) {
    if (!Stamp) {
      return Buckets - 1;
    }
    const uint64_t Since = Stamp >= Now ? 0 : Now - Stamp;
    for (size_t b = 0; b < Buckets - 2; ++b) {
      if (Since < (BucketLimitMB[b] << 20)) {
        return b;
      }
    }
    return Buckets - 2;
  }

  // Finds kernelbase's (and kernel32's own, if any) ReadFile and switches the check in ExitFunctionEC on. Kernelbase
  // isn't loaded yet when FEX's process init runs, so FEXFileCache tries until it is.
  bool Arm() {
    const wchar_t* Modules[] = {L"kernelbase.dll", L"kernel32.dll"};
    uint64_t Found[3] {};
    size_t Count {};
    for (const auto* Name : Modules) {
      const uint64_t Base = FrameLatency::FindModule(Name);
      uint64_t Entries[3] {};
      const size_t N = Base ? FrameLatency::FindExportEntries(Base, "ReadFile", Entries) : 0;
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
    __atomic_store_n(&FEXReadWatch[2], Found[Count > 2 ? 2 : 0], __ATOMIC_RELAXED);
    __atomic_store_n(&FEXReadWatch[1], Found[Count > 1 ? 1 : 0], __ATOMIC_RELAXED);
    __atomic_store_n(&FEXReadWatch[0], Found[0], __ATOMIC_RELEASE);
    Armed.store(true, std::memory_order_release);
    fextl::string Text;
    for (size_t i = 0; i < Count; ++i) {
      Text += fextl::fmt::format(" {:X}", Found[i]);
    }
    FEX::Windows::StatsLogNote(fextl::fmt::format("game files: ReadFile found (entries{}); the game's reads of its {} watched files are "
                                                  "now followed",
                                                  Text, WatchedCount.load(std::memory_order_relaxed)));
    return true;
  }

  // Which piece to keep at a pass, by reason (0 read lately, 1 read lately by the main thread, 2 readahead, 3 learned),
  // or -1.
  int KeepReason(uint32_t Piece, uint64_t AllNow, uint64_t MainNow) {
    if (Within(AllStamps[Piece].load(std::memory_order_relaxed), AllNow, KeepRecent)) {
      return 0;
    }
    if (Within(MainStamps[Piece].load(std::memory_order_relaxed), MainNow, KeepMain)) {
      return 1;
    }
    if (Within(AheadStamps[Piece].load(std::memory_order_relaxed), AllNow, KeepRecent)) {
      return 2;
    }
    if (LearnBudget && (LearnedPieces[Piece].load(std::memory_order_relaxed) & PieceLearned)) {
      return 3;
    }
    return -1;
  }

  // One drop call; counts it.
  void Drop(int Fd, uint64_t Offset, uint64_t Size, uint64_t& Calls, uint64_t& Failed) {
    ++Calls;
    if (FEX::Windows::Linux::DropFromFileCache(Fd, Offset, Size) < 0) {
      ++Failed;
    }
  }

  double MB(uint64_t Bytes);

  // Build 89, the learned list (FEX_GAME_FILES_LEARN). The frame clock: marks written so far, and the time of mark Index.
  uint64_t FrameCount() {
    return __atomic_load_n(&FEXFrameMarks[0], __ATOMIC_ACQUIRE);
  }

  uint64_t FrameMark(uint64_t Index) {
    return __atomic_load_n(&FEXFrameMarks[1 + (Index & (MarkRing - 1))], __ATOMIC_RELAXED);
  }

  // Game threads: the main thread's read of Bytes at Offset of watched file Watch waited for storage.
  void PushCandidate(int32_t Watch, uint64_t Offset, uint64_t Bytes) {
    const uint64_t Frame = FrameCount();
    if (!Frame) {
      return; // no frame clock yet (start-up)
    }
    std::scoped_lock Guard {CandidateLock};
    if (CandidateCount < MaxCandidates) {
      Candidates[CandidateCount++] = {Watch, Offset, Bytes, Frame};
    } else {
      Learning.Lost.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // The pieces a range covers, as [First, Last] indices into the stamp arrays.
  void RangePieces(const LearnedRange& R, uint32_t& First, uint32_t& Last) {
    const auto& W = Watched[R.Watch];
    First = W.FirstPiece + static_cast<uint32_t>(R.Offset >> PieceShift);
    Last = W.FirstPiece + static_cast<uint32_t>(std::min<uint64_t>((R.Offset + R.Bytes - 1) >> PieceShift, W.Pieces - 1));
  }

  // FEXFileCache: marks the pieces of every range in a file that is here, after ranges went.
  void MarkLearnedPieces() {
    for (uint32_t p = 0; p < PiecesUsed; ++p) {
      LearnedPieces[p].fetch_and(static_cast<uint8_t>(~PieceLearned), std::memory_order_relaxed);
    }
    for (size_t i = 0; i < RangeCount; ++i) {
      if (Ranges[i].Watch < 0) {
        continue;
      }
      uint32_t First, Last;
      RangePieces(Ranges[i], First, Last);
      for (uint32_t p = First; p <= Last; ++p) {
        LearnedPieces[p].fetch_or(PieceLearned, std::memory_order_relaxed);
      }
    }
  }

  bool RangeUsed(const LearnedRange& R) {
    if (R.Watch < 0) {
      return false;
    }
    uint32_t First, Last;
    RangePieces(R, First, Last);
    for (uint32_t p = First; p <= Last; ++p) {
      if (LearnedPieces[p].load(std::memory_order_relaxed) & PieceUsed) {
        return true;
      }
    }
    return false;
  }

  uint64_t BytesHere() {
    uint64_t Bytes {};
    for (size_t i = 0; i < RangeCount; ++i) {
      Bytes += Ranges[i].Watch >= 0 ? Ranges[i].Bytes : 0;
    }
    return Bytes;
  }

  // The range least worth keeping among those not read this session: read longest ago, then in the fewest sessions,
  // then the biggest. RangeCount when there is none.
  size_t LeastWorth() {
    size_t Worst = RangeCount;
    for (size_t i = 0; i < RangeCount; ++i) {
      const auto& R = Ranges[i];
      if (R.New || R.Counted) {
        continue;
      }
      if (Worst == RangeCount) {
        Worst = i;
        continue;
      }
      const auto& B = Ranges[Worst];
      if (R.Last != B.Last ? R.Last < B.Last : R.Seen != B.Seen ? R.Seen < B.Seen : R.Bytes > B.Bytes) {
        Worst = i;
      }
    }
    return Worst;
  }

  void RemoveRange(size_t i) {
    Ranges[i] = Ranges[--RangeCount];
    LearnDirty = true;
  }

  // Past the budget, the ranges least worth keeping go (ranges read this session stay).
  void FitBudget() {
    bool Removed = false;
    while (BytesHere() > LearnBudget) {
      const size_t Worst = LeastWorth();
      if (Worst == RangeCount) {
        break;
      }
      RemoveRange(Worst);
      Learning.Evicted.fetch_add(1, std::memory_order_relaxed);
      Removed = true;
    }
    if (Removed) {
      MarkLearnedPieces();
    }
  }

  void UpdateLearnTotals() {
    uint32_t Here {}, Used {}, New {};
    for (size_t i = 0; i < RangeCount; ++i) {
      const auto& R = Ranges[i];
      Here += R.Watch >= 0 ? 1 : 0;
      Used += R.Counted ? 1 : 0;
      New += R.New ? 1 : 0;
    }
    Learning.ListRanges.store(static_cast<uint32_t>(RangeCount), std::memory_order_relaxed);
    Learning.HereRanges.store(Here, std::memory_order_relaxed);
    Learning.UsedRanges.store(Used, std::memory_order_relaxed);
    Learning.SessionRanges.store(New, std::memory_order_relaxed);
    Learning.HereBytes.store(BytesHere(), std::memory_order_relaxed);
  }

  // Adds a read to the list: a new range, or an existing one of the same file it overlaps or touches grows.
  void Learn(const Candidate& C) {
    const auto& W = Watched[C.Watch];
    const uint64_t Start = C.Offset & ~(LearnBlock - 1);
    const uint64_t End = std::min<uint64_t>((C.Offset + C.Bytes + LearnBlock - 1) & ~(LearnBlock - 1), W.Size);
    if (End <= Start) {
      return;
    }
    LearnedRange* Into = nullptr;
    for (size_t i = 0; i < RangeCount; ++i) {
      auto& R = Ranges[i];
      if (R.Watch == C.Watch && Start <= R.Offset + R.Bytes && End >= R.Offset) {
        Into = &R;
        break;
      }
    }
    uint64_t Grew {};
    if (Into) {
      const uint64_t NewStart = std::min(Into->Offset, Start);
      const uint64_t NewEnd = std::max(Into->Offset + Into->Bytes, End);
      Grew = (NewEnd - NewStart) - Into->Bytes;
      Into->Offset = NewStart;
      Into->Bytes = NewEnd - NewStart;
    } else {
      if (RangeCount == MaxLearned) {
        const size_t Worst = LeastWorth();
        if (Worst == RangeCount) {
          Learning.Lost.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        RemoveRange(Worst);
        Learning.Evicted.fetch_add(1, std::memory_order_relaxed);
        MarkLearnedPieces();
      }
      Into = &Ranges[RangeCount++];
      *Into = {};
      snprintf(Into->Name, sizeof(Into->Name), "%s", W.Name);
      Into->FileSize = W.Size;
      Into->Offset = Start;
      Into->Bytes = End - Start;
      Into->Watch = C.Watch;
      Into->New = true;
      Grew = End - Start;
      Learning.NewRanges.fetch_add(1, std::memory_order_relaxed);
    }
    if (!Grew) {
      return;
    }
    Learning.NewBytes.fetch_add(Grew, std::memory_order_relaxed);
    Learning.SessionBytes.fetch_add(Grew, std::memory_order_relaxed);
    // Learned, and read this session (it just was): kept from now on.
    uint32_t First, Last;
    RangePieces(*Into, First, Last);
    for (uint32_t p = First; p <= Last; ++p) {
      LearnedPieces[p].fetch_or(static_cast<uint8_t>(PieceLearned | PieceUsed), std::memory_order_relaxed);
    }
    LearnDirty = true;
    FitBudget();
  }

  // The candidates whose frame ended: learned when it was a frame of play, left out when it was a loading screen's.
  void TakeCandidates() {
    Candidate Local[MaxCandidates];
    size_t Count {};
    {
      std::scoped_lock Guard {CandidateLock};
      Count = CandidateCount;
      std::copy(Candidates, Candidates + Count, Local);
      CandidateCount = 0;
    }
    if (!Count) {
      return;
    }
    const uint64_t Marks = FrameCount();
    const uint64_t Now = GetCounter();
    const uint64_t PlayTicks = CounterFreq * PlayFrameMs / 1000;
    size_t Waiting {};
    for (size_t i = 0; i < Count; ++i) {
      const auto& C = Local[i];
      if (Marks - C.Frame >= MarkRing - 1) {
        Learning.Skipped.fetch_add(1, std::memory_order_relaxed); // too long ago to tell
        continue;
      }
      const uint64_t Began = FrameMark(C.Frame - 1);
      if (Marks > C.Frame) {
        const uint64_t Ended = FrameMark(C.Frame);
        if (Ended > Began && Ended - Began < PlayTicks) {
          Learn(C);
        } else {
          Learning.Skipped.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (Now > Began && Now - Began >= PlayTicks) {
        Learning.Skipped.fetch_add(1, std::memory_order_relaxed); // its frame is a loading screen's
      } else {
        Local[Waiting++] = C; // its frame hasn't ended yet
      }
    }
    if (Waiting) {
      std::scoped_lock Guard {CandidateLock};
      for (size_t i = 0; i < Waiting; ++i) {
        if (CandidateCount < MaxCandidates) {
          Candidates[CandidateCount++] = Local[i];
        } else {
          Learning.Lost.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }

  // Ranges the main thread read this session count it once (Seen, Last).
  void CountUsed() {
    for (size_t i = 0; i < RangeCount; ++i) {
      auto& R = Ranges[i];
      if (!R.Counted && (R.New || RangeUsed(R))) {
        R.Counted = true;
        ++R.Seen;
        R.Last = Session;
        LearnDirty = true;
      }
    }
  }

  // Asks storage for every range in a file that is here (WILLNEED, the kernel's read-ahead size per call); pages already
  // in the file cache cost only the look-up.
  void AskLearned() {
    FEX::Windows::Linux::ThreadUsage Before {}, After {};
    const bool Measure = FEX::Windows::Linux::GetThreadUsage(Before);
    const uint64_t Begin = GetCounter();
    const uint64_t Part = AheadPart.load(std::memory_order_relaxed);
    uint64_t Bytes {}, Calls {};
    for (size_t i = 0; i < RangeCount; ++i) {
      const auto& R = Ranges[i];
      if (R.Watch < 0) {
        continue;
      }
      const int Fd = Watched[R.Watch].Fd;
      const uint64_t End = R.Offset + R.Bytes;
      for (uint64_t At = R.Offset; At < End; At += Part) {
        FEX::Windows::Linux::ReadAhead(Fd, At, std::min(Part, End - At));
        ++Calls;
      }
      Bytes += R.Bytes;
    }
    const uint64_t Ns = (GetCounter() - Begin) * 1'000'000'000 / CounterFreq;
    if (Measure && FEX::Windows::Linux::GetThreadUsage(After)) {
      Learning.AskStorageKB.fetch_add((After.InBlocks - Before.InBlocks) / 2, std::memory_order_relaxed);
    }
    Learning.Asks.fetch_add(1, std::memory_order_relaxed);
    Learning.AskBytes.fetch_add(Bytes, std::memory_order_relaxed);
    Learning.AskCalls.fetch_add(Calls, std::memory_order_relaxed);
    Learning.AskNs.fetch_add(Ns, std::memory_order_relaxed);
  }

  // A decimal number at P, moving P past it and the spaces after; false when there is none.
  bool ParseNumber(const char*& P, uint64_t& Out) {
    if (*P < '0' || *P > '9') {
      return false;
    }
    char* After {};
    Out = strtoull(P, &After, 10);
    P = After;
    while (*P == ' ') {
      ++P;
    }
    return true;
  }

  struct LoadResult {
    bool Found;      // the file was there
    bool Bad;        // ... but not a list this build reads
    uint32_t Loaded; // ranges loaded (in files here or not)
    uint32_t Missing, Changed, Forgotten, Skipped;
  };

  // Reads the list. Format: "FEX game reads learned 1", "session N", then "name file-size offset bytes seen last" lines.
  LoadResult LoadLearned() {
    LoadResult Result {};
    fextl::string Text;
    if (!Store::ReadText(LearnFile, Text, MaxListBytes)) {
      return Result;
    }
    Result.Found = true;
    constexpr const char* Header = "FEX game reads learned 1\n";
    if (Text.compare(0, strlen(Header), Header) != 0) {
      Result.Bad = true;
      return Result;
    }
    const char* P = Text.c_str() + strlen(Header);
    uint64_t Previous {};
    if (strncmp(P, "session ", 8) != 0) {
      Result.Bad = true;
      return Result;
    }
    P += 8;
    if (!ParseNumber(P, Previous)) {
      Result.Bad = true;
      return Result;
    }
    Session = static_cast<uint32_t>(std::min<uint64_t>(Previous, 1u << 30)) + 1;
    while (*P) {
      const char* Line = P;
      const char* End = strchr(Line, '\n');
      P = End ? End + 1 : Line + strlen(Line);
      const char* Space = static_cast<const char*>(memchr(Line, ' ', End ? static_cast<size_t>(End - Line) : strlen(Line)));
      if (!Space || Space == Line || static_cast<size_t>(Space - Line) >= sizeof(LearnedRange::Name)) {
        Result.Skipped += End && End == Line ? 0 : 1;
        continue;
      }
      LearnedRange R {};
      memcpy(R.Name, Line, static_cast<size_t>(Space - Line));
      const char* Q = Space + 1;
      uint64_t FileSize, Offset, Bytes, Seen, Last;
      if (!ParseNumber(Q, FileSize) || !ParseNumber(Q, Offset) || !ParseNumber(Q, Bytes) || !ParseNumber(Q, Seen) || !ParseNumber(Q, Last) ||
          !Bytes || Offset + Bytes > FileSize) {
        ++Result.Skipped;
        continue;
      }
      if (Last + ForgetSessions < Session) {
        ++Result.Forgotten;
        continue;
      }
      R.FileSize = FileSize;
      R.Offset = Offset;
      R.Bytes = Bytes;
      R.Seen = static_cast<uint32_t>(Seen);
      R.Last = static_cast<uint32_t>(Last);
      R.Watch = -1;
      bool Changed = false;
      const uint32_t Count = WatchedCount.load(std::memory_order_relaxed);
      for (uint32_t w = 0; w < Count; ++w) {
        if (!strcmp(Watched[w].Name, R.Name)) {
          if (Watched[w].Size == FileSize) {
            R.Watch = static_cast<int32_t>(w);
          } else {
            Changed = true;
          }
          break;
        }
      }
      if (Changed) {
        ++Result.Changed; // the game was updated: its data moved
        continue;
      }
      Result.Missing += R.Watch < 0 ? 1 : 0;
      if (RangeCount < MaxLearned) {
        Ranges[RangeCount++] = R;
        ++Result.Loaded;
      }
    }
    return Result;
  }

  // Writes the list when it changed, at most every SaveEveryMs unless Now.
  void SaveLearned(bool Now) {
    if (!LearnDirty || LearnFile.empty()) {
      return;
    }
    const uint64_t At = GetCounter();
    if (!Now && LastSave && At - LastSave < SaveEveryMs * CounterFreq / 1000) {
      return;
    }
    LastSave = At;
    fextl::string Text = fextl::fmt::format("FEX game reads learned 1\nsession {}\n", Session);
    for (size_t i = 0; i < RangeCount; ++i) {
      const auto& R = Ranges[i];
      if (strchr(R.Name, ' ')) {
        continue;
      }
      Text += fextl::fmt::format("{} {} {} {} {} {}\n", R.Name, R.FileSize, R.Offset, R.Bytes, R.Seen, R.Last);
    }
    if (Store::WriteText(LearnFile, Text)) {
      LearnDirty = false;
    } else {
      Learning.SaveFailed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // At FEXFileCache's start, once the game's files are watched: loads the list (storage is asked for it at the first pass
  // that keeps pieces, see LearnPass).
  void StartLearning(const fextl::string& Device) {
    const LoadResult L = LoadLearned();
    FitBudget();
    MarkLearnedPieces();
    UpdateLearnTotals();
    LearnDirty = true; // the session number moved on
    fextl::string From;
    if (!L.Found) {
      From = "no list yet: this session learns, the next one reads it in";
    } else if (L.Bad) {
      From = "the list there isn't one this build reads, so it starts over";
    } else {
      From = fextl::fmt::format(
        "{} ranges {:.1f} MB in files here from earlier sessions{}{}{}{}", Learning.HereRanges.load(), MB(Learning.HereBytes.load()),
        L.Missing ? fextl::fmt::format(", {} in files not here", L.Missing) : fextl::string(),
        L.Changed ? fextl::fmt::format(", {} dropped (their file changed size: a game update)", L.Changed) : fextl::string(),
        L.Forgotten ? fextl::fmt::format(", {} dropped (not read in {} sessions)", L.Forgotten, ForgetSessions) : fextl::string(),
        L.Skipped ? fextl::fmt::format(", {} lines unreadable", L.Skipped) : fextl::string());
    }
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "game files: learned reads: the main thread's reads that wait for storage during play (a frame under {} ms) are remembered in {} "
      "and asked of storage at the start and at every loading screen, then kept in the file cache (budget {} MB; FEX_GAME_FILES_LEARN=0 "
      "turns it off); session {}: {} ({})",
      PlayFrameMs, LearnFile, LearnBudget >> 20, Session, From, Device));
  }

  // Files the game opened after the start (found among its open files) can hold ranges of the list: they are matched
  // by name and size when the watched files grew.
  void MatchNewFiles(uint32_t& Matched) {
    const uint32_t Count = WatchedCount.load(std::memory_order_acquire);
    if (Count == Matched) {
      return;
    }
    bool Found = false;
    for (size_t i = 0; i < RangeCount; ++i) {
      auto& R = Ranges[i];
      if (R.Watch >= 0) {
        continue;
      }
      for (uint32_t w = Matched; w < Count; ++w) {
        if (!strcmp(Watched[w].Name, R.Name) && Watched[w].Size == R.FileSize) {
          R.Watch = static_cast<int32_t>(w);
          Found = true;
          break;
        }
      }
    }
    Matched = Count;
    if (Found) {
      FitBudget();
      MarkLearnedPieces();
    }
  }

  // Once per FEXFileCache pass: learns the candidates, counts the ranges read, asks storage for the list once the drops
  // keep pieces (the read watch found ReadFile; before that a pass drops the files whole) and when a loading screen
  // starts, saves the list.
  void LearnPass(bool& InLoad, bool& Asked, bool LowMemory, uint32_t& Matched) {
    MatchNewFiles(Matched);
    TakeCandidates();
    CountUsed();
    const uint64_t Marks = FrameCount();
    const uint64_t Now = GetCounter();
    const uint64_t Last = Marks ? FrameMark(Marks - 1) : 0;
    const bool Gap = Marks && Now > Last && Now - Last >= CounterFreq * PlayFrameMs / 1000;
    const bool Keeping = Armed.load(std::memory_order_acquire);
    if (Keeping && !LowMemory && Learning.HereRanges.load(std::memory_order_relaxed) && (!Asked || (Gap && !InLoad))) {
      AskLearned();
      Asked = true;
    }
    InLoad = Gap;
    UpdateLearnTotals();
    SaveLearned(false);
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
    if (ReadWatchOn) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("game files: reads followed in {} of {} files ({} MB pieces)", WatchedCount.load(),
                                                    FileCount, PiecesUsed));
    }
    if (ReadWatchOn) {
      FindStorageClock();
    }
    const fextl::string Device = !(AheadMin || LearnBudget) ? fextl::string() :
                                 Folder.empty()              ? fextl::string("game folder not found") :
                                                               StorageReadAhead(Folder.c_str());
    if (AheadMin) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("game files: the main thread's reads of {} KB or more are asked of storage whole before "
                                                    "they run, in {} KB parts ({}; FEX_GAME_FILES_AHEAD=0 turns it off)",
                                                    AheadMin >> 10, AheadPart.load() >> 10, Device));
    }
    if (LearnBudget) {
      StartLearning(fextl::fmt::format("{}, asked in {} KB parts", Device, AheadPart.load() >> 10));
    }
    bool InLoad {}, Asked {};
    uint32_t Matched = WatchedCount.load(std::memory_order_acquire); // StartLearning matched the files watched so far

    uint32_t ArmTries {};
    for (uint64_t Pass = 0;; ++Pass) {
      const uint64_t Begin = GetCounter();
      if (ReadWatchOn && !Armed.load(std::memory_order_relaxed) && WatchedCount.load(std::memory_order_relaxed) && ArmTries < 240) {
        if (!Arm() && ++ArmTries == 240) {
          FEX::Windows::StatsLogNote("game files: no ReadFile found in kernelbase or kernel32 after 2 minutes; the game's files are dropped "
                                     "whole");
        }
      }
      // Every 2 s, look for big files the game has open that aren't in the list.
      if (Pass % 4 == 0) {
        ScanArgs Args {};
        FEX::Windows::Linux::ForEachEntry("/proc/self/fd", ScanEntry, &Args);
      }
      uint64_t Calls = 0, Failed = 0;
      uint32_t Kept[4] {};
      if (FileCount) {
        const MemInfo Before = ReadMemInfo();
        const bool Keeping = Armed.load(std::memory_order_acquire) && (KeepRecent || KeepMain);
        const bool Guard = Keeping && Before.AvailableKB && Before.AvailableKB < (GuardMB << 10);
        if (Guard) {
          Stats.GuardPasses.fetch_add(1, std::memory_order_relaxed);
        }
        Stats.AvailableMB.store(Before.AvailableKB >> 10, std::memory_order_relaxed);
        const uint64_t AllNow = AllClock.load(std::memory_order_relaxed);
        const uint64_t MainNow = MainClock.load(std::memory_order_relaxed);
        for (size_t i = 0; i < FileCount; ++i) {
          const int Fd = Files[i].Fd;
          if (!Keeping || Guard || Files[i].Watch < 0) {
            Drop(Fd, 0, 0, Calls, Failed);
            continue;
          }
          // Drop the stretches between the pieces kept, then the rest to the end (all of it when none is kept).
          const auto& W = Watched[Files[i].Watch];
          uint64_t From = 0;
          for (uint32_t p = 0; p < W.Pieces; ++p) {
            const int Reason = KeepReason(W.FirstPiece + p, AllNow, MainNow);
            if (Reason < 0) {
              continue;
            }
            ++Kept[Reason];
            if (p > From) {
              Drop(Fd, From << PieceShift, (p - From) << PieceShift, Calls, Failed);
            }
            From = p + 1;
          }
          Drop(Fd, From << PieceShift, 0, Calls, Failed);
        }
        const uint64_t After = ReadMemInfo().CachedKB;
        if (Before.CachedKB && After && After < Before.CachedKB) {
          Stats.ShrankKB.fetch_add(Before.CachedKB - After, std::memory_order_relaxed);
        }
      }
      Stats.Files.store(static_cast<uint32_t>(FileCount), std::memory_order_relaxed);
      Stats.KeptRecent.store(Kept[0], std::memory_order_relaxed);
      Stats.KeptMain.store(Kept[1], std::memory_order_relaxed);
      Stats.KeptAhead.store(Kept[2], std::memory_order_relaxed);
      Stats.KeptLearned.store(Kept[3], std::memory_order_relaxed);
      if (LearnBudget) {
        const uint64_t AvailableMB = Stats.AvailableMB.load(std::memory_order_relaxed);
        LearnPass(InLoad, Asked, AvailableMB && AvailableMB < GuardMB, Matched);
      }
      Stats.Drops.fetch_add(Calls - Failed, std::memory_order_relaxed);
      Stats.Failed.fetch_add(Failed, std::memory_order_relaxed);
      Stats.Passes.fetch_add(1, std::memory_order_relaxed);
      const uint64_t Ns = (GetCounter() - Begin) * 1'000'000'000 / CounterFreq;
      uint64_t Longest = Stats.LongestNs.load(std::memory_order_relaxed);
      while (Ns > Longest && !Stats.LongestNs.compare_exchange_weak(Longest, Ns, std::memory_order_relaxed)) {}
      SleepMs(IntervalMs);
    }
    return nullptr;
  }

  // The watched file a handle reads, or -1: by the file's Linux inode, which Wine gives as FileInternalInformation.
  int WatchOf(HANDLE File, uint64_t Now) {
    const auto H = reinterpret_cast<uint64_t>(File);
    if (!H || H == ~0ull) {
      return -1;
    }
    const uint64_t KeepTicks = CounterFreq * HandleKeepMs / 1000;
    {
      std::scoped_lock Guard {ReadLock};
      for (const auto& S : Handles) {
        if (S.Handle == H && Now - S.At < KeepTicks) {
          return S.Watch;
        }
      }
    }
    int Index = -1;
    IO_STATUS_BLOCK Io {};
    InternalInformation Info {};
    if (NtQueryInformationFile(File, &Io, &Info, sizeof(Info), static_cast<FILE_INFORMATION_CLASS>(ClassInternal)) == STATUS_SUCCESS) {
      const uint32_t Count = WatchedCount.load(std::memory_order_acquire);
      for (uint32_t i = 0; i < Count; ++i) {
        if (Watched[i].Inode == static_cast<uint64_t>(Info.IndexNumber)) {
          Index = static_cast<int>(i);
          break;
        }
      }
    }
    std::scoped_lock Guard {ReadLock};
    HandleSlot* Slot = nullptr;
    for (auto& S : Handles) {
      if (S.Handle == H) {
        Slot = &S;
      }
    }
    if (!Slot) {
      Slot = &Handles[NextHandle++ % HandleSlots];
    }
    *Slot = {H, Index, Now};
    return Index;
  }

  // A finished (or, Queued, started) read of Bytes at Offset of File by this thread, which waited Ticks for it.
  uint32_t StorageClockMHz();

  void NoteRead(HANDLE File, uint64_t Offset, bool HaveOffset, uint64_t Bytes, bool Queued, uint64_t Ticks, uint64_t Start,
                ReadUsage* Usage = nullptr) {
    if (!Bytes) {
      return;
    }
    const int Index = WatchOf(File, Start);
    if (Index < 0) {
      std::scoped_lock Guard {ReadLock};
      ++Reads.OtherFiles;
      Reads.OtherFilesBytes += Bytes;
      return;
    }
    if (!HaveOffset && !Queued) {
      // A read at the file position: it ended where the position is now.
      IO_STATUS_BLOCK Io {};
      PositionInformation Position {};
      if (NtQueryInformationFile(File, &Io, &Position, sizeof(Position), static_cast<FILE_INFORMATION_CLASS>(ClassPosition)) == STATUS_SUCCESS &&
          Position.CurrentByteOffset >= static_cast<int64_t>(Bytes)) {
        Offset = static_cast<uint64_t>(Position.CurrentByteOffset) - Bytes;
        HaveOffset = true;
      }
    }
    const bool Main = reinterpret_cast<uint64_t>(NtCurrentTeb()) == MainTeb;
    const auto& W = Watched[Index];
    uint64_t Reuse[Buckets] {}, ReuseOwn[Buckets] {};
    bool AllKept = HaveOffset;
    bool LearnedHit = false;
    uint8_t FirstBucket = Buckets - 1;
    if (HaveOffset && (Offset >> PieceShift) < W.Pieces) {
      const uint64_t AllEnd = AllClock.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
      const uint64_t MainEnd = Main ? MainClock.fetch_add(Bytes, std::memory_order_relaxed) + Bytes : MainClock.load(std::memory_order_relaxed);
      const uint64_t AllBefore = AllEnd - Bytes;
      const uint64_t MainBefore = Main ? MainEnd - Bytes : MainEnd;
      const uint64_t First = Offset >> PieceShift;
      const uint64_t Last = std::min<uint64_t>((Offset + Bytes - 1) >> PieceShift, W.Pieces - 1);
      for (uint64_t p = First; p <= Last; ++p) {
        const uint32_t Piece = W.FirstPiece + static_cast<uint32_t>(p);
        const uint64_t Lo = std::max(Offset, p << PieceShift);
        const uint64_t Hi = std::min(Offset + Bytes, (p + 1) << PieceShift);
        const uint64_t Part = Hi > Lo ? Hi - Lo : 0;
        const uint64_t PrevAll = AllStamps[Piece].exchange(AllEnd, std::memory_order_relaxed);
        const uint64_t PrevMain = Main ? MainStamps[Piece].exchange(MainEnd, std::memory_order_relaxed) :
                                         MainStamps[Piece].load(std::memory_order_relaxed);
        const uint64_t PrevAhead = AheadStamps[Piece].load(std::memory_order_relaxed);
        const uint8_t Flags = LearnBudget ? LearnedPieces[Piece].load(std::memory_order_relaxed) : uint8_t {};
        if (Main && (Flags & PieceLearned)) {
          LearnedHit = true;
          if (!(Flags & PieceUsed)) {
            LearnedPieces[Piece].fetch_or(PieceUsed, std::memory_order_relaxed);
          }
        }
        const size_t Bucket = BucketOf(PrevAll, AllBefore);
        if (p == First) {
          FirstBucket = static_cast<uint8_t>(Bucket);
        }
        Reuse[Bucket] += Part;
        if (Main) {
          ReuseOwn[BucketOf(PrevMain, MainBefore)] += Part;
        }
        AllKept = AllKept && (Within(PrevAll, AllBefore, KeepRecent) || Within(PrevMain, MainBefore, KeepMain) ||
                              Within(PrevAhead, AllBefore, KeepRecent) || (Flags & PieceLearned));
      }
      if (Last + 1 < W.Pieces) {
        AheadStamps[W.FirstPiece + Last + 1].store(AllEnd, std::memory_order_relaxed);
      }
    } else {
      AllKept = false;
    }

    const double Ms = static_cast<double>(Ticks) * 1000.0 / static_cast<double>(CounterFreq);
    // Build 89: a main thread read that waited for storage is a candidate for the learned list; FEXFileCache adds it once
    // its frame turns out to be one of play.
    if (Main && !Queued && LearnBudget && HaveOffset && (Offset >> PieceShift) < W.Pieces &&
        (Usage && Usage->Have ? Usage->StorageKB > 0 && Ms >= 1.0 : Ms >= 2.0)) {
      PushCandidate(Index, Offset, Bytes);
    }

    std::scoped_lock Guard {ReadLock};
    auto& R = Reads;
    ++R.Reads;
    R.Bytes += Bytes;
    R.NoOffset += HaveOffset ? 0 : 1;
    for (size_t b = 0; b < Buckets; ++b) {
      (Main ? R.ReuseMain : R.ReuseOther)[b] += Reuse[b];
      R.ReuseMainOwn[b] += ReuseOwn[b];
    }
    if (Queued) {
      ++R.Queued;
      R.QueuedBytes += Bytes;
      return;
    }
    if (Main) {
      ++R.MainReads;
      R.MainBytes += Bytes;
      R.MainTicks += Ticks;
      R.MainLongest = std::max(R.MainLongest, Ticks);
      if (AllKept) {
        ++R.MainKeptReads;
        R.MainKeptBytes += Bytes;
        R.MainKeptTicks += Ticks;
      }
      if (Usage && Usage->Have) {
        ++R.MainMeasured;
        R.MainCpuUs += static_cast<uint64_t>(Usage->CpuMs * 1000.0f);
        R.MainStorageKB += Usage->StorageKB;
      }
      if (LearnedHit) {
        ++R.LearnedReads;
        R.LearnedBytes += Bytes;
        R.LearnedTicks += Ticks;
        R.LearnedLongest = std::max(R.LearnedLongest, Ticks);
        R.LearnedStorageKB += Usage && Usage->Have ? Usage->StorageKB : 0;
      }
      if (Ms >= SlowReadMs) {
        if (R.SlowCount < MaxSlowReads) {
          if (Usage && Usage->Have) {
            Usage->ClockAfterMHz = StorageClockMHz();
          }
          R.Slow[R.SlowCount++] = {Index,       static_cast<uint32_t>(Offset >> 20), static_cast<uint32_t>(Bytes >> 10), static_cast<float>(Ms),
                                   FirstBucket, AllKept, LearnedHit, Usage ? *Usage : ReadUsage {}};
        } else {
          ++R.SlowMore;
        }
      }
    } else {
      ++R.OtherReads;
      R.OtherBytes += Bytes;
      R.OtherTicks += Ticks;
      R.OtherLongest = std::max(R.OtherLongest, Ticks);
      if (AllKept) {
        ++R.OtherKeptReads;
        R.OtherKeptBytes += Bytes;
        R.OtherKeptTicks += Ticks;
      }
    }
  }

  // Build 87: asks storage for all of a big read of a watched file at once, before the main thread's ReadFile runs (see
  // AheadMin). The pieces are marked as read ahead first, so a drop pass meanwhile leaves them. The number of WILLNEED
  // calls made, 0 when the file isn't watched or the offset can't be told.
  uint32_t AskAhead(HANDLE File, bool HaveOffset, uint64_t Offset, uint64_t Bytes, uint64_t Now) {
    const int Index = WatchOf(File, Now);
    if (Index < 0) {
      return 0;
    }
    if (!HaveOffset) {
      // A read at the file position: it starts where the position is now.
      IO_STATUS_BLOCK Io {};
      PositionInformation Position {};
      if (NtQueryInformationFile(File, &Io, &Position, sizeof(Position), static_cast<FILE_INFORMATION_CLASS>(ClassPosition)) != STATUS_SUCCESS ||
          Position.CurrentByteOffset < 0) {
        return 0;
      }
      Offset = static_cast<uint64_t>(Position.CurrentByteOffset);
    }
    const auto& W = Watched[Index];
    if ((Offset >> PieceShift) >= W.Pieces) {
      return 0;
    }
    const uint64_t Stamp = std::max<uint64_t>(AllClock.load(std::memory_order_relaxed), 1);
    const uint64_t Last = std::min<uint64_t>((Offset + Bytes - 1) >> PieceShift, W.Pieces - 1);
    for (uint64_t p = Offset >> PieceShift; p <= Last; ++p) {
      AheadStamps[W.FirstPiece + p].store(Stamp, std::memory_order_relaxed);
    }
    const uint64_t End = Offset + Bytes;
    const uint64_t Part = AheadPart.load(std::memory_order_relaxed);
    uint32_t Calls {};
    for (uint64_t At = Offset & ~uint64_t {4095}; At < End; At += Part) {
      FEX::Windows::Linux::ReadAhead(W.Fd, At, std::min(Part, End - At));
      ++Calls;
    }
    return Calls;
  }

  void NoteAhead(uint64_t Bytes, uint32_t Calls, uint64_t AskTicks, uint64_t ReadTicks) {
    std::scoped_lock Guard {ReadLock};
    auto& R = Reads;
    ++R.AheadReads;
    R.AheadBytes += Bytes;
    R.AheadCalls += Calls;
    R.AheadAskTicks += AskTicks;
    R.AheadReadTicks += ReadTicks;
    R.AheadLongest = std::max(R.AheadLongest, ReadTicks);
  }

  // The device a path is on and its read-ahead settings, for the log; sets AheadPart to the kernel's largest read-ahead
  // per call when it can be read.
  fextl::string StorageReadAhead(const char* Path) {
    FEX::Windows::Linux::FileInfo Info {};
    if (!FEX::Windows::Linux::Stat(Path, Info)) {
      return "device unknown";
    }
    const uint64_t Major = (Info.Dev >> 8) & 0xfff;
    const uint64_t Minor = (Info.Dev & 0xff) | ((Info.Dev >> 12) & 0xfff00);
    auto ReadKB = [&](const char* Name) -> int64_t {
      char Text[32];
      for (const char* Up : {"", "../"}) {
        char SysPath[96];
        snprintf(SysPath, sizeof(SysPath), "/sys/dev/block/%llu:%llu/%squeue/%s", static_cast<unsigned long long>(Major),
                 static_cast<unsigned long long>(Minor), Up, Name);
        if (FEX::Windows::Linux::ReadSmallFile(SysPath, Text, sizeof(Text)) > 0) {
          return strtoll(Text, nullptr, 10);
        }
      }
      return -1;
    };
    const int64_t ReadAheadKB = ReadKB("read_ahead_kb");
    const int64_t MaxSectorsKB = ReadKB("max_sectors_kb");
    // The kernel's cap per WILLNEED call: the larger of the two (its read-ahead pages and its largest request).
    const int64_t CapKB = std::max(ReadAheadKB, MaxSectorsKB);
    if (CapKB >= 128) {
      AheadPart.store(static_cast<uint64_t>(std::min<int64_t>(CapKB, 2048)) << 10, std::memory_order_relaxed);
    }
    auto Text = [](int64_t KB) {
      return KB < 0 ? fextl::string("unreadable") : fextl::fmt::format("{} KB", KB);
    };
    return fextl::fmt::format("device {}:{}, read-ahead {}, largest request {}", Major, Minor, Text(ReadAheadKB), Text(MaxSectorsKB));
  }

  // Build 88: the storage controller's clock (UFS on the Thor), from its devfreq device: cur_freq now, and trans_stat's
  // time at each clock for a per-second split. Found by FEXFileCache at start; empty when there is none or it can't be
  // read.
  char ClockDir[128];
  std::atomic<bool> ClockOn {};
  constexpr size_t MaxClocks = 16;
  struct ClockTimes {
    size_t Count;
    uint64_t Hz[MaxClocks];
    uint64_t Ms[MaxClocks];
    uint64_t Changes;
  };
  ClockTimes PrevClock {}; // stats log thread only
  bool HavePrevClock {};

  uint32_t StorageClockMHz() {
    if (!ClockOn.load(std::memory_order_acquire)) {
      return 0;
    }
    char Path[160];
    snprintf(Path, sizeof(Path), "%s/cur_freq", ClockDir);
    char Text[32];
    if (FEX::Windows::Linux::ReadSmallFile(Path, Text, sizeof(Text)) <= 0) {
      return 0;
    }
    return static_cast<uint32_t>(strtoull(Text, nullptr, 10) / 1'000'000);
  }

  // trans_stat: "*  37500000:  0  5  ...  12345" rows (the last number is the time at that clock in ms), then "Total
  // transition : N".
  bool ReadClockTimes(ClockTimes& Out) {
    char Path[160];
    snprintf(Path, sizeof(Path), "%s/trans_stat", ClockDir);
    char Text[2048];
    if (FEX::Windows::Linux::ReadSmallFile(Path, Text, sizeof(Text)) <= 0) {
      return false;
    }
    Out = {};
    for (const char* Line = Text; Line && *Line;) {
      const char* End = strchr(Line, '\n');
      const char* P = Line;
      while (*P == ' ' || *P == '*') {
        ++P;
      }
      char* After {};
      const uint64_t Hz = strtoull(P, &After, 10);
      if (After != P && *After == ':' && Out.Count < MaxClocks) {
        // The last number on the line.
        uint64_t Last {};
        for (const char* Q = After + 1; Q < (End ? End : Q + strlen(Q));) {
          char* Next {};
          const uint64_t V = strtoull(Q, &Next, 10);
          if (Next == Q) {
            break;
          }
          Last = V;
          Q = Next;
        }
        Out.Hz[Out.Count] = Hz;
        Out.Ms[Out.Count] = Last;
        ++Out.Count;
      } else if (const char* Total = strstr(Line, "Total transition"); Total && (!End || Total < End)) {
        if (const char* Colon = strchr(Total, ':')) {
          Out.Changes = strtoull(Colon + 1, nullptr, 10);
        }
      }
      Line = End ? End + 1 : nullptr;
    }
    return Out.Count > 0;
  }

  // Once a second: the time at each clock since the last second, printed when Show.
  fextl::string StorageClockLine(bool Show) {
    if (!ClockOn.load(std::memory_order_acquire)) {
      return {};
    }
    ClockTimes Now {};
    if (!ReadClockTimes(Now)) {
      return {};
    }
    fextl::string Out;
    if (Show && HavePrevClock && Now.Count == PrevClock.Count) {
      for (size_t i = 0; i < Now.Count; ++i) {
        const uint64_t Ms = Now.Ms[i] >= PrevClock.Ms[i] ? Now.Ms[i] - PrevClock.Ms[i] : 0;
        if (Ms) {
          Out += fextl::fmt::format("{}{} MHz {} ms", Out.empty() ? "" : ", ", Now.Hz[i] / 1'000'000, Ms);
        }
      }
      Out = fextl::fmt::format("  storage clock: {} | {} changes | now {} MHz\n", Out.empty() ? fextl::string("-") : Out,
                               Now.Changes >= PrevClock.Changes ? Now.Changes - PrevClock.Changes : 0, StorageClockMHz());
    }
    PrevClock = Now;
    HavePrevClock = true;
    return Out;
  }

  struct ClockFind {
    char Name[64];
  };

  void FindClockEntry(const char* Name, void* Arg) {
    auto* F = static_cast<ClockFind*>(Arg);
    if (!F->Name[0] && (strstr(Name, "ufs") || strstr(Name, "mmc") || strstr(Name, "sdhc"))) {
      snprintf(F->Name, sizeof(F->Name), "%s", Name);
    }
  }

  // Looks for the storage controller's devfreq device and notes what it says.
  void FindStorageClock() {
    ClockFind F {};
    if (!FEX::Windows::Linux::ForEachEntry("/sys/class/devfreq", FindClockEntry, &F)) {
      FEX::Windows::StatsLogNote("game files: storage clock: /sys/class/devfreq can't be read");
      return;
    }
    if (!F.Name[0]) {
      FEX::Windows::StatsLogNote("game files: storage clock: no storage controller among the devfreq devices");
      return;
    }
    snprintf(ClockDir, sizeof(ClockDir), "/sys/class/devfreq/%s", F.Name);
    auto ReadText = [&](const char* Leaf) {
      char Path[160];
      snprintf(Path, sizeof(Path), "%s/%s", ClockDir, Leaf);
      char Text[512];
      if (FEX::Windows::Linux::ReadSmallFile(Path, Text, sizeof(Text)) <= 0) {
        return fextl::string("unreadable");
      }
      fextl::string Value(Text);
      while (!Value.empty() && (Value.back() == '\n' || Value.back() == ' ')) {
        Value.pop_back();
      }
      return Value;
    };
    const fextl::string Cur = ReadText("cur_freq");
    // Build 89: whether this app could set the lowest clock itself (Android normally lets only the system do that).
    char MinPath[160];
    snprintf(MinPath, sizeof(MinPath), "%s/min_freq", ClockDir);
    const int Writable = FEX::Windows::Linux::Access(MinPath, 2);
    ClockTimes Times {};
    const bool HaveTimes = ReadClockTimes(Times);
    ClockOn.store(Cur != "unreadable", std::memory_order_release);
    FEX::Windows::StatsLogNote(fextl::fmt::format("game files: storage clock: {} now {} Hz (min {}, max {}, governor {}, polling {} ms; "
                                                  "clocks {}; min_freq {}), time at each clock {}",
                                                  F.Name, Cur, ReadText("min_freq"), ReadText("max_freq"), ReadText("governor"),
                                                  ReadText("polling_interval"), ReadText("available_frequencies"),
                                                  Writable == 0 ? fextl::string("writable by this app") :
                                                                  fextl::fmt::format("not writable by this app (error {})", -Writable),
                                                  HaveTimes ? "readable: a 'storage clock' line in seconds the main thread read game data" :
                                                              "unreadable"));
  }

  double MB(uint64_t Bytes) {
    return static_cast<double>(Bytes) / (1024.0 * 1024.0);
  }

  fextl::string Histogram(const uint64_t (&Bytes)[Buckets]) {
    fextl::string Out;
    for (size_t b = 0; b < Buckets; ++b) {
      Out += fextl::fmt::format("{}{:.1f}", b ? "/" : "", MB(Bytes[b]));
    }
    return Out;
  }

  fextl::string ReadsLine() {
    ReadCounts R;
    {
      std::scoped_lock Guard {ReadLock};
      R = Reads;
      Reads = {};
    }
    const fextl::string Clock = StorageClockLine(R.MainReads > 0);
    if (!R.Reads && !R.OtherFiles && !R.AheadReads) {
      return Clock;
    }
    const double MsPerTickLearned = 1000.0 / static_cast<double>(CounterFreq);
    const fextl::string LearnedText =
      R.LearnedReads ? fextl::fmt::format(" | learned data: main {} reads {:.1f} MB {:.1f} ms (longest {:.1f}), from storage {:.1f} MB",
                                          R.LearnedReads, MB(R.LearnedBytes), static_cast<double>(R.LearnedTicks) * MsPerTickLearned,
                                          static_cast<double>(R.LearnedLongest) * MsPerTickLearned,
                                          static_cast<double>(R.LearnedStorageKB) / 1024.0) :
                       fextl::string();
    const double MsPerTick = 1000.0 / static_cast<double>(CounterFreq);
    auto Ms = [&](uint64_t Ticks) {
      return static_cast<double>(Ticks) * MsPerTick;
    };
    fextl::string Slow;
    for (size_t i = 0; i < R.SlowCount; ++i) {
      const auto& S = R.Slow[i];
      Slow += fextl::fmt::format("{}{}@{}MB {}KB {:.1f}ms {}{}{}", i ? ", " : " | slow main: ", Watched[S.Watch].Name, S.OffsetMB, S.KB, S.Ms,
                                 BucketNames[S.Bucket], S.Kept ? " kept" : "", S.Learned ? " learned" : "");
      if (S.U.Have) {
        Slow += fextl::fmt::format(" [cpu {:.1f} ms (kernel {:.1f}), from storage {} KB, faults {}+{} major, slept {}, preempted {}, storage clock "
                                   "{}->{} MHz]",
                                   S.U.CpuMs, S.U.SystemMs, S.U.StorageKB, S.U.MinorFaults, S.U.MajorFaults, S.U.Slept, S.U.Preempted,
                                   S.U.ClockBeforeMHz, S.U.ClockAfterMHz);
      }
    }
    if (R.SlowMore) {
      Slow += fextl::fmt::format(", {} more", R.SlowMore);
    }
    return Clock + fextl::fmt::format(
      "  game reads: {} reads {:.1f} MB ({} queued {:.1f} MB) | main {} reads {:.1f} MB {:.1f} ms (longest {:.1f}{}), kept {} {:.1f} MB "
      "{:.1f} ms | others {} {:.1f} MB {:.1f} ms (longest {:.1f}), kept {} {:.1f} MB {:.1f} ms | MB by read since its last read "
      "{}: main {}, main by its own {}, others {} | other files {} reads {:.1f} MB{}{}{}{}\n",
      R.Reads, MB(R.Bytes), R.Queued, MB(R.QueuedBytes), R.MainReads, MB(R.MainBytes), Ms(R.MainTicks), Ms(R.MainLongest),
      R.MainMeasured ? fextl::fmt::format("; on a CPU {:.1f} ms, from storage {:.1f} MB", static_cast<double>(R.MainCpuUs) / 1000.0,
                                          static_cast<double>(R.MainStorageKB) / 1024.0) :
                       fextl::string(),
      R.MainKeptReads,
      MB(R.MainKeptBytes), Ms(R.MainKeptTicks), R.OtherReads, MB(R.OtherBytes), Ms(R.OtherTicks), Ms(R.OtherLongest), R.OtherKeptReads,
      MB(R.OtherKeptBytes), Ms(R.OtherKeptTicks), "<128M/<512M/<1G/<2G/<4G/<8G/more/first", Histogram(R.ReuseMain),
      Histogram(R.ReuseMainOwn), Histogram(R.ReuseOther), R.OtherFiles, MB(R.OtherFilesBytes),
      R.NoOffset ? fextl::fmt::format(", {} without offset", R.NoOffset) : fextl::string(),
      R.AheadReads ? fextl::fmt::format(" | asked ahead: {} main reads {:.1f} MB, asking {:.2f} ms ({} calls), reads {:.1f} ms (longest {:.1f})",
                                        R.AheadReads, MB(R.AheadBytes), Ms(R.AheadAskTicks), R.AheadCalls, Ms(R.AheadReadTicks),
                                        Ms(R.AheadLongest)) :
                     fextl::string(),
      LearnedText, Slow);
  }

  // Build 89: the learned list's line, in seconds something happened to it.
  fextl::string LearnLine() {
    if (!LearnBudget) {
      return {};
    }
    const uint64_t NewRanges = Learning.NewRanges.exchange(0, std::memory_order_relaxed);
    const uint64_t NewBytes = Learning.NewBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t Skipped = Learning.Skipped.exchange(0, std::memory_order_relaxed);
    const uint64_t Lost = Learning.Lost.exchange(0, std::memory_order_relaxed);
    const uint64_t Asks = Learning.Asks.exchange(0, std::memory_order_relaxed);
    const uint64_t AskBytes = Learning.AskBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t AskKB = Learning.AskStorageKB.exchange(0, std::memory_order_relaxed);
    const uint64_t AskNs = Learning.AskNs.exchange(0, std::memory_order_relaxed);
    const uint64_t AskCalls = Learning.AskCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t Evicted = Learning.Evicted.exchange(0, std::memory_order_relaxed);
    const uint64_t SaveFailed = Learning.SaveFailed.exchange(0, std::memory_order_relaxed);
    if (!NewRanges && !NewBytes && !Skipped && !Lost && !Asks && !Evicted && !SaveFailed) {
      return {};
    }
    fextl::string Out = fextl::fmt::format(
      "  learned reads: {} ranges {:.1f} MB in files here ({} read by the main thread this session, {} learned this session {:.1f} MB) | "
      "this second: learned {} new ranges, {:.1f} MB added",
      Learning.HereRanges.load(std::memory_order_relaxed), MB(Learning.HereBytes.load(std::memory_order_relaxed)),
      Learning.UsedRanges.load(std::memory_order_relaxed), Learning.SessionRanges.load(std::memory_order_relaxed),
      MB(Learning.SessionBytes.load(std::memory_order_relaxed)), NewRanges, MB(NewBytes));
    if (Skipped) {
      Out += fextl::fmt::format(", {} storage reads in a loading screen left out", Skipped);
    }
    if (Lost) {
      Out += fextl::fmt::format(", {} not noted (no room)", Lost);
    }
    if (Evicted) {
      Out += fextl::fmt::format(", {} ranges dropped for the budget", Evicted);
    }
    if (Asks) {
      Out += fextl::fmt::format(" | asked of storage (start or loading screen): {:.1f} MB in {} calls, {:.1f} MB of it read from storage, {:.1f} ms",
                                MB(AskBytes), AskCalls, static_cast<double>(AskKB) / 1024.0, static_cast<double>(AskNs) / 1e6);
    }
    if (SaveFailed) {
      Out += fextl::fmt::format(" | saving the list failed ({})", LearnFile);
    }
    return Out + "\n";
  }

  fextl::string StatsLine() {
    const uint64_t Passes = Stats.Passes.exchange(0, std::memory_order_relaxed);
    const uint64_t Drops = Stats.Drops.exchange(0, std::memory_order_relaxed);
    const uint64_t Failed = Stats.Failed.exchange(0, std::memory_order_relaxed);
    const uint64_t ShrankKB = Stats.ShrankKB.exchange(0, std::memory_order_relaxed);
    const uint64_t LongestNs = Stats.LongestNs.exchange(0, std::memory_order_relaxed);
    const uint64_t GuardPasses = Stats.GuardPasses.exchange(0, std::memory_order_relaxed);
    const uint32_t Count = Stats.Files.load(std::memory_order_relaxed);
    if (!Passes) {
      return ReadsLine() + LearnLine();
    }
    const uint32_t Recent = Stats.KeptRecent.load(std::memory_order_relaxed);
    const uint32_t Main = Stats.KeptMain.load(std::memory_order_relaxed);
    const uint32_t Ahead = Stats.KeptAhead.load(std::memory_order_relaxed);
    const uint32_t LearnedKept = Stats.KeptLearned.load(std::memory_order_relaxed);
    const fextl::string Kept =
      Armed.load(std::memory_order_relaxed) && KeepMB ?
        fextl::fmt::format(", kept {} MB of {} (read lately {}, by the main thread {}, readahead {}){}", Recent + Main + Ahead, KeepMB, Recent,
                           Main, Ahead, LearnBudget ? fextl::fmt::format(" + learned {} MB", LearnedKept) : fextl::string()) :
        fextl::string();
    const fextl::string GuardText =
      GuardPasses ? fextl::fmt::format(", memory guard: available {} MB below {}, all dropped in {} passes",
                                       Stats.AvailableMB.load(std::memory_order_relaxed), GuardMB, GuardPasses) :
                    fextl::string();
    return fextl::fmt::format("  game files: {} files{}, file cache {:.0f} MB smaller after the drops ({} drops in {} passes, longest {:.1f} "
                              "ms{}){}\n",
                              Count, Kept, static_cast<double>(ShrankKB) / 1024.0, Drops, Passes, static_cast<double>(LongestNs) / 1e6,
                              Failed ? fextl::fmt::format(", {} failed", Failed) : fextl::string(), GuardText) +
           ReadsLine() + LearnLine();
  }

  uint64_t Number(const char* Name, uint64_t Default, uint64_t Min, uint64_t Max) {
    const char* Value = FEX::Windows::Switch(Name);
    return std::clamp<uint64_t>(Value && Value[0] >= '0' && Value[0] <= '9' ? strtoull(Value, nullptr, 10) : Default, Min, Max);
  }
} // namespace

bool WantsFrameMarks() {
  return LearnBudget > 0;
}

void Init(bool StatsLog) {
  const char* Value = FEX::Windows::Switch("FEX_GAME_FILES_UNCACHED");
  if (!Value || Value[0] != '1') {
    return;
  }
  IntervalMs = Number("FEX_GAME_FILES_MS", 500, 100, 10'000);
  KeepMB = Number("FEX_GAME_FILES_KEEP_MB", 512, 0, 8192);
  GuardMB = Number("FEX_GAME_FILES_GUARD_MB", 2048, 0, 65536);
  const char* WatchSwitch = FEX::Windows::Switch("FEX_GAME_FILES_READ_WATCH");
  ReadWatchOn = !(WatchSwitch && WatchSwitch[0] == '0');
  if (!ReadWatchOn) {
    KeepMB = 0;
  }
  // KB; needs the read watch (it finds the reads).
  AheadMin = ReadWatchOn ? Number("FEX_GAME_FILES_AHEAD", 0, 0, 1 << 20) << 10 : 0;
  // Build 89: MB; needs the read watch and the keep budget (the drops leave learned pieces like kept ones).
  LearnBudget = ReadWatchOn && KeepMB ? Number("FEX_GAME_FILES_LEARN", 0, 0, 4096) << 20 : 0;
  if (LearnBudget) {
    LearnFile = Store::LearnedPath();
  }
  // A quarter for what anyone read lately (an area load's own readahead and re-reads), the rest for the main thread's
  // reads.
  KeepRecent = (KeepMB << 20) / 4;
  KeepMain = (KeepMB << 20) - KeepRecent;
  MainTeb = reinterpret_cast<uint64_t>(NtCurrentTeb());
  CounterFreq = GetCounterFrequency();
  FEX::Windows::StatsLogNote(fextl::fmt::format(
    "game files: the game's files of {} MB or more are dropped from the file cache every {} ms by FEXFileCache{} (FEX_GAME_FILES_UNCACHED=0 "
    "turns it off, FEX_GAME_FILES_MS sets the time, FEX_GAME_FILES_KEEP_MB the data kept, FEX_GAME_FILES_GUARD_MB the memory guard, "
    "FEX_GAME_FILES_READ_WATCH=0 turns the read watch off)",
    MinBytes >> 20, IntervalMs,
    KeepMB ? fextl::fmt::format(", except the last {} MB anyone read and the last {} MB the main thread read (and the 1 MB after each "
                                "read), unless available memory is below {} MB",
                                KeepRecent >> 20, KeepMain >> 20, GuardMB) :
             fextl::string(ReadWatchOn ? ", all of it (keep budget 0); the game's reads are followed" : ", all of it; reads not followed")));
  if (StatsLog) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(FileCacheThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXFileCache");
}
} // namespace GameFiles

using namespace GameFiles;

using ReadFileFn = BOOL (*)(HANDLE, void*, DWORD, DWORD*, void*);

// The game's ReadFile, through Module.S (see FEXReadWatch). Lr and Sp locate the x64 caller's stack: the fifth argument
// (the OVERLAPPED) is above the 32 bytes of home space, past the return address when Module.S left it on the stack (lr is
// then X64ReturnInstr).
extern "C" BOOL FEXNativeReadFile(HANDLE File, void* Buffer, DWORD Bytes, DWORD* Read, uint64_t Lr, uint64_t Sp) {
  const auto Real = reinterpret_cast<ReadFileFn>(__atomic_load_n(&FEXReadWatch[3], __ATOMIC_ACQUIRE));
  const bool ReturnOnStack = Lr == reinterpret_cast<uint64_t>(X64ReturnInstr);
  auto* Overlapped = *reinterpret_cast<X64Overlapped* const*>(Sp + (ReturnOnStack ? 0x28 : 0x20));
  const uint64_t Offset = Overlapped ? (static_cast<uint64_t>(Overlapped->OffsetHigh) << 32) | Overlapped->Offset : 0;
  const uint64_t Start = GetCounter();
  uint32_t AskCalls {};
  uint64_t Asked = Start;
  const bool MainThread = WatchedCount.load(std::memory_order_relaxed) && reinterpret_cast<uint64_t>(NtCurrentTeb()) == MainTeb;
  if (AheadMin && Bytes >= AheadMin && MainThread) {
    AskCalls = AskAhead(File, Overlapped != nullptr, Offset, Bytes, Start);
    Asked = GetCounter();
  }
  // Build 88: the main thread's reads are taken apart (see ReadUsage).
  FEX::Windows::Linux::ThreadUsage Before {};
  ReadUsage Usage {};
  const bool Measure = MainThread && FEX::Windows::Linux::GetThreadUsage(Before);
  if (Measure && Bytes >= (256u << 10)) {
    Usage.ClockBeforeMHz = StorageClockMHz();
  }
  const BOOL Result = Real(File, Buffer, Bytes, Read, Overlapped);
  const uint64_t End = GetCounter();
  if (!WatchedCount.load(std::memory_order_relaxed)) {
    return Result;
  }
  if (FEX::Windows::Linux::ThreadUsage After {}; Measure && FEX::Windows::Linux::GetThreadUsage(After)) {
    Usage.Have = true;
    Usage.CpuMs = static_cast<float>((After.UserUs + After.SystemUs) - (Before.UserUs + Before.SystemUs)) / 1000.0f;
    Usage.SystemMs = static_cast<float>(After.SystemUs - Before.SystemUs) / 1000.0f;
    Usage.StorageKB = static_cast<uint32_t>((After.InBlocks - Before.InBlocks) / 2);
    Usage.MinorFaults = static_cast<uint32_t>(After.MinorFaults - Before.MinorFaults);
    Usage.MajorFaults = static_cast<uint32_t>(After.MajorFaults - Before.MajorFaults);
    Usage.Slept = static_cast<uint32_t>(After.Voluntary - Before.Voluntary);
    Usage.Preempted = static_cast<uint32_t>(After.Involuntary - Before.Involuntary);
  }
  // Nothing below may change what the game sees: the last error stays as ReadFile left it.
  auto* LastError = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + TebLastError);
  const uint32_t Error = *LastError;
  const bool Queued = !Result && Error == ErrorIoPending;
  if (Result || Queued) {
    const uint64_t Got = Queued ? Bytes : Read ? *Read : Overlapped ? Overlapped->InternalHigh : Bytes;
    NoteRead(File, Offset, Overlapped != nullptr, Got, Queued, End - Start, Start, Usage.Have ? &Usage : nullptr);
  }
  if (AskCalls) {
    NoteAhead(Bytes, AskCalls, Asked - Start, End - Asked);
  }
  *LastError = Error;
  return Result;
}
