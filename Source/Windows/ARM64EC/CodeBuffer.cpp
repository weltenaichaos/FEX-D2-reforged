// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Starts FEX's translated-code buffer at full size and renews it at loading screens rather than letting it fill in play
$end_info$
*/

#include "CodeBuffer.h"
#include "LoadScreen.h"
#include "Windows/Common/InvalidationTracker.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdlib>
#include <utility>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>

extern "C" {
// NativeRedirect.cpp: [0] = frame starts counted so far (0 until the frame clock is found), then a ring of 1024
// CNTVCT_EL0 times.
extern uint64_t FEXFrameMarks[];
}

namespace CodeBufferPolicy {
namespace {
  constexpr size_t MarkRing = 1024; // NativeRedirect.cpp FrameMarkRing
  constexpr uint32_t TickMs = 50;
  // Frames that must have run since the last loading screen before another one is counted (so a load that shows a few
  // frames between its steps counts once).
  constexpr uint64_t PlayFrames = 30;
  // The first loading screens (start-up, menus, entering the game) renew at RenewEarlyPercent: what the buffer holds then
  // is mostly start-up and menu code the game won't run again, and the game's own code loads for the first time after
  // them anyway. In the build 83 run the buffer would have been ~25-45% used when the game was entered.
  constexpr uint64_t EarlyLoads = 3;

  FEXCore::Context::Context* Ctx {};
  bool Logging {};
  bool FrameMarksWanted {};
  uint32_t RenewPercent {};
  uint32_t RenewEarlyPercent {};
  uint32_t LoadMs {};
  // Renewals done (NoteStarted), so a loading screen asks for at most one.
  std::atomic<uint64_t> Renewals {};

  uint64_t Counter() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Result));
    return Result;
  }

  uint64_t CounterFrequency() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Result));
    return Result ? Result : 1;
  }

  // Code buffer starts, written under FEX's code locks by whichever thread started one (rarely two at once: the slot
  // index is taken atomically), read once a second by the log thread.
  struct Started {
    std::atomic<uint64_t> Seq; // slot number + 1 once written
    uint64_t At;
    FEXCore::HLE::SyscallHandler::CodeBufferStart Info;
  };
  constexpr size_t StartRing = 16;
  Started Starts[StartRing];
  std::atomic<uint64_t> StartCount {};
  uint64_t StartsRead {};

  // Loading screens seen, for the log.
  std::atomic<uint64_t> Loads {};
  std::atomic<uint64_t> LoadAt {};
  std::atomic<uint32_t> LoadPercent {};
  std::atomic<bool> LoadLight {}; // build 94: seen by GPU-light frames (LoadScreen.h), not a frame gap
  uint64_t LoadsRead {};

  const char* ReasonText(uint8_t Reason) {
    switch (Reason) {
    case 0: return "first buffer";
    case 1: return "the last one was full";
    case 2: return "renewed at a loading screen";
    case 3: return "start size changed before any code";
    default: return "?";
    }
  }

  // Every 10 s: how full the buffer is, what was placed in it, what was served by code already in it, and what was
  // dropped and why.
  constexpr uint64_t UseEverySeconds = 10;
  uint64_t UseLastAt {};
  FEXCore::Context::Context::CodeBufferUse UseLast {};
  FEX::Windows::InvalidationTracker::DropCounts DropsLast {};

  // Build 94: the main exe's range, to name where big placements' guest code is.
  uint64_t MainBase {};
  uint64_t MainSize {};

  fextl::string GuestPlace(uint64_t RIP) {
    if (!RIP) {
      return "?";
    }
    if (RIP - MainBase < MainSize) {
      return fextl::fmt::format("exe+{:X}", RIP - MainBase);
    }
    MEMORY_BASIC_INFORMATION Info {};
    if (NtQueryVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(RIP), MemoryBasicInformation, &Info, sizeof(Info), nullptr) ==
          STATUS_SUCCESS &&
        Info.AllocationBase) {
      const uint64_t Base = reinterpret_cast<uint64_t>(Info.AllocationBase);
      return fextl::fmt::format("{:X}+{:X}", Base, RIP - Base);
    }
    return fextl::fmt::format("{:X}", RIP);
  }

  // Build 94: placements of 64 KB or more since the last line, by guest address: the biggest share first. In calm play
  // of the build 93 run the buffer grew 2.9-6.8 MB in some 10 s windows that placed only 120-200 blocks (most ~0.5 KB).
  fextl::string LargeText(const FEXCore::Context::Context::CodeBufferUse& U) {
    constexpr double MB = 1024.0 * 1024.0;
    const uint64_t Count = U.Large - UseLast.Large;
    if (!Count) {
      return {};
    }
    struct Place {
      uint64_t RIP;
      uint64_t Bytes;
      uint64_t Largest;
      uint32_t Times;
      uint32_t GuestSize;
      bool Loaded;
    };
    Place Places[64];
    size_t Used {};
    uint64_t Lost {};
    for (uint64_t i = UseLast.Large; i < U.Large; ++i) {
      FEXCore::Context::Context::LargeCodePlacement L;
      if (U.Large - i > 64 || !Ctx->GetLargeCodePlacement(i, L)) {
        ++Lost;
        continue;
      }
      auto* P = std::find_if(Places, Places + Used, [&](const Place& Q) { return Q.RIP == L.GuestRIP; });
      if (P == Places + Used) {
        *P = {L.GuestRIP, 0, 0, 0, L.GuestSize, L.Loaded};
        ++Used;
      }
      P->Bytes += L.Size;
      P->Largest = std::max<uint64_t>(P->Largest, L.Size);
      ++P->Times;
      P->GuestSize = std::max(P->GuestSize, L.GuestSize);
      P->Loaded = P->Loaded && L.Loaded;
    }
    std::sort(Places, Places + Used, [](const Place& a, const Place& b) { return a.Bytes > b.Bytes; });
    fextl::string Out = fextl::fmt::format(" | of those, {} of 64 KB or more ({:.2f} MB)", Count,
                                           static_cast<double>(U.LargeBytes - UseLast.LargeBytes) / MB);
    for (size_t i = 0; i < std::min<size_t>(Used, 4); ++i) {
      const auto& P = Places[i];
      Out += fextl::fmt::format("{} {} {}x {:.0f} KB (largest {:.0f} KB, {}, guest {:.1f} KB)", i ? "," : ":", GuestPlace(P.RIP),
                                P.Times, static_cast<double>(P.Bytes) / 1024.0, static_cast<double>(P.Largest) / 1024.0,
                                P.Loaded ? "from the disk cache" : "translated", static_cast<double>(P.GuestSize) / 1024.0);
    }
    if (Used > 4) {
      Out += fextl::fmt::format(", {} more places", Used - 4);
    }
    if (Lost) {
      Out += fextl::fmt::format(", {} not read (ring passed)", Lost);
    }
    return Out;
  }

  fextl::string UseLine(uint64_t Now, uint64_t Freq) {
    using Tracker = FEX::Windows::InvalidationTracker;
    using R = Tracker::DropReason;
    const auto U = Ctx->GetCodeBufferUse();
    const auto D = Tracker::GetDropCounts();
    const double Seconds = UseLastAt ? static_cast<double>(Now - UseLastAt) / static_cast<double>(Freq) : 0.0;
    constexpr double MB = 1024.0 * 1024.0;

    fextl::string Out = fextl::fmt::format("# code buffer: #{} {:.0f}% used ({:.1f} of {:.0f} MB", U.Number,
                                           U.Size ? static_cast<double>(U.Used) * 100.0 / static_cast<double>(U.Size) : 0.0,
                                           static_cast<double>(U.Used) / MB, static_cast<double>(U.Size) / MB);
    if (UseLastAt && U.Number == UseLast.Number) {
      Out += fextl::fmt::format(", +{:.2f} MB", static_cast<double>(U.Used - std::min(U.Used, UseLast.Used)) / MB);
    }
    Out += fextl::fmt::format(") | in the last {:.0f} s: placed {} blocks ({} loaded from the disk cache {:.2f} MB, {} translated {:.2f} MB)",
                              Seconds, (U.Loaded - UseLast.Loaded) + (U.Translated - UseLast.Translated), U.Loaded - UseLast.Loaded,
                              static_cast<double>(U.LoadedBytes - UseLast.LoadedBytes) / MB, U.Translated - UseLast.Translated,
                              static_cast<double>(U.TranslatedBytes - UseLast.TranslatedBytes) / MB);
    Out += LargeText(U);
    Out += fextl::fmt::format(" | {} disk cache hits served by the copy already in the buffer ({:.2f} MB not copied)",
                              U.Reused - UseLast.Reused, static_cast<double>(U.ReusedBytes - UseLast.ReusedBytes) / MB);
    Out += fextl::fmt::format(", dropped {} blocks:", U.Dropped - UseLast.Dropped);

    static constexpr const char* ReasonNames[] = {"protection exec->exec", "protection exec removed", "protection made exec",
                                                  "write faults", "frees", "unmaps", "icache flushes", "dirty", "file reads", "other"};
    static_assert(std::size(ReasonNames) == size_t(R::Count));
    static constexpr const char* ProtNames[] = {"NOACCESS", "R", "RW", "WC", "X", "RX", "RWX", "XWC"};
    bool Any = false;
    for (size_t i = 0; i < size_t(R::Count); ++i) {
      const uint64_t Calls = D.Calls[i] - DropsLast.Calls[i];
      if (!Calls) {
        continue;
      }
      Out += fextl::fmt::format("{} {} {}x {} pages {} blocks", Any ? "," : "", ReasonNames[i], Calls, D.Pages[i] - DropsLast.Pages[i],
                                D.Blocks[i] - DropsLast.Blocks[i]);
      if (i == size_t(R::ProtectExecToExec) || i == size_t(R::ProtectExecRemoved)) {
        const size_t K = i == size_t(R::ProtectExecToExec) ? 0 : 1;
        fextl::string To;
        for (size_t b = 0; b < 8; ++b) {
          if (const uint64_t N = D.ProtBits[K][b] - DropsLast.ProtBits[K][b]) {
            To += fextl::fmt::format("{}{} {}", To.empty() ? "" : " ", ProtNames[b], N);
          }
        }
        if (!To.empty()) {
          Out += fextl::fmt::format(" (those that dropped code, by new protection: {})", To);
        }
      }
      Any = true;
    }
    if (!Any) {
      Out += " none";
    }
    Out += "\n";

    UseLastAt = Now;
    UseLast = U;
    DropsLast = D;
    return Out;
  }

  fextl::string Lines() {
    fextl::string Out;
    const uint64_t Now = Counter();
    const uint64_t Freq = CounterFrequency();
    const auto MsAgo = [&](uint64_t At) {
      return At && Now > At ? static_cast<double>(Now - At) * 1000.0 / static_cast<double>(Freq) : 0.0;
    };

    const uint64_t LoadCount = Loads.load(std::memory_order_acquire);
    if (LoadCount != LoadsRead) {
      const bool Light = LoadLight.load(std::memory_order_relaxed);
      Out += fextl::fmt::format("# code buffer: loading screen #{} ({}, {:.0f} ms before this line), renewal asked if the buffer is "
                                "{}% used or more\n",
                                LoadCount,
                                Light ? fextl::string("GPU-light frames") : fextl::fmt::format("no frame for {} ms or more", LoadMs),
                                MsAgo(LoadAt.load(std::memory_order_relaxed)), LoadPercent.load(std::memory_order_relaxed));
      LoadsRead = LoadCount;
    }

    const uint64_t Count = StartCount.load(std::memory_order_acquire);
    if (Count - StartsRead > StartRing) {
      StartsRead = Count - StartRing;
    }
    for (; StartsRead < Count; ++StartsRead) {
      const auto& S = Starts[StartsRead % StartRing];
      if (S.Seq.load(std::memory_order_acquire) != StartsRead + 1) {
        break; // still being written; next second
      }
      const auto& I = S.Info;
      constexpr double MB = 1024.0 * 1024.0;
      if (I.PrevSize) {
        const uint32_t Translated = I.PrevBlocks > I.PrevCachedBlocks ? I.PrevBlocks - I.PrevCachedBlocks : 0;
        Out += fextl::fmt::format("# code buffer: #{} started ({}), {:.0f} MB, {:.0f} ms before this line; the old one ({:.0f} MB) "
                                  "was {:.0f}% used by {} blocks ({} loaded from the disk cache, {} translated, {:.2f} KB a block), "
                                  "all dropped: every thread loads or translates its code again\n",
                                  I.Number, ReasonText(I.Reason), static_cast<double>(I.Size) / MB, MsAgo(S.At),
                                  static_cast<double>(I.PrevSize) / MB, static_cast<double>(I.PrevUsed) * 100.0 / static_cast<double>(I.PrevSize),
                                  I.PrevBlocks, I.PrevCachedBlocks, Translated,
                                  I.PrevBlocks ? static_cast<double>(I.PrevUsed) / 1024.0 / I.PrevBlocks : 0.0);
      } else {
        Out += fextl::fmt::format("# code buffer: #{} started ({}), {:.0f} MB\n", I.Number, ReasonText(I.Reason),
                                  static_cast<double>(I.Size) / MB);
      }
    }

    if (!UseLastAt) {
      UseLine(Now, Freq); // the baseline for the first line
    } else if (Now - UseLastAt >= UseEverySeconds * Freq - Freq / 4) {
      Out += UseLine(Now, Freq);
    }
    return Out;
  }

  // Watches the frame clock: no new frame for LoadMs after at least PlayFrames frames is a loading screen, and (build 94)
  // so are frames that ask the GPU for little (LoadScreen::LightNow: D2R draws most of a loading screen at full speed).
  // Until it ends or a renewal was done, a renewal is asked for every tick (each valid for two ticks); the first code allocation takes
  // it, and starts a fresh buffer if the current one is used to the load's mark or more (RenewEarlyPercent for the first
  // EarlyLoads, RenewPercent after). Asking again later in the same load lets a load that fills the buffer past the mark
  // renew it still inside the load.
  void* WatchThread(void*) {
    const uint64_t Freq = CounterFrequency();
    const uint64_t LoadTicks = static_cast<uint64_t>(LoadMs) * Freq / 1000;
    uint64_t FramesAtLoad {};
    uint64_t LoadCount {};
    uint64_t RenewalsAtLoad {};
    uint32_t Percent {};
    bool InLoad {};

    for (;;) {
      LARGE_INTEGER Interval {.QuadPart = -static_cast<LONGLONG>(TickMs) * 10'000};
      NtDelayExecution(FALSE, &Interval);

      const uint64_t Frames = __atomic_load_n(&FEXFrameMarks[0], __ATOMIC_ACQUIRE);
      if (!Frames) {
        continue; // frame clock not found yet
      }
      const uint64_t LastFrame = __atomic_load_n(&FEXFrameMarks[1 + ((Frames - 1) & (MarkRing - 1))], __ATOMIC_RELAXED);
      const uint64_t Now = Counter();
      const bool NoFrame = Now > LastFrame && Now - LastFrame >= LoadTicks;
      const bool Light = LoadScreen::LightNow();
      const bool Gap = NoFrame || Light;

      if (InLoad) {
        if (!Gap) {
          InLoad = false;
          FramesAtLoad = Frames;
          continue;
        }
        if (Renewals.load(std::memory_order_relaxed) == RenewalsAtLoad) {
          Ctx->RequestCodeBufferRenewal(Percent, 2 * TickMs);
        }
        continue;
      }
      if (Gap && Frames - FramesAtLoad >= PlayFrames) {
        InLoad = true;
        ++LoadCount;
        Percent = LoadCount <= EarlyLoads ? std::min(RenewEarlyPercent, RenewPercent) : RenewPercent;
        RenewalsAtLoad = Renewals.load(std::memory_order_relaxed);
        Ctx->RequestCodeBufferRenewal(Percent, 2 * TickMs);
        LoadAt.store(NoFrame ? LastFrame + LoadTicks : Now, std::memory_order_relaxed);
        LoadLight.store(!NoFrame, std::memory_order_relaxed);
        LoadPercent.store(Percent, std::memory_order_relaxed);
        Loads.fetch_add(1, std::memory_order_release);
      }
    }
    return nullptr;
  }
} // namespace

void Init(FEXCore::Context::Context* CTX, bool StatsLog) {
  Ctx = CTX;
  Logging = StatsLog;

  const char* StartSwitch = FEX::Windows::Switch("FEX_CODE_BUFFER_START");
  const bool StartMax = StartSwitch && StartSwitch[0] == 'm';
  if (StartMax) {
    CTX->UseMaximalCodeBuffer();
  }

  const char* RenewSwitch = FEX::Windows::Switch("FEX_CODE_BUFFER_RENEW");
  RenewPercent = RenewSwitch && RenewSwitch[0] >= '0' && RenewSwitch[0] <= '9' ? std::min<uint32_t>(strtoul(RenewSwitch, nullptr, 10), 100) : 0;
  const char* EarlySwitch = FEX::Windows::Switch("FEX_CODE_BUFFER_RENEW_EARLY");
  RenewEarlyPercent = EarlySwitch && EarlySwitch[0] >= '0' && EarlySwitch[0] <= '9' ?
                        std::min<uint32_t>(strtoul(EarlySwitch, nullptr, 10), 100) :
                        RenewPercent;
  // FEX_CODE_REUSE=1 (the D2R default): disk cache code that comes back the same is mapped again from the copy already in
  // the buffer instead of placing another.
  const char* ReuseSwitch = FEX::Windows::Switch("FEX_CODE_REUSE");
  const bool Reuse = ReuseSwitch && ReuseSwitch[0] == '1';
  CTX->SetPlacedCodeReuse(Reuse);

  const char* LoadSwitch = FEX::Windows::Switch("FEX_CODE_BUFFER_LOAD_MS");
  LoadMs = LoadSwitch && LoadSwitch[0] >= '0' && LoadSwitch[0] <= '9' ? std::clamp<uint32_t>(strtoul(LoadSwitch, nullptr, 10), 100, 5000) : 250;

  if (StatsLog) {
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "code buffer: {}; {}; {} (FEX_CODE_BUFFER_START=max|grow, FEX_CODE_BUFFER_RENEW=N|off, FEX_CODE_BUFFER_RENEW_EARLY=N, "
      "FEX_CODE_BUFFER_LOAD_MS={}, FEX_CODE_REUSE=1|0)",
      StartMax ? "starts at 128 MB" : "starts at 16 MB and doubles up to 128 MB (upstream)",
      RenewPercent ? fextl::fmt::format("renewed at a loading screen when {}% used or more ({}% at the first {})", RenewPercent,
                                        std::min(RenewEarlyPercent, RenewPercent), EarlyLoads) :
                     fextl::string("never renewed early"),
      Reuse ? "disk cache code that comes back the same after its blocks were dropped is mapped again from the copy already in the "
              "buffer" :
              "dropped code is always placed again (upstream)",
      LoadMs));
    FEX::Windows::StatsLogSetLineProvider(Lines);
  }

  if (RenewPercent) {
    FrameMarksWanted = true;
    static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
    Thread = FEXCore::Threads::Thread::Create(WatchThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXCodeBuffer");
  }
}

bool WantsFrameMarks() {
  return FrameMarksWanted;
}

void SetMainImage(uint64_t Base, uint64_t Size) {
  MainBase = Base;
  MainSize = Size;
}

void NoteStarted(const FEXCore::HLE::SyscallHandler::CodeBufferStart& Info) {
  if (Info.Reason == 2) {
    Renewals.fetch_add(1, std::memory_order_relaxed);
  }
  if (!Logging) {
    return;
  }
  // Claim a slot; a racing start takes the next one. The reader skips a slot until its Seq says it was written.
  const uint64_t Slot = StartCount.fetch_add(1, std::memory_order_acq_rel);
  auto& S = Starts[Slot % StartRing];
  S.Seq.store(0, std::memory_order_relaxed);
  S.At = Counter();
  S.Info = Info;
  S.Seq.store(Slot + 1, std::memory_order_release);
}
} // namespace CodeBufferPolicy
