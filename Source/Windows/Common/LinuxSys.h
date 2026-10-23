// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace FEX::Windows::Linux {
// Direct Linux system calls, for looking at and adjusting how the kernel schedules this process, its threads and the
// wineserver. Wine has no API for any of it, and going through Wine's file functions would cost a wineserver round trip
// per file. Paths are Linux paths ("/proc/self/..."). Every call returns a negative errno on failure.
int64_t Syscall(int64_t Nr, int64_t A0 = 0, int64_t A1 = 0, int64_t A2 = 0, int64_t A3 = 0, int64_t A4 = 0, int64_t A5 = 0);

int Open(const char* Path, bool Directory = false);
void Close(int Fd);
int64_t Pread(int Fd, void* Buffer, size_t Size, uint64_t Offset);
// Reads a small file into Buffer, NUL-terminated. Returns its length, or a negative errno.
int64_t ReadSmallFile(const char* Path, char* Buffer, size_t Size);
int GetPid();

// Nice value of a Linux thread (tid; a pid names its first thread), or a value below -100 on failure.
int GetNice(int Tid);
// 0 or a negative errno.
int SetNice(int Tid, int Nice);
// CPUs a Linux thread may run on (the first 64), or 0 on failure.
uint64_t GetAffinity(int Tid);

// Calls Fn(const char* Name) for each entry of a directory except "." and "..". False when the directory can't be read.
bool ForEachEntry(const char* Path, void (*Fn)(const char* Name, void* Arg), void* Arg);

// The fields of /proc/<pid>/task/<tid>/stat the stats log uses.
struct TaskStat {
  char Comm[17];
  char State;
  uint64_t UserTicks;   // clock ticks (USER_HZ, 100 per second)
  uint64_t SystemTicks;
  int Nice;
  int Processor;        // the CPU it last ran on
};
bool ParseTaskStat(const char* Text, TaskStat& Out);
bool ReadTaskStat(const char* Path, TaskStat& Out);

// /proc/.../schedstat: time on a CPU, time runnable but waiting for one, and how many times it got one.
struct SchedStat {
  uint64_t RunNs;
  uint64_t WaitNs;
  uint64_t Slices;
};
bool ReadSchedStat(const char* Path, SchedStat& Out);

// The pid of a process of this user named Name (its comm, or the file name of one of its first three arguments), other
// than this one; 0 when there is none.
int FindProcess(const char* Name);

// Maximum clock of each CPU in kHz (cpufreq cpuinfo_max_freq), 0 for CPUs that don't exist or can't be read.
constexpr int MaxCPUs = 16;
void ReadMaxClocks(uint32_t (&KHz)[MaxCPUs]);
} // namespace FEX::Windows::Linux
