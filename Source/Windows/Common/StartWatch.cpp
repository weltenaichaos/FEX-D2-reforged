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
  constexpr uint64_t SteamAgainS = 60;            // Steam's console log copied again after this long

  struct WatchState {
    FILE* File;
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

  void Put(FILE* File, const fextl::string& Text) {
    // FEX's CRT fwrite goes straight to WriteFile (its fflush is a stub that ends the process), so each line is in the
    // file once this returns.
    fwrite(Text.data(), 1, Text.size(), File);
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

  // Steam's logs folder as a Linux path, from Wine (C:\Program Files (x86)\Steam\logs); empty when Wine can't say.
  fextl::string SteamLogsPath(fextl::string& Why) {
    UNICODE_STRING NtName {};
    if (!RtlDosPathNameToNtPathName_U(L"C:\\Program Files (x86)\\Steam\\logs", &NtName, nullptr, nullptr)) {
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

  void CopyTail(const WatchState& S, const fextl::string& Dir, const char* Name) {
    const fextl::string Path = Dir + "/" + Name;
    Linux::FileInfo Info {};
    if (!Linux::Stat(Path.c_str(), Info) || !Info.Regular) {
      return;
    }
    const int Fd = Linux::OpenRead(Path.c_str(), false);
    if (Fd < 0) {
      Put(S.File, fextl::fmt::format("## steam log {}: can't open ({})\n", Name, Fd));
      return;
    }
    const uint64_t From = Info.Size > SteamTailBytes ? Info.Size - SteamTailBytes : 0;
    fextl::string Data;
    Data.resize(Info.Size - From);
    const int64_t Got = Linux::Pread(Fd, Data.data(), Data.size(), From);
    Linux::Close(Fd);
    Data.resize(Got > 0 ? static_cast<size_t>(Got) : 0);
    std::replace(Data.begin(), Data.end(), '\0', ' ');
    Put(S.File, fextl::fmt::format("## steam log {} ({} bytes, {}):\n", Name, Info.Size,
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
    Put(S.File, Data);
    Put(S.File, fextl::fmt::format("## end of steam log {}\n", Name));
  }

  struct DirListArgs {
    fextl::string* Dir;
    fextl::string* Text;
  };

  void CopySteamLogs(const WatchState& S, bool ConsoleOnly) {
    fextl::string Why;
    const fextl::string Dir = SteamLogsPath(Why);
    if (Dir.empty()) {
      Put(S.File, fextl::fmt::format("## steam logs: C:\\Program Files (x86)\\Steam\\logs not found: {}\n", Why));
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
      Put(S.File, fextl::fmt::format("## steam logs folder {}: {}\n", Dir, Listed ? (Text.empty() ? fextl::string(" empty") : Text) :
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
    Put(S.File, fextl::fmt::format("{} this process: {} | parent: {}/{} | {}\n", Stamp(S), ReadCmdline(S.SelfPid), ReadComm(Parent), Parent,
                                   ReadCmdline(Parent)));
    size_t Mine {};
    for (const auto& P : Known) {
      if (P.Mine && P.Pid != S.SelfPid) {
        Put(S.File, fextl::fmt::format("{} running: {}\n", Stamp(S), ProcLine(S, P.Pid, P)));
        ++Mine;
      }
    }
    Put(S.File, fextl::fmt::format("{} {} other processes of this user (uid {}) running; {} processes in all\n", Stamp(S), Mine, S.Uid,
                                   Known.size()));

    CopySteamLogs(S, false);
    Put(S.File, fextl::fmt::format("{} watching process starts and ends every {} ms for {} s\n", Stamp(S), PollNs / 1'000'000, S.Seconds));

    bool CopiedAgain = false;
    uint64_t NextBeat = BeginMs + 1000;
    while (BootMs() - BeginMs < S.Seconds * 1000) {
      Linux::SleepNs(PollNs);
      const auto Pids = ListPids();
      fextl::vector<Proc> Next;
      Next.reserve(Pids.size());
      size_t k = 0;
      for (int Pid : Pids) {
        while (k < Known.size() && Known[k].Pid < Pid) {
          if (Known[k].Mine) {
            const int64_t Lived = static_cast<int64_t>(BootMs()) - static_cast<int64_t>(Known[k].StartMs);
            Put(S.File, fextl::fmt::format("{} ended: {}/{} after {:.2f} s\n", Stamp(S), Known[k].Name, Known[k].Pid,
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
            Put(S.File, fextl::fmt::format("{} started: {}\n", Stamp(S), ProcLine(S, Pid, P)));
          }
          Next.push_back(std::move(P));
        }
      }
      for (; k < Known.size(); ++k) {
        if (Known[k].Mine) {
          const int64_t Lived = static_cast<int64_t>(BootMs()) - static_cast<int64_t>(Known[k].StartMs);
          Put(S.File, fextl::fmt::format("{} ended: {}/{} after {:.2f} s\n", Stamp(S), Known[k].Name, Known[k].Pid,
                                         static_cast<double>(Lived) / 1000.0));
        }
      }
      Known = std::move(Next);

      const uint64_t Now = BootMs();
      if (Now >= NextBeat) {
        size_t Count {};
        for (const auto& P : Known) {
          Count += P.Mine;
        }
        Put(S.File, fextl::fmt::format("{} alive, {} processes of this user\n", Stamp(S), Count));
        NextBeat += 1000;
      }
      if (!CopiedAgain && Now - BeginMs >= SteamAgainS * 1000) {
        CopiedAgain = true;
        Put(S.File, fextl::fmt::format("{} Steam's logs again after {} s:\n", Stamp(S), SteamAgainS));
        CopySteamLogs(S, true);
      }
    }
    Put(S.File, fextl::fmt::format("{} start watch done\n", Stamp(S)));
    if (!CopiedAgain) {
      CopySteamLogs(S, true);
    }
    return nullptr;
  }
} // namespace

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
  int Parent {};
  uint64_t StartMs {};
  S->SelfStartMs = ReadParentAndStart(S->SelfPid, Parent, StartMs) ? StartMs : BootMs();

  Put(File, fextl::fmt::format("# FEX start watch for {} (Windows pid {}, Linux pid {}, build {}). Times are seconds since this process "
                               "started (Linux). Lines: this process and its parent; every process of this Linux user at the start; the "
                               "end of Steam's logs (the previous Steam session's is console_log.previous.txt); then every start and end "
                               "of this user's processes, checked every {} ms, and 'alive' once a second, for {} s. A file whose last "
                               "line is an 'alive' or 'ended' line belongs to a process that was ended from outside then "
                               "(FEX_START_WATCH=0 turns this off)\n",
                               AppNameCopy, GetCurrentProcessId(), S->SelfPid, BuildInfo, PollNs / 1'000'000, Seconds));
  StatsLogNote(fextl::fmt::format("start watch: process starts and ends of this user and the end of Steam's logs go to {} (FEX_START_WATCH=0 "
                                  "turns it off)",
                                  Path));

  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(WatchThread, S, {.LowPriority = false, .Internal = true}, "FEXStartWatch");
}
} // namespace FEX::Windows
