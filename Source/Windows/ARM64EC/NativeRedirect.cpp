// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Runs native ARM64EC builds of hot guest functions in place of their emulated x86 code
$end_info$
*/

// Diablo II: Resurrected statically links zlib and inflates game data on its worker threads in every fight, and checks
// data with MD5; emulated, these take a large share of those threads' time. When switched on, the JIT hands out a stub
// for those functions' entry points instead of compiling them (SyscallHandler::RedirectBlock). The stub (Module.S) leaves
// the JIT exactly like a call from JIT code to an ARM64EC function, and the native code (NativeZlib.c: zlib 1.3.1,
// NativeMD5.c) then works on the game's own structures, whose layout is the same.
//
//   FEX_NATIVE_ZLIB=1   inflate_fast and adler32_z
//   FEX_NATIVE_MD5=1    the MD5 update and block transform
//   FEX_UNPACK_CACHE=1  with FEX_NATIVE_ZLIB=1: serve repeated zlib streams from disk (NativeUnpackCache.h)
//   FEX_JOBWAIT=0       keep the job system getter emulated (see JobWait below; on by default)
//   FEX_JOBWAIT_BACKOFF=N  microseconds the frame wait loop pauses when it has nothing to help with (default 0 = no pause;
//                          build 42 tests: 2 us left 0.3-0.4 s frame waits, 0 had none)
//
// A function is found by its offset in the main executable and the first bytes of its code, which are compared when the
// JIT first asks for that address (and, for MD5, the game's round tables). Anything that doesn't match stays emulated.

#include "NativeRedirect.h"
#include "NativeStats.h"
#include "NativeUnpackCache.h"
#include "Windows/Common/StatsLog.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

extern "C" NTSTATUS WINAPI NtReadVirtualMemory(HANDLE Process, const void* Address, void* Buffer, SIZE_T Size, SIZE_T* BytesRead);

extern "C" {
// Read by the stubs in Module.S, which store the call-ret stack pointer the way a JIT exit does.
uint64_t NativeRedirectCallRetSPOffset = offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp);

// Module.S
extern void* NativeInflateFastRedirect;
extern void* NativeAdler32ZRedirect;
extern void* NativeMD5UpdateRedirect;
extern void* NativeMD5TransformRedirect;
// Where each stub enters native code (x9 at ExitFunctionEC), named for the stats log's native-call sampler.
extern void* NativeInflateFastTarget;
extern void* NativeAdler32ZTarget;
extern void* NativeMD5UpdateTarget;
extern void* NativeMD5TransformTarget;
extern void* NativeJobSystemGetRedirect;
extern void* NativeJobSystemGetTarget;

uint64_t FEXNativeJobSystemGet(uint64_t ReturnAddress);
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

  constexpr uint8_t MD5TransformSignature[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x05, 0xd5, 0x92, 0xdb, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x44, 0x24, 0x40, 0x8b,
    0x02, 0x4c, 0x8d, 0x25, 0x4c, 0xf1, 0x43, 0xff, 0x8b, 0x71, 0x08, 0x41, 0xbe, 0x10, 0x00, 0x00, 0x00, 0x8b, 0x59, 0x0c, 0x4c, 0x8b,
    0xf9, 0x44, 0x8b, 0x51, 0x10, 0x45, 0x33, 0xc9, 0x44, 0x8b, 0x59, 0x14, 0x41, 0x8b, 0xfe, 0x89, 0x04, 0x24, 0x8b, 0x42, 0x04, 0x89,
    0x44, 0x24, 0x04, 0x8b, 0x42, 0x08, 0x89, 0x44, 0x24, 0x08, 0x8b, 0x42, 0x0c, 0x89, 0x44, 0x24, 0x0c, 0x8b, 0x42, 0x10, 0x89, 0x44,
    0x24, 0x10, 0x8b, 0x42, 0x14, 0x89, 0x44, 0x24, 0x14, 0x8b, 0x42, 0x18, 0x89, 0x44, 0x24, 0x18, 0x8b, 0x42,
  };

  constexpr uint8_t MD5UpdateSignature[] = {
    0x4d, 0x85, 0xc0, 0x0f, 0x84, 0xb7, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x48, 0x83,
    0xec, 0x20, 0x4c, 0x89, 0x74, 0x24, 0x38, 0x49, 0x8b, 0xf0, 0x45, 0x33, 0xf6, 0x48, 0x89, 0x7c, 0x24, 0x30, 0x48, 0x8b, 0xea, 0x48,
    0x8b, 0xd9, 0x66, 0x90, 0x8b, 0x53, 0x18, 0x85, 0xd2, 0x75, 0x22, 0x48, 0x83, 0xfe, 0x40, 0x72, 0x1c, 0x48, 0x8b, 0xd5, 0x48, 0x8b,
    0xcb, 0xe8, 0x48, 0xfd, 0xff, 0xff, 0x48, 0x81, 0x03, 0x00, 0x02, 0x00, 0x00, 0x48, 0x83, 0xc5, 0x40, 0x48, 0x83, 0xee, 0x40, 0xeb,
    0x49, 0xb9, 0x40, 0x00, 0x00, 0x00, 0x8b, 0xc6, 0x2b, 0xca, 0x48, 0x3b, 0xce, 0x0f, 0x46, 0xc1, 0x48, 0x8d, 0x4b, 0x1c, 0x48, 0x03,
    0xca, 0x44, 0x8b, 0xc0, 0x48, 0x8b, 0xd5, 0x8b, 0xf8, 0xe8, 0x94, 0x0c, 0x9a, 0x00, 0x01, 0x7b, 0x18, 0x48,
  };

  constexpr size_t MaxSignatureSize = 128;
  static_assert(sizeof(InflateFastSignature) <= MaxSignatureSize && sizeof(Adler32ZSignature) <= MaxSignatureSize &&
                sizeof(MD5TransformSignature) <= MaxSignatureSize && sizeof(MD5UpdateSignature) <= MaxSignatureSize);

  // The game's MD5 transform reads its round constants from three tables (shift amounts, message word order, sines);
  // NativeMD5.c hard-codes the standard ones, so they must match.
  constexpr uint32_t MD5TablesRVA = 0x16976A0;
  constexpr uint8_t MD5Shifts[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
  };
  constexpr uint8_t MD5Order[64] = {
    0, 1, 2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 1, 6, 11, 0,  5,  10, 15, 4,  9,  14, 3,  8,  13, 2,  7,  12,
    5, 8, 11, 14, 1,  4,  7,  10, 13, 0,  3,  6,  9,  12, 15, 2,  0, 7, 14, 5,  12, 3,  10, 1,  8,  15, 6,  13, 4,  11, 2,  9,
  };
  constexpr uint32_t MD5Sines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
  };

  // A target whose bytes keep differing this many times is given up on. Unreadable code (not yet decrypted by the game's
  // protector) is not counted: the JIT can't run it either, so it will be asked for again once it is readable.
  constexpr uint32_t MaxMismatches = 16;

  enum class Group {
    Zlib,
    MD5,
  };

  enum class TargetState : uint32_t {
    Unchecked,
    Armed,
    Rejected,
  };

  enum class CheckResult {
    Match,
    Mismatch,
    Unreadable,
  };

  struct Target {
    const char* Name;
    Group Group;
    uint32_t RVA;
    const uint8_t* Signature;
    size_t SignatureSize;
    void* Stub;
    FEXNativeCallStats* Stats;
    void* Entry;

    uint64_t GuestRIP;
    std::atomic<TargetState> State;
    std::atomic<uint32_t> Mismatches;
    FEXNativeCallStats Prev; // Log thread only
  };

  Target Targets[] = {
    {"inflate_fast", Group::Zlib, 0xBD2090, InflateFastSignature, sizeof(InflateFastSignature), &NativeInflateFastRedirect,
     &FEXNativeInflateFastStats, &NativeInflateFastTarget},
    {"adler32_z", Group::Zlib, 0xBD17F0, Adler32ZSignature, sizeof(Adler32ZSignature), &NativeAdler32ZRedirect, &FEXNativeAdler32ZStats,
     &NativeAdler32ZTarget},
    {"md5", Group::MD5, 0xBC10F0, MD5UpdateSignature, sizeof(MD5UpdateSignature), &NativeMD5UpdateRedirect, &FEXNativeMD5UpdateStats,
     &NativeMD5UpdateTarget},
    {"md5block", Group::MD5, 0xBC0E80, MD5TransformSignature, sizeof(MD5TransformSignature), &NativeMD5TransformRedirect,
     &FEXNativeMD5TransformStats, &NativeMD5TransformTarget},
  };

  bool ZlibEnabled {};
  bool MD5Enabled {};
  bool UnpackCacheRequested {};
  std::mutex UnpackCacheLock;
  std::atomic<bool> UnpackCacheOpened {};
  uint64_t ImageBase {};
  uint64_t TickFrequency {1};

  bool Enabled(Group G) {
    return G == Group::Zlib ? ZlibEnabled : MD5Enabled;
  }

  const char* GroupName(Group G) {
    return G == Group::Zlib ? "native zlib" : "native md5";
  }

  fextl::string Hex(const uint8_t* Bytes, size_t Size) {
    fextl::string Out;
    for (size_t i = 0; i < Size; ++i) {
      Out += fextl::fmt::format("{:02x}", Bytes[i]);
    }
    return Out;
  }

  bool Read(uint64_t Address, void* Buffer, size_t Size) {
    SIZE_T Read {};
    return NtReadVirtualMemory(NtCurrentProcess(), reinterpret_cast<void*>(Address), Buffer, Size, &Read) == STATUS_SUCCESS && Read == Size;
  }

  // Checks the parts of a target beyond its first bytes. Notes what differs.
  CheckResult CheckExtra(Target& T) {
    if (T.Group != Group::MD5) {
      return CheckResult::Match;
    }
    struct {
      uint8_t Shifts[64];
      uint8_t Order[64];
      uint32_t Sines[64];
    } Tables;
    static_assert(sizeof(Tables) == 384);
    if (!Read(ImageBase + MD5TablesRVA, &Tables, sizeof(Tables))) {
      return CheckResult::Unreadable;
    }
    const bool Shifts = memcmp(Tables.Shifts, MD5Shifts, sizeof(MD5Shifts)) == 0;
    const bool Order = memcmp(Tables.Order, MD5Order, sizeof(MD5Order)) == 0;
    const bool Sines = memcmp(Tables.Sines, MD5Sines, sizeof(MD5Sines)) == 0;
    if (Shifts && Order && Sines) {
      return CheckResult::Match;
    }
    FEX::Windows::StatsLogNote(fextl::fmt::format("native md5: {} round tables at {:X} are not standard MD5 (shifts {}, order {}, sines {})",
                                                  T.Name, ImageBase + MD5TablesRVA, Shifts ? "ok" : "differ", Order ? "ok" : "differ",
                                                  Sines ? "ok" : "differ"));
    return CheckResult::Mismatch;
  }

  // Opens the unpack cache once the game's inflate_fast is found, so other processes started with the same settings
  // (launchers, helpers) never open the cache files. Runs before the stub is handed out, so the stub sees the cache.
  void OpenUnpackCache() {
    if (!UnpackCacheRequested || UnpackCacheOpened.load(std::memory_order_acquire)) {
      return;
    }
    std::scoped_lock Guard {UnpackCacheLock};
    if (!UnpackCacheOpened.load(std::memory_order_relaxed)) {
      const char* MD5Cache = getenv("FEX_NATIVE_MD5_CACHE");
      NativeUnpackCache::Init(MD5Enabled && !(MD5Cache && MD5Cache[0] == '0'));
      UnpackCacheOpened.store(true, std::memory_order_release);
    }
  }

  uintptr_t Check(Target& T) {
    uint8_t Code[MaxSignatureSize];
    if (!Read(T.GuestRIP, Code, T.SignatureSize)) {
      return 0;
    }

    CheckResult Result = memcmp(Code, T.Signature, T.SignatureSize) == 0 ? CheckResult::Match : CheckResult::Mismatch;
    if (Result == CheckResult::Mismatch) {
      const uint32_t Count = T.Mismatches.load(std::memory_order_relaxed) + 1;
      if (Count == 1 || Count == MaxMismatches) {
        FEX::Windows::StatsLogNote(fextl::fmt::format("{}: {} at {:X} has different code ({}/{}){}: {}", GroupName(T.Group), T.Name,
                                                      T.GuestRIP, Count, MaxMismatches, Count == MaxMismatches ? ", stays emulated" : "",
                                                      Hex(Code, T.SignatureSize)));
      }
    } else {
      Result = CheckExtra(T);
      if (Result == CheckResult::Unreadable) {
        return 0;
      }
    }

    if (Result == CheckResult::Mismatch) {
      if (T.Mismatches.fetch_add(1, std::memory_order_relaxed) + 1 >= MaxMismatches) {
        T.State.store(TargetState::Rejected, std::memory_order_release);
      }
      return 0;
    }

    if (T.Stub == &NativeInflateFastRedirect) {
      OpenUnpackCache();
    }

    auto Expected = TargetState::Unchecked;
    if (T.State.compare_exchange_strong(Expected, TargetState::Armed, std::memory_order_acq_rel)) {
      FEX::Windows::StatsLogNote(
        fextl::fmt::format("{}: {} at {:X} matched, runs natively from now on", GroupName(T.Group), T.Name, T.GuestRIP));
    }
    return reinterpret_cast<uintptr_t>(T.Stub);
  }

  // The executable sections of the main image, to tell return addresses on the stack apart from other values. Falls back
  // to the whole image if the headers can't be read.
  void FindText(uint64_t Base) {
    FEXNativeImageBase = Base;
    IMAGE_DOS_HEADER Dos {};
    IMAGE_NT_HEADERS64 Nt {};
    if (!Read(Base, &Dos, sizeof(Dos)) || Dos.e_magic != IMAGE_DOS_SIGNATURE || !Read(Base + Dos.e_lfanew, &Nt, sizeof(Nt)) ||
        Nt.Signature != IMAGE_NT_SIGNATURE) {
      FEXNativeTextBegin = Base;
      FEXNativeTextEnd = Base + 0x2000000;
      return;
    }
    uint64_t Begin = ~0ULL, End = 0;
    const uint64_t SectionsAt = Base + Dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + Nt.FileHeader.SizeOfOptionalHeader;
    for (uint32_t i = 0; i < Nt.FileHeader.NumberOfSections; ++i) {
      IMAGE_SECTION_HEADER Section {};
      if (!Read(SectionsAt + i * sizeof(Section), &Section, sizeof(Section))) {
        break;
      }
      if (Section.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
        Begin = std::min<uint64_t>(Begin, Base + Section.VirtualAddress);
        End = std::max<uint64_t>(End, Base + Section.VirtualAddress + Section.Misc.VirtualSize);
      }
    }
    if (End <= Begin) {
      Begin = Base;
      End = Base + Nt.OptionalHeader.SizeOfImage;
    }
    FEXNativeTextBegin = Begin;
    FEXNativeTextEnd = End;
  }

  // Log thread state.
  FEXNativeZlibStreams PrevStreams {};
  FEXNativeZlibStreams WindowStart {};
  uint64_t Seconds {};
  fextl::vector<uint32_t> DumpedCallSites;
  constexpr uint64_t WindowSeconds = 10;

  fextl::string MagicText(uint32_t Magic) {
    char Text[5] {};
    for (int i = 0; i < 4; ++i) {
      const char c = static_cast<char>((Magic >> (i * 8)) & 0xff);
      Text[i] = (c >= 0x20 && c < 0x7f && c != '\'') ? c : '.';
    }
    if (!Magic) {
      return "binary";
    }
    return fextl::fmt::format("{:08x}'{}'", __builtin_bswap32(Magic), Text);
  }

  // Every WindowSeconds: what was inflated in that window, by first bytes and by caller, with how much of it repeats
  // earlier output. Call sites of the top callers are dumped once (#C lines) so they can be disassembled.
  fextl::string WindowLines(const FEXNativeZlibStreams& Now) {
    const auto& Old = WindowStart;
    const uint64_t Streams = Now.Streams - Old.Streams;
    if (!Streams) {
      return {};
    }
    const uint64_t Repeats = Now.Repeats - Old.Repeats;
    const double MB = static_cast<double>(Now.Bytes - Old.Bytes) / (1024.0 * 1024.0);
    const double RepeatMB = static_cast<double>(Now.RepeatBytes - Old.RepeatBytes) / (1024.0 * 1024.0);
    fextl::string Sizes;
    for (int i = 0; i < FEX_NATIVE_ZLIB_SIZE_BUCKETS; ++i) {
      Sizes += fextl::fmt::format("{}{}", i ? "/" : "", Now.SizeBuckets[i] - Old.SizeBuckets[i]);
    }
    // Session totals too: distinctMB is how much memory a cache of every distinct stream seen so far would need.
    auto Lines = fextl::fmt::format("  zlib{}s streams={} repeats={} ({:.0f}%) MB={:.1f} repeatMB={:.1f} sizes(<1K/4K/16K/64K/256K/1M/4M/more)={} "
                                    "oneshot={} | total streams={} repeats={} MB={:.0f} distinctMB={:.0f}\n",
                                    WindowSeconds, Streams, Repeats, 100.0 * static_cast<double>(Repeats) / static_cast<double>(Streams), MB,
                                    RepeatMB, Sizes, Now.Oneshot - Old.Oneshot, Now.Streams, Now.Repeats,
                                    static_cast<double>(Now.Bytes) / (1024.0 * 1024.0),
                                    static_cast<double>(Now.Bytes - Now.RepeatBytes) / (1024.0 * 1024.0));

    struct Row {
      size_t Index;
      uint64_t Streams, Repeats, Bytes;
    };
    fextl::vector<Row> Rows;
    for (size_t i = 0; i < FEX_NATIVE_ZLIB_MAGIC_SLOTS; ++i) {
      const uint64_t S = Now.Magics[i].Streams - Old.Magics[i].Streams;
      if (Now.Magics[i].Used && S) {
        Rows.push_back({i, S, Now.Magics[i].Repeats - Old.Magics[i].Repeats, Now.Magics[i].Bytes - Old.Magics[i].Bytes});
      }
    }
    std::sort(Rows.begin(), Rows.end(), [](const Row& a, const Row& b) { return a.Bytes > b.Bytes; });
    Lines += "  zlib by first bytes:";
    for (size_t i = 0; i < std::min<size_t>(Rows.size(), 8); ++i) {
      const auto& R = Rows[i];
      Lines += fextl::fmt::format(" {}={}/rep{}/{:.1f}MB", MagicText(Now.Magics[R.Index].Magic), R.Streams, R.Repeats,
                                  static_cast<double>(R.Bytes) / (1024.0 * 1024.0));
    }
    Lines += fextl::fmt::format("{}\n", Now.Unlisted - Old.Unlisted ? fextl::fmt::format(" unlisted={}", Now.Unlisted - Old.Unlisted) : "");

    Rows.clear();
    for (size_t i = 0; i < FEX_NATIVE_ZLIB_CALLER_SLOTS; ++i) {
      const uint64_t S = Now.Callers[i].Streams - Old.Callers[i].Streams;
      if (Now.Callers[i].Used && S) {
        Rows.push_back({i, S, Now.Callers[i].Repeats - Old.Callers[i].Repeats, Now.Callers[i].Bytes - Old.Callers[i].Bytes});
      }
    }
    std::sort(Rows.begin(), Rows.end(), [](const Row& a, const Row& b) { return a.Bytes > b.Bytes; });
    Lines += "  zlib by caller:";
    for (size_t i = 0; i < std::min<size_t>(Rows.size(), 6); ++i) {
      const auto& R = Rows[i];
      const auto& C = Now.Callers[R.Index];
      Lines += fextl::fmt::format(" {:X}<{:X}<{:X}={}/rep{}/{:.1f}MB", C.RVA[0], C.RVA[1], C.RVA[2], R.Streams, R.Repeats,
                                  static_cast<double>(R.Bytes) / (1024.0 * 1024.0));
      // Dump the code around each return address once: 192 bytes before it (the call and the code leading up to it).
      for (const uint32_t RVA : C.RVA) {
        if (!RVA || std::find(DumpedCallSites.begin(), DumpedCallSites.end(), RVA) != DumpedCallSites.end()) {
          continue;
        }
        uint8_t Code[256];
        if (Read(ImageBase + RVA - 192, Code, sizeof(Code))) {
          DumpedCallSites.push_back(RVA);
          Lines += fextl::fmt::format("\n#C ret={:X} from={:X} bytes={}", ImageBase + RVA, ImageBase + RVA - 192, Hex(Code, sizeof(Code)));
        }
      }
    }
    Lines += "\n";
    return Lines;
  }

  fextl::string JobWaitStatsLine();

  // Once per second in the stats log: calls, time and MB per function since the last line, plus stream starts.
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

    fextl::string Window;
    if (ZlibEnabled && FEXNativeZlibTrackStreams) {
      static FEXNativeZlibStreams Now;
      FEXNativeZlibStreamSnapshot(&Now);
      const uint64_t Streams = Now.Streams - PrevStreams.Streams;
      if (Streams) {
        Line += fextl::fmt::format(" streams={}/rep{}", Streams, Now.Repeats - PrevStreams.Repeats);
      }
      PrevStreams = Now;
      if (++Seconds % WindowSeconds == 0) {
        Window = WindowLines(Now);
        WindowStart = Now;
      }
    }
    return (Line.empty() ? Line : fextl::fmt::format("  native{}\n", Line)) + NativeUnpackCache::StatsLine() + JobWaitStatsLine() + Window;
  }

  // D2R's main thread waits for a frame's parallel tasks in a loop that never sleeps (D2R.exe 0x1408764A8):
  //
  //   do { JobSystem()->Queue->TryRunJob(priority 2) } while (finished task bits != all tasks)
  //
  // TryRunJob (0x1407B7700) takes the queue's spinlock (xchg + pause), looks at the priority 2 list and releases the
  // lock. With nothing to run, the loop retakes the lock every few hundred nanoseconds. A worker that wants to queue a
  // job (0x1407B7210) has to win the same lock between two of those, and on these cores it can lose for hundreds of
  // milliseconds: build 41 logs show the worst frames (190-570 ms) as Main spinning here while one worker spends the
  // whole time in that lock. The loop asks for the job system singleton (0x1407B75B0, an MSVC thread-safe static) right
  // before each TryRunJob. Running that getter natively lets the wait loop's call pause for a moment when the priority 2
  // list is empty, so the lock stays free for the workers; every other caller just gets the object back.
  namespace JobWait {
    constexpr uint32_t GetterRVA = 0x7B75B0;
    constexpr uint32_t ObjectRVA = 0x2056B00;  // what the getter returns
    constexpr uint32_t GuardRVA = 0x2056CD8;   // its init guard: 0 = not constructed, -1 = being constructed, else epoch
    constexpr uint32_t CallSiteRVA = 0x8764A8; // the wait loop's call of the getter
    constexpr uint32_t ReturnRVA = CallSiteRVA + 5;
    constexpr uint32_t TryRunRVA = 0x7B7700;
    constexpr uint64_t QueueOffset = 0x1A0; // job system -> queue the wait loop helps with
    constexpr uint64_t ListOffset = 0x38;   // queue -> list head of priority 2 jobs (empty when it points to itself)

    // The whole getter, the wait loop from the getter call to its loop branch, and TryRunJob up to where it reads the
    // priority 2 list: they fix the addresses and offsets above.
    constexpr uint8_t GetterCode[] = {
      0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xba, 0x18, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x08,
      0x8b, 0x04, 0x0a, 0x39, 0x05, 0x0a, 0xf7, 0x89, 0x01, 0x0f, 0x8e, 0x8a, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xfd, 0xf6, 0x89,
      0x01, 0xe8, 0x0c, 0x76, 0xda, 0x00, 0x83, 0x3d, 0xf1, 0xf6, 0x89, 0x01, 0xff, 0x75, 0x75, 0x33, 0xd2, 0xc6, 0x05, 0x0e, 0xf5,
      0x89, 0x01, 0x01, 0x48, 0x8d, 0x05, 0x27, 0xf5, 0x89, 0x01, 0x48, 0x89, 0x15, 0x10, 0xf5, 0x89, 0x01, 0x48, 0x89, 0x05, 0x01,
      0xf5, 0x89, 0x01, 0x48, 0x8d, 0x0d, 0xc2, 0x2e, 0xdf, 0x00, 0x48, 0xb8, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x48,
      0x89, 0x15, 0x81, 0xf6, 0x89, 0x01, 0x48, 0x89, 0x05, 0xf2, 0xf4, 0x89, 0x01, 0x0f, 0x57, 0xc0, 0x48, 0x8d, 0x05, 0x08, 0xd1,
      0xf8, 0x00, 0x66, 0x0f, 0x7f, 0x05, 0x78, 0xf6, 0x89, 0x01, 0x48, 0x89, 0x05, 0x69, 0xf6, 0x89, 0x01, 0x48, 0x89, 0x15, 0x7a,
      0xf6, 0x89, 0x01, 0x48, 0x89, 0x15, 0x83, 0xf6, 0x89, 0x01, 0xe8, 0xfa, 0x73, 0xda, 0x00, 0x48, 0x8d, 0x0d, 0x7f, 0xf6, 0x89,
      0x01, 0xe8, 0x2e, 0x75, 0xda, 0x00, 0x48, 0x8d, 0x05, 0x9b, 0xf4, 0x89, 0x01, 0x48, 0x83, 0xc4, 0x28, 0xc3,
    };
    constexpr uint8_t CallSiteCode[] = {
      0xe8, 0x03, 0x11, 0xf4, 0xff, 0x41, 0xb1, 0xff, 0x45, 0x8b, 0xc4, 0x48, 0x8b, 0x90, 0xa0, 0x01, 0x00, 0x00, 0xe8, 0x41, 0x12,
      0xf4, 0xff, 0x8b, 0x97, 0xe8, 0x60, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0xc0, 0x5e, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x48,
      0xd3, 0xe0, 0xff, 0xc8, 0x3b, 0xd0, 0x75, 0xce,
    };
    constexpr uint8_t TryRunCode[] = {
      0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x42, 0x10, 0x4c, 0x8b, 0xda, 0x48, 0x87, 0x42, 0x10, 0x48, 0x85, 0xc0, 0x74, 0x18, 0xf3,
      0x90, 0x48, 0x8b, 0x42, 0x10, 0x48, 0x85, 0xc0, 0x75, 0xf5, 0x48, 0x8d, 0x42, 0x10, 0x48, 0x87, 0x42, 0x10, 0x48, 0x85, 0xc0,
      0x75, 0xe8, 0x4d, 0x63, 0xc0, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x49, 0x83, 0xf8, 0x02, 0x7f, 0x30, 0xba, 0x02, 0x00, 0x00, 0x00,
      0x49, 0x8d, 0x4b, 0x38,
    };

    bool Enabled {};
    uint64_t BackoffMicroseconds {};
    std::atomic<TargetState> State {TargetState::Unchecked};
    std::atomic<bool> Pending {};      // waiting for the object to be constructed or the code to be readable
    std::atomic<uint64_t> NextRetry {}; // tick of the next PreCompile check while Pending
    uint32_t Retries {};
    void (*Invalidate)(uint64_t Address, uint64_t Size) {};
  } // namespace JobWait

  CheckResult CheckJobWait() {
    uint8_t Getter[sizeof(JobWait::GetterCode)];
    uint8_t CallSite[sizeof(JobWait::CallSiteCode)];
    uint8_t TryRun[sizeof(JobWait::TryRunCode)];
    int32_t Guard {};
    if (!Read(ImageBase + JobWait::GetterRVA, Getter, sizeof(Getter)) || !Read(ImageBase + JobWait::CallSiteRVA, CallSite, sizeof(CallSite)) ||
        !Read(ImageBase + JobWait::TryRunRVA, TryRun, sizeof(TryRun)) || !Read(ImageBase + JobWait::GuardRVA, &Guard, sizeof(Guard))) {
      return CheckResult::Unreadable;
    }
    const bool GetterOk = memcmp(Getter, JobWait::GetterCode, sizeof(Getter)) == 0;
    const bool CallSiteOk = memcmp(CallSite, JobWait::CallSiteCode, sizeof(CallSite)) == 0;
    const bool TryRunOk = memcmp(TryRun, JobWait::TryRunCode, sizeof(TryRun)) == 0;
    if (!GetterOk || !CallSiteOk || !TryRunOk) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("native jobwait: different code (getter {}, wait loop {}, TryRunJob {}), stays emulated",
                                                    GetterOk ? "ok" : "differs", CallSiteOk ? "ok" : "differs", TryRunOk ? "ok" : "differs"));
      return CheckResult::Mismatch;
    }
    // Not constructed yet: the getter would have to run the constructor, which only the guest code can do. Once it is,
    // MSVC stores the global init epoch there, which counts up from INT_MIN.
    return Guard != 0 && Guard != -1 ? CheckResult::Match : CheckResult::Unreadable;
  }

  uint64_t NowTicks() {
    uint64_t Ticks;
    __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Ticks));
    return Ticks;
  }

  uintptr_t LookupJobWait() {
    switch (JobWait::State.load(std::memory_order_acquire)) {
    case TargetState::Armed: return reinterpret_cast<uintptr_t>(&NativeJobSystemGetRedirect);
    case TargetState::Rejected: return 0;
    case TargetState::Unchecked: break;
    }
    switch (CheckJobWait()) {
    case CheckResult::Match: {
      auto Expected = TargetState::Unchecked;
      if (JobWait::State.compare_exchange_strong(Expected, TargetState::Armed, std::memory_order_acq_rel)) {
        FEX::Windows::StatsLogNote(fextl::fmt::format("native jobwait: getter at {:X} matched, frame wait loop pauses {} us when idle",
                                                      ImageBase + JobWait::GetterRVA, JobWait::BackoffMicroseconds));
      }
      return reinterpret_cast<uintptr_t>(&NativeJobSystemGetRedirect);
    }
    case CheckResult::Mismatch: JobWait::State.store(TargetState::Rejected, std::memory_order_release); return 0;
    case CheckResult::Unreadable:
      // The guest getter gets compiled now and stays in the code cache, so the JIT won't ask again by itself.
      // PreCompile drops it once the check can pass.
      JobWait::NextRetry.store(NowTicks() + TickFrequency / 20, std::memory_order_relaxed);
      JobWait::Pending.store(true, std::memory_order_release);
      return 0;
    }
    return 0;
  }

  // The wait loop's own counters. Only the thread running the loop writes the episode fields.
  struct {
    std::atomic<uint64_t> Waits;        // calls from the wait loop that found nothing to run
    std::atomic<uint64_t> Busy;         // calls from the wait loop that found priority 2 jobs
    std::atomic<uint64_t> BackoffTicks; // time spent pausing
    std::atomic<uint64_t> Longest;      // longest stretch of uninterrupted idle waiting, in ticks
    std::atomic<uint64_t> EpisodeStart;
    std::atomic<uint64_t> LastWait;
  } JobWaitStats;
  uint64_t JobWaitObject {};
  uint64_t JobWaitReturn {};
  uint64_t JobWaitBackoff {};    // ticks
  uint64_t JobWaitEpisodeGap {}; // ticks: idle waits further apart than this start a new stretch

  fextl::string JobWaitStatsLine() {
    if (JobWait::State.load(std::memory_order_relaxed) != TargetState::Armed) {
      return {};
    }
    const uint64_t Waits = JobWaitStats.Waits.exchange(0, std::memory_order_relaxed);
    const uint64_t Busy = JobWaitStats.Busy.exchange(0, std::memory_order_relaxed);
    const uint64_t Backoff = JobWaitStats.BackoffTicks.exchange(0, std::memory_order_relaxed);
    const uint64_t Longest = JobWaitStats.Longest.exchange(0, std::memory_order_relaxed);
    if (!Waits && !Busy) {
      return {};
    }
    const double MsPerTick = 1000.0 / static_cast<double>(TickFrequency);
    return fextl::fmt::format("  jobwait idle={} jobs={} paused={:.1f}ms longest={:.1f}ms\n", Waits, Busy,
                              static_cast<double>(Backoff) * MsPerTick, static_cast<double>(Longest) * MsPerTick);
  }
} // namespace

void Init(uint64_t MainImageBase, bool StatsLog, void (*Invalidate)(uint64_t Address, uint64_t Size)) {
  const char* Zlib = getenv("FEX_NATIVE_ZLIB");
  const char* MD5 = getenv("FEX_NATIVE_MD5");
  ZlibEnabled = Zlib && Zlib[0] == '1';
  MD5Enabled = MD5 && MD5[0] == '1';
  ImageBase = MainImageBase;
  for (auto& T : Targets) {
    T.GuestRIP = MainImageBase + T.RVA;
    if (StatsLog) {
      FEX::Windows::StatsLogNameNative(reinterpret_cast<uint64_t>(T.Entry), fextl::fmt::format("fex:{}", T.Name));
    }
  }
  uint64_t Frequency {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
  TickFrequency = Frequency ? Frequency : 1;

  {
    const char* JobWaitSwitch = getenv("FEX_JOBWAIT");
    const char* Backoff = getenv("FEX_JOBWAIT_BACKOFF");
    JobWait::Enabled = !(JobWaitSwitch && JobWaitSwitch[0] == '0') && Invalidate;
    JobWait::BackoffMicroseconds = Backoff && Backoff[0] ? std::min<uint64_t>(strtoull(Backoff, nullptr, 10), 1000) : 0;
    JobWait::Invalidate = Invalidate;
    JobWaitObject = MainImageBase + JobWait::ObjectRVA;
    JobWaitReturn = MainImageBase + JobWait::ReturnRVA;
    JobWaitBackoff = JobWait::BackoffMicroseconds ? std::max<uint64_t>(JobWait::BackoffMicroseconds * TickFrequency / 1000000, 1) : 0;
    JobWaitEpisodeGap = std::max<uint64_t>(TickFrequency / 5000, 1); // 200 us
    if (StatsLog) {
      FEX::Windows::StatsLogNameNative(reinterpret_cast<uint64_t>(&NativeJobSystemGetTarget), "fex:jobsystem");
    }
  }

  if (StatsLog && ZlibEnabled) {
    FindText(MainImageBase);
    FEXNativeZlibTrackStreams = 1;
  }

  // On unless set to 0.
  auto DefaultOn = [](const char* Name) {
    const char* Value = getenv(Name);
    return !(Value && Value[0] == '0');
  };
  FEXNativeSkipHitAdler = DefaultOn("FEX_UNPACK_CACHE_SKIP_ADLER");
  FEXNativeAdlerNeon = DefaultOn("FEX_NATIVE_ADLER_NEON");

  FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: {}", ZlibEnabled ? "on (FEX_NATIVE_ZLIB=1)" : "off"));
  if (ZlibEnabled) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: adler32 {}, cache hits' adler32 check {}",
                                                  FEXNativeAdlerNeon ? "NEON" : "zlib C code (FEX_NATIVE_ADLER_NEON=0)",
                                                  FEXNativeSkipHitAdler ? "answered from the stream's trailer" :
                                                                          "computed (FEX_UNPACK_CACHE_SKIP_ADLER=0)"));
  }
  FEX::Windows::StatsLogNote(fextl::fmt::format("native md5: {}", MD5Enabled ? "on (FEX_NATIVE_MD5=1)" : "off"));
  FEX::Windows::StatsLogNote(JobWait::Enabled ? fextl::fmt::format("native jobwait: on, pause {} us (FEX_JOBWAIT_BACKOFF), FEX_JOBWAIT=0 turns it off",
                                                                   JobWait::BackoffMicroseconds) :
                                                "native jobwait: off (FEX_JOBWAIT=0)");
  if (FEXNativeZlibTrackStreams) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: tracking streams, code range {:X}-{:X}", FEXNativeTextBegin, FEXNativeTextEnd));
  }
  if (const char* Cache = getenv("FEX_UNPACK_CACHE"); Cache && Cache[0] == '1') {
    UnpackCacheRequested = ZlibEnabled;
    FEX::Windows::StatsLogNote(ZlibEnabled ? "unpack cache: requested, opens when the game's inflate_fast is found" :
                                             "unpack cache: off, it needs FEX_NATIVE_ZLIB=1");
    if (UnpackCacheRequested) {
      // Started with the process, like FEX's other helper threads: in build 37 the writer started later from a game
      // thread never got ready.
      NativeUnpackCache::StartWriter();
    }
  } else {
    FEX::Windows::StatsLogNote("unpack cache: off");
  }
  if (ZlibEnabled || MD5Enabled || JobWait::Enabled) {
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
}

void PreCompile() {
  if (!JobWait::Pending.load(std::memory_order_relaxed)) {
    return;
  }
  const uint64_t Now = NowTicks();
  uint64_t Due = JobWait::NextRetry.load(std::memory_order_relaxed);
  if (Now < Due || !JobWait::NextRetry.compare_exchange_strong(Due, Now + TickFrequency / 20, std::memory_order_relaxed)) {
    return;
  }
  // Only this thread got past the exchange for this retry.
  if (++JobWait::Retries > 2000) {
    JobWait::Pending.store(false, std::memory_order_relaxed);
    FEX::Windows::StatsLogNote("native jobwait: the job system never became ready, stays emulated");
    return;
  }
  switch (CheckJobWait()) {
  case CheckResult::Match: break;
  case CheckResult::Mismatch:
    JobWait::State.store(TargetState::Rejected, std::memory_order_release);
    JobWait::Pending.store(false, std::memory_order_relaxed);
    return;
  case CheckResult::Unreadable: return;
  }
  // Drop the compiled guest getter, so the next call asks Lookup again and gets the native one.
  JobWait::Pending.store(false, std::memory_order_relaxed);
  JobWait::Invalidate(ImageBase + JobWait::GetterRVA, 1);
}

uintptr_t Lookup(uint64_t GuestRIP) {
  if (JobWait::Enabled && GuestRIP == ImageBase + JobWait::GetterRVA) {
    return LookupJobWait();
  }
  if (!ZlibEnabled && !MD5Enabled) {
    return 0;
  }
  for (auto& T : Targets) {
    if (T.GuestRIP != GuestRIP) {
      continue;
    }
    if (!Enabled(T.Group)) {
      return 0;
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

// The job system getter, run in place of D2R.exe 0x1407B75B0 once its object exists (see JobWait). Every caller gets the
// object; the frame wait loop's call first pauses when the priority 2 list is empty.
extern "C" uint64_t FEXNativeJobSystemGet(uint64_t ReturnAddress) {
  using namespace NativeRedirect;
  const uint64_t Object = JobWaitObject;
  // No shared counter for the other callers: workers call this around every queue operation, and a contended counter
  // is the kind of cache line traffic this redirect is meant to reduce.
  if (ReturnAddress != JobWaitReturn) {
    return Object;
  }
  const uint64_t Queue = __atomic_load_n(reinterpret_cast<const uint64_t*>(Object + JobWait::QueueOffset), __ATOMIC_RELAXED);
  if (!Queue) {
    return Object;
  }
  const uint64_t Head = Queue + JobWait::ListOffset;
  if (__atomic_load_n(reinterpret_cast<const uint64_t*>(Head), __ATOMIC_ACQUIRE) != Head) {
    JobWaitStats.Busy.fetch_add(1, std::memory_order_relaxed);
    return Object;
  }

  const uint64_t Start = NowTicks();
  if (Start - JobWaitStats.LastWait.load(std::memory_order_relaxed) > JobWaitEpisodeGap) {
    JobWaitStats.EpisodeStart.store(Start, std::memory_order_relaxed);
  }
  uint64_t Now = Start;
  if (JobWaitBackoff) {
    do {
      __asm volatile("yield" ::: "memory");
      Now = NowTicks();
    } while (Now - Start < JobWaitBackoff);
    JobWaitStats.BackoffTicks.fetch_add(Now - Start, std::memory_order_relaxed);
  }
  JobWaitStats.LastWait.store(Now, std::memory_order_relaxed);
  JobWaitStats.Waits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t Episode = Now - JobWaitStats.EpisodeStart.load(std::memory_order_relaxed);
  uint64_t Longest = JobWaitStats.Longest.load(std::memory_order_relaxed);
  while (Episode > Longest && !JobWaitStats.Longest.compare_exchange_weak(Longest, Episode, std::memory_order_relaxed)) {
  }
  return Object;
}
