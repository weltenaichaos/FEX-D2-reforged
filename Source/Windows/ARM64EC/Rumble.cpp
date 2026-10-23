// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Hands the game's controller rumble changes to a FEX thread so the game's threads never wait for them
$end_info$
*/

#include "Rumble.h"
#include "FrameLatency.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>

extern "C" {
// Module.S ExitFunctionEC: a call from x64 code into any of [0..2] (all 0 until xinput is found) runs FEXNativeRumbleSetState
// (through NativeRumbleTarget) instead, after storing the entry the game called in [3].
alignas(64) uint64_t FEXRumbleTargets[4];
DWORD FEXNativeRumbleSetState(DWORD Index, const void* Vibration);
}

namespace Rumble {
namespace {
  enum class Mode { Keep, Async, Off };

  constexpr DWORD ResultSuccess = 0;          // ERROR_SUCCESS
  constexpr DWORD ResultUnknown = 0xffffffff; // no call for this controller yet
  constexpr DWORD UserCount = 4;              // XUSER_MAX_COUNT

  struct Vibration {
    uint16_t Left;
    uint16_t Right;
  };
  using SetStateFn = DWORD (*)(DWORD, const Vibration*);

  struct Slot {
    std::atomic<uint32_t> Wanted {};             // Left << 16 | Right, as the game last asked
    std::atomic<uint32_t> Sent {};               // what Wine last got
    std::atomic<DWORD> Result {ResultUnknown};   // Wine's last answer for this controller
    uint64_t LastSend {};                        // counter value of the last change sent (FEXRumble thread only)
  };

  struct Counters {
    std::atomic<uint64_t> Calls;
    std::atomic<uint64_t> Changes;
    std::atomic<uint64_t> Direct;
    std::atomic<uint64_t> Sends;
    std::atomic<uint64_t> SendTicks;
    std::atomic<uint64_t> LongestSend;
    std::atomic<uint64_t> Failed;
  };

  Mode M {Mode::Keep};
  uint64_t IntervalTicks {};
  uint64_t CounterFreq {1};
  Slot Slots[UserCount];
  Counters Stats {};
  alignas(64) std::atomic<uint32_t> Generation {}; // bumped on every change; FEXRumble waits on it

  uint64_t GetCounter() {
    uint64_t Result {};
    __asm volatile("isb; mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Result));
    return Result;
  }

  uint64_t GetCounterFrequency() {
    uint64_t Result {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Result));
    return Result ? Result : 1;
  }

  SetStateFn Real() {
    return reinterpret_cast<SetStateFn>(__atomic_load_n(&FEXRumbleTargets[3], __ATOMIC_ACQUIRE));
  }

  Vibration Unpack(uint32_t Packed) {
    return {static_cast<uint16_t>(Packed >> 16), static_cast<uint16_t>(Packed)};
  }

  void Sleep100ns(int64_t Units) {
    LARGE_INTEGER Interval {.QuadPart = -std::max<int64_t>(Units, 1)};
    NtDelayExecution(FALSE, &Interval);
  }

  // Finds xinput's XInputSetState and arms ExitFunctionEC. D2R imports it from xinput9_1_0.dll, which Wine forwards to
  // xinput1_4.dll; a forwarded export has no code of its own and is skipped.
  bool Arm() {
    const wchar_t* Modules[] = {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
    const char* Names[] = {"xinput1_4", "xinput9_1_0", "xinput1_3"};
    for (size_t i = 0; i < std::size(Modules); ++i) {
      const uint64_t Base = FrameLatency::FindModule(Modules[i]);
      if (!Base) {
        continue;
      }
      uint64_t Entries[3] {};
      const size_t N = FrameLatency::FindExportEntries(Base, "XInputSetState", Entries);
      if (!N) {
        continue;
      }
      // [0] last: it switches the check on.
      __atomic_store_n(&FEXRumbleTargets[2], Entries[N > 2 ? 2 : 0], __ATOMIC_RELAXED);
      __atomic_store_n(&FEXRumbleTargets[1], Entries[N > 1 ? 1 : 0], __ATOMIC_RELAXED);
      __atomic_store_n(&FEXRumbleTargets[0], Entries[0], __ATOMIC_RELEASE);
      fextl::string Text;
      for (size_t j = 0; j < N; ++j) {
        Text += fextl::fmt::format(" {:X}", Entries[j]);
      }
      FEX::Windows::StatsLogNote(fextl::fmt::format("rumble: {}!XInputSetState found (entries{}); the game's calls now go through FEX", Names[i], Text));
      return true;
    }
    return false;
  }

  void* RumbleThread(void*) {
    // xinput loads with the game's imports, but this thread can start first.
    for (uint32_t Tries = 0; !Arm(); ++Tries) {
      if (Tries == 120) {
        FEX::Windows::StatsLogNote("rumble: no xinput DLL with XInputSetState loaded after 2 minutes; rumble left to the game");
        return nullptr;
      }
      Sleep100ns(10'000'000); // 1s
    }

    for (;;) {
      const uint32_t Seen = Generation.load(std::memory_order_acquire);
      uint64_t WaitTicks = ~0ull;
      for (DWORD i = 0; i < UserCount; ++i) {
        auto& S = Slots[i];
        if (S.Result.load(std::memory_order_acquire) != ResultSuccess) {
          continue; // the game's thread calls Wine itself until a call succeeded
        }
        const uint32_t Wanted = S.Wanted.load(std::memory_order_relaxed);
        if (Wanted == S.Sent.load(std::memory_order_relaxed)) {
          continue;
        }
        const uint64_t Now = GetCounter();
        if (S.LastSend && Now - S.LastSend < IntervalTicks) {
          WaitTicks = std::min(WaitTicks, IntervalTicks - (Now - S.LastSend));
          continue;
        }
        const auto Fn = Real();
        if (!Fn) {
          continue;
        }
        const Vibration V = Unpack(Wanted);
        const DWORD Result = Fn(i, &V);
        const uint64_t End = GetCounter();
        S.LastSend = End;
        S.Sent.store(Wanted, std::memory_order_relaxed);
        if (Result != ResultSuccess) {
          S.Result.store(Result, std::memory_order_release);
          Stats.Failed.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t Took = End - Now;
        Stats.Sends.fetch_add(1, std::memory_order_relaxed);
        Stats.SendTicks.fetch_add(Took, std::memory_order_relaxed);
        uint64_t Longest = Stats.LongestSend.load(std::memory_order_relaxed);
        while (Took > Longest && !Stats.LongestSend.compare_exchange_weak(Longest, Took, std::memory_order_relaxed)) {
        }
      }
      if (WaitTicks != ~0ull) {
        // A change came sooner than FEX_RUMBLE_MS after the last one: send the latest strength when the time is up.
        Sleep100ns(static_cast<int64_t>(WaitTicks * 10'000'000 / CounterFreq) + 1);
        continue;
      }
      LARGE_INTEGER Timeout {.QuadPart = -10'000'000}; // 1s, in case a wake is missed
      uint32_t Compare = Seen;
      RtlWaitOnAddress(&Generation, &Compare, sizeof(Compare), &Timeout);
    }
    return nullptr;
  }

  fextl::string StatsLine() {
    const uint64_t Calls = Stats.Calls.exchange(0, std::memory_order_relaxed);
    const uint64_t Changes = Stats.Changes.exchange(0, std::memory_order_relaxed);
    const uint64_t Direct = Stats.Direct.exchange(0, std::memory_order_relaxed);
    const uint64_t Sends = Stats.Sends.exchange(0, std::memory_order_relaxed);
    const uint64_t SendTicks = Stats.SendTicks.exchange(0, std::memory_order_relaxed);
    const uint64_t Longest = Stats.LongestSend.exchange(0, std::memory_order_relaxed);
    const uint64_t Failed = Stats.Failed.exchange(0, std::memory_order_relaxed);
    if (!Calls && !Sends) {
      return {};
    }
    const double MsPerTick = 1000.0 / static_cast<double>(CounterFreq);
    return fextl::fmt::format("  rumble calls={} changes={} sent={} send avg={:.1f}ms max={:.1f}ms direct={} failed={}\n", Calls, Changes,
                              Sends, Sends ? static_cast<double>(SendTicks) * MsPerTick / static_cast<double>(Sends) : 0.0,
                              static_cast<double>(Longest) * MsPerTick, Direct, Failed);
  }
} // namespace

void Init(bool StatsLog) {
  const char* Value = FEX::Windows::Switch("FEX_RUMBLE");
  if (!Value || !Value[0] || Value[0] == 'k' || Value[0] == '0') {
    return;
  }
  M = Value[0] == 'o' ? Mode::Off : Mode::Async;
  const char* Ms = FEX::Windows::Switch("FEX_RUMBLE_MS");
  const uint64_t IntervalMs = std::clamp<uint64_t>(Ms && Ms[0] >= '0' && Ms[0] <= '9' ? strtoull(Ms, nullptr, 10) : 50, 10, 1000);
  CounterFreq = GetCounterFrequency();
  IntervalTicks = IntervalMs * CounterFreq / 1000;
  FEX::Windows::StatsLogNote(
    M == Mode::Off ? fextl::string("rumble: off, the controller gets strength 0; the game's XInputSetState calls return at once (FEX_RUMBLE=keep "
                                   "leaves them to Wine)") :
                     fextl::fmt::format("rumble: the game's XInputSetState calls return at once; FEXRumble sends the latest strength to Wine, at "
                                        "most every {} ms per controller (FEX_RUMBLE=keep leaves them to Wine, FEX_RUMBLE_MS sets the time)",
                                        IntervalMs));
  if (StatsLog) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
  static fextl::unique_ptr<FEXCore::Threads::Thread> Thread;
  Thread = FEXCore::Threads::Thread::Create(RumbleThread, nullptr, {.LowPriority = false, .Internal = true}, "FEXRumble");
}
} // namespace Rumble

// The game's XInputSetState, on the game's thread (Module.S runs it through NativeRumbleThunk). Never waits for Wine once
// the controller answered a call: the strength goes to its slot and FEXRumble sends it.
DWORD FEXNativeRumbleSetState(DWORD Index, const void* VibrationPtr) {
  using namespace Rumble;
  Stats.Calls.fetch_add(1, std::memory_order_relaxed);
  const auto Fn = Real();
  if (!Fn) {
    return 1167; // ERROR_DEVICE_NOT_CONNECTED; can't happen, ExitFunctionEC stores the entry before coming here
  }
  if (Index >= UserCount || !VibrationPtr) {
    Stats.Direct.fetch_add(1, std::memory_order_relaxed);
    return Fn(Index, static_cast<const Vibration*>(VibrationPtr));
  }
  Vibration Asked {};
  memcpy(&Asked, VibrationPtr, sizeof(Asked));
  const uint32_t Packed = M == Mode::Off ? 0 : (static_cast<uint32_t>(Asked.Left) << 16 | Asked.Right);
  auto& S = Slots[Index];
  if (S.Result.load(std::memory_order_acquire) != ResultSuccess) {
    // No call for this controller has succeeded yet (first call, or none there): Wine answers on this thread, as without FEX.
    Stats.Direct.fetch_add(1, std::memory_order_relaxed);
    const Vibration V = Unpack(Packed);
    const DWORD Result = Fn(Index, &V);
    if (Result == ResultSuccess) {
      S.Wanted.store(Packed, std::memory_order_relaxed);
      S.Sent.store(Packed, std::memory_order_relaxed);
    }
    S.Result.store(Result, std::memory_order_release);
    return Result;
  }
  if (S.Wanted.exchange(Packed, std::memory_order_relaxed) != Packed) {
    Stats.Changes.fetch_add(1, std::memory_order_relaxed);
    Generation.fetch_add(1, std::memory_order_release);
    RtlWakeAddressSingle(&Generation);
  }
  return ResultSuccess;
}
