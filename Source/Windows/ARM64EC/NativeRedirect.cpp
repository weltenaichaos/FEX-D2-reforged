// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Runs native ARM64EC builds of hot guest functions in place of their emulated x86 code
$end_info$
*/

// Diablo II: Resurrected statically links zlib and inflates game data on its worker threads in every fight; emulated,
// inflate_fast and adler32_z take a large share of those threads' time. With FEX_NATIVE_ZLIB=1 the JIT hands out a stub
// for those functions' entry points instead of compiling them (SyscallHandler::RedirectBlock). The stub (Module.S) leaves
// the JIT exactly like a call from JIT code to an ARM64EC function, and the zlib 1.3.1 code in NativeZlib.c then works
// on the game's own z_stream and inflate_state, whose layout is the same.
//
// A function is found by its offset in the main executable and the first bytes of its code, which are compared when the
// JIT first asks for that address. Anything that doesn't match stays emulated.

#include "NativeRedirect.h"
#include "Windows/Common/StatsLog.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>

extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);

extern "C" {
// Read by the stubs in Module.S, which store the call-ret stack pointer the way a JIT exit does.
uint64_t NativeRedirectCallRetSPOffset = offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp);

// Module.S
extern void* NativeInflateFastRedirect;
extern void* NativeAdler32ZRedirect;

// NativeZlib.c
struct FEXNativeCallStats {
  uint64_t Calls;
  uint64_t Ticks;
  uint64_t Bytes;
};
extern FEXNativeCallStats FEXNativeInflateFastStats;
extern FEXNativeCallStats FEXNativeAdler32ZStats;
}

namespace NativeRedirect {
namespace {
  // The first 128 bytes of each function in D2R.exe (base 0x140000000), from FEX_STATSLOG_PROFILE logs.
  constexpr uint8_t InflateFastSignature[] = {
    0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x38, 0x4c,
    0x8b, 0x69, 0x28, 0xbd, 0x01, 0x00, 0x00, 0x00, 0x8b, 0x41, 0x08, 0x4c, 0x8b, 0x41, 0x10, 0x83, 0xe8, 0x05, 0x48, 0x8b, 0x31, 0x8b,
    0x49, 0x18, 0x48, 0x03, 0xc6, 0x4d, 0x8b, 0x4d, 0x68, 0x2b, 0xd1, 0x45, 0x8b, 0x75, 0x3c, 0x4d, 0x8b, 0x7d, 0x40, 0x41, 0x8b, 0x5d,
    0x48, 0x8d, 0xb9, 0xff, 0xfe, 0xff, 0xff, 0x41, 0x8b, 0x4d, 0x70, 0x49, 0x03, 0xf8, 0x45, 0x8b, 0x5d, 0x4c, 0x4d, 0x8b, 0x65, 0x60,
    0x48, 0x89, 0x44, 0x24, 0x18, 0x8b, 0xc2, 0x49, 0x8b, 0xd0, 0x48, 0x2b, 0xd0, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x41, 0x8b, 0x45, 0x34,
    0x89, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x41, 0x8b, 0x45, 0x38, 0x48, 0x89, 0x54, 0x24, 0x20, 0x8b, 0xd5,
  };

  constexpr uint8_t Adler32ZSignature[] = {
    0x44, 0x8b, 0xc9, 0x4c, 0x8b, 0xd2, 0x41, 0xc1, 0xe9, 0x10, 0x0f, 0xb7, 0xc9, 0x49, 0x83, 0xf8, 0x01, 0x75, 0x31, 0x0f, 0xb6, 0x12,
    0x03, 0xd1, 0x81, 0xfa, 0xf1, 0xff, 0x00, 0x00, 0x44, 0x8d, 0x82, 0x0f, 0x00, 0xff, 0xff, 0x44, 0x0f, 0x42, 0xc2, 0x43, 0x8d, 0x04,
    0x08, 0x3d, 0xf1, 0xff, 0x00, 0x00, 0x8d, 0x88, 0x0f, 0x00, 0xff, 0xff, 0x0f, 0x42, 0xc8, 0xc1, 0xe1, 0x10, 0x41, 0x0b, 0xc8, 0x8b,
    0xc1, 0xc3, 0x48, 0x85, 0xd2, 0x75, 0x04, 0x8d, 0x42, 0x01, 0xc3, 0x49, 0x83, 0xf8, 0x10, 0x73, 0x49, 0x4d, 0x85, 0xc0, 0x74, 0x1b,
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x0f, 0xb6, 0x02, 0x4d, 0x8d, 0x52, 0x01, 0x03, 0xc8, 0x44, 0x03, 0xc9, 0x49,
    0x83, 0xe8, 0x01, 0x75, 0xed, 0x81, 0xf9, 0xf1, 0xff, 0x00, 0x00, 0x44, 0x8d, 0x81, 0x0f, 0x00, 0xff, 0xff,
  };

  constexpr size_t MaxSignatureSize = 128;
  static_assert(sizeof(InflateFastSignature) <= MaxSignatureSize && sizeof(Adler32ZSignature) <= MaxSignatureSize);

  // A target whose bytes keep differing this many times is given up on. Unreadable code (not yet decrypted by the game's
  // protector) is not counted: the JIT can't run it either, so it will be asked for again once it is readable.
  constexpr uint32_t MaxMismatches = 16;

  enum class TargetState : uint32_t {
    Unchecked,
    Armed,
    Rejected,
  };

  struct Target {
    const char* Name;
    uint32_t RVA;
    const uint8_t* Signature;
    size_t SignatureSize;
    void* Stub;
    FEXNativeCallStats* Stats;

    uint64_t GuestRIP;
    std::atomic<TargetState> State;
    std::atomic<uint32_t> Mismatches;
    FEXNativeCallStats Prev; // Log thread only
  };

  Target Targets[] = {
    {"inflate_fast", 0xBD2090, InflateFastSignature, sizeof(InflateFastSignature), &NativeInflateFastRedirect, &FEXNativeInflateFastStats},
    {"adler32_z", 0xBD17F0, Adler32ZSignature, sizeof(Adler32ZSignature), &NativeAdler32ZRedirect, &FEXNativeAdler32ZStats},
  };

  bool Enabled {};
  uint64_t TickFrequency {1};

  fextl::string Hex(const uint8_t* Bytes, size_t Size) {
    fextl::string Out;
    for (size_t i = 0; i < Size; ++i) {
      Out += fextl::fmt::format("{:02x}", Bytes[i]);
    }
    return Out;
  }

  uintptr_t Check(Target& T) {
    uint8_t Code[MaxSignatureSize];
    SIZE_T Read {};
    if (NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(T.GuestRIP), Code, T.SignatureSize, &Read) != STATUS_SUCCESS ||
        Read != T.SignatureSize) {
      return 0;
    }

    if (memcmp(Code, T.Signature, T.SignatureSize) != 0) {
      const uint32_t Count = T.Mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
      if (Count == 1 || Count == MaxMismatches) {
        FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: {} at {:X} has different code ({}/{}){}: {}", T.Name, T.GuestRIP, Count,
                                                      MaxMismatches, Count == MaxMismatches ? ", stays emulated" : "",
                                                      Hex(Code, T.SignatureSize)));
      }
      if (Count >= MaxMismatches) {
        T.State.store(TargetState::Rejected, std::memory_order_release);
      }
      return 0;
    }

    auto Expected = TargetState::Unchecked;
    if (T.State.compare_exchange_strong(Expected, TargetState::Armed, std::memory_order_acq_rel)) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: {} at {:X} matched, runs natively from now on", T.Name, T.GuestRIP));
    }
    return reinterpret_cast<uintptr_t>(T.Stub);
  }

  // One line per second in the stats log: calls, time and MB per function since the last line.
  fextl::string StatsLine() {
    fextl::string Line;
    for (auto& T : Targets) {
      const FEXNativeCallStats Now {
        .Calls = __atomic_load_n(&T.Stats->Calls, __ATOMIC_RELAXED),
        .Ticks = __atomic_load_n(&T.Stats->Ticks, __ATOMIC_RELAXED),
        .Bytes = __atomic_load_n(&T.Stats->Bytes, __ATOMIC_RELAXED),
      };
      const uint64_t Calls = Now.Calls - T.Prev.Calls;
      const double Ms = static_cast<double>(Now.Ticks - T.Prev.Ticks) * 1000.0 / static_cast<double>(TickFrequency);
      const double MB = static_cast<double>(Now.Bytes - T.Prev.Bytes) / (1024.0 * 1024.0);
      T.Prev = Now;
      if (Calls) {
        Line += fextl::fmt::format(" {}={}/{:.1f}ms/{:.1f}MB", T.Name, Calls, Ms, MB);
      }
    }
    return Line.empty() ? Line : fextl::fmt::format("  native{}\n", Line);
  }
} // namespace

void Init(uint64_t MainImageBase) {
  const char* Env = getenv("FEX_NATIVE_ZLIB");
  Enabled = Env && Env[0] == '1';
  for (auto& T : Targets) {
    T.GuestRIP = MainImageBase + T.RVA;
  }
  uint64_t Frequency {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
  TickFrequency = Frequency ? Frequency : 1;

  FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: {}", Enabled ? "on (FEX_NATIVE_ZLIB=1)" : "off"));
  if (Enabled) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
}

uintptr_t Lookup(uint64_t GuestRIP) {
  if (!Enabled) {
    return 0;
  }
  for (auto& T : Targets) {
    if (T.GuestRIP != GuestRIP) {
      continue;
    }
    switch (T.State.load(std::memory_order_acquire)) {
    case TargetState::Armed: return reinterpret_cast<uintptr_t>(T.Stub);
    case TargetState::Rejected: return 0;
    case TargetState::Unchecked: return Check(T);
    }
  }
  return 0;
}
} // namespace NativeRedirect
