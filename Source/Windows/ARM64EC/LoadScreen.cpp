// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Tells loading screens that still draw frames from play by how little the GPU is asked to draw in each frame
$end_info$
*/

#include "LoadScreen.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"
#include "Windows/Common/TurnipChannel.h"

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

namespace LoadScreen {
namespace {
  constexpr uint32_t TickMs = 50;
  constexpr uint64_t Open = ~0ULL;

  uint32_t PerFrame {}; // FEX_LOAD_LIGHT
  uint32_t LightMs {};  // FEX_LOAD_LIGHT_MS
  FEX::Windows::Turnip::Channel* Chan {};
  bool Running {};

  std::atomic<bool> Light {};
  // Frames up to this FEXFrameMarks[0] value are decided: FrameLight knows whether they were in a stretch.
  std::atomic<uint64_t> Decided {};
  // GPU-light stretches as FEXFrameMarks[0] values: frames after From up to To (Open while it lasts).
  struct Stretch {
    std::atomic<uint64_t> From;
    std::atomic<uint64_t> To;
  };
  constexpr size_t StretchRing = 16;
  Stretch Stretches[StretchRing];
  std::atomic<uint64_t> StretchCount {};

  // For the log: each stretch when it was seen and when it ended, written by the watch thread, read by the log thread.
  struct Event {
    std::atomic<uint64_t> Seq; // slot number + 1 once written
    bool End;
    uint64_t At;     // counter when the stretch began (seen) or ended (end)
    uint64_t Ticks;  // how long it had lasted
    uint64_t Frames; // frames in it so far
    uint64_t Pushes; // push descriptor updates in it so far
  };
  constexpr size_t EventRing = 16;
  Event Events[EventRing];
  std::atomic<uint64_t> EventCount {};
  uint64_t EventsRead {};

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

  bool Attached() {
    return (Chan->Status & FEX::Windows::Turnip::StatusAttached) != 0;
  }

  uint64_t Pushes() {
    return Chan->PushTotal[0] + Chan->PushTotal[1];
  }

  void AddEvent(bool End, uint64_t At, uint64_t Ticks, uint64_t Frames, uint64_t Pushes) {
    const uint64_t Slot = EventCount.load(std::memory_order_relaxed); // only the watch thread writes
    auto& E = Events[Slot % EventRing];
    E.Seq.store(0, std::memory_order_relaxed);
    E.End = End;
    E.At = At;
    E.Ticks = Ticks;
    E.Frames = Frames;
    E.Pushes = Pushes;
    E.Seq.store(Slot + 1, std::memory_order_release);
    EventCount.store(Slot + 1, std::memory_order_release);
  }

  fextl::string Lines() {
    fextl::string Out;
    const uint64_t Count = EventCount.load(std::memory_order_acquire);
    if (Count - EventsRead > EventRing) {
      EventsRead = Count - EventRing;
    }
    const uint64_t Now = Counter();
    const double Freq = static_cast<double>(CounterFrequency());
    for (; EventsRead < Count; ++EventsRead) {
      const auto& E = Events[EventsRead % EventRing];
      if (E.Seq.load(std::memory_order_acquire) != EventsRead + 1) {
        break;
      }
      const double Ago = Now > E.At ? static_cast<double>(Now - E.At) * 1000.0 / Freq : 0.0;
      const double PerFrameSeen = E.Frames ? static_cast<double>(E.Pushes) / static_cast<double>(E.Frames) : 0.0;
      if (!E.End) {
        Out += fextl::fmt::format("# load: GPU-light frames since {:.0f} ms before this line, a loading screen: {} frames in {:.2f} s "
                                  "with {:.1f} push descriptor updates each (below {}; play has 190 or more)\n",
                                  Ago, E.Frames, static_cast<double>(E.Ticks) / Freq, PerFrameSeen, PerFrame);
      } else {
        Out += fextl::fmt::format("# load: GPU-light stretch ended {:.0f} ms before this line, after {:.1f} s and {} frames ({:.1f} "
                                  "push descriptor updates each)\n",
                                  Ago, static_cast<double>(E.Ticks) / Freq, E.Frames, PerFrameSeen);
      }
    }
    return Out;
  }

  // Every TickMs: the frames and push descriptor updates since the last look. A look with frames and fewer than PerFrame
  // updates each is light, one with frames and more ends a run of light looks, one without frames changes nothing (a
  // frame gap is the frame gap rule's). A run that lasts LightMs is a stretch, from the frame it began with.
  void* WatchThread(void*) {
    const uint64_t Freq = CounterFrequency();
    const uint64_t LightTicks = static_cast<uint64_t>(LightMs) * Freq / 1000;
    uint64_t PrevFrames = __atomic_load_n(&FEXFrameMarks[0], __ATOMIC_ACQUIRE);
    uint64_t PrevPushes = Pushes();
    uint64_t PrevAt = Counter();
    bool InRun {};
    bool Seen {};
    uint64_t RunFrames {};
    uint64_t RunPushes {};
    uint64_t RunAt {};

    for (;;) {
      LARGE_INTEGER Interval {.QuadPart = -static_cast<LONGLONG>(TickMs) * 10'000};
      NtDelayExecution(FALSE, &Interval);

      const uint64_t Frames = __atomic_load_n(&FEXFrameMarks[0], __ATOMIC_ACQUIRE);
      const uint64_t PushCount = Pushes();
      const uint64_t Now = Counter();
      const uint64_t NewFrames = Frames - PrevFrames;
      const uint64_t NewPushes = PushCount - PrevPushes;

      if (!Attached() || !Frames) {
        // Not FEX's Turnip build (yet), or no frame clock yet: nothing is light.
        Decided.store(Frames, std::memory_order_release);
      } else if (NewFrames) {
        if (NewPushes < static_cast<uint64_t>(PerFrame) * NewFrames) {
          if (!InRun) {
            InRun = true;
            RunFrames = PrevFrames;
            RunPushes = PrevPushes;
            RunAt = PrevAt;
          }
          if (!Seen && Now - RunAt >= LightTicks) {
            Seen = true;
            const uint64_t Slot = StretchCount.load(std::memory_order_relaxed);
            auto& S = Stretches[Slot % StretchRing];
            S.To.store(Open, std::memory_order_relaxed);
            S.From.store(RunFrames, std::memory_order_relaxed);
            StretchCount.store(Slot + 1, std::memory_order_release);
            Light.store(true, std::memory_order_release);
            AddEvent(false, RunAt, Now - RunAt, Frames - RunFrames, PushCount - RunPushes);
          }
          if (Seen) {
            Decided.store(Frames, std::memory_order_release);
          }
          // Else frames after RunFrames stay undecided until the run is a stretch or ends.
        } else {
          if (Seen) {
            const uint64_t Slot = StretchCount.load(std::memory_order_relaxed) - 1;
            Stretches[Slot % StretchRing].To.store(PrevFrames, std::memory_order_release);
            Light.store(false, std::memory_order_release);
            AddEvent(true, PrevAt, PrevAt - RunAt, PrevFrames - RunFrames, PrevPushes - RunPushes);
          }
          InRun = false;
          Seen = false;
          Decided.store(Frames, std::memory_order_release);
        }
      } else if (!InRun) {
        Decided.store(Frames, std::memory_order_release);
      }

      PrevFrames = Frames;
      PrevPushes = PushCount;
      PrevAt = Now;
    }
    return nullptr;
  }
} // namespace

void Init(bool StatsLog, bool Wanted) {
  const char* LightSwitch = FEX::Windows::Switch("FEX_LOAD_LIGHT");
  PerFrame = LightSwitch && LightSwitch[0] >= '0' && LightSwitch[0] <= '9' ? std::min<uint32_t>(strtoul(LightSwitch, nullptr, 10), 1'000'000) : 0;
  const char* MsSwitch = FEX::Windows::Switch("FEX_LOAD_LIGHT_MS");
  LightMs = MsSwitch && MsSwitch[0] >= '0' && MsSwitch[0] <= '9' ? std::clamp<uint32_t>(strtoul(MsSwitch, nullptr, 10), 100, 5000) : 500;
  Chan = FEX::Windows::Turnip::GetChannel();

  if (StatsLog && Wanted) {
    FEX::Windows::StatsLogNote(
      !PerFrame ? fextl::string("load: a loading screen is no new frame for 250 ms (FEX_LOAD_LIGHT=0; FEX_LOAD_LIGHT=64 also counts "
                                "frames that ask the GPU for little)") :
      !Chan ?
                  fextl::string("load: a loading screen is no new frame for 250 ms only: there is no FEX link to Turnip (FEX_TU_CHANNEL), "
                                "so nothing counts the GPU work of a frame") :
                  fextl::fmt::format("load: a loading screen is no new frame for 250 ms, or frames with fewer than {} push descriptor "
                                     "updates each (GPU-light: loading screens 7-9, menus 33-37, play 190 or more) for {} ms or more, "
                                     "as counted by FEX's Turnip build (FEX_LOAD_LIGHT=N|0, FEX_LOAD_LIGHT_MS=N); for the code buffer "
                                     "renewal and the learned game data and unpack cache lists",
                                     PerFrame, LightMs));
  }
  if (!Wanted || !PerFrame || !Chan) {
    return;
  }
  Running = true;
  if (StatsLog) {
    FEX::Windows::StatsLogSetLineProvider(Lines);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(WatchThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXLoadScreen");
}

bool LightNow() {
  return Light.load(std::memory_order_acquire);
}

int FrameLight(uint64_t Frame) {
  if (!Running) {
    return 0;
  }
  if (Frame > Decided.load(std::memory_order_acquire)) {
    return -1;
  }
  const uint64_t Count = StretchCount.load(std::memory_order_acquire);
  for (uint64_t i = Count; i > 0 && Count - i < StretchRing; --i) {
    const auto& S = Stretches[(i - 1) % StretchRing];
    const uint64_t From = S.From.load(std::memory_order_relaxed);
    const uint64_t To = S.To.load(std::memory_order_acquire);
    if (Frame > From && Frame <= To) {
      return 1;
    }
    if (Frame > To) {
      return 0; // after this stretch, and the ones before it are older
    }
  }
  return 0;
}
} // namespace LoadScreen
