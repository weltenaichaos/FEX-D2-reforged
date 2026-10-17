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
// streams aren't stored this time. The ring is allocated once, so copying into it doesn't fault in fresh pages.

#include "NativeUnpackCache.h"
#include "Windows/Common/StatsLog.h"

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

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
int FEXUnpackCacheEnabled;
struct FEXUnpackCacheCounters FEXUnpackCacheStats;
}

namespace {
constexpr char DataMagic[8] = {'F', 'E', 'X', 'U', 'N', 'P', 'K', 'D'};
constexpr char IndexMagic[8] = {'F', 'E', 'X', 'U', 'N', 'P', 'K', 'I'};
constexpr uint32_t FormatVersion = 1;
constexpr uint64_t HeaderSize = 64;
constexpr uint64_t DataAlignment = 64;
constexpr uint64_t RingSize = 256ull << 20; // output bytes waiting for the writer

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
  fextl::unique_ptr<FEXCore::Threads::Thread> Writer;
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

// Log thread state.
FEXUnpackCacheCounters Prev {};
} // namespace

extern "C" int FEXUnpackCacheGet(const uint64_t Key[2], uint8_t* Out, uint32_t OutCap, uint32_t* Produced, uint32_t* Trailer) {
  auto* C = TheCache;
  if (!C) {
    return 0;
  }
  const uint64_t Start = ReadTicks();
  __atomic_fetch_add(&FEXUnpackCacheStats.Lookups, 1, __ATOMIC_RELAXED);

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
  if (!ReadExact(C->Data, Out, E.Produced, E.Offset) || XXH3_64bits(Out, E.Produced) != E.OutputHash) {
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

extern "C" void FEXUnpackCachePut(const uint64_t Key[2], const uint8_t* Out, uint32_t Produced, uint32_t Trailer) {
  auto* C = TheCache;
  if (!C || C->Full) {
    return;
  }
  const uint64_t Start = ReadTicks();

  if (!C->RingReady.load(std::memory_order_acquire)) {
    __atomic_fetch_add(&FEXUnpackCacheStats.Dropped, 1, __ATOMIC_RELAXED);
    return;
  }

  Job* J;
  {
    std::scoped_lock Guard {C->Lock};
    if (C->Entries.contains({Key[0], Key[1]})) {
      return;
    }
    // The data has to be in one piece: skip what's left at the end of the ring if it doesn't fit there.
    uint64_t RingStart = C->RingHead;
    if (RingStart % RingSize + Produced > RingSize) {
      RingStart += RingSize - RingStart % RingSize;
    }
    if (RingStart + Produced - C->RingTail > RingSize) {
      __atomic_fetch_add(&FEXUnpackCacheStats.Dropped, 1, __ATOMIC_RELAXED);
      return;
    }
    if (C->DataEnd + Produced > C->MaxDataBytes) {
      C->Full = true;
      FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: full at {} MB, not storing more", C->DataEnd >> 20));
      return;
    }
    Job New {};
    New.R.Key[0] = Key[0];
    New.R.Key[1] = Key[1];
    New.R.Produced = Produced;
    New.R.Trailer = Trailer;
    New.R.Offset = C->DataEnd;
    New.RecordOffset = C->IndexEnd;
    New.RingStart = RingStart;
    New.RingEnd = RingStart + Produced;
    C->DataEnd = AlignUp(C->DataEnd + Produced, DataAlignment);
    C->IndexEnd += sizeof(Record);
    C->RingHead = New.RingEnd;
    C->Entries.emplace(std::pair {Key[0], Key[1]}, Entry {New.R.Offset, 0, Produced, Trailer, false});
    // Stays valid while other jobs are added and taken: a deque keeps its elements in place, and the writer doesn't take
    // this one before it's ready.
    J = &C->Queue.emplace_back(New);
  }

  memcpy(C->Ring + J->RingStart % RingSize, Out, Produced);
  {
    std::scoped_lock Guard {C->Lock};
    J->Ready = true;
  }
  C->QueueWake.notify_one();
  __atomic_fetch_add(&FEXUnpackCacheStats.Stores, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.StoreBytes, Produced, __ATOMIC_RELAXED);
  __atomic_fetch_add(&FEXUnpackCacheStats.StoreTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
}

namespace {
void* WriterThread(void* Arg) {
  auto* C = static_cast<Cache*>(Arg);
  // Fault the ring's pages in here, so the game's threads don't pay for it when they copy into it.
  memset(C->Ring, 0, RingSize);
  C->RingReady.store(true, std::memory_order_release);
  for (;;) {
    Job J;
    {
      std::unique_lock Guard {C->Lock};
      C->QueueWake.wait(Guard, [C] { return !C->Queue.empty() && C->Queue.front().Ready; });
      J = C->Queue.front();
      C->Queue.pop_front();
    }
    const uint64_t Start = ReadTicks();
    const uint8_t* Buffer = C->Ring + J.RingStart % RingSize;
    J.R.OutputHash = XXH3_64bits(Buffer, J.R.Produced);
    J.R.Check = RecordCheck(J.R);
    const bool Written = WriteExact(C->Data, Buffer, J.R.Produced, J.R.Offset) && WriteExact(C->Index, &J.R, sizeof(J.R), J.RecordOffset);
    {
      std::scoped_lock Guard {C->Lock};
      C->RingTail = J.RingEnd;
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
  return nullptr;
}
} // namespace

namespace NativeUnpackCache {
bool Init() {
  const char* ResetEnv = getenv("FEX_UNPACK_CACHE_RESET");
  const bool Reset = ResetEnv && ResetEnv[0] == '1';
  const char* MaxEnv = getenv("FEX_UNPACK_CACHE_MAX_GB");
  uint64_t MaxGB = MaxEnv ? strtoull(MaxEnv, nullptr, 10) : 0;
  if (!MaxGB) {
    MaxGB = 16;
  }

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
  C->IndexEnd = HeaderSize + RecordCount * sizeof(Record);
  C->MaxDataBytes = MaxGB << 30;
  C->Full = C->DataEnd >= C->MaxDataBytes;

  C->Ring = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(RingSize));
  if (!C->Ring) {
    FEX::Windows::StatsLogNote("unpack cache: off, can't allocate its write buffer");
    delete C;
    return false;
  }
  C->Writer = FEXCore::Threads::Thread::Create(WriterThread, C, {.LowPriority = false, .Internal = true}, "FEXUnpackCache");
  if (!C->Writer || !C->Writer->joinable()) {
    FEX::Windows::StatsLogNote("unpack cache: off, can't start its writer thread");
    delete C;
    return false;
  }

  TheCache = C;
  FEXUnpackCacheEnabled = 1;
  FEX::Windows::StatsLogNote(fextl::fmt::format("unpack cache: on{}, {} streams ({} MB) in {}, {} records skipped, limit {} GB{}",
                                                Reset ? " (reset)" : Started ? " (new)" : "", C->Entries.size(), (C->DataEnd - HeaderSize) >> 20, Dir,
                                                Skipped, MaxGB, C->Full ? ", full" : ""));
  return true;
}

fextl::string StatsLine() {
  if (!FEXUnpackCacheEnabled) {
    return {};
  }
  FEXUnpackCacheCounters Now;
  Now.Lookups = __atomic_load_n(&FEXUnpackCacheStats.Lookups, __ATOMIC_RELAXED);
  Now.KeyTicks = __atomic_load_n(&FEXUnpackCacheStats.KeyTicks, __ATOMIC_RELAXED);
  Now.Hits = __atomic_load_n(&FEXUnpackCacheStats.Hits, __ATOMIC_RELAXED);
  Now.HitBytes = __atomic_load_n(&FEXUnpackCacheStats.HitBytes, __ATOMIC_RELAXED);
  Now.HitTicks = __atomic_load_n(&FEXUnpackCacheStats.HitTicks, __ATOMIC_RELAXED);
  Now.Stores = __atomic_load_n(&FEXUnpackCacheStats.Stores, __ATOMIC_RELAXED);
  Now.StoreBytes = __atomic_load_n(&FEXUnpackCacheStats.StoreBytes, __ATOMIC_RELAXED);
  Now.StoreTicks = __atomic_load_n(&FEXUnpackCacheStats.StoreTicks, __ATOMIC_RELAXED);
  Now.Dropped = __atomic_load_n(&FEXUnpackCacheStats.Dropped, __ATOMIC_RELAXED);
  Now.Written = __atomic_load_n(&FEXUnpackCacheStats.Written, __ATOMIC_RELAXED);
  Now.WriteTicks = __atomic_load_n(&FEXUnpackCacheStats.WriteTicks, __ATOMIC_RELAXED);
  Now.Failures = __atomic_load_n(&FEXUnpackCacheStats.Failures, __ATOMIC_RELAXED);
  const auto Ms = [](uint64_t Ticks) {
    return static_cast<double>(Ticks) * 1000.0 / static_cast<double>(TickFrequency);
  };
  const auto MB = [](uint64_t Bytes) {
    return static_cast<double>(Bytes) / (1024.0 * 1024.0);
  };
  fextl::string Line;
  if (Now.Lookups != Prev.Lookups || Now.Stores != Prev.Stores || Now.Written != Prev.Written || Now.Dropped != Prev.Dropped ||
      Now.Failures != Prev.Failures) {
    uint64_t Size, Waiting;
    {
      std::scoped_lock Guard {TheCache->Lock};
      Size = TheCache->DataEnd - HeaderSize;
      Waiting = TheCache->RingHead - TheCache->RingTail;
    }
    Line = fextl::fmt::format("  ucache lookups={} key={:.1f}ms hit={}/{:.1f}MB/{:.1f}ms queued={}/{:.1f}MB/{:.1f}ms written={}/{:.1f}ms "
                              "waiting={:.1f}MB dropped={} failed={} size={}MB{}\n",
                              Now.Lookups - Prev.Lookups, Ms(Now.KeyTicks - Prev.KeyTicks), Now.Hits - Prev.Hits,
                              MB(Now.HitBytes - Prev.HitBytes), Ms(Now.HitTicks - Prev.HitTicks), Now.Stores - Prev.Stores,
                              MB(Now.StoreBytes - Prev.StoreBytes), Ms(Now.StoreTicks - Prev.StoreTicks), Now.Written - Prev.Written,
                              Ms(Now.WriteTicks - Prev.WriteTicks), MB(Waiting), Now.Dropped - Prev.Dropped, Now.Failures - Prev.Failures,
                              Size >> 20, TheCache->Full ? " full" : "");
  }
  Prev = Now;
  return Line;
}
} // namespace NativeUnpackCache
