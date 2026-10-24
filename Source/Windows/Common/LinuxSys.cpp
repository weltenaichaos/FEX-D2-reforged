// SPDX-License-Identifier: MIT
#include "Windows/Common/LinuxSys.h"

#include <algorithm>
#include <cstring>

namespace FEX::Windows::Linux {
namespace {
  // arm64 Linux system call numbers (asm-generic/unistd.h).
  constexpr int64_t NR_openat = 56;
  constexpr int64_t NR_close = 57;
  constexpr int64_t NR_statfs = 43;
  constexpr int64_t NR_getdents64 = 61;
  constexpr int64_t NR_pread64 = 67;
  constexpr int64_t NR_readlinkat = 78;
  constexpr int64_t NR_newfstatat = 79;
  constexpr int64_t NR_sched_setaffinity = 122;
  constexpr int64_t NR_sched_getaffinity = 123;
  constexpr int64_t NR_setpriority = 140;
  constexpr int64_t NR_getpriority = 141;
  constexpr int64_t NR_getpid = 172;
  constexpr int64_t NR_gettid = 178;
  constexpr int64_t NR_fadvise64 = 223;

  constexpr int64_t AT_FDCWD = -100;
  constexpr int64_t O_RDONLY = 0;
  constexpr int64_t O_DIRECTORY = 0x4000;
  constexpr int64_t O_DIRECT = 0x10000; // arm64's value
  constexpr int64_t O_CLOEXEC = 0x80000;
  constexpr int64_t PRIO_PROCESS = 0;
  constexpr int64_t POSIX_FADV_DONTNEED = 4;

  struct Dirent64 {
    uint64_t Ino;
    int64_t Off;
    uint16_t RecLen;
    uint8_t Type;
    char Name[1];
  };

  bool ParseU64(const char*& P, uint64_t& Out) {
    while (*P == ' ') {
      ++P;
    }
    if (*P < '0' || *P > '9') {
      return false;
    }
    uint64_t V {};
    while (*P >= '0' && *P <= '9') {
      V = V * 10 + static_cast<uint64_t>(*P++ - '0');
    }
    Out = V;
    return true;
  }

  bool ParseI64(const char*& P, int64_t& Out) {
    while (*P == ' ') {
      ++P;
    }
    const bool Negative = *P == '-';
    if (Negative) {
      ++P;
    }
    uint64_t V {};
    if (!ParseU64(P, V)) {
      return false;
    }
    Out = Negative ? -static_cast<int64_t>(V) : static_cast<int64_t>(V);
    return true;
  }

  // Appends Text, then a decimal number when Number >= 0, to Out[0..Size), keeping it NUL-terminated.
  void Append(char* Out, size_t Size, size_t& Len, const char* Text, int64_t Number = -1) {
    for (; *Text && Len + 1 < Size; ++Text) {
      Out[Len++] = *Text;
    }
    if (Number >= 0) {
      char Digits[24];
      int N {};
      do {
        Digits[N++] = static_cast<char>('0' + Number % 10);
        Number /= 10;
      } while (Number);
      while (N && Len + 1 < Size) {
        Out[Len++] = Digits[--N];
      }
    }
    Out[Len] = 0;
  }

  void SkipField(const char*& P) {
    while (*P == ' ') {
      ++P;
    }
    while (*P && *P != ' ') {
      ++P;
    }
  }
} // namespace

int64_t Syscall(int64_t Nr, int64_t A0, int64_t A1, int64_t A2, int64_t A3, int64_t A4, int64_t A5) {
  register int64_t X8 asm("x8") = Nr;
  register int64_t X0 asm("x0") = A0;
  register int64_t X1 asm("x1") = A1;
  register int64_t X2 asm("x2") = A2;
  register int64_t X3 asm("x3") = A3;
  register int64_t X4 asm("x4") = A4;
  register int64_t X5 asm("x5") = A5;
  asm volatile("svc #0" : "+r"(X0) : "r"(X8), "r"(X1), "r"(X2), "r"(X3), "r"(X4), "r"(X5) : "memory", "cc");
  return X0;
}

int Open(const char* Path, bool Directory) {
  return static_cast<int>(
    Syscall(NR_openat, AT_FDCWD, reinterpret_cast<intptr_t>(Path), O_RDONLY | O_CLOEXEC | (Directory ? O_DIRECTORY : 0), 0));
}

void Close(int Fd) {
  if (Fd >= 0) {
    Syscall(NR_close, Fd);
  }
}

int64_t Pread(int Fd, void* Buffer, size_t Size, uint64_t Offset) {
  return Syscall(NR_pread64, Fd, reinterpret_cast<intptr_t>(Buffer), static_cast<int64_t>(Size), static_cast<int64_t>(Offset));
}

int OpenRead(const char* Path, bool Direct) {
  return static_cast<int>(Syscall(NR_openat, AT_FDCWD, reinterpret_cast<intptr_t>(Path), O_RDONLY | O_CLOEXEC | (Direct ? O_DIRECT : 0), 0));
}

int DropFromFileCache(int Fd, uint64_t Offset, uint64_t Size) {
  return static_cast<int>(Syscall(NR_fadvise64, Fd, static_cast<int64_t>(Offset), static_cast<int64_t>(Size), POSIX_FADV_DONTNEED));
}

bool Stat(const char* Path, FileInfo& Out) {
  // struct stat of arm64 Linux (asm-generic): st_mode at byte 16, st_size at byte 48, 128 bytes in all.
  alignas(8) uint8_t Buffer[128];
  if (Syscall(NR_newfstatat, AT_FDCWD, reinterpret_cast<intptr_t>(Path), reinterpret_cast<intptr_t>(Buffer), 0) != 0) {
    return false;
  }
  uint32_t Mode {};
  int64_t Size {};
  memcpy(&Mode, Buffer + 16, sizeof(Mode));
  memcpy(&Size, Buffer + 48, sizeof(Size));
  Out.Size = Size > 0 ? static_cast<uint64_t>(Size) : 0;
  Out.Regular = (Mode & 0170000) == 0100000;
  Out.Directory = (Mode & 0170000) == 0040000;
  return true;
}

uint64_t FileSystemType(const char* Path) {
  // struct statfs of arm64 Linux: f_type first, 120 bytes in all.
  alignas(8) uint8_t Buffer[120];
  if (Syscall(NR_statfs, reinterpret_cast<intptr_t>(Path), reinterpret_cast<intptr_t>(Buffer)) != 0) {
    return 0;
  }
  uint64_t Type {};
  memcpy(&Type, Buffer, sizeof(Type));
  return Type;
}

int64_t ReadLink(const char* Path, char* Buffer, size_t Size) {
  if (Size < 2) {
    return -22;
  }
  const int64_t Len =
    Syscall(NR_readlinkat, AT_FDCWD, reinterpret_cast<intptr_t>(Path), reinterpret_cast<intptr_t>(Buffer), static_cast<int64_t>(Size - 1));
  Buffer[Len > 0 ? Len : 0] = 0;
  return Len;
}

int64_t ReadSmallFile(const char* Path, char* Buffer, size_t Size) {
  if (!Size) {
    return -22;
  }
  const int Fd = Open(Path);
  if (Fd < 0) {
    Buffer[0] = 0;
    return Fd;
  }
  const int64_t Len = Pread(Fd, Buffer, Size - 1, 0);
  Close(Fd);
  Buffer[Len > 0 ? Len : 0] = 0;
  return Len;
}

int GetPid() {
  return static_cast<int>(Syscall(NR_getpid));
}

int GetTid() {
  return static_cast<int>(Syscall(NR_gettid));
}

int GetNice(int Tid) {
  // The raw call returns 20 - nice (1..40) so that it is never negative.
  const int64_t Result = Syscall(NR_getpriority, PRIO_PROCESS, Tid);
  return Result < 0 ? -1000 : static_cast<int>(20 - Result);
}

int SetNice(int Tid, int Nice) {
  return static_cast<int>(Syscall(NR_setpriority, PRIO_PROCESS, Tid, Nice));
}

uint64_t GetAffinity(int Tid) {
  uint64_t Mask[16] {};
  const int64_t Result = Syscall(NR_sched_getaffinity, Tid, sizeof(Mask), reinterpret_cast<intptr_t>(Mask));
  return Result < 0 ? 0 : Mask[0];
}

uint64_t ReadAllowedMask(const char* StatusPath) {
  char Buffer[4096];
  if (ReadSmallFile(StatusPath, Buffer, sizeof(Buffer)) <= 0) {
    return 0;
  }
  const char* Line = strstr(Buffer, "\nCpus_allowed:");
  if (!Line) {
    return 0;
  }
  Line += 14;
  // Hex digits, in 32-bit groups separated by commas, most significant first.
  uint64_t Mask {};
  for (; *Line && *Line != '\n'; ++Line) {
    const char c = *Line;
    if (c >= '0' && c <= '9') {
      Mask = Mask << 4 | static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      Mask = Mask << 4 | static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      Mask = Mask << 4 | static_cast<uint64_t>(c - 'A' + 10);
    }
  }
  return Mask;
}

int SetAffinity(int Tid, uint64_t Mask) {
  uint64_t Masks[1] {Mask};
  return static_cast<int>(Syscall(NR_sched_setaffinity, Tid, sizeof(Masks), reinterpret_cast<intptr_t>(Masks)));
}

bool ForEachEntry(const char* Path, void (*Fn)(const char* Name, void* Arg), void* Arg) {
  const int Fd = Open(Path, true);
  if (Fd < 0) {
    return false;
  }
  alignas(8) char Buffer[4096];
  for (;;) {
    const int64_t Len = Syscall(NR_getdents64, Fd, reinterpret_cast<intptr_t>(Buffer), sizeof(Buffer));
    if (Len <= 0) {
      break;
    }
    for (int64_t Pos = 0; Pos < Len;) {
      const auto* Entry = reinterpret_cast<const Dirent64*>(Buffer + Pos);
      if (Entry->RecLen == 0) {
        break;
      }
      const char* Name = Entry->Name;
      if (!(Name[0] == '.' && (Name[1] == 0 || (Name[1] == '.' && Name[2] == 0)))) {
        Fn(Name, Arg);
      }
      Pos += Entry->RecLen;
    }
  }
  Close(Fd);
  return true;
}

bool ParseTaskStat(const char* Text, TaskStat& Out) {
  // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so it ends at the last ')'.
  const char* Open = strchr(Text, '(');
  const char* CloseParen = strrchr(Text, ')');
  if (!Open || !CloseParen || CloseParen < Open) {
    return false;
  }
  const size_t CommLen = std::min<size_t>(CloseParen - Open - 1, sizeof(Out.Comm) - 1);
  memcpy(Out.Comm, Open + 1, CommLen);
  Out.Comm[CommLen] = 0;
  const char* P = CloseParen + 1;
  while (*P == ' ') {
    ++P;
  }
  Out.State = *P;
  // Field 3 is the state; utime and stime are fields 14 and 15, nice 19, processor 39.
  int Field = 3;
  SkipField(P);
  ++Field;
  uint64_t Value {};
  int64_t Signed {};
  for (; Field < 14; ++Field) {
    SkipField(P);
  }
  if (!ParseU64(P, Out.UserTicks) || !ParseU64(P, Out.SystemTicks)) {
    return false;
  }
  Field = 16;
  for (; Field < 19; ++Field) {
    SkipField(P);
  }
  if (!ParseI64(P, Signed)) {
    return false;
  }
  Out.Nice = static_cast<int>(Signed);
  Field = 20;
  for (; Field < 39; ++Field) {
    SkipField(P);
  }
  if (!ParseU64(P, Value)) {
    return false;
  }
  Out.Processor = static_cast<int>(Value);
  return true;
}

bool ReadTaskStat(const char* Path, TaskStat& Out) {
  char Buffer[1024];
  if (ReadSmallFile(Path, Buffer, sizeof(Buffer)) <= 0) {
    return false;
  }
  return ParseTaskStat(Buffer, Out);
}

bool ReadSchedStat(const char* Path, SchedStat& Out) {
  char Buffer[128];
  if (ReadSmallFile(Path, Buffer, sizeof(Buffer)) <= 0) {
    return false;
  }
  const char* P = Buffer;
  return ParseU64(P, Out.RunNs) && ParseU64(P, Out.WaitNs) && ParseU64(P, Out.Slices);
}

namespace {
  struct FindArgs {
    const char* Name;
    int Self;
    int Found;
  };

  // True when Path's last component is Name.
  bool BaseNameIs(const char* Path, const char* Name) {
    const char* Slash = strrchr(Path, '/');
    return strcmp(Slash ? Slash + 1 : Path, Name) == 0;
  }

  void FindOne(const char* Entry, void* Arg) {
    auto* A = static_cast<FindArgs*>(Arg);
    if (A->Found || Entry[0] < '1' || Entry[0] > '9') {
      return;
    }
    int Pid {};
    for (const char* P = Entry; *P; ++P) {
      if (*P < '0' || *P > '9') {
        return;
      }
      Pid = Pid * 10 + (*P - '0');
    }
    if (Pid == A->Self) {
      return;
    }
    char Path[64];
    size_t PathLen {};
    Append(Path, sizeof(Path), PathLen, "/proc/", Pid);
    const size_t DirLen = PathLen;
    Append(Path, sizeof(Path), PathLen, "/comm");
    char Text[512];
    int64_t Len = ReadSmallFile(Path, Text, sizeof(Text));
    if (Len > 0) {
      if (Text[Len - 1] == '\n') {
        Text[Len - 1] = 0;
      }
      if (strcmp(Text, A->Name) == 0) {
        A->Found = Pid;
        return;
      }
    }
    // Started through the dynamic loader or a wrapper, the comm is the loader's: look at the first arguments too.
    PathLen = DirLen;
    Path[PathLen] = 0;
    Append(Path, sizeof(Path), PathLen, "/cmdline");
    Len = ReadSmallFile(Path, Text, sizeof(Text));
    const char* Arg0 = Text;
    for (int i = 0; i < 3 && Len > 0 && Arg0 < Text + Len; ++i) {
      if (BaseNameIs(Arg0, A->Name)) {
        A->Found = Pid;
        return;
      }
      Arg0 += strlen(Arg0) + 1;
    }
  }
} // namespace

int FindProcess(const char* Name) {
  FindArgs Args {.Name = Name, .Self = GetPid(), .Found = 0};
  ForEachEntry("/proc", FindOne, &Args);
  return Args.Found;
}

void ReadMaxClocks(uint32_t (&KHz)[MaxCPUs]) {
  for (int CPU = 0; CPU < MaxCPUs; ++CPU) {
    char Path[96];
    char Buffer[32];
    size_t PathLen {};
    Append(Path, sizeof(Path), PathLen, "/sys/devices/system/cpu/cpu", CPU);
    Append(Path, sizeof(Path), PathLen, "/cpufreq/cpuinfo_max_freq");
    KHz[CPU] = 0;
    if (ReadSmallFile(Path, Buffer, sizeof(Buffer)) > 0) {
      const char* P = Buffer;
      uint64_t Value {};
      if (ParseU64(P, Value)) {
        KHz[CPU] = static_cast<uint32_t>(Value);
      }
    }
  }
}
} // namespace FEX::Windows::Linux
