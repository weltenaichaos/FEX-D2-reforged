// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

namespace FEX::Windows::ThreadTable {
// Build 161: our Proton d2r10's thread table (patch 0011). The wineserver keeps one 128-byte entry per thread of this
// process in a read-only shared mapping and rewrites it the moment a value changes (start, Linux tid, TEB, priority,
// base priority, priority class, affinity of the thread or the process, boost, name). PE ntdll's export
// __wine_d2r_threads says where the mapping is. FEX's helpers read it instead of asking the wineserver: in the b159 run
// they sent ~66 get_next_thread, ~160 get_thread_info and many of the ~205 close_handle requests a second, each a round
// trip through the wineserver that also serves the game. Without the export (stock Proton, d2r9 and older) or with
// WINE_D2R_THREAD_TABLE=0, Available() is false and the helpers ask Wine as before.

struct Entry {
  uint32_t Tid;      // ClientId.UniqueThread
  int32_t UnixTid;   // Linux tid, -1 until the thread runs
  int32_t Priority;  // ThreadBasicInformation.Priority
  int32_t BasePriority;
  uint32_t Flags;    // 1 first thread, 2 boost off, 4 hidden from debugger, 8 name cut or changed
  uint64_t Teb;
  uint64_t Affinity; // ThreadBasicInformation.AffinityMask (the server's value AND the system's mask)
  uint32_t Serial;   // never reused: a reused tid gets a new one
  uint32_t NameLen;
  int64_t CreationTime;
  char Name[72];     // ASCII, '?' for other characters, NUL terminated
};

// Once at start, before the helpers start: Export = GetProcAddress(ntdll, "__wine_d2r_threads") or nullptr.
void Init(const void* Export);
bool Available();

// Every live thread of the process (tid != 0), each entry read whole. Returns false without a table.
bool ReadAll(fextl::vector<Entry>& Out);
// One thread by Windows tid. False when there's no table or no such thread.
bool Find(uint32_t Tid, Entry& Out);
// The table's change counter (+1 after every entry write), 0 without a table.
uint32_t Changes();

// The name as FEX's helpers write it: printable ASCII, spaces and other characters as '_'.
fextl::string HelperName(const Entry& E);

// Its CPU time so far in 100 ns units from Linux (/proc/self/task/<unix tid>/schedstat), 0 when it can't be read.
uint64_t CpuTime100ns(const Entry& E);

// Start note for the stats log.
fextl::string Note();

// Counters for the stats log: entries read, retries because the server was writing one, gave up.
struct Counts {
  uint64_t Reads;
  uint64_t Retries;
  uint64_t GaveUp;
};
Counts GetCounts();
} // namespace FEX::Windows::ThreadTable
