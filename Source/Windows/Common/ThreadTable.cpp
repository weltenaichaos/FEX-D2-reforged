// SPDX-License-Identifier: MIT
#include "Windows/Common/ThreadTable.h"
#include "Windows/Common/LinuxSys.h"

#include <FEXCore/fextl/fmt.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace FEX::Windows::ThreadTable {
namespace {
  // PE ntdll's export __wine_d2r_threads (proton-patches/11.0-1/0011).
  struct ExportInfo {
    uint32_t Version; // 1 once the table is mapped (written last)
    uint32_t Size;    // bytes mapped
    uint64_t Table;
    uint64_t SystemAffinity;
    uint32_t Status;  // when Version is 0: why there is no table
  };
  static_assert(offsetof(ExportInfo, Table) == 8 && offsetof(ExportInfo, SystemAffinity) == 16 && offsetof(ExportInfo, Status) == 24);

  // The table's header (128 bytes) and entries (128 bytes each, at 128 + 128 * i).
  struct HeaderInfo {
    uint32_t Version;
    uint32_t HeaderSize;
    uint32_t EntrySize;
    uint32_t MaxEntries;
    uint32_t Used;    // entries [0, Used) can be in use
    uint32_t Count;   // threads listed now
    uint32_t Changes; // +1 after every entry write
    uint32_t NotListed;
    uint32_t Pid;
    uint32_t Serial;
  };
  struct RawEntry {
    uint32_t Seq; // odd while the server writes it
    uint32_t Tid;
    int32_t UnixTid;
    int32_t Priority;
    int32_t BasePriority;
    uint32_t Flags;
    uint64_t Teb;
    uint64_t Affinity;
    uint32_t Serial;
    uint32_t NameLen;
    int64_t CreationTime;
    char Name[72];
  };
  static_assert(sizeof(RawEntry) == 128);
  static_assert(offsetof(RawEntry, Teb) == 24 && offsetof(RawEntry, Affinity) == 32 && offsetof(RawEntry, Serial) == 40 &&
                offsetof(RawEntry, CreationTime) == 48 && offsetof(RawEntry, Name) == 56);
  constexpr size_t HeaderBytes = 128;
  constexpr size_t EntryBytes = 128;
  // A write takes the server well under a microsecond; one it was pushed off its CPU in the middle of is skipped.
  constexpr int MaxTries = 64;

  const ExportInfo* Export {};
  std::atomic<const uint8_t*> Table {};
  uint64_t SystemAffinity {};
  uint32_t MaxEntries {};
  std::atomic<uint64_t> Reads {}, Retries {}, GaveUp {};

  const HeaderInfo* Header() {
    return reinterpret_cast<const HeaderInfo*>(Table.load(std::memory_order_acquire));
  }

  const RawEntry* EntryAt(const uint8_t* Base, uint32_t i) {
    return reinterpret_cast<const RawEntry*>(Base + HeaderBytes + EntryBytes * i);
  }

  // One whole write of the entry into Out: copied word by word between two reads of the same even sequence number.
  bool ReadEntry(const RawEntry* E, Entry& Out) {
    static_assert(sizeof(RawEntry) % sizeof(uint64_t) == 0);
    uint64_t Words[sizeof(RawEntry) / sizeof(uint64_t)];
    const auto* Src = reinterpret_cast<const uint64_t*>(E);
    for (int Try = 0; Try < MaxTries; ++Try) {
      const uint32_t S0 = __atomic_load_n(&E->Seq, __ATOMIC_ACQUIRE);
      if (S0 & 1) {
        Retries.fetch_add(1, std::memory_order_relaxed);
        __asm volatile("yield");
        continue;
      }
      for (size_t w = 0; w < std::size(Words); ++w) {
        Words[w] = __atomic_load_n(&Src[w], __ATOMIC_RELAXED);
      }
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&E->Seq, __ATOMIC_RELAXED) != S0) {
        Retries.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      RawEntry Raw;
      memcpy(&Raw, Words, sizeof(Raw));
      Out.Tid = Raw.Tid;
      Out.UnixTid = Raw.UnixTid;
      Out.Priority = Raw.Priority;
      Out.BasePriority = Raw.BasePriority;
      Out.Flags = Raw.Flags;
      Out.Teb = Raw.Teb;
      Out.Affinity = Raw.Affinity & SystemAffinity;
      Out.Serial = Raw.Serial;
      Out.NameLen = Raw.NameLen;
      Out.CreationTime = Raw.CreationTime;
      memcpy(Out.Name, Raw.Name, sizeof(Out.Name));
      Out.Name[sizeof(Out.Name) - 1] = 0;
      Reads.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    GaveUp.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint32_t UsedEntries(const HeaderInfo* H) {
    const uint32_t Used = __atomic_load_n(&H->Used, __ATOMIC_ACQUIRE);
    return Used < MaxEntries ? Used : MaxEntries;
  }
} // namespace

void Init(const void* ExportAddress) {
  Export = static_cast<const ExportInfo*>(ExportAddress);
  if (!Export || __atomic_load_n(&Export->Version, __ATOMIC_ACQUIRE) != 1) {
    return;
  }
  const auto* Base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(Export->Table));
  const uint32_t Size = Export->Size;
  if (!Base || Size < HeaderBytes) {
    return;
  }
  const auto* H = reinterpret_cast<const HeaderInfo*>(Base);
  if (H->Version != 1 || H->HeaderSize != HeaderBytes || H->EntrySize != EntryBytes || !H->MaxEntries ||
      HeaderBytes + EntryBytes * static_cast<uint64_t>(H->MaxEntries) > Size) {
    return;
  }
  SystemAffinity = Export->SystemAffinity ? Export->SystemAffinity : ~0ULL;
  MaxEntries = H->MaxEntries;
  Table.store(Base, std::memory_order_release);
}

bool Available() {
  return Header() != nullptr;
}

bool ReadAll(fextl::vector<Entry>& Out) {
  Out.clear();
  const auto* H = Header();
  if (!H) {
    return false;
  }
  const auto* Base = reinterpret_cast<const uint8_t*>(H);
  const uint32_t Used = UsedEntries(H);
  for (uint32_t i = 0; i < Used; ++i) {
    const auto* E = EntryAt(Base, i);
    if (!__atomic_load_n(&E->Tid, __ATOMIC_RELAXED)) {
      continue;
    }
    Entry Copy;
    if (ReadEntry(E, Copy) && Copy.Tid) {
      Out.push_back(Copy);
    }
  }
  return true;
}

bool Find(uint32_t Tid, Entry& Out) {
  const auto* H = Header();
  if (!H || !Tid) {
    return false;
  }
  const auto* Base = reinterpret_cast<const uint8_t*>(H);
  const uint32_t Used = UsedEntries(H);
  for (uint32_t i = 0; i < Used; ++i) {
    const auto* E = EntryAt(Base, i);
    if (__atomic_load_n(&E->Tid, __ATOMIC_RELAXED) != Tid) {
      continue;
    }
    if (ReadEntry(E, Out) && Out.Tid == Tid) {
      return true;
    }
  }
  return false;
}

uint32_t Changes() {
  const auto* H = Header();
  return H ? __atomic_load_n(&H->Changes, __ATOMIC_ACQUIRE) : 0;
}

fextl::string HelperName(const Entry& E) {
  fextl::string Result;
  for (size_t i = 0; i < sizeof(E.Name) && E.Name[i]; ++i) {
    const char c = E.Name[i];
    Result.push_back(c > 0x20 && c < 0x7f ? c : '_');
  }
  return Result;
}

uint64_t CpuTime100ns(const Entry& E) {
  if (E.UnixTid <= 0) {
    return 0;
  }
  char Path[48];
  snprintf(Path, sizeof(Path), "/proc/self/task/%d/schedstat", E.UnixTid);
  char Text[96];
  const int64_t Len = Linux::ReadSmallFile(Path, Text, sizeof(Text) - 1);
  if (Len <= 0) {
    return 0;
  }
  Text[Len] = 0;
  uint64_t Ns {};
  for (const char* P = Text; *P >= '0' && *P <= '9'; ++P) {
    Ns = Ns * 10 + static_cast<uint64_t>(*P - '0');
  }
  return Ns / 100;
}

fextl::string Note() {
  if (const auto* H = Header()) {
    return fextl::fmt::format("thread table: build 161: our Proton's thread table found (d2r10 and later, __wine_d2r_threads version 1, "
                              "{} threads listed, {} not listed, up to {}): the priority guard and the stats log read thread ids, "
                              "names, priorities, affinities and TEBs from it and CPU times from Linux, instead of asking the "
                              "wineserver (b159: ~66 get_next_thread, ~160 get_thread_info and many close_handle requests a second "
                              "from FEX's helpers). WINE_D2R_THREAD_TABLE=0 turns it off",
                              __atomic_load_n(&H->Count, __ATOMIC_RELAXED), __atomic_load_n(&H->NotListed, __ATOMIC_RELAXED),
                              MaxEntries);
  }
  if (!Export) {
    return "thread table: none (stock Proton or our Proton older than d2r10): FEX's helpers ask the wineserver about threads";
  }
  const uint32_t Version = __atomic_load_n(&Export->Version, __ATOMIC_ACQUIRE);
  return fextl::fmt::format("thread table: not used (__wine_d2r_threads version {}, status {:#x}{}): FEX's helpers ask the wineserver "
                            "about threads",
                            Version, Export->Status, Version == 1 ? ", the table's header didn't match" : ", WINE_D2R_THREAD_TABLE=0 or no mapping");
}

Counts GetCounts() {
  return {Reads.load(std::memory_order_relaxed), Retries.load(std::memory_order_relaxed), GaveUp.load(std::memory_order_relaxed)};
}
} // namespace FEX::Windows::ThreadTable
