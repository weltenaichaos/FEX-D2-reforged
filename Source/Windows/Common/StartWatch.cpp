// SPDX-License-Identifier: MIT
// Build 128: start watch (FEX_START_WATCH), see StartWatch.h.
#include "Windows/Common/StartWatch.h"
#include "Windows/Common/LinuxSys.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

extern "C" NTSTATUS WINAPI RtlSystemTimeToLocalTime(const LARGE_INTEGER* SystemTime, LARGE_INTEGER* LocalTime);

namespace FEX::Windows {
namespace {
#ifdef FEX_BUILD_NUMBER
  constexpr const char* BuildNumberSuffix = "-b" FEX_BUILD_NUMBER;
#else
  constexpr const char* BuildNumberSuffix = "";
#endif
#ifdef FEX_BUILD_INFO
  constexpr const char* BuildInfo = FEX_BUILD_INFO;
#else
  constexpr const char* BuildInfo = "local";
#endif

  constexpr int64_t NR_clock_gettime = 113;
  constexpr int64_t CLOCK_BOOTTIME = 7;
  constexpr uint64_t PollNs = 25'000'000;         // process list read every 25 ms
  constexpr uint64_t SteamTailBytes = 64 * 1024;  // the end of each Steam log

  struct WatchState {
    FILE* File;
    int Fd = -1; // build 136: the same file opened by Linux, written without Wine (see Put)
    uint32_t Uid;
    int SelfPid;
    uint64_t SelfStartMs; // this process's start, ms since boot
    uint64_t Seconds;     // how long to watch processes
  };

  struct Proc {
    int Pid;
    bool Mine;
    uint64_t StartMs; // ms since boot
    fextl::string Name;
  };

  uint64_t BootMs() {
    struct {
      int64_t Sec;
      int64_t Nsec;
    } Ts {};
    Linux::Syscall(NR_clock_gettime, CLOCK_BOOTTIME, reinterpret_cast<intptr_t>(&Ts));
    return static_cast<uint64_t>(Ts.Sec) * 1000 + static_cast<uint64_t>(Ts.Nsec) / 1'000'000;
  }

  constexpr int64_t NR_openat = 56;
  constexpr int64_t NR_write_ = 64;

  void Put(const WatchState& S, const fextl::string& Text) {
    // Build 136: written straight to Linux when the file could be opened there, so lines keep coming even when a Wine
    // thread holds a lock or the wineserver doesn't answer (a hang looks the same as an ended process otherwise).
    if (S.Fd >= 0) {
      if (Linux::Syscall(NR_write_, S.Fd, reinterpret_cast<intptr_t>(Text.data()), Text.size()) >= 0) {
        return;
      }
    }
    // FEX's CRT fwrite goes straight to WriteFile (its fflush is a stub that ends the process), so each line is in the
    // file once this returns.
    fwrite(Text.data(), 1, Text.size(), S.File);
  }

  int ParsePid(const char* Name) {
    if (Name[0] < '1' || Name[0] > '9') {
      return 0;
    }
    int Pid {};
    for (const char* P = Name; *P; ++P) {
      if (*P < '0' || *P > '9') {
        return 0;
      }
      Pid = Pid * 10 + (*P - '0');
    }
    return Pid;
  }

  // Parent pid and start time (ms since boot) from /proc/<pid>/stat; false when it is gone.
  bool ReadParentAndStart(int Pid, int& Parent, uint64_t& StartMs) {
    char Path[48];
    snprintf(Path, sizeof(Path), "/proc/%d/stat", Pid);
    char Text[1024];
    if (Linux::ReadSmallFile(Path, Text, sizeof(Text)) <= 0) {
      return false;
    }
    const char* P = strrchr(Text, ')');
    if (!P) {
      return false;
    }
    ++P;
    // Fields after the name: state (3), ppid (4), ..., starttime (22).
    uint64_t Values[20] {};
    int Field = 3;
    while (*P && Field <= 22) {
      while (*P == ' ') {
        ++P;
      }
      if (!*P) {
        break;
      }
      if (Field >= 4) {
        Values[Field - 4 < 19 ? Field - 4 : 18] = strtoull(P, nullptr, 10);
      }
      while (*P && *P != ' ') {
        ++P;
      }
      ++Field;
    }
    if (Field <= 22) {
      return false;
    }
    Parent = static_cast<int>(Values[0]);
    StartMs = Values[18] * 10; // clock ticks of 10 ms
    return true;
  }

  fextl::string ReadComm(int Pid) {
    char Path[48];
    snprintf(Path, sizeof(Path), "/proc/%d/comm", Pid);
    char Text[64];
    int64_t Len = Linux::ReadSmallFile(Path, Text, sizeof(Text));
    if (Len <= 0) {
      return "?";
    }
    if (Text[Len - 1] == '\n') {
      Text[--Len] = 0;
    }
    return Text;
  }

  fextl::string ReadCmdline(int Pid) {
    char Path[48];
    snprintf(Path, sizeof(Path), "/proc/%d/cmdline", Pid);
    char Text[320];
    const int64_t Len = Linux::ReadSmallFile(Path, Text, sizeof(Text));
    if (Len <= 0) {
      return "";
    }
    for (int64_t i = 0; i < Len; ++i) {
      if (Text[i] == 0) {
        Text[i] = ' ';
      } else if (Text[i] == '\n' || Text[i] == '\r') {
        Text[i] = ' ';
      }
    }
    Text[Len] = 0;
    fextl::string Result = Text;
    while (!Result.empty() && Result.back() == ' ') {
      Result.pop_back();
    }
    return Result;
  }

  struct ListArgs {
    fextl::vector<int>* Pids;
  };

  fextl::vector<int> ListPids() {
    fextl::vector<int> Pids;
    ListArgs A {&Pids};
    Linux::ForEachEntry(
      "/proc",
      [](const char* Name, void* Arg) {
        if (const int Pid = ParsePid(Name)) {
          static_cast<ListArgs*>(Arg)->Pids->push_back(Pid);
        }
      },
      &A);
    std::sort(Pids.begin(), Pids.end());
    return Pids;
  }

  // A process seen for the first time: whether it is this user's, its name and start.
  Proc Describe(const WatchState& S, int Pid) {
    Proc P {.Pid = Pid};
    char Path[32];
    snprintf(Path, sizeof(Path), "/proc/%d", Pid);
    Linux::FileInfo Info {};
    P.Mine = Linux::Stat(Path, Info) && Info.Uid == S.Uid;
    if (P.Mine) {
      int Parent {};
      ReadParentAndStart(Pid, Parent, P.StartMs);
      P.Name = ReadComm(Pid);
    }
    return P;
  }

  // Seconds since this process started, for every line.
  fextl::string Stamp(const WatchState& S) {
    const uint64_t Now = BootMs();
    const int64_t Ms = static_cast<int64_t>(Now) - static_cast<int64_t>(S.SelfStartMs);
    return fextl::fmt::format("{}{}.{:03}", Ms < 0 ? "-" : "", std::abs(Ms) / 1000, std::abs(Ms) % 1000);
  }

  fextl::string ProcLine(const WatchState& S, int Pid, const Proc& P) {
    int Parent {};
    uint64_t StartMs {};
    ReadParentAndStart(Pid, Parent, StartMs);
    const int64_t Since = static_cast<int64_t>(StartMs) - static_cast<int64_t>(S.SelfStartMs);
    return fextl::fmt::format("{}/{} parent {} ({}) started {:+.2f} s | {}", P.Name, Pid, Parent, Parent > 0 ? ReadComm(Parent) : "-",
                              static_cast<double>(Since) / 1000.0, ReadCmdline(Pid));
  }

  // A Windows path of an existing file or folder as a Linux path, from Wine; empty with the reason in Why when Wine can't say.
  fextl::string UnixPathOf(const wchar_t* DosPath, fextl::string& Why) {
    UNICODE_STRING NtName {};
    if (!RtlDosPathNameToNtPathName_U(DosPath, &NtName, nullptr, nullptr)) {
      Why = "RtlDosPathNameToNtPathName_U failed";
      return {};
    }
    UNICODE_STRING NtDll {};
    RtlInitUnicodeString(&NtDll, L"ntdll.dll");
    HMODULE Module {};
    void* ProcAddr {};
    ANSI_STRING ProcName {};
    RtlInitAnsiString(&ProcName, "wine_nt_to_unix_file_name");
    using ToUnixFn = NTSTATUS(WINAPI*)(const OBJECT_ATTRIBUTES*, char*, ULONG*, UINT);
    fextl::string Result;
    if (LdrGetDllHandle(nullptr, 0, &NtDll, &Module) != STATUS_SUCCESS ||
        LdrGetProcedureAddress(Module, &ProcName, 0, &ProcAddr) != STATUS_SUCCESS || !ProcAddr) {
      Why = "ntdll has no wine_nt_to_unix_file_name";
    } else {
      OBJECT_ATTRIBUTES Attr {};
      InitializeObjectAttributes(&Attr, &NtName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
      char Unix[1024];
      ULONG Size = sizeof(Unix);
      const NTSTATUS Status = reinterpret_cast<ToUnixFn>(ProcAddr)(&Attr, Unix, &Size, 1 /* FILE_OPEN */);
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

  // Steam's logs folder as a Linux path (C:\Program Files (x86)\Steam\logs); empty when Wine can't say.
  fextl::string SteamLogsPath(fextl::string& Why) {
    return UnixPathOf(L"C:\\Program Files (x86)\\Steam\\logs", Why);
  }

  void CopyTail(const WatchState& S, const fextl::string& Dir, const char* Name) {
    const fextl::string Path = Dir + "/" + Name;
    Linux::FileInfo Info {};
    if (!Linux::Stat(Path.c_str(), Info) || !Info.Regular) {
      return;
    }
    const int Fd = Linux::OpenRead(Path.c_str(), false);
    if (Fd < 0) {
      Put(S, fextl::fmt::format("## steam log {}: can't open ({})\n", Name, Fd));
      return;
    }
    const uint64_t From = Info.Size > SteamTailBytes ? Info.Size - SteamTailBytes : 0;
    fextl::string Data;
    Data.resize(Info.Size - From);
    const int64_t Got = Linux::Pread(Fd, Data.data(), Data.size(), From);
    Linux::Close(Fd);
    Data.resize(Got > 0 ? static_cast<size_t>(Got) : 0);
    std::replace(Data.begin(), Data.end(), '\0', ' ');
    Put(S, fextl::fmt::format("## steam log {} ({} bytes, {}):\n", Name, Info.Size,
                                   From ? fextl::fmt::format("its last {} KB", SteamTailBytes / 1024) : fextl::string("all of it")));
    if (From) {
      // Start at a whole line.
      const size_t Newline = Data.find('\n');
      if (Newline != fextl::string::npos) {
        Data.erase(0, Newline + 1);
      }
    }
    if (!Data.empty() && Data.back() != '\n') {
      Data += '\n';
    }
    Put(S, Data);
    Put(S, fextl::fmt::format("## end of steam log {}\n", Name));
  }

  struct DirListArgs {
    fextl::string* Dir;
    fextl::string* Text;
  };

  [[maybe_unused]] void CopySteamLogs(const WatchState& S, bool ConsoleOnly) {
    fextl::string Why;
    const fextl::string Dir = SteamLogsPath(Why);
    if (Dir.empty()) {
      Put(S, fextl::fmt::format("## steam logs: C:\\Program Files (x86)\\Steam\\logs not found: {}\n", Why));
      return;
    }
    if (!ConsoleOnly) {
      fextl::string Text;
      fextl::string DirCopy = Dir;
      DirListArgs A {&DirCopy, &Text};
      const bool Listed = Linux::ForEachEntry(
        Dir.c_str(),
        [](const char* Name, void* Arg) {
          auto* A = static_cast<DirListArgs*>(Arg);
          Linux::FileInfo Info {};
          const fextl::string Path = *A->Dir + "/" + Name;
          if (Linux::Stat(Path.c_str(), Info)) {
            *A->Text += fextl::fmt::format(" {} {} KB;", Name, (Info.Size + 1023) / 1024);
          }
        },
        &A);
      Put(S, fextl::fmt::format("## steam logs folder {}: {}\n", Dir, Listed ? (Text.empty() ? fextl::string(" empty") : Text) :
                                                                                     fextl::string(" can't be read")));
    }
    constexpr const char* All[] = {"console_log.previous.txt", "console_log.txt",          "bootstrap_log.txt", "connection_log.previous.txt",
                                   "connection_log.txt",       "stderr.txt",               "compat_log.txt",    "content_log.txt",
                                   "cef_log.previous.txt",     "cef_log.txt",              "webhelper.txt",     "sitelicense_log.txt"};
    if (ConsoleOnly) {
      CopyTail(S, Dir, "console_log.txt");
      CopyTail(S, Dir, "connection_log.txt");
      CopyTail(S, Dir, "compat_log.txt");
    } else {
      for (const char* Name : All) {
        CopyTail(S, Dir, Name);
      }
    }
  }


  // Android's log (logd), build 132. An app may read the log entries of its own Linux user: the launcher's (GameNative's)
  // Java and native errors, a crash's "FATAL EXCEPTION" or "Fatal signal" lines, and the crash buffer. In the failed
  // starts of 2026-11-01 every process of the container went away at once (none ended before the others in 25 ms
  // checks), which is what Android does when the launcher's own app process dies: it ends everything in its group.
  constexpr int64_t NR_kill = 129;
  constexpr int64_t NR_socket = 198;
  constexpr int64_t NR_connect = 203;
  constexpr int64_t NR_setsockopt = 208;
  constexpr int64_t NR_read = 63;
  constexpr int64_t NR_write = 64;

  struct LogCursor {
    uint64_t Sec;
    uint64_t Nsec;
  };

  int64_t LocalOffsetS() {
    LARGE_INTEGER SystemTime {}, LocalTime {};
    NtQuerySystemTime(&SystemTime);
    RtlSystemTimeToLocalTime(&SystemTime, &LocalTime);
    return (LocalTime.QuadPart - SystemTime.QuadPart) / 10'000'000LL;
  }

  // Runs one logd reader command ("dumpAndClose ...") and appends the entries at MinPrio or above (all of the crash
  // buffer) newer than the cursor as text lines. Returns how many entries logd sent, or a negative errno.
  int64_t ReadLogd(const fextl::string& Command, int MinPrio, LogCursor& Cursor, fextl::vector<fextl::string>& Lines, int64_t OffsetS) {
    const int64_t Fd = Linux::Syscall(NR_socket, 1 /* AF_UNIX */, 5 /* SOCK_SEQPACKET */ | 0x80000 /* SOCK_CLOEXEC */, 0);
    if (Fd < 0) {
      return Fd;
    }
    struct {
      uint16_t Family;
      char Path[108];
    } Addr {};
    Addr.Family = 1;
    strcpy(Addr.Path, "/dev/socket/logdr");
    int64_t Result = Linux::Syscall(NR_connect, Fd, reinterpret_cast<intptr_t>(&Addr), 2 + strlen(Addr.Path) + 1);
    if (Result < 0) {
      Linux::Close(static_cast<int>(Fd));
      return Result;
    }
    struct {
      int64_t Sec;
      int64_t Usec;
    } Timeout {1, 0};
    Linux::Syscall(NR_setsockopt, Fd, 1 /* SOL_SOCKET */, 20 /* SO_RCVTIMEO */, reinterpret_cast<intptr_t>(&Timeout), sizeof(Timeout));
    Result = Linux::Syscall(NR_write, Fd, reinterpret_cast<intptr_t>(Command.data()), Command.size());
    if (Result < 0) {
      Linux::Close(static_cast<int>(Fd));
      return Result;
    }
    alignas(8) unsigned char Buffer[8192 + 1];
    int64_t Entries {};
    for (int Guard = 0; Guard < 200000; ++Guard) {
      const int64_t Len = Linux::Syscall(NR_read, Fd, reinterpret_cast<intptr_t>(Buffer), sizeof(Buffer) - 1);
      if (Len <= 0) {
        break;
      }
      if (Len < 28) {
        continue;
      }
      ++Entries;
      uint16_t PayloadLen, HdrSize;
      int32_t Pid;
      uint32_t Tid, Sec, Nsec, Lid;
      memcpy(&PayloadLen, Buffer + 0, 2);
      memcpy(&HdrSize, Buffer + 2, 2);
      memcpy(&Pid, Buffer + 4, 4);
      memcpy(&Tid, Buffer + 8, 4);
      memcpy(&Sec, Buffer + 12, 4);
      memcpy(&Nsec, Buffer + 16, 4);
      memcpy(&Lid, Buffer + 20, 4);
      if (HdrSize < 20 || HdrSize > Len) {
        HdrSize = 20;
      }
      if (Sec < Cursor.Sec || (Sec == Cursor.Sec && Nsec <= Cursor.Nsec)) {
        continue;
      }
      Cursor = {Sec, Nsec};
      if (Lid == 2 || Lid == 5 || Lid == 6) { // events, stats, security: binary
        continue;
      }
      const size_t End = std::min<size_t>(static_cast<size_t>(Len), static_cast<size_t>(HdrSize) + PayloadLen);
      if (End <= HdrSize + 1u) {
        continue;
      }
      Buffer[End] = 0;
      const unsigned Prio = Buffer[HdrSize];
      if (Lid != 4 && static_cast<int>(Prio) < MinPrio) {
        continue;
      }
      const char* Tag = reinterpret_cast<const char*>(Buffer + HdrSize + 1);
      const size_t TagLen = strnlen(Tag, End - HdrSize - 1);
      const char* Msg = Tag + TagLen + 1;
      fextl::string Text = reinterpret_cast<const char*>(Buffer + HdrSize + 1) + TagLen < reinterpret_cast<const char*>(Buffer + End) ?
                             fextl::string(Msg, strnlen(Msg, reinterpret_cast<const char*>(Buffer + End) - Msg)) :
                             fextl::string();
      while (!Text.empty() && (Text.back() == '\n' || Text.back() == ' ')) {
        Text.pop_back();
      }
      std::replace(Text.begin(), Text.end(), '\n', '|');
      constexpr char Letters[] = "??VDIWEFS";
      constexpr const char* Buffers[] = {"main", "radio", "events", "system", "crash", "stats", "security", "kernel"};
      const int64_t Local = static_cast<int64_t>(Sec) + OffsetS;
      Lines.push_back(fextl::fmt::format("{:02}:{:02}:{:02}.{:03} {} {}/{} {} {}: {}", (Local / 3600) % 24, (Local / 60) % 60, Local % 60,
                                         Nsec / 1'000'000, Lid < 8 ? Buffers[Lid] : "?", Pid, Tid, Prio < 9 ? Letters[Prio] : '?',
                                         fextl::string(Tag, TagLen), Text));
    }
    Linux::Close(static_cast<int>(Fd));
    return Entries;
  }

  // The launcher's own app process: the pid in this process's Android group (/uid_N/pid_M in /proc/self/cgroup).
  int LauncherPid() {
    char Text[1024];
    if (Linux::ReadSmallFile("/proc/self/cgroup", Text, sizeof(Text)) <= 0) {
      return 0;
    }
    const char* P = strstr(Text, "/pid_");
    return P ? atoi(P + 5) : 0;
  }

  bool ProcessExists(int Pid) {
    // 0 or -EPERM: there; -ESRCH (3): gone. /proc hides the launcher's process from the game.
    return Linux::Syscall(NR_kill, Pid, 0) != -3;
  }

  // Build 136: CPU time this whole process has used, in ms.
  uint64_t ProcessCpuMs() {
    struct {
      int64_t Sec;
      int64_t Nsec;
    } Ts {};
    Linux::Syscall(NR_clock_gettime, 2 /* CLOCK_PROCESS_CPUTIME_ID */, reinterpret_cast<intptr_t>(&Ts));
    return static_cast<uint64_t>(Ts.Sec) * 1000 + static_cast<uint64_t>(Ts.Nsec) / 1'000'000;
  }

  // One line of a task's state: name, state, CPU time, the kernel function it sleeps in and its system call (number and
  // first arguments; the futex address shows which threads wait for the same lock).
  fextl::string TaskLine(const char* Base) {
    char Path[96];
    char Text[256];
    fextl::string Line;
    snprintf(Path, sizeof(Path), "%s/stat", Base);
    Linux::TaskStat T {};
    if (Linux::ReadTaskStat(Path, T)) {
      Line = fextl::fmt::format("{} state {} cpu {} ms core {}", T.Comm, T.State, (T.UserTicks + T.SystemTicks) * 10, T.Processor);
    } else {
      return "gone";
    }
    snprintf(Path, sizeof(Path), "%s/wchan", Base);
    if (Linux::ReadSmallFile(Path, Text, sizeof(Text)) > 0) {
      Line += fextl::fmt::format(" in {}", Text);
    }
    snprintf(Path, sizeof(Path), "%s/syscall", Base);
    if (Linux::ReadSmallFile(Path, Text, sizeof(Text)) > 0) {
      // "nr a0 a1 a2 a3 a4 a5 sp pc": keep the number and the first three arguments.
      char* End = Text;
      for (int Fields = 0; *End && Fields < 4; ++End) {
        if (*End == ' ' && ++Fields == 4) {
          break;
        }
      }
      *End = 0;
      for (char* C = Text; *C; ++C) {
        if (*C == '\n') {
          *C = 0;
          break;
        }
      }
      Line += fextl::fmt::format(" | syscall {}", Text);
    }
    return Line;
  }

  // Every thread of this process (and the wineserver): who waits where when the game makes no progress.
  void DumpThreads(const WatchState& S, int Server, const char* Why) {
    fextl::vector<int> Tids;
    Linux::ForEachEntry(
      "/proc/self/task",
      [](const char* Name, void* Arg) {
        if (const int Tid = ParsePid(Name)) {
          static_cast<fextl::vector<int>*>(Arg)->push_back(Tid);
        }
      },
      &Tids);
    std::sort(Tids.begin(), Tids.end());
    Put(S, fextl::fmt::format("{} ## threads ({}): {} threads\n", Stamp(S), Why, Tids.size()));
    for (int Tid : Tids) {
      char Base[48];
      snprintf(Base, sizeof(Base), "/proc/self/task/%d", Tid);
      Put(S, fextl::fmt::format("  tid {}: {}\n", Tid, TaskLine(Base)));
    }
    if (Server) {
      char Base[48];
      snprintf(Base, sizeof(Base), "/proc/%d", Server);
      Put(S, fextl::fmt::format("  wineserver {}: {}\n", Server, TaskLine(Base)));
    }
    Put(S, "  ## end of threads\n");
  }

  void* WatchThread(void* Arg) {
    auto& S = *static_cast<WatchState*>(Arg);
    const uint64_t BeginMs = BootMs();

    // Everything of this user at the start, the parent first.
    fextl::vector<Proc> Known;
    for (int Pid : ListPids()) {
      Known.push_back(Describe(S, Pid));
    }
    int Parent {};
    uint64_t Unused {};
    ReadParentAndStart(S.SelfPid, Parent, Unused);
    Put(S, fextl::fmt::format("{} this process: {} | parent: {}/{} | {}\n", Stamp(S), ReadCmdline(S.SelfPid), ReadComm(Parent), Parent,
                                   ReadCmdline(Parent)));
    size_t Mine {};
    for (const auto& P : Known) {
      if (P.Mine && P.Pid != S.SelfPid) {
        Put(S, fextl::fmt::format("{} running: {}\n", Stamp(S), ProcLine(S, P.Pid, P)));
        ++Mine;
      }
    }
    Put(S, fextl::fmt::format("{} {} other processes of this user (uid {}) running; {} processes in all\n", Stamp(S), Mine, S.Uid,
                                   Known.size()));

    // Steam's own logs (build 129) were stale in GameNative's container (its steam.exe doesn't write them): left out.
    // Android's log instead: what the launcher logged lately, including a previous start that failed.
    const int64_t OffsetS = LocalOffsetS();
    LogCursor Cursor {};
    {
      fextl::vector<fextl::string> Lines;
      const int64_t Got = ReadLogd("dumpAndClose lids=0,3,4 tail=30000", 5 /* W */, Cursor, Lines, OffsetS);
      constexpr size_t Keep = 1500;
      Put(S, fextl::fmt::format("## android log (this app's entries, warnings and errors, local time): {} entries read{}, {} lines{}\n", Got < 0 ? 0 : Got,
                                     Got < 0 ? fextl::fmt::format(" (logd error {})", Got) : fextl::string(), Lines.size(),
                                     Lines.size() > Keep ? fextl::fmt::format(", the last {}", Keep) : fextl::string()));
      for (size_t i = Lines.size() > Keep ? Lines.size() - Keep : 0; i < Lines.size(); ++i) {
        Put(S, Lines[i] + "\n");
      }
      Put(S, "## end of android log\n");
    }
    {
      // Build 134: every entry (info and debug too) of the last 10 minutes. GameNative's own messages about the container
      // (Wine's programs ending, its X server) and Wine's own output are at info level, and in the build 133 failed start
      // no warning or error came before the container went away.
      LARGE_INTEGER SystemTime {};
      NtQuerySystemTime(&SystemTime);
      const int64_t NowS = SystemTime.QuadPart / 10'000'000LL - 11'644'473'600LL;
      LogCursor Recent {static_cast<uint64_t>(NowS - 600), 0};
      fextl::vector<fextl::string> Lines;
      const int64_t Got = ReadLogd(fextl::fmt::format("dumpAndClose lids=0,3,4 start={}.000000000", NowS - 600), 0, Recent, Lines, OffsetS);
      constexpr size_t Keep = 6000;
      Put(S, fextl::fmt::format("## android log, all entries of the last 10 minutes (local time): {} entries read{}, {} lines{}\n",
                                     Got < 0 ? 0 : Got, Got < 0 ? fextl::fmt::format(" (logd error {})", Got) : fextl::string(), Lines.size(),
                                     Lines.size() > Keep ? fextl::fmt::format(", the last {}", Keep) : fextl::string()));
      for (size_t i = Lines.size() > Keep ? Lines.size() - Keep : 0; i < Lines.size(); ++i) {
        Put(S, Lines[i] + "\n");
      }
      Put(S, "## end of android log, all entries\n");
      if (Recent.Sec > Cursor.Sec || (Recent.Sec == Cursor.Sec && Recent.Nsec > Cursor.Nsec)) {
        Cursor = Recent;
      }
    }
    // The wineserver: when it ends, every Wine program ends within milliseconds, which is what the failed starts look
    // like. Checked every 2 ms so its end is seen before this process goes.
    int Server {};
    for (const auto& P : Known) {
      if (P.Mine && P.Name == "wineserver") {
        Server = P.Pid;
      }
    }
    bool ServerGone = false;
    char ServerState = '?';
    const int Launcher = LauncherPid();
    bool LauncherGone = false;
    Put(S, fextl::fmt::format("{} launcher app process {} {}; watching process starts and ends every {} ms and new android log warnings "
                                   "and errors every 250 ms (every entry every 100 ms in the first 30 s), the wineserver every 2 ms, for {} s\n",
                                   Stamp(S), Launcher, Launcher ? (ProcessExists(Launcher) ? "running" : "not found") : "unknown",
                                   PollNs / 1'000'000, S.Seconds));

    uint64_t LastCpuMs = ProcessCpuMs();
    int Dumps {};
    bool EarlyDumped = false;
    uint64_t NextLog = BeginMs + 250;
    uint64_t NextBeat = BeginMs + 1000;
    while (BootMs() - BeginMs < S.Seconds * 1000) {
      for (uint64_t Slept = 0; Slept < PollNs; Slept += 2'000'000) {
        Linux::SleepNs(2'000'000);
        if (Server && !ServerGone) {
          char Path[48];
          snprintf(Path, sizeof(Path), "/proc/%d/stat", Server);
          Linux::TaskStat T {};
          if (Linux::ReadTaskStat(Path, T)) {
            ServerState = T.State;
          } else if (!ProcessExists(Server)) {
            ServerGone = true;
            Put(S, fextl::fmt::format("{} ended: the wineserver {} (last seen in state {})\n", Stamp(S), Server, ServerState));
          }
        }
      }
      const auto Pids = ListPids();
      fextl::vector<Proc> Next;
      Next.reserve(Pids.size());
      size_t k = 0;
      for (int Pid : Pids) {
        while (k < Known.size() && Known[k].Pid < Pid) {
          if (Known[k].Mine) {
            const int64_t Lived = static_cast<int64_t>(BootMs()) - static_cast<int64_t>(Known[k].StartMs);
            Put(S, fextl::fmt::format("{} ended: {}/{} after {:.2f} s\n", Stamp(S), Known[k].Name, Known[k].Pid,
                                           static_cast<double>(Lived) / 1000.0));
          }
          ++k;
        }
        if (k < Known.size() && Known[k].Pid == Pid) {
          Next.push_back(std::move(Known[k]));
          ++k;
        } else {
          Proc P = Describe(S, Pid);
          if (P.Mine) {
            Put(S, fextl::fmt::format("{} started: {}\n", Stamp(S), ProcLine(S, Pid, P)));
          }
          Next.push_back(std::move(P));
        }
      }
      for (; k < Known.size(); ++k) {
        if (Known[k].Mine) {
          const int64_t Lived = static_cast<int64_t>(BootMs()) - static_cast<int64_t>(Known[k].StartMs);
          Put(S, fextl::fmt::format("{} ended: {}/{} after {:.2f} s\n", Stamp(S), Known[k].Name, Known[k].Pid,
                                         static_cast<double>(Lived) / 1000.0));
        }
      }
      Known = std::move(Next);

      if (Launcher && !LauncherGone && !ProcessExists(Launcher)) {
        LauncherGone = true;
        Put(S, fextl::fmt::format("{} ended: the launcher's app process {}\n", Stamp(S), Launcher));
      }
      const uint64_t Now = BootMs();
      if (Now >= NextLog) {
        // Every entry for the first 30 s (the failed starts ended at 1-2 s), then warnings and errors.
        const bool Early = Now - BeginMs < 30'000;
        NextLog = Now + (Early ? 100 : 250);
        fextl::vector<fextl::string> Lines;
        ReadLogd(fextl::fmt::format("dumpAndClose lids=0,3,4 start={}.{:09}", Cursor.Sec, Cursor.Nsec), Early ? 0 : 5, Cursor, Lines, OffsetS);
        for (const auto& L : Lines) {
          Put(S, fextl::fmt::format("{} log: {}\n", Stamp(S), L));
        }
      }
      if (Now >= NextBeat) {
        size_t Count {};
        for (const auto& P : Known) {
          Count += P.Mine;
        }
        const uint64_t Cpu = ProcessCpuMs();
        Put(S, fextl::fmt::format("{} alive, {} processes of this user, this process used {} ms CPU in the last second\n", Stamp(S), Count,
                                  Cpu - LastCpuMs));
        NextBeat += 1000;
        // Build 136: in the first 30 s a starting D2R keeps a core busy; next to no CPU for a second means it waits on
        // something (a lock, the wineserver): write down every thread's state (up to 6 times; also once at 1 s, just before
        // the failed starts stopped).
        const uint64_t Since = Now - BeginMs;
        if (Dumps < 6 && Since < 30'000 && Cpu - LastCpuMs < 100) {
          ++Dumps;
          DumpThreads(S, Server, "this process used little CPU in the last second");
        }
        LastCpuMs = Cpu;
      }
      if (!EarlyDumped && Now >= S.SelfStartMs + 1000) {
        EarlyDumped = true;
        DumpThreads(S, Server, "1 s after this process started, for comparison");
      }
    }
    Put(S, fextl::fmt::format("{} start watch done\n", Stamp(S)));
    return nullptr;
  }
} // namespace

namespace {
  WatchState* Active;
}

void StartWatchNote(std::string_view Text) {
  if (auto* S = Active) {
    Put(*S, fextl::fmt::format("{} {}\n", Stamp(*S), Text));
  }
}

void StartStartWatch(std::string_view AppName) {
  const char* Dir = Switch("FEX_START_WATCH");
  if (!Dir || !Dir[0] || (Dir[0] == '0' && !Dir[1])) {
    return;
  }
  const char* SecondsText = Switch("FEX_START_WATCH_S");
  const uint64_t Seconds = std::clamp<uint64_t>(SecondsText ? strtoull(SecondsText, nullptr, 10) : 120, 5, 3600);

  fextl::string Folder = Dir;
  while (!Folder.empty() && (Folder.back() == '\\' || Folder.back() == '/')) {
    Folder.pop_back();
  }
  LARGE_INTEGER SystemTime {}, LocalTime {};
  NtQuerySystemTime(&SystemTime);
  RtlSystemTimeToLocalTime(&SystemTime, &LocalTime);
  const uint64_t SecondOfDay = (static_cast<uint64_t>(LocalTime.QuadPart) / 10'000'000ULL) % 86400;
  const fextl::string AppNameCopy {AppName};
  const auto Path = fextl::fmt::format("{}\\fexstart-{}{}-{:02}{:02}{:02}-{}.txt", Folder, AppNameCopy, BuildNumberSuffix, SecondOfDay / 3600,
                                       (SecondOfDay / 60) % 60, SecondOfDay % 60, GetCurrentProcessId());
  FILE* File = fopen(Path.c_str(), "w");
  if (!File) {
    StatsLogNote(fextl::fmt::format("start watch: can't open {}", Path));
    return;
  }

  // Lives for the rest of the process.
  auto* S = new WatchState {.File = File, .Uid = Linux::GetUid(), .SelfPid = Linux::GetPid(), .Seconds = Seconds};
  // Build 136: the same file opened by Linux (appending), so writing it needs no Wine.
  fextl::string FdWhy;
  {
    fextl::vector<wchar_t> Wide(Path.begin(), Path.end());
    Wide.push_back(0);
    const fextl::string UnixPath = UnixPathOf(Wide.data(), FdWhy);
    if (!UnixPath.empty()) {
      const int64_t Fd = Linux::Syscall(NR_openat, -100 /* AT_FDCWD */, reinterpret_cast<intptr_t>(UnixPath.c_str()),
                                        0x1 /* O_WRONLY */ | 0x400 /* O_APPEND */ | 0x80000 /* O_CLOEXEC */);
      if (Fd >= 0) {
        S->Fd = static_cast<int>(Fd);
      } else {
        FdWhy = fextl::fmt::format("open {} failed ({})", UnixPath, Fd);
      }
    }
  }
  int Parent {};
  uint64_t StartMs {};
  S->SelfStartMs = ReadParentAndStart(S->SelfPid, Parent, StartMs) ? StartMs : BootMs();

  Put(*S, fextl::fmt::format("# FEX start watch for {} (Windows pid {}, Linux pid {}, build {}). Times are seconds since this process "
                               "started (Linux). Lines: this process and its parent; every process of this Linux user at the start; this app's "
                               "Android log entries (warnings and errors, a previous failed start's included); then every start and end of "
                               "this user's processes and of the launcher's app process, checked every {} ms, new Android log warnings and "
                               "errors every 250 ms ('log:'), and 'alive' once a second, for {} s. A file whose last line is an 'alive', "
                               "'log' or 'ended' line belongs to a process that was ended from outside then (FEX_START_WATCH=0 turns this off)\n",
                               AppNameCopy, GetCurrentProcessId(), S->SelfPid, BuildInfo, PollNs / 1'000'000, Seconds));
  Put(*S, S->Fd >= 0 ? fextl::string("# written through Linux (no Wine), so lines keep coming while Wine hangs; '## threads' blocks = every "
                                     "thread's state when the process used little CPU for a second\n") :
                       fextl::fmt::format("# written through Wine ({}); lines stop when Wine hangs\n", FdWhy));
  StatsLogNote(fextl::fmt::format("start watch: process starts and ends of this user and the end of Steam's logs go to {} (FEX_START_WATCH=0 "
                                  "turns it off)",
                                  Path));

  Active = S;
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(WatchThread, S, {.LowPriority = false, .Internal = true}, "FEXStartWatch");
}
} // namespace FEX::Windows
