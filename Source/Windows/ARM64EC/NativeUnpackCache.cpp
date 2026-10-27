// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Disk cache of zlib streams the game decompressed before (see NativeUnpackCache.h)
$end_info$
*/

// Two files in <FEX cache directory>\UnpackCache\:
//   data.bin   a header, then the output of each stored stream, appended
//   index.bin  a header, then one 64-byte record per stored stream
// Both are only ever appended to. A record is written after its data, and carries a checksum, so a record cut short by
// a crash or a missing write is skipped when the index is loaded. Data is checked against its hash on every read.
//
// Storing doesn't wait for the disk: the game's thread copies the output into a ring buffer, and a writer thread hashes
// and writes it, in the order the space was taken. A stream can be found once it is written; when the ring is full, new
// streams aren't stored this time. The ring is allocated once and faulted in by the writer, so copying into it doesn't
// fault in fresh pages. If the writer isn't ready a few seconds after the cache opened, the game's threads write
// themselves.
//
// Reading ahead: data.bin holds the streams in the order they were first unpacked, and a later session asks for them in
// much the same order. Once hits come from the same or neighbouring 1 MB pieces of data.bin, a reader thread reads the
// next pieces into memory in large reads, and hits are copied from there instead of each reading from disk.
//
// Keeping data.bin out of the kernel's file cache (FEX_UNPACK_CACHE_DIRECT, from build 58): an area load reads 3-4 GB of
// it in a few seconds, a third of that read ahead and never used, and Android's whole-device load went up to 5-6 of 8
// cores right after such loads in the build 56 and 57 logs. Read through the file cache, all of it displaces other
// apps' cached files (which they then read back from storage), and pages read again after being displaced count as
// "thrashing" to the kernel and to Android's memory killer. The data is copied into our own memory anyway, so data.bin is
// read through a Linux file of its own: read-ahead with direct reads (storage to our memory, no file cache), other reads
// and writes are dropped from the file cache right after.
//
// While playing (FEX_UNPACK_CACHE_PLAY, from build 62): an area load looks up thousands of streams a second, play a few
// to a few hundred (the game streaming in the area around the player). In the fex10 run (FEX build 61) hits during play
// that weren't in read-ahead memory took 1.9 ms each on average to read from disk, up to 3.6 ms in busy seconds
// (59 hits, 214 ms in one second), while the game's own unpacking of the same ~59 KB stream takes ~0.3 ms in FEX's
// native inflate (~190 MB/s in that log) plus the game's own setup. The streaming hitches of 100-140 ms had the main
// thread in these reads and in the game's own file reads. Read-ahead during play read 1.9 GB, two thirds of it never
// used, on the storage the main thread's file reads wait for. So while fewer than PlayMaxLookups lookups came in the last and in the current PlayWindowMs, a hit
// that isn't in read-ahead memory counts as a miss (the game unpacks it itself), and read-ahead reads PlayAheadChunks
// past a hit instead of AheadChunks. Loads keep the full cache.
//
// Read-ahead that grows along the reads (FEX_UNPACK_CACHE_AHEAD=ramp, from build 83): up to build 82 every hit read the
// AheadChunks after it, however far the game went on reading in order. In the build 82 run 27 GB were read ahead, 52% of
// it never used (50-71% at every lookup rate, so not memory that was too small, but reads that stopped long before the
// 8 MB after them). While the game streamed a new area during play, storage read ~1 GB/s for seconds, ~3/4 of it this
// read-ahead, with up to 18% of the main thread's core going to interrupts. Now a hit reads 1 chunk past itself, and
// one more for every 2 chunks before it the game already used in order, up to AheadChunks (PlayAheadChunks while
// playing); a run of disk hits in sequence reads its own chunk and the next. FEX_UNPACK_CACHE_AHEAD=fixed reads the
// whole distance past every hit as before.
//
// The main thread while frames come (FEX_UNPACK_CACHE_MAIN_PLAY, from build 90): in the build 89 run storage ran at its
// low clock (75 of 300 MHz) during play, as it does whenever reads come in short bursts, and the main thread's 11 hits
// from disk at an area entry took 36.9 ms (3.4 ms each; ~0.45 ms each while storage was busy and fast), against ~0.2 ms
// for FEX's native inflate of the same ~60 KB. The lookup-rate gate above didn't apply: the entry looked up 221 streams in
// that second, so it counted as a load. Now a hit the main thread can't take from memory while frames are coming (the
// last began under 250 ms ago, GameFiles::FramesComing) is always left to the game.
//
// Learned streams (FEX_UNPACK_CACHE_LEARN, from build 90): the same 105 ms frame also unpacked 7.6 MB itself (20.9 ms) for
// streams that were stored but not in memory. The streams the main thread looks up in frames of play are much the same
// every session (like its game file reads, see GameFiles.h build 89), so their places in data.bin (whole 64 KB blocks)
// are kept in a list (UnpackCache\learned.txt). At the start and at every loading screen, when storage runs fast anyway,
// the list is asked of storage into the kernel's file cache (WILLNEED), drops of data.bin leave those blocks there, and
// a lookup of a learned stream not in read-ahead memory takes it from the file cache without waiting for storage
// (preadv2 RWF_NOWAIT), on any thread; when it isn't there, the rules above apply. Ranges not used in 30 sessions are
// forgotten, past the budget the ones used longest ago go first, and the list starts over with data.bin.
//
// Written data in batches (FEX_UNPACK_CACHE_FLUSH, from build 91): up to build 90 the writer dropped every new stream from
// the file cache right after writing it. A drop can't take pages that aren't on storage yet, it only starts writing
// them, so the data stayed in memory, and with streams 64 bytes apart every stream's first page was the page the stream
// before it had just sent to storage: f2fs makes a write to a page that is being written wait for that write, so each
// stream waited for a storage round trip. In the build 90 run, while the game streamed in new areas (t=58-65), the
// writer stored ~150 MB/s in ~2,400 streams a second, took a whole second of every second doing it (0.42 ms a stream),
// its buffer overflowed (up to 510 streams a second not stored), and the main thread's core ran other work 5,000-14,000
// times a second (3,000 in calm play) while the main thread waited 270-375 ms a second for it. Now streams start on a
// page of their own, the writer doesn't drop each one, and every FlushBatch it starts writing the batch to storage and
// waits for the batch before (normally done by then), then drops it from the file cache, which now takes all of it; an
// idle writer does the same with what is left after FlushIdleMs. FEX_UNPACK_CACHE_FLUSH=0 is build 90's way.

#include "NativeUnpackCache.h"
#include "GameFiles.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/File.h>
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/deque.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXHeaderUtils/Filesystem.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
int FEXUnpackCacheEnabled;
int FEXMD5CacheEnabled;
struct FEXUnpackCacheCounters FEXUnpackCacheStats;
}

namespace {
constexpr char DataMagic[8] = {'F', 'E', 'X', 'U', 'N', 'P', 'K', 'D'};
constexpr char IndexMagic[8] = {'F', 'E', 'X', 'U', 'N', 'P', 'K', 'I'};
constexpr uint32_t FormatVersion = 1;
constexpr uint64_t HeaderSize = 64;
constexpr uint64_t DataAlignment = 64;
constexpr uint64_t RingSize = 64ull << 20; // output bytes waiting for the writer (build 38 peaked at 24 MB)

// Read-ahead, see above.
constexpr uint64_t ChunkSize = 1ull << 20;
constexpr uint64_t AheadChunks = 8;       // how far past a hit to read
constexpr uint64_t MaxHitChunks = 4;      // larger hits are read from disk directly
constexpr size_t MaxAheadRequests = 64;   // chunks waiting for the reader
constexpr size_t RecentDiskChunks = 16;   // chunks of recent hits read from disk, to see when they come in sequence
constexpr uint64_t NoChunk = ~0ull;
constexpr uint64_t DefaultReadAheadMB = 64;
constexpr uint64_t PageSize = 4096; // direct reads need buffer, offset and size in multiples of the storage block size
// Build 91, written data in batches (see above).
constexpr uint64_t FlushBatch = 8ull << 20;
constexpr uint64_t FlushIdleMs = 250;
constexpr uint64_t PlayWindowMs = 250;
constexpr uint32_t PlayMaxLookups = 125; // per PlayWindowMs; loads have 1000-5500 lookups a second, play 0-500
constexpr uint64_t PlayAheadChunks = 2;

struct FileHeader {
  char Magic[8];
  uint32_t Version;
  uint32_t Reserved[13];
};
static_assert(sizeof(FileHeader) == HeaderSize);

struct Record {
  uint64_t Key[2];
  uint64_t Offset;
  uint64_t OutputHash;
  uint32_t Produced;
  uint32_t Trailer;
  uint32_t Reserved[4];
  uint64_t Check; // hash of everything above, never 0
};
static_assert(sizeof(Record) == 64);

uint64_t RecordCheck(const Record& R) {
  return XXH3_64bits(&R, offsetof(Record, Check)) | 1;
}

struct KeyHash {
  size_t operator()(const std::pair<uint64_t, uint64_t>& Key) const {
    return Key.first;
  }
};

struct Entry {
  uint64_t Offset;
  uint64_t OutputHash;
  uint32_t Produced;
  uint32_t Trailer;
  bool Written; // false while queued: a lookup misses
};

struct Job {
  Record R;
  uint64_t RecordOffset;
  uint64_t RingStart; // position in the ring, counting up forever; the data is at Ring + RingStart % RingSize
  uint64_t RingEnd;
  bool Ready; // copied in
};

struct Cache {
  FEXCore::File::File Data;
  FEXCore::File::File Index;
  // data.bin as Linux files of our own, see above: -1 when not used.
  int LinuxRead {-1};
  std::atomic<int> LinuxDirect {-1}; // used by the read-ahead thread only
  std::mutex Lock;
  fextl::unordered_map<std::pair<uint64_t, uint64_t>, Entry, KeyHash> Entries;
  uint64_t DataEnd {};  // next free data offset
  uint64_t IndexEnd {}; // next free record offset
  uint64_t MaxDataBytes {};
  bool Full {};

  // Jobs in the order their ring space was taken. Ring space up to RingTail is free again.
  fextl::deque<Job> Queue;
  uint8_t* Ring {};
  std::atomic<bool> RingReady {}; // its pages are faulted in
  uint64_t RingHead {};
  uint64_t RingTail {};
  std::condition_variable QueueWake;

  // Read-ahead memory, see above: ReadAheadSlots pieces of ChunkSize, filled by the reader thread. Its own lock, never
  // taken together with Lock.
  struct Slot {
    uint64_t Chunk {NoChunk}; // piece of data.bin held (or being read) here
    uint64_t LastUse {};
    uint32_t Bytes {}; // read so far: less than ChunkSize at the end of data.bin
    uint32_t Users {}; // hits copying out of it right now
    bool Loading {};
    bool Used {}; // served a hit since it was read
  };
  uint32_t ReadAheadSlots {}; // 0: read-ahead off
  std::atomic<bool> ReadAheadReady {};
  std::atomic<uint64_t> DataEndHint {}; // DataEnd, readable without Lock: chunks from here on aren't requested
  std::mutex AheadLock;
  std::condition_variable AheadWake;
  uint8_t* AheadMemory {};
  fextl::vector<Slot> Slots;
  fextl::unordered_map<uint64_t, uint32_t> ChunkSlots; // chunk -> slot holding it or reading it
  fextl::deque<uint64_t> AheadRequests;
  uint64_t AheadClock {};
  uint64_t RecentDisk[RecentDiskChunks] {}; // chunk + 1, 0 = empty
  size_t RecentDiskNext {};

  bool AheadRamp {}; // FEX_UNPACK_CACHE_AHEAD, see above

  // Build 91, written data in batches (FEX_UNPACK_CACHE_FLUSH): streams start on a page of their own, and the writer thread
  // sends what it wrote to storage in batches, then drops it. Offsets of data.bin, the writer thread's own.
  bool Flush {};
  uint64_t WrittenTo {}; // end of the last stream the writer wrote (streams are written in order)
  uint64_t StartedTo {}; // writing to storage started up to here
  uint64_t FlushedTo {}; // on storage and dropped up to here

  // Telling play from loads by the lookup rate, see FEX_UNPACK_CACHE_PLAY above.
  bool PlayGate {};
  // Build 90: the main thread never reads a hit from disk while frames are coming (FEX_UNPACK_CACHE_MAIN_PLAY).
  bool MainPlay {};
  uint64_t WindowTicks {1};
  std::atomic<uint64_t> WindowStart {};
  std::atomic<uint32_t> WindowLookups {};
  std::atomic<uint32_t> LastWindowLookups {};
};

Cache* TheCache {};
uint64_t TickFrequency {1};

uint64_t ReadTicks() {
  uint64_t Value {};
  __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Value));
  return Value;
}

uint64_t FileSize(FEXCore::File::File& F) {
  LARGE_INTEGER Size {};
  if (!GetFileSizeEx(F.GetHandle(), &Size)) {
    return 0;
  }
  return static_cast<uint64_t>(Size.QuadPart);
}

// Reads up to Size bytes, fewer at the end of the file. Returns how many.
uint64_t ReadUpTo(FEXCore::File::File& F, void* Buffer, uint64_t Size, uint64_t Offset) {
  auto* Bytes = static_cast<uint8_t*>(Buffer);
  uint64_t Total {};
  while (Total < Size) {
    const auto Result = F.PRead(Bytes + Total, static_cast<uint32_t>(std::min<uint64_t>(Size - Total, 1u << 30)), Offset + Total);
    if (Result <= 0) {
      break;
    }
    Total += Result;
  }
  return Total;
}

bool ReadExact(FEXCore::File::File& F, void* Buffer, uint64_t Size, uint64_t Offset) {
  auto* Bytes = static_cast<uint8_t*>(Buffer);
  while (Size) {
    const uint32_t Chunk = static_cast<uint32_t>(std::min<uint64_t>(Size, 1u << 30));
    const auto Result = F.PRead(Bytes, Chunk, Offset);
    if (Result <= 0) {
      return false;
    }
    Bytes += Result;
    Size -= Result;
    Offset += Result;
  }
  return true;
}

bool WriteExact(FEXCore::File::File& F, const void* Buffer, uint64_t Size, uint64_t Offset) {
  auto* Bytes = static_cast<const uint8_t*>(Buffer);
  while (Size) {
    const uint32_t Chunk = static_cast<uint32_t>(std::min<uint64_t>(Size, 1u << 30));
    const auto Result = F.PWrite(Bytes, Chunk, Offset);
    if (Result <= 0) {
      return false;
    }
    Bytes += Result;
    Size -= Result;
    Offset += Result;
  }
  return true;
}

enum class OpenResult {
  Failed,
  Existing, // a file of this format
  Fresh,    // empty, or not a file of this format: needs Start
};

OpenResult OpenFile(FEXCore::File::File& F, const fextl::string& Path, const char (&Magic)[8], bool Truncate, uint64_t& Size) {
  using FEXCore::File::FileModes;
  F = FEXCore::File::File(); // close any earlier handle first
  F = FEXCore::File::File(Path.c_str(),
                          FileModes::READ | FileModes::WRITE | FileModes::CREATE | (Truncate ? FileModes::TRUNCATE : FileModes {}), false);
  if (!F.IsValid()) {
    return OpenResult::Failed;
  }
  Size = FileSize(F);
  FileHeader Header {};
  if (Size >= HeaderSize && ReadExact(F, &Header, sizeof(Header), 0) && memcmp(Header.Magic, Magic, sizeof(Magic)) == 0 &&
      Header.Version == FormatVersion) {
    return OpenResult::Existing;
  }
  return OpenResult::Fresh;
}

bool Start(FEXCore::File::File& F, const char (&Magic)[8]) {
  FileHeader Header {};
  memcpy(Header.Magic, Magic, sizeof(Magic));
  Header.Version = FormatVersion;
  return WriteExact(F, &Header, sizeof(Header), 0);
}

uint64_t AlignUp(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

// Reads up to Size bytes with a Linux file, fewer at the end of the file. Returns how many, or a negative errno when
// the first read failed.
int64_t LinuxReadUpTo(int Fd, uint8_t* Buffer, uint64_t Size, uint64_t Offset) {
  uint64_t Total {};
  while (Total < Size) {
    const int64_t Result = FEX::Windows::Linux::Pread(Fd, Buffer + Total, Size - Total, Offset + Total);
    if (Result <= 0) {
      return Total ? static_cast<int64_t>(Total) : Result;
    }
    Total += Result;
  }
  return static_cast<int64_t>(Total);
}

// Build 90, the learned streams (FEX_UNPACK_CACHE_LEARN, see above). Ranges and their block bits change on FEXFileCache
// only (Pass); game threads read the bits, and hand in what the main thread used through Push.
namespace Learned {
  constexpr uint64_t BlockShift = 16; // ranges cover whole 64 KB blocks of data.bin
  constexpr uint64_t BlockSize = 1ull << BlockShift;
  constexpr size_t MaxRanges = 4096;
  constexpr uint32_t ForgetSessions = 30;
  constexpr size_t MaxCandidates = 1024;
  constexpr uint64_t SaveEveryMs = 3'000;
  constexpr size_t MaxListBytes = 2 << 20;
  constexpr uint64_t AskPart = 512 << 10; // per WILLNEED call: the device's read-ahead size in the build 88 and 89 logs

  struct Range {
    uint64_t Offset;
    uint64_t Bytes;
    uint32_t Seen; // sessions in which the main thread used it
    uint32_t Last; // the last of them
    bool New;      // learned in this session
    bool Counted;  // this session is counted in Seen
  };

  struct Candidate {
    uint64_t Offset;
    uint32_t Bytes;
    uint64_t Frame; // GameFiles::FrameNow() when it was used
  };

  struct Counters {
    // Per second.
    std::atomic<uint64_t> NewRanges, NewBytes; // ranges added, bytes added (new ranges and grown ones)
    std::atomic<uint64_t> LeftOut;             // used in a loading screen's frame, not learned
    std::atomic<uint64_t> Lost;                // not noted (no room)
    std::atomic<uint64_t> Evicted;             // ranges dropped for the budget
    std::atomic<uint64_t> Asks, AskBytes, AskCalls, AskNs;
    std::atomic<uint64_t> SaveFailed;
    // Now: ranges and bytes in the list, ranges used this session, ranges and bytes learned this session.
    std::atomic<uint64_t> ListRanges, ListBytes, UsedRanges, SessionRanges, SessionBytes;
  };

  uint64_t Budget {}; // bytes, 0 = off
  fextl::string Path;
  int Fd {-1}; // data.bin's Linux file (Cache::LinuxRead)
  uint32_t Session {1};
  Range Ranges[MaxRanges];
  size_t Count {};
  bool Dirty {};
  bool Unsorted {};
  uint64_t LastSave {};
  bool Asked {};
  uint64_t Words {};
  std::atomic<uint64_t>* Bits {};     // learned blocks
  std::atomic<uint64_t>* UsedBits {}; // learned blocks a lookup took this session
  std::mutex CandidateLock;
  Candidate Candidates[MaxCandidates];
  size_t CandidateCount {};
  Counters Stats {};
  std::atomic<bool> ErrorNoted {};

  double MB(uint64_t Bytes) {
    return static_cast<double>(Bytes) / (1024.0 * 1024.0);
  }

  bool Has(const std::atomic<uint64_t>* Map, uint64_t Block) {
    return Block < Words * 64 && (Map[Block >> 6].load(std::memory_order_relaxed) & (1ull << (Block & 63)));
  }

  // Whether every block of Size bytes at Offset is learned.
  bool AllLearned(uint64_t Offset, uint64_t Size) {
    if (!Budget || !Size) {
      return false;
    }
    for (uint64_t Block = Offset >> BlockShift; Block <= (Offset + Size - 1) >> BlockShift; ++Block) {
      if (!Has(Bits, Block)) {
        return false;
      }
    }
    return true;
  }

  void Mark(std::atomic<uint64_t>* Map, uint64_t Offset, uint64_t Size) {
    for (uint64_t Block = Offset >> BlockShift; Size && Block <= (Offset + Size - 1) >> BlockShift && Block < Words * 64; ++Block) {
      const uint64_t Bit = 1ull << (Block & 63);
      if (!(Map[Block >> 6].load(std::memory_order_relaxed) & Bit)) {
        Map[Block >> 6].fetch_or(Bit, std::memory_order_relaxed);
      }
    }
  }

  void MarkUsed(uint64_t Offset, uint64_t Size) {
    Mark(UsedBits, Offset, Size);
  }

  // FEXFileCache: the learned bits again from the ranges, after ranges went. Built aside and stored word by word, so a
  // drop on another thread never sees a block of a remaining range cleared.
  void RebuildBits() {
    static std::atomic<uint64_t>* Scratch = new std::atomic<uint64_t>[Words]();
    for (uint64_t i = 0; i < Words; ++i) {
      Scratch[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < Count; ++i) {
      Mark(Scratch, Ranges[i].Offset, Ranges[i].Bytes);
    }
    for (uint64_t i = 0; i < Words; ++i) {
      Bits[i].store(Scratch[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }

  bool RangeUsed(const Range& R) {
    for (uint64_t Block = R.Offset >> BlockShift; Block <= (R.Offset + R.Bytes - 1) >> BlockShift; ++Block) {
      if (Has(UsedBits, Block)) {
        return true;
      }
    }
    return false;
  }

  uint64_t TotalBytes() {
    uint64_t Bytes {};
    for (size_t i = 0; i < Count; ++i) {
      Bytes += Ranges[i].Bytes;
    }
    return Bytes;
  }

  // The range least worth keeping among those not used this session: used longest ago, then in the fewest sessions,
  // then the biggest. Count when there is none.
  size_t LeastWorth() {
    size_t Worst = Count;
    for (size_t i = 0; i < Count; ++i) {
      const auto& R = Ranges[i];
      if (R.New || R.Counted) {
        continue;
      }
      if (Worst == Count) {
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
    Ranges[i] = Ranges[--Count];
    Dirty = true;
    Unsorted = true;
  }

  // Past the budget, the ranges least worth keeping go (ranges used this session stay).
  void FitBudget() {
    bool Removed = false;
    while (TotalBytes() > Budget) {
      const size_t Worst = LeastWorth();
      if (Worst == Count) {
        break;
      }
      RemoveRange(Worst);
      Stats.Evicted.fetch_add(1, std::memory_order_relaxed);
      Removed = true;
    }
    if (Removed) {
      RebuildBits();
    }
  }

  // Sorts the ranges by offset and merges those that overlap or touch.
  void Coalesce() {
    if (!Unsorted) {
      return;
    }
    Unsorted = false;
    std::sort(Ranges, Ranges + Count, [](const Range& A, const Range& B) { return A.Offset < B.Offset; });
    size_t Out = 0;
    for (size_t i = 0; i < Count; ++i) {
      const auto& R = Ranges[i];
      if (Out && R.Offset <= Ranges[Out - 1].Offset + Ranges[Out - 1].Bytes) {
        auto& Into = Ranges[Out - 1];
        Into.Bytes = std::max(Into.Offset + Into.Bytes, R.Offset + R.Bytes) - Into.Offset;
        Into.Seen = std::max(Into.Seen, R.Seen);
        Into.Last = std::max(Into.Last, R.Last);
        Into.New = Into.New || R.New;
        Into.Counted = Into.Counted || R.Counted;
        Dirty = true;
      } else {
        Ranges[Out++] = R;
      }
    }
    Count = Out;
  }

  void UpdateTotals() {
    uint64_t Used {}, New {};
    for (size_t i = 0; i < Count; ++i) {
      Used += Ranges[i].Counted ? 1 : 0;
      New += Ranges[i].New ? 1 : 0;
    }
    Stats.ListRanges.store(Count, std::memory_order_relaxed);
    Stats.ListBytes.store(TotalBytes(), std::memory_order_relaxed);
    Stats.UsedRanges.store(Used, std::memory_order_relaxed);
    Stats.SessionRanges.store(New, std::memory_order_relaxed);
  }

  // Game threads: the main thread used a stream of Bytes at Offset of data.bin while frames were coming.
  void Push(uint64_t Offset, uint32_t Bytes) {
    const uint64_t Frame = GameFiles::FrameNow();
    if (!Frame || !Bytes) {
      return;
    }
    std::scoped_lock Guard {CandidateLock};
    if (CandidateCount < MaxCandidates) {
      Candidates[CandidateCount++] = {Offset, Bytes, Frame};
    } else {
      Stats.Lost.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Adds a stream to the list: a new range, or one it overlaps or touches grows.
  void Learn(const Candidate& C) {
    const uint64_t Start = C.Offset & ~(BlockSize - 1);
    const uint64_t End = AlignUp(C.Offset + C.Bytes, BlockSize);
    Range* Into = nullptr;
    for (size_t i = 0; i < Count; ++i) {
      auto& R = Ranges[i];
      if (Start <= R.Offset + R.Bytes && End >= R.Offset) {
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
      Unsorted = Unsorted || Grew; // it may touch its neighbours now
    } else {
      if (Count == MaxRanges) {
        Coalesce();
      }
      if (Count == MaxRanges) {
        const size_t Worst = LeastWorth();
        if (Worst == Count) {
          Stats.Lost.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        RemoveRange(Worst);
        Stats.Evicted.fetch_add(1, std::memory_order_relaxed);
        RebuildBits();
      }
      Into = &Ranges[Count++];
      *Into = {Start, End - Start, 0, 0, true, false};
      Grew = End - Start;
      Unsorted = true;
      Stats.NewRanges.fetch_add(1, std::memory_order_relaxed);
    }
    Mark(UsedBits, C.Offset, C.Bytes);
    if (!Grew) {
      return;
    }
    Stats.NewBytes.fetch_add(Grew, std::memory_order_relaxed);
    Stats.SessionBytes.fetch_add(Grew, std::memory_order_relaxed);
    Mark(Bits, Into->Offset, Into->Bytes);
    Dirty = true;
    FitBudget();
  }

  // The candidates whose frame ended: learned when it was a frame of play, left out when it was a loading screen's.
  void TakeCandidates() {
    static Candidate Local[MaxCandidates]; // FEXFileCache only
    size_t Taken {};
    {
      std::scoped_lock Guard {CandidateLock};
      Taken = CandidateCount;
      std::copy(Candidates, Candidates + Taken, Local);
      CandidateCount = 0;
    }
    size_t Waiting {};
    for (size_t i = 0; i < Taken; ++i) {
      const auto& C = Local[i];
      const int Kind = GameFiles::FrameKind(C.Frame);
      if (Kind == 1) {
        Learn(C);
      } else if (Kind == -1) {
        Local[Waiting++] = C; // its frame hasn't ended yet
      } else {
        Stats.LeftOut.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (Waiting) {
      std::scoped_lock Guard {CandidateLock};
      for (size_t i = 0; i < Waiting; ++i) {
        if (CandidateCount < MaxCandidates) {
          Candidates[CandidateCount++] = Local[i];
        } else {
          Stats.Lost.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }

  // Ranges a lookup took this session count it once (Seen, Last).
  void CountUsed() {
    for (size_t i = 0; i < Count; ++i) {
      auto& R = Ranges[i];
      if (!R.Counted && (R.New || RangeUsed(R))) {
        R.Counted = true;
        ++R.Seen;
        R.Last = Session;
        Dirty = true;
      }
    }
  }

  // Asks storage for every range (WILLNEED); blocks already in the file cache cost only the look-up.
  void Ask() {
    const uint64_t Begin = ReadTicks();
    uint64_t Bytes {}, Calls {};
    for (size_t i = 0; i < Count; ++i) {
      const auto& R = Ranges[i];
      const uint64_t End = R.Offset + R.Bytes;
      for (uint64_t At = R.Offset; At < End; At += AskPart) {
        FEX::Windows::Linux::ReadAhead(Fd, At, std::min(AskPart, End - At));
        ++Calls;
      }
      Bytes += R.Bytes;
    }
    Stats.Asks.fetch_add(1, std::memory_order_relaxed);
    Stats.AskBytes.fetch_add(Bytes, std::memory_order_relaxed);
    Stats.AskCalls.fetch_add(Calls, std::memory_order_relaxed);
    Stats.AskNs.fetch_add((ReadTicks() - Begin) * 1'000'000'000 / TickFrequency, std::memory_order_relaxed);
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

  constexpr char Header[] = "FEX unpack cache learned 1\n";

  // Reads the list ("FEX unpack cache learned 1", "session N", then "offset bytes seen last" lines). A note for the log.
  fextl::string Load(uint64_t DataEnd, bool Fresh) {
    if (Fresh) {
      return "data.bin started over, so the list does too";
    }
    fextl::string Text;
    if (!GameFiles::Store::ReadText(Path, Text, MaxListBytes)) {
      return "no list yet: this session learns, the next one reads it in";
    }
    if (Text.compare(0, sizeof(Header) - 1, Header) != 0) {
      return "the list there isn't one this build reads, so it starts over";
    }
    const char* P = Text.c_str() + sizeof(Header) - 1;
    uint64_t Previous {};
    if (strncmp(P, "session ", 8) != 0 || !ParseNumber(P += 8, Previous)) {
      return "the list there isn't one this build reads, so it starts over";
    }
    Session = static_cast<uint32_t>(std::min<uint64_t>(Previous, 1u << 30)) + 1;
    uint32_t Loaded {}, Outside {}, Forgotten {}, Unreadable {};
    while (*P) {
      while (*P == '\n') {
        ++P;
      }
      if (!*P) {
        break;
      }
      uint64_t Offset, Bytes, Seen, Last;
      const bool Good = ParseNumber(P, Offset) && ParseNumber(P, Bytes) && ParseNumber(P, Seen) && ParseNumber(P, Last) && Bytes;
      const char* End = strchr(P, '\n');
      P = End ? End : P + strlen(P);
      if (!Good) {
        ++Unreadable;
        continue;
      }
      if (Offset < HeaderSize || Offset + Bytes > AlignUp(DataEnd, BlockSize)) {
        ++Outside;
        continue;
      }
      if (Last + ForgetSessions < Session) {
        ++Forgotten;
        continue;
      }
      if (Count < MaxRanges) {
        Ranges[Count++] = {Offset, Bytes, static_cast<uint32_t>(Seen), static_cast<uint32_t>(Last), false, false};
        ++Loaded;
      }
    }
    Unsorted = true;
    return fextl::fmt::format("{} ranges from earlier sessions{}{}{}", Loaded,
                              Outside ? fextl::fmt::format(", {} dropped (past the end of data.bin)", Outside) : fextl::string(),
                              Forgotten ? fextl::fmt::format(", {} dropped (not used in {} sessions)", Forgotten, ForgetSessions) : fextl::string(),
                              Unreadable ? fextl::fmt::format(", {} lines unreadable", Unreadable) : fextl::string());
  }

  // Writes the list when it changed, at most every SaveEveryMs unless Now.
  void Save(bool Now) {
    if (!Dirty) {
      return;
    }
    const uint64_t At = ReadTicks();
    if (!Now && LastSave && At - LastSave < SaveEveryMs * TickFrequency / 1000) {
      return;
    }
    LastSave = At;
    fextl::string Text = fextl::fmt::format("{}session {}\n", Header, Session);
    for (size_t i = 0; i < Count; ++i) {
      const auto& R = Ranges[i];
      Text += fextl::fmt::format("{} {} {} {}\n", R.Offset, R.Bytes, R.Seen, R.Last);
    }
    if (GameFiles::Store::WriteText(Path, Text)) {
      Dirty = false;
    } else {
      Stats.SaveFailed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // FEXFileCache, every pass (GameFiles::SetPassHook): learns the candidates, counts the ranges used, asks storage for
  // the list at the first pass and when a loading screen starts, saves the list.
  void Pass(bool LoadStart, bool LowMemory) {
    TakeCandidates();
    CountUsed();
    Coalesce();
    // The first pass is in the first loading screen (the cache opens when the game first unpacks), so it asks at once;
    // in a first session the list fills during play, and is asked for at the next loading screen.
    if ((!Asked || LoadStart) && Count && !LowMemory) {
      Ask();
    }
    Asked = true;
    UpdateTotals();
    Save(false);
  }

  // The 'ucache learned' line, in seconds something happened to the list.
  fextl::string Line() {
    if (!Budget) {
      return {};
    }
    const uint64_t NewRanges = Stats.NewRanges.exchange(0, std::memory_order_relaxed);
    const uint64_t NewBytes = Stats.NewBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t LeftOut = Stats.LeftOut.exchange(0, std::memory_order_relaxed);
    const uint64_t Lost = Stats.Lost.exchange(0, std::memory_order_relaxed);
    const uint64_t Evicted = Stats.Evicted.exchange(0, std::memory_order_relaxed);
    const uint64_t Asks = Stats.Asks.exchange(0, std::memory_order_relaxed);
    const uint64_t AskBytes = Stats.AskBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t AskCalls = Stats.AskCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t AskNs = Stats.AskNs.exchange(0, std::memory_order_relaxed);
    const uint64_t SaveFailed = Stats.SaveFailed.exchange(0, std::memory_order_relaxed);
    if (!NewRanges && !NewBytes && !LeftOut && !Lost && !Evicted && !Asks && !SaveFailed) {
      return {};
    }
    fextl::string Out = fextl::fmt::format(
      "  ucache learned: {} ranges {:.1f} MB ({} used this session, {} learned this session {:.1f} MB) | this second: learned {} new ranges, "
      "{:.1f} MB added",
      Stats.ListRanges.load(std::memory_order_relaxed), MB(Stats.ListBytes.load(std::memory_order_relaxed)),
      Stats.UsedRanges.load(std::memory_order_relaxed), Stats.SessionRanges.load(std::memory_order_relaxed),
      MB(Stats.SessionBytes.load(std::memory_order_relaxed)), NewRanges, MB(NewBytes));
    if (LeftOut) {
      Out += fextl::fmt::format(", {} used in a loading screen left out", LeftOut);
    }
    if (Lost) {
      Out += fextl::fmt::format(", {} not noted (no room)", Lost);
    }
    if (Evicted) {
      Out += fextl::fmt::format(", {} ranges dropped for the budget", Evicted);
    }
    if (Asks) {
      Out += fextl::fmt::format(" | asked of storage (start or loading screen): {:.1f} MB in {} calls, {:.1f} ms", MB(AskBytes), AskCalls,
                                static_cast<double>(AskNs) / 1e6);
    }
    if (SaveFailed) {
      Out += fextl::fmt::format(" | saving the list failed ({})", Path);
    }
    return Out + "\n";
  }
} // namespace Learned

// Drops a range of data.bin from the kernel's file cache, see above. Learned blocks (build 90) stay.
void DropRange(const Cache* C, uint64_t Offset, uint64_t Size) {
  if (C->LinuxRead < 0 || !Size) {
    return;
  }
  const uint64_t Start = Offset & ~(PageSize - 1);
  const uint64_t End = AlignUp(Offset + Size, PageSize);
  if (!Learned::Budget) {
    FEX::Windows::Linux::DropFromFileCache(C->LinuxRead, Start, End - Start);
    __atomic_fetch_add(&FEXUnpackCacheStats.CacheDrops, 1, __ATOMIC_RELAXED);
    return;
  }
  // Runs of blocks that aren't learned (blocks are whole pages).
  uint64_t At = Start;
  while (At < End) {
    const auto BlockEnd = [End](uint64_t Pos) {
      return std::min(End, ((Pos >> Learned::BlockShift) + 1) << Learned::BlockShift);
    };
    if (Learned::Has(Learned::Bits, At >> Learned::BlockShift)) {
      At = BlockEnd(At);
      continue;
    }
    uint64_t RunEnd = BlockEnd(At);
    while (RunEnd < End && !Learned::Has(Learned::Bits, RunEnd >> Learned::BlockShift)) {
      RunEnd = BlockEnd(RunEnd);
    }
    FEX::Windows::Linux::DropFromFileCache(C->LinuxRead, At, RunEnd - At);
    __atomic_fetch_add(&FEXUnpackCacheStats.CacheDrops, 1, __ATOMIC_RELAXED);
    At = RunEnd;
  }
}

// A stored stream's data, read from disk.
bool ReadEntry(Cache* C, uint8_t* Out, uint64_t Offset, uint32_t Size) {
  if (C->LinuxRead >= 0) {
    const int64_t Result = LinuxReadUpTo(C->LinuxRead, Out, Size, Offset);
    DropRange(C, Offset, Size);
    if (Result == Size) {
      return true;
    }
    // Linux refuses to write into memory the process has protected (EFAULT), which Wine's reads handle.
  }
  return ReadExact(C->Data, Out, Size, Offset);
}

// A read-ahead chunk: a direct read where it works, else a read whose pages are dropped from the file cache afterwards.
uint64_t ReadChunk(Cache* C, uint8_t* Buffer, uint64_t Offset) {
  if (const int Direct = C->LinuxDirect.load(std::memory_order_relaxed); Direct >= 0) {
    const int64_t Result = FEX::Windows::Linux::Pread(Direct, Buffer, ChunkSize, Offset);
    if (Result >= 0) {
      __atomic_fetch_add(&FEXUnpackCacheStats.AheadDirect, 1, __ATOMIC_RELAXED);
      // Some files can't be read past the cache (f2fs reads compressed files and those without inline encryption through
      // it anyway, without an error): drop it from there too.
      DropRange(C, Offset, ChunkSize);
      return static_cast<uint64_t>(Result);
    }
    C->LinuxDirect.store(-1, std::memory_order_relaxed);
    FEX::Windows::Linux::Close(Direct);
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "unpack cache: a direct read failed (error {}), reading ahead through the file cache and dropping it from there", -Result));
  }
  if (C->LinuxRead >= 0) {
    const int64_t Result = LinuxReadUpTo(C->LinuxRead, Buffer, ChunkSize, Offset);
    DropRange(C, Offset, ChunkSize);
    return Result > 0 ? static_cast<uint64_t>(Result) : 0;
  }
  return ReadUpTo(C->Data, Buffer, ChunkSize, Offset);
}

// Wine keeps the Linux file it opened for data.bin, so data.bin's Linux path is the target of one of this process's
// /proc/self/fd links.
struct FindDataArgs {
  char Path[1024];
  bool Found;
};

void FindDataFile(const char* Name, void* Arg) {
  auto* A = static_cast<FindDataArgs*>(Arg);
  if (A->Found) {
    return;
  }
  const fextl::string Link = fextl::fmt::format("/proc/self/fd/{}", Name);
  const int64_t Len = FEX::Windows::Linux::ReadLink(Link.c_str(), A->Path, sizeof(A->Path));
  constexpr char Suffix[] = "/unpackcache/data.bin";
  constexpr int64_t SuffixLen = sizeof(Suffix) - 1;
  if (Len <= SuffixLen) {
    return;
  }
  for (int64_t i = 0; i < SuffixLen; ++i) {
    char Ch = A->Path[Len - SuffixLen + i];
    if (Ch >= 'A' && Ch <= 'Z') {
      Ch = static_cast<char>(Ch - 'A' + 'a');
    }
    if (Ch != Suffix[i]) {
      return;
    }
  }
  A->Found = true;
}

// Opens data.bin's Linux files (Probe: page-aligned memory for a test read). Returns a note for the log.
fextl::string OpenLinuxFiles(Cache* C, uint8_t* Probe) {
  const char* Env = FEX::Windows::Switch("FEX_UNPACK_CACHE_DIRECT");
  if (!Env || Env[0] != '1') {
    return "unpack cache: data.bin is read through the file cache (FEX_UNPACK_CACHE_DIRECT=1 keeps it out)";
  }
  FindDataArgs Args {};
  if (!FEX::Windows::Linux::ForEachEntry("/proc/self/fd", FindDataFile, &Args) || !Args.Found) {
    return "unpack cache: can't find data.bin's Linux file, it is read through the file cache";
  }
  C->LinuxRead = FEX::Windows::Linux::OpenRead(Args.Path, false);
  if (C->LinuxRead < 0) {
    const int Error = -C->LinuxRead;
    C->LinuxRead = -1;
    return fextl::fmt::format("unpack cache: can't open {} (error {}), it is read through the file cache", Args.Path, Error);
  }
  int Direct = FEX::Windows::Linux::OpenRead(Args.Path, true);
  int64_t Tried = Direct;
  if (Direct >= 0) {
    Tried = FEX::Windows::Linux::Pread(Direct, Probe, PageSize, 0);
    if (Tried < 0) {
      FEX::Windows::Linux::Close(Direct);
      Direct = -1;
    }
  }
  C->LinuxDirect.store(Direct, std::memory_order_relaxed);
  DropRange(C, 0, PageSize);
  return fextl::fmt::format(
    "unpack cache: data.bin is kept out of the file cache ({}): read ahead with {}, other reads and writes "
    "dropped from the cache afterwards (FEX_UNPACK_CACHE_DIRECT=0 turns it off)",
    Args.Path, Direct >= 0 ? fextl::string("direct reads") : fextl::fmt::format("normal reads (direct reads failed: error {})", -Tried));
}

// Log thread state.
FEXUnpackCacheCounters Prev {};
} // namespace

namespace {
// Queues the chunks First..First+Count-1 that aren't in memory or queued yet. Takes AheadLock.
void RequestChunksLocked(Cache* C, uint64_t First, uint64_t Count) {
  bool Added = false;
  const uint64_t End = (C->DataEndHint.load(std::memory_order_relaxed) + ChunkSize - 1) / ChunkSize;
  for (uint64_t Chunk = First; Chunk < std::min(First + Count, End); ++Chunk) {
    if (C->AheadRequests.size() >= MaxAheadRequests) {
      break;
    }
    if (C->ChunkSlots.contains(Chunk) || std::find(C->AheadRequests.begin(), C->AheadRequests.end(), Chunk) != C->AheadRequests.end()) {
      continue;
    }
    C->AheadRequests.push_back(Chunk);
    Added = true;
  }
  if (Added) {
    C->AheadWake.notify_one();
  }
}

// How many chunks to read past a hit at First, at most Distance: with FEX_UNPACK_CACHE_AHEAD=ramp one, plus one for every
// two chunks right before First the game already used, in order. Takes AheadLock.
uint64_t LeadLocked(Cache* C, uint64_t First, uint64_t Distance) {
  if (!C->AheadRamp) {
    return Distance;
  }
  uint64_t Run = 0;
  for (uint64_t Chunk = First; Chunk > 0 && Run < 2 * Distance;) {
    --Chunk;
    const auto It = C->ChunkSlots.find(Chunk);
    if (It == C->ChunkSlots.end() || C->Slots[It->second].Loading || !C->Slots[It->second].Used) {
      break;
    }
    ++Run;
  }
  return std::min(Distance, 1 + Run / 2);
}

// Copies the entry's data out of read-ahead memory if all of it is there, and asks for up to Distance chunks after it.
bool CopyFromMemory(Cache* C, const Entry& E, uint8_t* Out, uint64_t Distance) {
  const uint64_t First = E.Offset / ChunkSize;
  const uint64_t Last = (E.Offset + E.Produced - 1) / ChunkSize;
  if (!E.Produced || Last - First + 1 > MaxHitChunks) {
    return false;
  }
  uint32_t Pinned[MaxHitChunks];
  {
    std::scoped_lock Guard {C->AheadLock};
    for (uint64_t Chunk = First; Chunk <= Last; ++Chunk) {
      const auto It = C->ChunkSlots.find(Chunk);
      const uint64_t NeedEnd = std::min(E.Offset + E.Produced, (Chunk + 1) * ChunkSize) - Chunk * ChunkSize;
      if (It == C->ChunkSlots.end() || C->Slots[It->second].Loading || C->Slots[It->second].Bytes < NeedEnd) {
        return false;
      }
    }
    for (uint64_t Chunk = First; Chunk <= Last; ++Chunk) {
      const uint32_t Index = C->ChunkSlots[Chunk];
      auto& S = C->Slots[Index];
      ++S.Users;
      S.LastUse = ++C->AheadClock;
      S.Used = true;
      Pinned[Chunk - First] = Index;
    }
    RequestChunksLocked(C, Last + 1, LeadLocked(C, First, Distance));
  }
  uint64_t Done {};
  for (uint64_t Chunk = First; Chunk <= Last; ++Chunk) {
    const uint64_t From = std::max(E.Offset + Done, Chunk * ChunkSize);
    const uint64_t To = std::min<uint64_t>(E.Offset + E.Produced, (Chunk + 1) * ChunkSize);
    memcpy(Out + Done, C->AheadMemory + Pinned[Chunk - First] * ChunkSize + (From - Chunk * ChunkSize), To - From);
    Done += To - From;
  }
  std::scoped_lock Guard {C->AheadLock};
  for (uint64_t Chunk = First; Chunk <= Last; ++Chunk) {
    --C->Slots[Pinned[Chunk - First]].Users;
  }
  return true;
}

// A hit wasn't in read-ahead memory. When it follows another recent one in data.bin, the hits are coming in sequence:
// read its chunk and the Distance ones after it into memory (with FEX_UNPACK_CACHE_AHEAD=ramp the next one only; the
// hits in them then read further).
void NoteDiskHit(Cache* C, const Entry& E, uint64_t Distance) {
  const uint64_t Chunk = E.Offset / ChunkSize;
  std::scoped_lock Guard {C->AheadLock};
  bool Sequential = false;
  for (const uint64_t Recent : C->RecentDisk) {
    Sequential |= Recent && (Recent - 1 == Chunk || Recent == Chunk); // same chunk, or the one before
  }
  C->RecentDisk[C->RecentDiskNext] = Chunk + 1;
  C->RecentDiskNext = (C->RecentDiskNext + 1) % RecentDiskChunks;
  if (Sequential) {
    RequestChunksLocked(C, Chunk, (C->AheadRamp ? std::min<uint64_t>(Distance, 1) : Distance) + 1);
  }
}

// Data was written to data.bin: forget any copy of those chunks in memory, which may be from before the write.
void ForgetChunks(Cache* C, uint64_t Offset, uint64_t Size) {
  if (!C->ReadAheadSlots || !Size) {
    return;
  }
  std::scoped_lock Guard {C->AheadLock};
  for (uint64_t Chunk = Offset / ChunkSize; Chunk <= (Offset + Size - 1) / ChunkSize; ++Chunk) {
    if (const auto It = C->ChunkSlots.find(Chunk); It != C->ChunkSlots.end()) {
      // A chunk still being read is dropped by the reader when it finds it gone from ChunkSlots.
      if (!C->Slots[It->second].Loading) {
        C->Slots[It->second].Chunk = NoChunk;
      }
      C->ChunkSlots.erase(It);
    }
  }
}

// The read-ahead thread is started with the process, like the writer, and waits for the cache to open.
std::mutex ReaderLock;
std::condition_variable ReaderWake;
Cache* ReaderCache {};
fextl::unique_ptr<FEXCore::Threads::Thread> ReaderThreadObject;

void* ReaderThread(void*) {
  Cache* C;
  {
    std::unique_lock Guard {ReaderLock};
    ReaderWake.wait(Guard, [] { return ReaderCache != nullptr; });
    C = ReaderCache;
  }
  if (!C->ReadAheadSlots) {
    return nullptr;
  }
  C->AheadMemory = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(C->ReadAheadSlots * ChunkSize));
  if (!C->AheadMemory) {
    FEX::Windows::StatsLogNote("unpack cache: read-ahead off, can't allocate its memory");
    return nullptr;
  }
  C->Slots.resize(C->ReadAheadSlots);
  C->ReadAheadReady.store(true, std::memory_order_release);
  FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: read-ahead ready ({} MB)", C->ReadAheadSlots * ChunkSize >> 20));
  for (;;) {
    uint64_t Chunk;
    uint32_t Index;
    {
      std::unique_lock Guard {C->AheadLock};
      C->AheadWake.wait(Guard, [C] { return !C->AheadRequests.empty(); });
      Chunk = C->AheadRequests.front();
      C->AheadRequests.pop_front();
      if (C->ChunkSlots.contains(Chunk)) {
        continue;
      }
      // Reuse the least recently used slot no hit is copying from.
      Index = C->ReadAheadSlots;
      for (uint32_t i = 0; i < C->ReadAheadSlots; ++i) {
        const auto& S = C->Slots[i];
        if (!S.Users && !S.Loading && (Index == C->ReadAheadSlots || S.LastUse < C->Slots[Index].LastUse)) {
          Index = i;
        }
      }
      if (Index == C->ReadAheadSlots) {
        continue;
      }
      auto& S = C->Slots[Index];
      if (const auto Old = C->ChunkSlots.find(S.Chunk); S.Chunk != NoChunk && Old != C->ChunkSlots.end() && Old->second == Index) {
        if (!S.Used) {
          __atomic_fetch_add(&FEXUnpackCacheStats.AheadWasted, 1, __ATOMIC_RELAXED);
        }
        C->ChunkSlots.erase(Old);
      }
      S = {.Chunk = Chunk, .LastUse = ++C->AheadClock, .Loading = true};
      C->ChunkSlots[Chunk] = Index;
    }
    const uint64_t Start = ReadTicks();
    const uint64_t Bytes = ReadChunk(C, C->AheadMemory + Index * ChunkSize, Chunk * ChunkSize);
    __atomic_fetch_add(&FEXUnpackCacheStats.AheadChunks, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&FEXUnpackCacheStats.AheadBytes, Bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&FEXUnpackCacheStats.AheadTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
    std::scoped_lock Guard {C->AheadLock};
    auto& S = C->Slots[Index];
    S.Loading = false;
    S.Bytes = static_cast<uint32_t>(Bytes);
    if (const auto It = C->ChunkSlots.find(Chunk); It == C->ChunkSlots.end() || It->second != Index || !Bytes) {
      // Written to while it was read (ForgetChunks), or nothing there.
      if (It != C->ChunkSlots.end() && It->second == Index) {
        C->ChunkSlots.erase(It);
      }
      S.Chunk = NoChunk;
      S.LastUse = 0; // reused first
    }
  }
  return nullptr;
}
} // namespace

namespace {
// True while the game is playing rather than loading, see FEX_UNPACK_CACHE_PLAY above: fewer than PlayMaxLookups lookups in
// the previous window (counted as none when it ended more than a window ago) and so far in this one.
bool Playing(Cache* C, uint64_t Now) {
  if (!C->PlayGate) {
    return false;
  }
  uint64_t Start = C->WindowStart.load(std::memory_order_relaxed);
  if (Now - Start >= C->WindowTicks && C->WindowStart.compare_exchange_strong(Start, Now, std::memory_order_relaxed)) {
    const uint32_t Ended = C->WindowLookups.exchange(0, std::memory_order_relaxed);
    C->LastWindowLookups.store(Now - Start >= 2 * C->WindowTicks ? 0 : Ended, std::memory_order_relaxed);
  }
  const uint32_t InWindow = C->WindowLookups.fetch_add(1, std::memory_order_relaxed) + 1;
  return InWindow < PlayMaxLookups && C->LastWindowLookups.load(std::memory_order_relaxed) < PlayMaxLookups;
}
} // namespace

extern "C" int FEXUnpackCacheGet(const uint64_t Key[2], uint8_t* Out, uint32_t OutCap, uint32_t* Produced, uint32_t* Trailer) {
  auto* C = TheCache;
  if (!C) {
    return 0;
  }
  const uint64_t Start = ReadTicks();
  __atomic_fetch_add(&FEXUnpackCacheStats.Lookups, 1, __ATOMIC_RELAXED);
  const bool Play = Playing(C, Start);
  if (Play) {
    __atomic_fetch_add(&FEXUnpackCacheStats.PlayLookups, 1, __ATOMIC_RELAXED);
  }
  const uint64_t Distance = Play ? PlayAheadChunks : AheadChunks;

  Entry E;
  {
    std::scoped_lock Guard {C->Lock};
    const auto It = C->Entries.find({Key[0], Key[1]});
    if (It == C->Entries.end() || !It->second.Written) {
      return 0;
    }
    E = It->second;
  }
  if (E.Produced > OutCap) {
    return 0;
  }

  // Build 90: the main thread while frames are coming, see FEX_UNPACK_CACHE_MAIN_PLAY and FEX_UNPACK_CACHE_LEARN above.
  const bool MainPlay = (C->MainPlay || Learned::Budget) && GameFiles::OnMainThread() && GameFiles::FramesComing();
  const bool Known = Learned::AllLearned(E.Offset, E.Produced);
  if (MainPlay && Learned::Budget) {
    if (Known) {
      Learned::MarkUsed(E.Offset, E.Produced);
    } else {
      Learned::Push(E.Offset, E.Produced);
    }
  }

  // From read-ahead memory if it's there, else from disk. Either way it is checked against its hash.
  const bool Ahead = C->ReadAheadReady.load(std::memory_order_acquire);
  uint64_t Now = ReadTicks();
  bool Good = false;
  if (Ahead && CopyFromMemory(C, E, Out, Distance)) {
    const uint64_t Copied = ReadTicks();
    __atomic_fetch_add(&FEXUnpackCacheStats.RamHits, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&FEXUnpackCacheStats.RamTicks, Copied - Now, __ATOMIC_RELAXED);
    Good = XXH3_64bits(Out, E.Produced) == E.OutputHash;
    Now = ReadTicks();
    __atomic_fetch_add(&FEXUnpackCacheStats.VerifyTicks, Now - Copied, __ATOMIC_RELAXED);
    if (!Good) {
      // The copy in memory was older than the data on disk: drop it and read from disk.
      ForgetChunks(C, E.Offset, E.Produced);
    }
  }
  if (!Good && Known && C->LinuxRead >= 0) {
    // A learned stream: from the kernel's file cache if it is there, without waiting for storage.
    const int64_t Got = FEX::Windows::Linux::PreadCached(C->LinuxRead, Out, E.Produced, E.Offset);
    const uint64_t Copied = ReadTicks();
    if (Got == static_cast<int64_t>(E.Produced)) {
      __atomic_fetch_add(&FEXUnpackCacheStats.LearnedHits, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&FEXUnpackCacheStats.LearnedTicks, Copied - Now, __ATOMIC_RELAXED);
      Good = XXH3_64bits(Out, E.Produced) == E.OutputHash;
      Now = ReadTicks();
      __atomic_fetch_add(&FEXUnpackCacheStats.VerifyTicks, Now - Copied, __ATOMIC_RELAXED);
      if (!Learned::Has(Learned::UsedBits, E.Offset >> Learned::BlockShift)) {
        Learned::MarkUsed(E.Offset, E.Produced);
      }
    } else {
      __atomic_fetch_add(&FEXUnpackCacheStats.LearnedNotCached, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&FEXUnpackCacheStats.LearnedNotCachedTicks, Copied - Now, __ATOMIC_RELAXED);
      Now = Copied;
      constexpr int64_t EAGAIN_ = 11;
      if (Got < 0 && Got != -EAGAIN_ && !Learned::ErrorNoted.exchange(true, std::memory_order_relaxed)) {
        FEX::Windows::StatsLogNote(
          fextl::fmt::format("unpack cache: a read of a learned stream from the file cache failed (error {}), not only 'not there'", -Got));
      }
    }
  }
  if (!Good) {
    if (Ahead) {
      NoteDiskHit(C, E, Distance);
    }
    if (Play || (MainPlay && C->MainPlay)) {
      // Left to the game: its own unpacking is faster than this read, see FEX_UNPACK_CACHE_PLAY above. The entry stays.
      __atomic_fetch_add(&FEXUnpackCacheStats.PlaySkips, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&FEXUnpackCacheStats.PlaySkipBytes, E.Produced, __ATOMIC_RELAXED);
      if (!Play) {
        __atomic_fetch_add(&FEXUnpackCacheStats.MainPlaySkips, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&FEXUnpackCacheStats.MainPlaySkipBytes, E.Produced, __ATOMIC_RELAXED);
      }
      return 0;
    }
    const bool Read = ReadEntry(C, Out, E.Offset, E.Produced);
    const uint64_t ReadDone = ReadTicks();
    __atomic_fetch_add(&FEXUnpackCacheStats.DiskTicks, ReadDone - Now, __ATOMIC_RELAXED);
    Good = Read && XXH3_64bits(Out, E.Produced) == E.OutputHash;
    __atomic_fetch_add(&FEXUnpackCacheStats.VerifyTicks, ReadTicks() - ReadDone, __ATOMIC_RELAXED);
  }
  if (!Good) {
    // Bad data: forget it so it isn't read again. A later miss stores the stream anew.
    __atomic_fetch_add(&FEXUnpackCacheStats.Failures, 1, __ATOMIC_RELAXED);
    std::scoped_lock Guard {C->Lock};
    C->Entries.erase({Key[0], Key[1]});
    return 0;
  }
  *Produced = E.Produced;
  *Trailer = E.Trailer;
  __atomic_fetch_add(&FEXUnpackCacheStats.Hits, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.HitBytes, E.Produced, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.HitTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
  return 1;
}

namespace {
// Takes data and index space for a new stream and marks its key as taken, so it's stored only once. With UseRing, also
// takes ring space and queues the job (Job then points at the queued copy). Returns false when the stream isn't stored.
bool Reserve(Cache* C, const uint64_t Key[2], uint32_t Produced, uint32_t Trailer, bool UseRing, Job*& J, Job& Direct) {
  std::scoped_lock Guard {C->Lock};
  if (C->Entries.contains({Key[0], Key[1]})) {
    return false;
  }
  uint64_t RingStart = C->RingHead;
  if (UseRing) {
    // The data has to be in one piece: skip what's left at the end of the ring if it doesn't fit there.
    if (RingStart % RingSize + Produced > RingSize) {
      RingStart += RingSize - RingStart % RingSize;
    }
    if (RingStart + Produced - C->RingTail > RingSize) {
      __atomic_fetch_add(&FEXUnpackCacheStats.Dropped, 1, __ATOMIC_RELAXED);
      return false;
    }
  }
  // Build 91: a stream starts on a page of its own, so writing it never waits for the stream before it (see above).
  const uint64_t Offset = C->Flush ? AlignUp(C->DataEnd, PageSize) : C->DataEnd;
  if (Offset + Produced > C->MaxDataBytes) {
    C->Full = true;
    FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: full at {} MB, not storing more", C->DataEnd >> 20));
    return false;
  }
  Job New {};
  New.R.Key[0] = Key[0];
  New.R.Key[1] = Key[1];
  New.R.Produced = Produced;
  New.R.Trailer = Trailer;
  New.R.Offset = Offset;
  New.RecordOffset = C->IndexEnd;
  C->DataEnd = AlignUp(Offset + Produced, DataAlignment);
  C->DataEndHint.store(C->DataEnd, std::memory_order_relaxed);
  C->IndexEnd += sizeof(Record);
  C->Entries.emplace(std::pair {Key[0], Key[1]}, Entry {New.R.Offset, 0, Produced, Trailer, false});
  if (UseRing) {
    New.RingStart = RingStart;
    New.RingEnd = RingStart + Produced;
    C->RingHead = New.RingEnd;
    // Stays valid while other jobs are added and taken: a deque keeps its elements in place, and the writer doesn't take
    // this one before it's ready.
    J = &C->Queue.emplace_back(New);
  } else {
    Direct = New;
    J = &Direct;
  }
  return true;
}

// Hashes and writes a reserved stream's data and record, then makes it findable. Batched: the writer thread drops it from
// the file cache later, with its batch (build 91); otherwise it is dropped now.
void Write(Cache* C, Job& J, const uint8_t* Data, bool Batched) {
  const uint64_t Start = ReadTicks();
  J.R.OutputHash = XXH3_64bits(Data, J.R.Produced);
  J.R.Check = RecordCheck(J.R);
  const bool Written = WriteExact(C->Data, Data, J.R.Produced, J.R.Offset) && WriteExact(C->Index, &J.R, sizeof(J.R), J.RecordOffset);
  ForgetChunks(C, J.R.Offset, J.R.Produced);
  if (!Batched) {
    DropRange(C, J.R.Offset, J.R.Produced);
  }
  {
    std::scoped_lock Guard {C->Lock};
    if (Written) {
      C->Entries.insert_or_assign(std::pair {J.R.Key[0], J.R.Key[1]}, Entry {J.R.Offset, J.R.OutputHash, J.R.Produced, J.R.Trailer, true});
    } else {
      // The space stays unused; a record that wasn't written completely fails its check when loaded.
      C->Entries.erase({J.R.Key[0], J.R.Key[1]});
    }
  }
  if (Written) {
    __atomic_fetch_add(&FEXUnpackCacheStats.Written, 1, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&FEXUnpackCacheStats.Failures, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&FEXUnpackCacheStats.WriteTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
}

// Build 91: starts sending data.bin's written data up to Upto to storage (writer thread only).
void StartWriteOut(Cache* C, uint64_t Upto) {
  if (Upto <= C->StartedTo) {
    return;
  }
  const uint64_t From = C->StartedTo & ~(PageSize - 1);
  const uint64_t Start = ReadTicks();
  if (FEX::Windows::Linux::WriteOut(C->LinuxRead, From, Upto - From, false) < 0) {
    __atomic_fetch_add(&FEXUnpackCacheStats.FlushFailures, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&FEXUnpackCacheStats.FlushStartTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
  C->StartedTo = Upto;
}

// Build 91: waits until data.bin's written data up to Upto is on storage, then drops it from the file cache (learned
// blocks stay, see DropRange). Writer thread only.
void FlushWritten(Cache* C, uint64_t Upto) {
  if (Upto <= C->FlushedTo) {
    return;
  }
  const uint64_t From = C->FlushedTo & ~(PageSize - 1);
  const uint64_t Start = ReadTicks();
  if (FEX::Windows::Linux::WriteOut(C->LinuxRead, From, Upto - From, true) < 0) {
    __atomic_fetch_add(&FEXUnpackCacheStats.FlushFailures, 1, __ATOMIC_RELAXED);
  }
  const uint64_t Written = ReadTicks();
  DropRange(C, From, Upto - From);
  __atomic_fetch_add(&FEXUnpackCacheStats.Flushes, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.FlushBytes, Upto - C->FlushedTo, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.FlushWaitTicks, Written - Start, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.FlushDropTicks, ReadTicks() - Written, __ATOMIC_RELAXED);
  C->FlushedTo = Upto;
}

// The writer thread is started with the process (NativeUnpackCache::StartWriter) and waits for the cache to open.
std::mutex WriterLock;
std::condition_variable WriterWake;
Cache* WriterCache {}; // set when the cache opens
fextl::unique_ptr<FEXCore::Threads::Thread> WriterThreadObject;
std::atomic<int> WriterState {}; // 0 not running, 1 running and waiting for the cache, 2 its ring is ready
uint64_t OpenTicks {};
std::atomic<bool> DirectNoted {};
constexpr uint64_t WriterGraceSeconds = 3;

void* WriterThread(void*) {
  WriterState.store(1, std::memory_order_release);
  FEX::Windows::StatsLogNote("unpack cache: writer thread running");
  Cache* C;
  {
    std::unique_lock Guard {WriterLock};
    WriterWake.wait(Guard, [] { return WriterCache != nullptr; });
    C = WriterCache;
  }
  // Fault the ring's pages in here, so the game's threads don't pay for it when they copy into it.
  const uint64_t Start = ReadTicks();
  memset(C->Ring, 0, RingSize);
  C->RingReady.store(true, std::memory_order_release);
  WriterState.store(2, std::memory_order_release);
  FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: write buffer ready ({} MB in {} ms)", RingSize >> 20,
                                                (ReadTicks() - Start) * 1000 / TickFrequency));
  const bool Batched = C->Flush && C->LinuxRead >= 0;
  for (;;) {
    Job J;
    {
      std::unique_lock Guard {C->Lock};
      const auto Ready = [C] {
        return !C->Queue.empty() && C->Queue.front().Ready;
      };
      if (Batched && C->FlushedTo < C->WrittenTo) {
        // Something written isn't dropped yet: don't wait for the next stream longer than FlushIdleMs.
        if (!C->QueueWake.wait_for(Guard, std::chrono::milliseconds(FlushIdleMs), Ready)) {
          Guard.unlock();
          StartWriteOut(C, C->WrittenTo);
          FlushWritten(C, C->WrittenTo);
          continue;
        }
      } else {
        C->QueueWake.wait(Guard, Ready);
      }
      J = C->Queue.front();
      C->Queue.pop_front();
    }
    Write(C, J, C->Ring + J.RingStart % RingSize, Batched);
    if (Batched) {
      C->WrittenTo = std::max(C->WrittenTo, J.R.Offset + J.R.Produced);
      if (C->WrittenTo - C->StartedTo >= FlushBatch) {
        // Start this batch, then finish the one before it, which has had a whole batch's time to get to storage.
        const uint64_t Before = C->StartedTo;
        StartWriteOut(C, C->WrittenTo);
        FlushWritten(C, Before);
      }
    }
    std::scoped_lock Guard {C->Lock};
    C->RingTail = J.RingEnd;
  }
  return nullptr;
}
} // namespace

extern "C" void FEXUnpackCachePut(const uint64_t Key[2], const uint8_t* Out, uint32_t Produced, uint32_t Trailer) {
  auto* C = TheCache;
  if (!C || C->Full) {
    return;
  }
  const uint64_t Start = ReadTicks();

  // Normally the game's thread only copies the data into the ring. If the writer thread isn't ready a few seconds after
  // the cache opened, write on this thread instead, so the cache still fills.
  const bool UseRing = C->RingReady.load(std::memory_order_acquire);
  if (!UseRing) {
    if (ReadTicks() - OpenTicks < WriterGraceSeconds * TickFrequency) {
      __atomic_fetch_add(&FEXUnpackCacheStats.Dropped, 1, __ATOMIC_RELAXED);
      return;
    }
    if (!DirectNoted.exchange(true)) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: writer thread not ready (state {}), writing on the game's threads",
                                                    WriterState.load()));
    }
  }

  Job* J;
  Job Direct;
  if (!Reserve(C, Key, Produced, Trailer, UseRing, J, Direct)) {
    return;
  }
  __atomic_fetch_add(&FEXUnpackCacheStats.Stores, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.StoreBytes, Produced, __ATOMIC_RELAXED);
  if (Learned::Budget && GameFiles::OnMainThread() && GameFiles::FramesComing()) {
    // Build 90: a stream the main thread unpacked during play is learned where it is stored.
    Learned::Push(J->R.Offset, Produced);
  }
  if (UseRing) {
    memcpy(C->Ring + J->RingStart % RingSize, Out, Produced);
    {
      std::scoped_lock Guard {C->Lock};
      J->Ready = true;
    }
    C->QueueWake.notify_one();
  } else {
    Write(C, *J, Out, false);
  }
  __atomic_fetch_add(&FEXUnpackCacheStats.StoreTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
}

namespace {
// MD5 cache, see NativeUnpackCache.h. md5.bin: a header, then one 64-byte record per stored state, appended in batches
// of MD5Batch by the game thread that fills a batch (a batch not full at exit is lost, and stored again next time).
constexpr char MD5Magic[8] = {'F', 'E', 'X', 'U', 'N', 'M', 'D', '5'};
constexpr size_t MD5Batch = 64;
constexpr uint64_t MD5MaxRecords = 1u << 20; // 64 MB of records

struct MD5Record {
  uint64_t Key[2];
  uint64_t Bytes;
  uint32_t State[4];
  uint64_t Reserved[2];
  uint64_t Check; // hash of everything above, never 0
};
static_assert(sizeof(MD5Record) == 64);

uint64_t MD5RecordCheck(const MD5Record& R) {
  return XXH3_64bits(&R, offsetof(MD5Record, Check)) | 1;
}

struct MD5Value {
  uint64_t Bytes;
  uint32_t State[4];
};

struct MD5Cache {
  FEXCore::File::File F;
  std::mutex Lock;
  fextl::unordered_map<std::pair<uint64_t, uint64_t>, MD5Value, KeyHash> Map;
  MD5Record Batch[MD5Batch];
  size_t BatchCount {};
  uint64_t FileEnd {};
};
MD5Cache* TheMD5 {};

// Opens md5.bin in Dir and loads it. Returns a note for the log.
fextl::string OpenMD5Cache(const fextl::string& Dir, bool Reset) {
  auto* M = new MD5Cache;
  uint64_t Size {};
  auto Open = OpenFile(M->F, Dir + "md5.bin", MD5Magic, Reset, Size);
  if (Open == OpenResult::Fresh) {
    Open = OpenFile(M->F, Dir + "md5.bin", MD5Magic, true, Size);
    if (Open == OpenResult::Failed || !Start(M->F, MD5Magic)) {
      Open = OpenResult::Failed;
    }
    Size = HeaderSize;
  }
  if (Open == OpenResult::Failed) {
    delete M;
    return "md5 cache: off, can't open md5.bin";
  }
  const uint64_t Count = std::min((Size - HeaderSize) / sizeof(MD5Record), MD5MaxRecords);
  uint64_t Skipped {};
  if (Count) {
    fextl::vector<MD5Record> Records(Count);
    if (!ReadExact(M->F, Records.data(), Count * sizeof(MD5Record), HeaderSize)) {
      Records.clear();
      Skipped = Count;
    }
    M->Map.reserve(Records.size());
    for (const auto& R : Records) {
      if (R.Check != MD5RecordCheck(R)) {
        ++Skipped;
        continue;
      }
      M->Map.insert_or_assign(std::pair {R.Key[0], R.Key[1]}, MD5Value {R.Bytes, {R.State[0], R.State[1], R.State[2], R.State[3]}});
    }
  }
  M->FileEnd = HeaderSize + Count * sizeof(MD5Record);
  TheMD5 = M;
  __atomic_store_n(&FEXMD5CacheEnabled, 1, __ATOMIC_RELEASE);
  return fextl::fmt::format("md5 cache: on{}, {} states, {} records skipped", Reset ? " (reset)" : "", M->Map.size(), Skipped);
}
} // namespace

extern "C" int FEXMD5CacheGet(const uint64_t Key[2], uint64_t Bytes, uint32_t State[4]) {
  auto* M = TheMD5;
  std::scoped_lock Guard {M->Lock};
  const auto It = M->Map.find({Key[0], Key[1]});
  if (It == M->Map.end() || It->second.Bytes != Bytes) {
    return 0;
  }
  memcpy(State, It->second.State, sizeof(It->second.State));
  return 1;
}

extern "C" void FEXMD5CachePut(const uint64_t Key[2], uint64_t Bytes, const uint32_t State[4]) {
  auto* M = TheMD5;
  MD5Record Out[MD5Batch];
  uint64_t Offset {};
  {
    std::scoped_lock Guard {M->Lock};
    if (M->Map.size() >= MD5MaxRecords) {
      return;
    }
    if (!M->Map.try_emplace(std::pair {Key[0], Key[1]}, MD5Value {Bytes, {State[0], State[1], State[2], State[3]}}).second) {
      return;
    }
    auto& R = M->Batch[M->BatchCount++];
    R = {.Key = {Key[0], Key[1]}, .Bytes = Bytes, .State = {State[0], State[1], State[2], State[3]}};
    R.Check = MD5RecordCheck(R);
    if (M->BatchCount < MD5Batch) {
      __atomic_fetch_add(&FEXUnpackCacheStats.MD5Stores, 1, __ATOMIC_RELAXED);
      return;
    }
    memcpy(Out, M->Batch, sizeof(Out));
    M->BatchCount = 0;
    Offset = M->FileEnd;
    M->FileEnd += sizeof(Out);
  }
  __atomic_fetch_add(&FEXUnpackCacheStats.MD5Stores, 1, __ATOMIC_RELAXED);
  // A failed write leaves a gap whose records fail their check when loaded.
  if (!WriteExact(M->F, Out, sizeof(Out), Offset)) {
    __atomic_fetch_add(&FEXUnpackCacheStats.Failures, 1, __ATOMIC_RELAXED);
  }
}

namespace NativeUnpackCache {
void StartWriter() {
  uint64_t Frequency {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
  TickFrequency = Frequency ? Frequency : 1;
  WriterThreadObject = FEXCore::Threads::Thread::Create(WriterThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXUnpackCache");
  if (!WriterThreadObject || !WriterThreadObject->joinable()) {
    FEX::Windows::StatsLogNote("unpack cache: can't start the writer thread, the game's threads will write");
  }
  ReaderThreadObject = FEXCore::Threads::Thread::Create(ReaderThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXUnpackRead");
  if (!ReaderThreadObject || !ReaderThreadObject->joinable()) {
    FEX::Windows::StatsLogNote("unpack cache: can't start the read-ahead thread, hits are read from disk");
  }
}

bool Init(bool MD5Cache) {
  const char* ResetEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_RESET");
  const bool Reset = ResetEnv && ResetEnv[0] == '1';
  const char* MaxEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_MAX_GB");
  uint64_t MaxGB = MaxEnv ? strtoull(MaxEnv, nullptr, 10) : 0;
  if (!MaxGB) {
    MaxGB = 16;
  }
  const char* AheadEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_READAHEAD_MB");
  const uint64_t AheadMB = std::min<uint64_t>(AheadEnv && AheadEnv[0] ? strtoull(AheadEnv, nullptr, 10) : DefaultReadAheadMB, 1024);

  uint64_t Frequency {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
  TickFrequency = Frequency ? Frequency : 1;

  // Created without the trailing separator, which create_directories may report as a failure even when it worked.
  const fextl::string DirName = FEXCore::Config::GetCacheDirectory() + "UnpackCache";
  const fextl::string Dir = DirName + "\\";
  FHU::Filesystem::CreateDirectories(DirName);
  if (!FHU::Filesystem::Exists(DirName)) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: off, can't create {}", Dir));
    return false;
  }

  auto* C = new Cache;
  uint64_t DataSize {}, IndexSize {};
  const fextl::string DataPath = Dir + "data.bin";
  const fextl::string IndexPath = Dir + "index.bin";
  auto DataOpen = OpenFile(C->Data, DataPath, DataMagic, Reset, DataSize);
  auto IndexOpen = OpenFile(C->Index, IndexPath, IndexMagic, Reset, IndexSize);
  bool Started = false;
  if (DataOpen == OpenResult::Fresh || IndexOpen == OpenResult::Fresh) {
    // Either file is new or unusable, so the other one no longer matches it: start both over, empty.
    DataOpen = OpenFile(C->Data, DataPath, DataMagic, true, DataSize);
    IndexOpen = OpenFile(C->Index, IndexPath, IndexMagic, true, IndexSize);
    if (DataOpen == OpenResult::Failed || IndexOpen == OpenResult::Failed || !Start(C->Data, DataMagic) || !Start(C->Index, IndexMagic)) {
      DataOpen = OpenResult::Failed;
    }
    DataSize = HeaderSize;
    IndexSize = HeaderSize;
    Started = true;
  }
  if (DataOpen == OpenResult::Failed || IndexOpen == OpenResult::Failed) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: off, can't open the files in {}", Dir));
    delete C;
    return false;
  }

  // Load the index, skipping records that are incomplete or point outside the data.
  uint64_t Skipped {};
  uint64_t DataEnd = HeaderSize;
  const uint64_t RecordCount = (IndexSize - HeaderSize) / sizeof(Record);
  if (RecordCount) {
    fextl::vector<Record> Records(RecordCount);
    if (!ReadExact(C->Index, Records.data(), RecordCount * sizeof(Record), HeaderSize)) {
      Records.clear();
      Skipped = RecordCount;
    }
    C->Entries.reserve(Records.size());
    for (const auto& R : Records) {
      if (R.Check != RecordCheck(R) || R.Offset < HeaderSize || R.Offset + R.Produced > DataSize) {
        ++Skipped;
        continue;
      }
      // A later record for the same key wins: it was stored again after the earlier one's data turned out bad.
      C->Entries.insert_or_assign(std::pair {R.Key[0], R.Key[1]}, Entry {R.Offset, R.OutputHash, R.Produced, R.Trailer, true});
      DataEnd = std::max(DataEnd, AlignUp(R.Offset + R.Produced, DataAlignment));
    }
  }
  C->DataEnd = std::max(DataEnd, AlignUp(DataSize, DataAlignment));
  C->DataEndHint.store(C->DataEnd, std::memory_order_relaxed);
  C->IndexEnd = HeaderSize + RecordCount * sizeof(Record);
  C->MaxDataBytes = MaxGB << 30;
  C->Full = C->DataEnd >= C->MaxDataBytes;

  C->Ring = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(RingSize));
  if (!C->Ring) {
    FEX::Windows::StatsLogNote("unpack cache: off, can't allocate its write buffer");
    delete C;
    return false;
  }
  // Before the writer gets the ring: its first page is free for the test read.
  const fextl::string LinuxNote = OpenLinuxFiles(C, C->Ring);
  C->ReadAheadSlots = static_cast<uint32_t>(AheadMB * (1ull << 20) / ChunkSize);
  if (C->ReadAheadSlots && C->ReadAheadSlots < AheadChunks + MaxHitChunks) {
    C->ReadAheadSlots = AheadChunks + MaxHitChunks;
  }
  const char* AheadMode = FEX::Windows::Switch("FEX_UNPACK_CACHE_AHEAD");
  C->AheadRamp = !(AheadMode && AheadMode[0] == 'f');
  // Build 91, see the top of this file. Needs data.bin's Linux file (FEX_UNPACK_CACHE_DIRECT=1).
  const char* FlushEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_FLUSH");
  C->Flush = FlushEnv && FlushEnv[0] == '1' && C->LinuxRead >= 0;
  C->WrittenTo = C->StartedTo = C->FlushedTo = C->DataEnd;
  const char* PlayEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_PLAY");
  C->PlayGate = PlayEnv && PlayEnv[0] == '1';
  C->WindowTicks = std::max<uint64_t>(TickFrequency * PlayWindowMs / 1000, 1);
  // Build 90, see the top of this file. Both need the frame clock and FEXFileCache (FEX_GAME_FILES_UNCACHED=1); learning
  // also needs data.bin's Linux file (FEX_UNPACK_CACHE_DIRECT=1).
  const char* MainPlayEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_MAIN_PLAY");
  C->MainPlay = MainPlayEnv && MainPlayEnv[0] == '1' && GameFiles::Running();
  const char* LearnEnv = FEX::Windows::Switch("FEX_UNPACK_CACHE_LEARN");
  const uint64_t LearnMB = std::min<uint64_t>(LearnEnv && LearnEnv[0] ? strtoull(LearnEnv, nullptr, 10) : 0, 4096);
  fextl::string LearnNote;
  if (!LearnMB) {
    LearnNote = "unpack cache: learned streams off (FEX_UNPACK_CACHE_LEARN=0)";
  } else if (!GameFiles::Running() || C->LinuxRead < 0) {
    LearnNote = fextl::fmt::format("unpack cache: learned streams off, they need {}",
                                   !GameFiles::Running() ? "FEX_GAME_FILES_UNCACHED=1" : "data.bin's Linux file (FEX_UNPACK_CACHE_DIRECT=1)");
  } else {
    Learned::Words = ((C->MaxDataBytes >> Learned::BlockShift) + 63) / 64;
    Learned::Bits = new std::atomic<uint64_t>[Learned::Words]();
    Learned::UsedBits = new std::atomic<uint64_t>[Learned::Words]();
    Learned::Path = Dir + "learned.txt";
    Learned::Fd = C->LinuxRead;
    const fextl::string From = Learned::Load(C->DataEnd, Reset || Started);
    Learned::Budget = LearnMB << 20;
    Learned::Coalesce();
    Learned::FitBudget();
    Learned::RebuildBits();
    Learned::UpdateTotals();
    Learned::Dirty = true; // the session number moved on
    LearnNote = fextl::fmt::format(
      "unpack cache: learned streams: the streams the main thread looks up in frames of play are remembered in {} and asked of storage "
      "into the file cache at the start and at every loading screen, then read from there without waiting for storage (budget {} MB; "
      "FEX_UNPACK_CACHE_LEARN=0 turns it off); session {}: {} ({} ranges {:.1f} MB)",
      Learned::Path, LearnMB, Learned::Session, From, Learned::Count, Learned::MB(Learned::TotalBytes()));
  }
  OpenTicks = ReadTicks();
  TheCache = C;
  FEXUnpackCacheEnabled = 1;
  {
    std::scoped_lock Guard {WriterLock};
    WriterCache = C;
  }
  WriterWake.notify_all();
  {
    std::scoped_lock Guard {ReaderLock};
    ReaderCache = C;
  }
  ReaderWake.notify_all();
  if (Learned::Budget) {
    GameFiles::SetPassHook(Learned::Pass);
  }
  FEX::Windows::StatsLogNote(LinuxNote);
  FEX::Windows::StatsLogNote(
    C->Flush ? fextl::fmt::format("unpack cache: new streams start on a page of their own and are written to storage in {} MB batches, then "
                                  "dropped from the file cache (also after {} ms without new streams; FEX_UNPACK_CACHE_FLUSH=0 drops each "
                                  "one right after writing it, as build 90)",
                                  FlushBatch >> 20, FlushIdleMs) :
               fextl::fmt::format("unpack cache: new streams are dropped from the file cache one by one right after writing ({})",
                                  C->LinuxRead >= 0 ? "FEX_UNPACK_CACHE_FLUSH=1 writes them in batches" :
                                                      "batches need data.bin's Linux file, FEX_UNPACK_CACHE_DIRECT=1"));
  FEX::Windows::StatsLogNote(LearnNote);
  FEX::Windows::StatsLogNote(C->MainPlay ? "unpack cache: the main thread never reads a hit from disk while frames are coming (the last began under "
                                           "250 ms ago): it is left to the game to unpack (FEX_UNPACK_CACHE_MAIN_PLAY=0 turns it off)" :
                                           "unpack cache: the main thread reads hits from disk like any thread (FEX_UNPACK_CACHE_MAIN_PLAY=1 needs "
                                           "FEX_GAME_FILES_UNCACHED=1)");
  if (MD5Cache) {
    FEX::Windows::StatsLogNote(OpenMD5Cache(Dir, Reset));
  } else {
    FEX::Windows::StatsLogNote("md5 cache: off");
  }
  FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: on{}, {} streams ({} MB) in {}, {} records skipped, limit {} GB{}, read-ahead {}",
                                                Reset ? " (reset)" : Started ? " (new)" : "", C->Entries.size(), (C->DataEnd - HeaderSize) >> 20, Dir,
                                                Skipped, MaxGB, C->Full ? ", full" : "",
                                                C->ReadAheadSlots ? fextl::fmt::format("{} MB", C->ReadAheadSlots * ChunkSize >> 20) : fextl::string("off")));
  FEX::Windows::StatsLogNote(
    C->PlayGate ? fextl::fmt::format("unpack cache: while playing (under {} lookups per {} ms) hits not in read-ahead memory are left to the "
                                     "game to unpack instead of read from disk, read-ahead {} MB past a hit instead of {} "
                                     "(FEX_UNPACK_CACHE_PLAY=0 turns it off)",
                                     PlayMaxLookups, PlayWindowMs, PlayAheadChunks * ChunkSize >> 20, AheadChunks * ChunkSize >> 20) :
                  fextl::string("unpack cache: play and loads alike, every hit read (FEX_UNPACK_CACHE_PLAY=1 leaves disk hits during play to the game)"));
  if (C->ReadAheadSlots) {
    FEX::Windows::StatsLogNote(
      C->AheadRamp ? fextl::fmt::format("unpack cache: read-ahead grows along the reads: 1 MB past a hit, 1 MB more for every 2 MB before it the game "
                                        "already used in order, up to {} MB ({} while playing); disk hits in sequence read their own MB and the next "
                                        "(FEX_UNPACK_CACHE_AHEAD=fixed reads the whole distance past every hit, as build 82)",
                                        AheadChunks * ChunkSize >> 20, PlayAheadChunks * ChunkSize >> 20) :
                     fextl::fmt::format("unpack cache: read-ahead the whole {} MB past every hit ({} while playing; FEX_UNPACK_CACHE_AHEAD=ramp "
                                        "grows it along the reads)",
                                        AheadChunks * ChunkSize >> 20, PlayAheadChunks * ChunkSize >> 20));
  }
  return true;
}

fextl::string StatsLine() {
  if (!FEXUnpackCacheEnabled) {
    return {};
  }
  // Every field is a uint64_t counter.
  constexpr size_t Fields = sizeof(FEXUnpackCacheCounters) / sizeof(uint64_t);
  FEXUnpackCacheCounters Now;
  for (size_t i = 0; i < Fields; ++i) {
    reinterpret_cast<uint64_t*>(&Now)[i] = __atomic_load_n(&reinterpret_cast<const uint64_t*>(&FEXUnpackCacheStats)[i], __ATOMIC_RELAXED);
  }
  FEXUnpackCacheCounters D;
  for (size_t i = 0; i < Fields; ++i) {
    reinterpret_cast<uint64_t*>(&D)[i] = reinterpret_cast<const uint64_t*>(&Now)[i] - reinterpret_cast<const uint64_t*>(&Prev)[i];
  }
  Prev = Now;
  const auto Ms = [](uint64_t Ticks) {
    return static_cast<double>(Ticks) * 1000.0 / static_cast<double>(TickFrequency);
  };
  const auto MB = [](uint64_t Bytes) {
    return static_cast<double>(Bytes) / (1024.0 * 1024.0);
  };
  if (!D.Lookups && !D.Stores && !D.Written && !D.Dropped && !D.Failures && !D.AheadChunks && !D.AdlerSkips && !D.MD5Lookups && !D.PlaySkips && !D.Flushes) {
    return Learned::Line();
  }
  uint64_t Size, Waiting;
  {
    std::scoped_lock Guard {TheCache->Lock};
    Size = TheCache->DataEnd - HeaderSize;
    Waiting = TheCache->RingHead - TheCache->RingTail;
  }
  // hit: total time per hit, then ram = hits copied from read-ahead memory, disk = time reading the others, verify =
  // hashing all of them. ahead = chunks the read-ahead thread read (direct = of those, read past the file cache; wasted =
  // dropped unused). uncached = reads and writes dropped from the file cache. adler = checks of a hit's output answered
  // without computing them. play = lookups while playing / of those left to the game (not in read-ahead memory) / their MB.
  // flushed (build 91, FEX_UNPACK_CACHE_FLUSH) = batches of written data sent to storage and dropped / MB / ms starting the
  // writes + waiting for them + dropping.
  fextl::string Out = fextl::fmt::format("  ucache lookups={} play={}/{}/{:.1f}MB key={:.1f}ms hit={}/{:.1f}MB/{:.1f}ms ram={}/{:.1f}ms disk={:.1f}ms verify={:.1f}ms "
                            "ahead={}/{:.1f}MB/{:.1f}ms direct={} wasted={} uncached={} adler={}/{:.1f}MB stored={}/{:.1f}MB/{:.1f}ms "
                            "written={}/{:.1f}ms {}"
                            "waiting={:.1f}MB dropped={} failed={} size={}MB writer={}{} md5cache={}/{}/{:.1f}MB/{:.1f}ms stored={}\n",
                            D.Lookups, D.PlayLookups, D.PlaySkips, MB(D.PlaySkipBytes), Ms(D.KeyTicks), D.Hits, MB(D.HitBytes), Ms(D.HitTicks), D.RamHits, Ms(D.RamTicks), Ms(D.DiskTicks),
                            Ms(D.VerifyTicks), D.AheadChunks, MB(D.AheadBytes), Ms(D.AheadTicks), D.AheadDirect, D.AheadWasted,
                            D.CacheDrops, D.AdlerSkips, MB(D.AdlerSkipBytes), D.Stores, MB(D.StoreBytes), Ms(D.StoreTicks), D.Written,
                            Ms(D.WriteTicks),
                            D.Flushes || D.FlushStartTicks || D.FlushFailures ?
                              fextl::fmt::format("flushed={}/{:.1f}MB/{:.1f}+{:.1f}+{:.1f}ms{} ", D.Flushes, MB(D.FlushBytes), Ms(D.FlushStartTicks),
                                                 Ms(D.FlushWaitTicks), Ms(D.FlushDropTicks),
                                                 D.FlushFailures ? fextl::fmt::format(" ({} failed)", D.FlushFailures) : fextl::string {}) :
                              fextl::string {},
                            MB(Waiting), D.Dropped, D.Failures, Size >> 20, WriterState.load(std::memory_order_relaxed),
                            TheCache->Full ? " full" : "", D.MD5Hits, D.MD5Lookups, MB(D.MD5HitBytes), Ms(D.MD5Ticks), D.MD5Stores);
  if (D.MainPlaySkips || D.LearnedHits || D.LearnedNotCached) {
    Out += fextl::fmt::format("  ucache main thread in play: left to the game {}/{:.1f}MB | learned streams: from the file cache {}/{:.1f}ms, "
                                        "not there {}/{:.1f}ms\n",
                                        D.MainPlaySkips, MB(D.MainPlaySkipBytes), D.LearnedHits, Ms(D.LearnedTicks), D.LearnedNotCached,
                                        Ms(D.LearnedNotCachedTicks));
  }
  return Out + Learned::Line();
}
} // namespace NativeUnpackCache
