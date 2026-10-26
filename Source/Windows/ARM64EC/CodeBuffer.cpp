// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Starts FEX's translated-code buffer at full size and renews it at loading screens rather than letting it fill in play
$end_info$
*/

#include "CodeBuffer.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
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

  FEXCore::Context::Context* Ctx {};
  bool Logging {};
  bool FrameMarksWanted {};
  uint32_t RenewPercent {};
  uint32_t LoadMs {};

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

  fextl::string Lines() {
    fextl::string Out;
    const uint64_t Now = Counter();
    const uint64_t Freq = CounterFrequency();
    const auto MsAgo = [&](uint64_t At) {
      return At && Now > At ? static_cast<double>(Now - At) * 1000.0 / static_cast<double>(Freq) : 0.0;
    };

    const uint64_t LoadCount = Loads.load(std::memory_order_acquire);
    if (LoadCount != LoadsRead) {
      Out += fextl::fmt::format("# code buffer: loading screen (no frame for {} ms or more, {:.0f} ms before this line), "
                                "renewal asked if the buffer is {}% used or more\n",
                                LoadMs, MsAgo(LoadAt.load(std::memory_order_relaxed)), RenewPercent);
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
    return Out;
  }

  // Watches the frame clock: no new frame for LoadMs after at least PlayFrames frames is a loading screen. While it lasts,
  // a renewal is asked for every tick (each valid for two ticks); the first code allocation takes it, and starts a fresh
  // buffer if the current one is RenewPercent used or more. Asking again later in the same load lets a load that fills
  // the buffer past that mark renew it still inside the load.
  void* WatchThread(void*) {
    const uint64_t Freq = CounterFrequency();
    const uint64_t LoadTicks = static_cast<uint64_t>(LoadMs) * Freq / 1000;
    uint64_t FramesAtLoad {};
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
      const bool Gap = Now > LastFrame && Now - LastFrame >= LoadTicks;

      if (InLoad) {
        if (!Gap) {
          InLoad = false;
          FramesAtLoad = Frames;
          continue;
        }
        Ctx->RequestCodeBufferRenewal(RenewPercent, 2 * TickMs);
        continue;
      }
      if (Gap && Frames - FramesAtLoad >= PlayFrames) {
        InLoad = true;
        Ctx->RequestCodeBufferRenewal(RenewPercent, 2 * TickMs);
        LoadAt.store(LastFrame + LoadTicks, std::memory_order_relaxed);
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
  const char* LoadSwitch = FEX::Windows::Switch("FEX_CODE_BUFFER_LOAD_MS");
  LoadMs = LoadSwitch && LoadSwitch[0] >= '0' && LoadSwitch[0] <= '9' ? std::clamp<uint32_t>(strtoul(LoadSwitch, nullptr, 10), 100, 5000) : 250;

  if (StatsLog) {
    FEX::Windows::StatsLogNote(fextl::fmt::format(
      "code buffer: {}; {} (FEX_CODE_BUFFER_START=max|grow, FEX_CODE_BUFFER_RENEW=N|off, FEX_CODE_BUFFER_LOAD_MS={})",
      StartMax ? "starts at 128 MB" : "starts at 16 MB and doubles up to 128 MB (upstream)",
      RenewPercent ? fextl::fmt::format("renewed at a loading screen when {}% used or more", RenewPercent) :
                     fextl::string("never renewed early"),
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

void NoteStarted(const FEXCore::HLE::SyscallHandler::CodeBufferStart& Info) {
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
