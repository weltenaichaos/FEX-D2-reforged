// SPDX-License-Identifier: MIT
#include "Windows/Common/ThreadPlacer.h"
#include "Windows/Common/StatsLog.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <cstdlib>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winbase.h>

// Exported by ntdll (Defs/ntdll.def) but not declared by the bundled headers.
extern "C" NTSTATUS WINAPI NtGetNextThread(HANDLE Process, HANDLE Thread, ACCESS_MASK Access, ULONG Attributes, ULONG Flags, HANDLE* Next);

namespace FEX::Windows {
namespace {
  const THREADINFOCLASS ThreadAffinityMaskClass = static_cast<THREADINFOCLASS>(4);
  const THREADINFOCLASS ThreadNameInformationClass = static_cast<THREADINFOCLASS>(38);
  // A thread must use at least this share of a core to count as one of the busiest.
  constexpr double MinHeavyLoad = 15.0;
  // A busy thread keeps the big cores until it has been outside the busiest set for this many seconds in a row.
  constexpr uint32_t DemoteAfterSeconds = 3;
  // After a failed affinity change, wait this many seconds before trying that thread again.
  constexpr uint32_t RetryAfterSeconds = 10;
  // Sticky mode: a game thread goes to the little cores only after this many seconds in a row below IdleLoad, and
  // returns to the big cores the first second it is above WakeLoad.
  constexpr uint32_t IdleAfterSeconds = 30;
  constexpr double IdleLoad = 0.3;
  constexpr double WakeLoad = 2.0;
  // A thread that fails this many times in a row with STATUS_THREAD_IS_TERMINATING is left alone.
  constexpr uint32_t MaxTerminatingFailures = 2;

  // Same layout as Windows' THREAD_BASIC_INFORMATION; spelled out because the bundled headers differ in field names.
  struct ThreadBasicInfo {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    LONG Priority;
    LONG BasePriority;
  };

  struct Config {
    uint64_t Big;
    uint64_t Little;
    uint64_t Rest;
    uint32_t Heavy;
    int Fast;
    fextl::string RestName;
    bool Sticky; // FEX_PIN_MODE=sticky (default): game threads stay on the big cores unless idle; =load: busiest-N ranking
    bool Other; // a process other than FEX_PIN_APP: everything goes to the little cores
  };

  struct Tracked {
    uint64_t LastTime100ns;
    double Load; // smoothed share of one core, in percent
    bool Heavy;
    uint32_t OutCount;
    uint32_t IdleCount;
    uint32_t TerminatingFailures;
    uint64_t SetMask; // what we last set, 0 = nothing yet
    uint32_t RetryAt;
    bool ReportedForeign;
    fextl::string Name;
    uint32_t SeenAt;
  };

  // "3-7", "0,1,2", "0-2,7" -> bit mask. Returns 0 on anything unparsable.
  uint64_t ParseCPUList(const char* Text) {
    uint64_t Mask {};
    const char* p = Text;
    while (*p) {
      if (*p < '0' || *p > '9') {
        return 0;
      }
      int First = 0;
      while (*p >= '0' && *p <= '9') {
        First = First * 10 + (*p++ - '0');
      }
      int Last = First;
      if (*p == '-') {
        ++p;
        if (*p < '0' || *p > '9') {
          return 0;
        }
        Last = 0;
        while (*p >= '0' && *p <= '9') {
          Last = Last * 10 + (*p++ - '0');
        }
      }
      if (First > Last || Last > 63) {
        return 0;
      }
      for (int c = First; c <= Last; ++c) {
        Mask |= 1ULL << c;
      }
      if (*p == ',') {
        ++p;
      } else if (*p) {
        return 0;
      }
    }
    return Mask;
  }

  int ParseInt(const char* Text, int Default) {
    if (!Text || *Text < '0' || *Text > '9') {
      return Default;
    }
    int Value = 0;
    while (*Text >= '0' && *Text <= '9' && Value < 1000) {
      Value = Value * 10 + (*Text++ - '0');
    }
    return Value;
  }

  uint64_t EnvCPUList(const char* Name, uint64_t Default) {
    const char* Value = getenv(Name);
    if (!Value || !Value[0]) {
      return Default;
    }
    const uint64_t Mask = ParseCPUList(Value);
    if (!Mask) {
      StatsLogNote(fextl::fmt::format("pin: can't parse {}={}, using default", Name, Value));
      return Default;
    }
    return Mask;
  }

  fextl::string QueryName(HANDLE Thread) {
    alignas(8) uint8_t Buffer[sizeof(UNICODE_STRING) + 128 * sizeof(WCHAR)] {};
    fextl::string Result;
    ULONG Length {};
    if (NtQueryInformationThread(Thread, ThreadNameInformationClass, Buffer, sizeof(Buffer), &Length) == STATUS_SUCCESS) {
      const auto* Name = reinterpret_cast<const UNICODE_STRING*>(Buffer);
      for (USHORT i = 0; Name->Buffer && i < Name->Length / sizeof(WCHAR); ++i) {
        const WCHAR c = Name->Buffer[i];
        Result.push_back(c >= 0x20 && c < 0x7f && c != ' ' ? static_cast<char>(c) : '_');
      }
    }
    return Result;
  }

  bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      auto Lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      };
      if (Lower(a[i]) != Lower(b[i])) {
        return false;
      }
    }
    return true;
  }

  bool IsFEXThread(const fextl::string& Name) {
    return Name.size() >= 3 && Name.compare(0, 3, "FEX") == 0;
  }

  bool StartsWith(const fextl::string& Name, std::string_view Prefix) {
    return Name.size() >= Prefix.size() && Name.compare(0, Prefix.size(), Prefix) == 0;
  }

  // Threads that use little CPU but wake every frame (GPU submission, fences, present, audio). A slow little core or a
  // migration delays the frame, so sticky mode never moves them off the big cores.
  bool IsFrameThread(const fextl::string& Name) {
    return StartsWith(Name, "vkd3d") || StartsWith(Name, "dxvk") || StartsWith(Name, "wine_vk") || StartsWith(Name, "audio") ||
           StartsWith(Name, "Audio");
  }

  const char* MaskKind(const Config& C, uint64_t Mask) {
    if (C.Fast >= 0 && Mask == (1ULL << C.Fast)) {
      return "fast";
    }
    if (Mask == C.Big || (C.Fast >= 0 && Mask == (C.Big & ~(1ULL << C.Fast)))) {
      return "big";
    }
    if (Mask == C.Little) {
      return "little";
    }
    return "all";
  }

  struct Snapshot {
    uint32_t TID;
    HANDLE Handle;
    uint64_t Time100ns;
    uint64_t CurrentMask;
  };

  void* PlacerThread(void* Arg) {
    auto* C = static_cast<Config*>(Arg);
    const uint32_t SelfTID = GetCurrentThreadId();

    // Self-check: move this thread to the little cores and see where it lands. Shows whether Windows CPU numbers reach
    // the little cores at all (they don't when the container restricts the process to the big cores).
    {
      KAFFINITY Mask = C->Little;
      const NTSTATUS Status = NtSetInformationThread(NtCurrentThread(), ThreadAffinityMaskClass, &Mask, sizeof(Mask));
      LARGE_INTEGER Delay {.QuadPart = -500'000}; // 50ms
      NtDelayExecution(FALSE, &Delay);
      StatsLogNote(fextl::fmt::format("pin: self-check set little {:#x} status={:#x}, now on cpu {}", C->Little,
                                      static_cast<uint32_t>(Status), NtGetCurrentProcessorNumber()));
    }

    fextl::unordered_map<uint32_t, Tracked> Threads;
    fextl::vector<Snapshot> Snap;
    fextl::vector<HANDLE> Handles;
    uint32_t Second {};
    uint32_t FastTID {};
    bool ReportedWalkFailure {};

    for (;;) {
      LARGE_INTEGER Interval {.QuadPart = -10'000'000}; // 1s
      NtDelayExecution(FALSE, &Interval);
      ++Second;

      Snap.clear();
      // NtGetNextThread continues from the previous handle, so all handles stay open until the end of this round.
      Handles.clear();
      HANDLE Next {};
      NTSTATUS WalkStatus;
      while ((WalkStatus = NtGetNextThread(NtCurrentProcess(), Handles.empty() ? nullptr : Handles.back(),
                                           THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, 0, 0, &Next)) ==
             STATUS_SUCCESS) {
        Handles.push_back(Next);
        ThreadBasicInfo Basic {};
        KERNEL_USER_TIMES Times {};
        if (NtQueryInformationThread(Next, ThreadBasicInformation, &Basic, sizeof(Basic), nullptr) != STATUS_SUCCESS ||
            NtQueryInformationThread(Next, ThreadTimes, &Times, sizeof(Times), nullptr) != STATUS_SUCCESS) {
          continue;
        }
        Snap.push_back({
          .TID = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Basic.ClientId.UniqueThread)),
          .Handle = Next,
          .Time100ns = static_cast<uint64_t>(Times.KernelTime.QuadPart + Times.UserTime.QuadPart),
          .CurrentMask = Basic.AffinityMask,
        });
      }

      if (Handles.empty() && !ReportedWalkFailure) {
        ReportedWalkFailure = true;
        StatsLogNote(fextl::fmt::format("pin: can't list the process's threads, NtGetNextThread status={:#x}", static_cast<uint32_t>(WalkStatus)));
      }

      // Update loads and names.
      for (auto& S : Snap) {
        auto [It, New] = Threads.try_emplace(S.TID);
        auto& T = It->second;
        if (New) {
          T.LastTime100ns = S.Time100ns;
        }
        // 100ns units over ~1s: 100'000 == 1% of a core.
        const double Now = static_cast<double>(S.Time100ns >= T.LastTime100ns ? S.Time100ns - T.LastTime100ns : 0) / 100'000.0;
        T.Load = New ? Now : 0.5 * T.Load + 0.5 * Now;
        T.LastTime100ns = S.Time100ns;
        T.SeenAt = Second;
        // A new thread has no load history yet, so it never counts as idle in its first round.
        if (New || T.Load >= WakeLoad) {
          T.IdleCount = 0;
        } else if (T.Load < IdleLoad) {
          ++T.IdleCount;
        }
        if (T.Name.empty()) {
          T.Name = QueryName(S.Handle);
        }
      }

      // The busiest set: the C->Heavy highest loads above MinHeavyLoad, FEX's own threads excluded.
      fextl::vector<std::pair<double, uint32_t>> Ranked;
      for (auto& S : Snap) {
        const auto& T = Threads[S.TID];
        if (S.TID != SelfTID && !IsFEXThread(T.Name) && T.Load >= MinHeavyLoad) {
          Ranked.push_back({T.Load, S.TID});
        }
      }
      std::sort(Ranked.begin(), Ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      if (Ranked.size() > C->Heavy) {
        Ranked.resize(C->Heavy);
      }
      // The fast core goes to the busiest thread, but stays with its current owner while that one is still busy and within
      // 20% of the top, so two similar threads don't swap it every second.
      if (!Ranked.empty()) {
        const auto Owner = std::find_if(Ranked.begin(), Ranked.end(), [&](const auto& R) { return R.second == FastTID; });
        if (Owner == Ranked.end() || Owner->first < 0.8 * Ranked.front().first) {
          FastTID = Ranked.front().second;
        }
      }
      for (auto& S : Snap) {
        auto& T = Threads[S.TID];
        const bool InTop = std::any_of(Ranked.begin(), Ranked.end(), [&](const auto& R) { return R.second == S.TID; });
        if (InTop) {
          T.Heavy = true;
          T.OutCount = 0;
        } else if (T.Heavy && ++T.OutCount >= DemoteAfterSeconds) {
          T.Heavy = false;
        }
      }

      // Apply.
      uint32_t Moves {}, Failed {}, Rest {};
      fextl::string HeavyList;
      for (auto& S : Snap) {
        auto& T = Threads[S.TID];
        uint64_t Target;
        if (S.TID == SelfTID || IsFEXThread(T.Name)) {
          Target = C->Little;
        } else if (C->Sticky) {
          if (T.IdleCount >= IdleAfterSeconds && !IsFrameThread(T.Name)) {
            Target = C->Little;
            ++Rest;
          } else {
            Target = C->Big;
            if (T.Load >= MinHeavyLoad) {
              HeavyList += fextl::fmt::format(" {}{}{}:{:.0f}%", S.TID, T.Name.empty() ? "" : "/", T.Name.substr(0, 16), T.Load);
            }
          }
        } else if (T.Heavy) {
          if (C->Fast >= 0 && S.TID == FastTID) {
            Target = 1ULL << C->Fast;
          } else if (C->Fast >= 0) {
            Target = C->Big & ~(1ULL << C->Fast);
          } else {
            Target = C->Big;
          }
          HeavyList += fextl::fmt::format(" {}{}{}:{:.0f}%", S.TID, T.Name.empty() ? "" : "/", T.Name.substr(0, 16), T.Load);
        } else {
          Target = C->Rest;
          ++Rest;
        }

        if (T.SetMask && S.CurrentMask != T.SetMask && !T.ReportedForeign) {
          T.ReportedForeign = true;
          StatsLogNote(fextl::fmt::format("pin: s={} tid={} {} affinity changed by someone else: {:#x} (we set {:#x})", Second, S.TID,
                                          T.Name, S.CurrentMask, T.SetMask));
        }

        if ((Target != T.SetMask || S.CurrentMask != Target) && Second >= T.RetryAt && T.TerminatingFailures < MaxTerminatingFailures) {
          KAFFINITY Mask = Target;
          const NTSTATUS Status = NtSetInformationThread(S.Handle, ThreadAffinityMaskClass, &Mask, sizeof(Mask));
          if (Status == STATUS_SUCCESS) {
            if (Target != T.SetMask) {
              ++Moves;
              StatsLogNote(fextl::fmt::format("pin: s={} tid={} {} load={:.0f}% -> {}", Second, S.TID, T.Name, T.Load, MaskKind(*C, Target)));
            }
            T.SetMask = Target;
            T.TerminatingFailures = 0;
          } else {
            T.TerminatingFailures = Status == STATUS_THREAD_IS_TERMINATING ? T.TerminatingFailures + 1 : 0;
            ++Failed;
            T.RetryAt = Second + RetryAfterSeconds;
            StatsLogNote(fextl::fmt::format("pin: s={} tid={} {} load={:.0f}% -> {} FAILED status={:#x}", Second, S.TID, T.Name, T.Load,
                                            MaskKind(*C, Target), static_cast<uint32_t>(Status)));
          }
        }
      }
      for (HANDLE H : Handles) {
        NtClose(H);
      }

      StatsLogNote(fextl::fmt::format("pin: s={} heavy={} rest={} moves={} failed={}", Second, HeavyList.empty() ? " -" : HeavyList, Rest,
                                      Moves, Failed));

      // Forget threads that are gone.
      for (auto It = Threads.begin(); It != Threads.end();) {
        It = It->second.SeenAt == Second ? std::next(It) : Threads.erase(It);
      }
    }
    return nullptr;
  }
} // namespace

void StartThreadPlacer(std::string_view AppName) {
  const char* Enabled = getenv("FEX_THREADPIN");
  if (!Enabled || Enabled[0] != '1') {
    return;
  }

  const char* App = getenv("FEX_PIN_APP");
  const bool Other = App && App[0] && !EqualsIgnoreCase(App, AppName);
  const char* Others = getenv("FEX_PIN_OTHERS");
  if (Other && Others && Others[0] == 'o') {
    return;
  }

  auto* C = new Config {};
  C->Other = Other;
  C->Big = EnvCPUList("FEX_PIN_BIG", 0xF8);
  C->Little = EnvCPUList("FEX_PIN_LITTLE", 0x07);
  C->Heavy = static_cast<uint32_t>(std::max(0, ParseInt(getenv("FEX_PIN_HEAVY"), 4)));
  const char* Rest = getenv("FEX_PIN_REST");
  const bool RestAll = Rest && Rest[0] == 'a';
  C->Rest = RestAll ? (C->Big | C->Little) : C->Little;
  C->RestName = RestAll ? "all" : "little";
  C->Fast = ParseInt(getenv("FEX_PIN_FAST"), -1);
  const char* Mode = getenv("FEX_PIN_MODE");
  C->Sticky = !(Mode && Mode[0] == 'l');
  if (C->Sticky) {
    C->Fast = -1;
    C->RestName = "little after 30s idle";
  }
  if (Other) {
    C->Sticky = false;
    C->Heavy = 0;
    C->Rest = C->Little;
    C->RestName = "little (not FEX_PIN_APP)";
    C->Fast = -1;
  }
  if (C->Fast > 63 || (C->Fast >= 0 && !(C->Big & (1ULL << C->Fast)))) {
    StatsLogNote(fextl::fmt::format("pin: FEX_PIN_FAST={} is not one of the big cores, ignoring it", C->Fast));
    C->Fast = -1;
  }

  const auto* Peb = reinterpret_cast<__TEB*>(NtCurrentTeb())->Peb;
  const char* Topology = getenv("WINE_CPU_TOPOLOGY");
  StatsLogNote(fextl::fmt::format("pin: on in {} mode={} heavy={} big={:#x} little={:#x} rest={} fast={} | cpus={} process_affinity={:#x} "
                                  "WINE_CPU_TOPOLOGY={}",
                                  AppName, C->Sticky ? "sticky" : "load", C->Heavy, C->Big, C->Little, C->RestName, C->Fast, Peb->NumberOfProcessors,
                                  static_cast<uint64_t>(Peb->ActiveProcessAffinityMask), Topology ? Topology : "-"));

  // Keep the thread object alive: its RunThread reads the object after creation, so it must outlive the thread.
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(PlacerThread, C, {.LowPriority = false, .Internal = true}, "FEXThreadPin");
}
} // namespace FEX::Windows
