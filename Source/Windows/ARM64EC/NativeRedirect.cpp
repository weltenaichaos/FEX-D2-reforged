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
// A function is found by its code, not by its address: game updates move code and data around (the 2026-10 update moved
// these functions by 0x390-0x700 bytes and the data by 0x2000), so until everything is found the first bytes of every
// block the JIT compiles in the main executable are compared with each function's first bytes. Fields that hold a
// RIP-relative displacement, a call's target or a table's offset are skipped in that comparison; the data those fields
// point at is found from the matched code (job system, MD5 round tables) or searched for near where the last known
// version had it (the frame wait loop's call). Anything that isn't found stays emulated.

#include "NativeRedirect.h"
#include "NativeStats.h"
#include "NativeUnpackCache.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

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

// Frame times (see FrameClock): ExitFunctionEC compares the x64 return address of every call into native code with
// FEXFrameMarkReturn (0 = off) and on a match stores the counter in FEXFrameMarks: [0] = marks written so far, then a
// ring of FrameMarkRing times.
constexpr size_t FrameMarkRing = 1024;
uint64_t FEXFrameMarkReturn;
alignas(64) uint64_t FEXFrameMarks[1 + FrameMarkRing];

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

  // The MD5 transform up to the end of its first round loop, which shows where the round tables are (offsets below).
  constexpr uint8_t MD5TransformSignature[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x05, 0xd5, 0xab, 0xdb, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x44, 0x24, 0x40, 0x8b,
    0x02, 0x4c, 0x8d, 0x25, 0x4c, 0xea, 0x43, 0xff, 0x8b, 0x71, 0x08, 0x41, 0xbe, 0x10, 0x00, 0x00, 0x00, 0x8b, 0x59, 0x0c, 0x4c, 0x8b,
    0xf9, 0x44, 0x8b, 0x51, 0x10, 0x45, 0x33, 0xc9, 0x44, 0x8b, 0x59, 0x14, 0x41, 0x8b, 0xfe, 0x89, 0x04, 0x24, 0x8b, 0x42, 0x04, 0x89,
    0x44, 0x24, 0x04, 0x8b, 0x42, 0x08, 0x89, 0x44, 0x24, 0x08, 0x8b, 0x42, 0x0c, 0x89, 0x44, 0x24, 0x0c, 0x8b, 0x42, 0x10, 0x89, 0x44,
    0x24, 0x10, 0x8b, 0x42, 0x14, 0x89, 0x44, 0x24, 0x14, 0x8b, 0x42, 0x18, 0x89, 0x44, 0x24, 0x18, 0x8b, 0x42, 0x1c, 0x89, 0x44, 0x24,
    0x1c, 0x8b, 0x42, 0x20, 0x89, 0x44, 0x24, 0x20, 0x8b, 0x42, 0x24, 0x89, 0x44, 0x24, 0x24, 0x8b, 0x42, 0x28, 0x89, 0x44, 0x24, 0x28,
    0x8b, 0x42, 0x2c, 0x89, 0x44, 0x24, 0x2c, 0x8b, 0x42, 0x30, 0x89, 0x44, 0x24, 0x30, 0x8b, 0x42, 0x34, 0x89, 0x44, 0x24, 0x34, 0x8b,
    0x42, 0x38, 0x89, 0x44, 0x24, 0x38, 0x8b, 0x42, 0x3c, 0x89, 0x44, 0x24, 0x3c, 0x0f, 0x1f, 0x00, 0x43, 0x0f, 0xb6, 0x84, 0x21, 0xd0,
    0x97, 0x69, 0x01, 0x41, 0x8b, 0xd3, 0x43, 0x0f, 0xb6, 0x8c, 0x21, 0x90, 0x97, 0x69, 0x01, 0x4d, 0x8d, 0x49, 0x01, 0x41, 0x33, 0xd2,
    0x23, 0xd3, 0x44, 0x8b, 0x04, 0x84, 0x41, 0x33, 0xd3, 0x44, 0x03, 0xc2, 0x47, 0x03, 0x84, 0x8c, 0x8c, 0x96, 0x69, 0x01,
  };

  constexpr uint8_t MD5UpdateSignature[] = {
    0x4d, 0x85, 0xc0, 0x0f, 0x84, 0xb7, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x48, 0x83,
    0xec, 0x20, 0x4c, 0x89, 0x74, 0x24, 0x38, 0x49, 0x8b, 0xf0, 0x45, 0x33, 0xf6, 0x48, 0x89, 0x7c, 0x24, 0x30, 0x48, 0x8b, 0xea, 0x48,
    0x8b, 0xd9, 0x66, 0x90, 0x8b, 0x53, 0x18, 0x85, 0xd2, 0x75, 0x22, 0x48, 0x83, 0xfe, 0x40, 0x72, 0x1c, 0x48, 0x8b, 0xd5, 0x48, 0x8b,
    0xcb, 0xe8, 0x48, 0xfd, 0xff, 0xff, 0x48, 0x81, 0x03, 0x00, 0x02, 0x00, 0x00, 0x48, 0x83, 0xc5, 0x40, 0x48, 0x83, 0xee, 0x40, 0xeb,
    0x49, 0xb9, 0x40, 0x00, 0x00, 0x00, 0x8b, 0xc6, 0x2b, 0xca, 0x48, 0x3b, 0xce, 0x0f, 0x46, 0xc1, 0x48, 0x8d, 0x4b, 0x1c, 0x48, 0x03,
    0xca, 0x44, 0x8b, 0xc0, 0x48, 0x8b, 0xd5, 0x8b, 0xf8, 0xe8, 0x94, 0x0c, 0x9a, 0x00, 0x01, 0x7b, 0x18, 0x48,
  };

  // Offsets of the 4-byte fields skipped when comparing: RIP-relative displacements, call targets and table offsets.
  // The MD5 transform loads the image base into r12 (lea at 0x2d), then its first round loop reads the round tables at
  // offsets from it: movzx eax, byte [r9+r12+order]; movzx ecx, byte [r9+r12+shifts]; add r8d, [r12+r9*4+sines-4] (r9
  // already counts the next step there).
  constexpr uint8_t MD5TransformImageBase = 0x30;
  constexpr uint8_t MD5TransformOrder = 0xc5;
  constexpr uint8_t MD5TransformShifts = 0xd1;
  constexpr uint8_t MD5TransformSines = 0xec;
  constexpr uint8_t MD5TransformRel32[] = {0x1f, MD5TransformImageBase, MD5TransformOrder, MD5TransformShifts, MD5TransformSines};
  constexpr uint8_t MD5UpdateCallTransform = 0x44;
  constexpr uint8_t MD5UpdateRel32[] = {MD5UpdateCallTransform, 0x78}; // call md5block; call memcpy

  struct Pattern {
    const uint8_t* Bytes;
    size_t Size;
    const uint8_t* Rel32;
    size_t Rel32Count;
  };

  constexpr Pattern MD5TransformPattern {MD5TransformSignature, sizeof(MD5TransformSignature), MD5TransformRel32, sizeof(MD5TransformRel32)};

  // Compares the first Size bytes of Code with the pattern (Size <= P.Size).
  bool Matches(const Pattern& P, const uint8_t* Code, size_t Size) {
    for (size_t i = 0; i < Size; ++i) {
      bool Skip = false;
      for (size_t f = 0; f < P.Rel32Count; ++f) {
        Skip |= i >= P.Rel32[f] && i < P.Rel32[f] + 4u;
      }
      if (!Skip && Code[i] != P.Bytes[i]) {
        return false;
      }
    }
    return true;
  }

  int32_t Rel32At(const uint8_t* Code, size_t Offset) {
    int32_t Value;
    memcpy(&Value, Code + Offset, sizeof(Value));
    return Value;
  }

  // Bytes always read for a compiled block, even when its page ends sooner.
  constexpr size_t PrefixSize = 16;
  constexpr size_t MaxPatternSize = 256;

  // How far from the last known place the frame wait loop's call is searched for.
  constexpr uint64_t SearchRadius = 4 * 1024 * 1024;
  // Compiled blocks looked at before giving up on functions that weren't found.
  constexpr uint32_t MaxSearchedBlocks = 300000;

  // The game's MD5 transform reads its round constants from three tables (shift amounts, message word order, sines);
  // NativeMD5.c hard-codes the standard ones, so they must match.
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
    uint32_t KnownRVA; // where the last known version had it (2026-10 update), for notes
    Pattern Code;
    void* Stub;
    FEXNativeCallStats* Stats;
    void* Entry;

    std::atomic<uint64_t> GuestRIP; // where its code was found, 0 until then
    std::atomic<TargetState> State;
    FEXNativeCallStats Prev; // Log thread only
  };

  Target Targets[] = {
    {"inflate_fast",
     Group::Zlib,
     0xBD2790,
     {InflateFastSignature, sizeof(InflateFastSignature), nullptr, 0},
     &NativeInflateFastRedirect,
     &FEXNativeInflateFastStats,
     &NativeInflateFastTarget},
    {"adler32_z", Group::Zlib, 0xBD1EF0, {Adler32ZSignature, sizeof(Adler32ZSignature), nullptr, 0}, &NativeAdler32ZRedirect, &FEXNativeAdler32ZStats, &NativeAdler32ZTarget},
    {"md5",
     Group::MD5,
     0xBC17F0,
     {MD5UpdateSignature, sizeof(MD5UpdateSignature), MD5UpdateRel32, sizeof(MD5UpdateRel32)},
     &NativeMD5UpdateRedirect,
     &FEXNativeMD5UpdateStats,
     &NativeMD5UpdateTarget},
    {"md5block", Group::MD5, 0xBC1580, MD5TransformPattern, &NativeMD5TransformRedirect, &FEXNativeMD5TransformStats, &NativeMD5TransformTarget},
  };

  bool ZlibEnabled {};
  bool MD5Enabled {};
  // Drops the JIT's code for a guest range, so the next call asks Lookup again.
  void (*InvalidateCode)(uint64_t Address, uint64_t Size) {};
  // An MD5 target was found but its round tables couldn't be read yet: PreCompile checks again every 50 ms.
  std::atomic<bool> MD5Pending {};
  std::atomic<uint64_t> MD5NextRetry {};
  uint32_t MD5Retries {};
  bool UnpackCacheRequested {};
  std::mutex UnpackCacheLock;
  std::atomic<bool> UnpackCacheOpened {};
  uint64_t ImageBase {};
  uint64_t ImageEnd {};
  // True until every switched-on function is found (or LookupByCode gives up).
  std::atomic<bool> Searching {};
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

  enum class ScanResult {
    Found,
    NotFound,
    Unreadable, // not found, and part of the range couldn't be read
  };

  // Reads [Begin, End), limited to SearchRadius around Center, in 64 KiB chunks: the chunk holding Center first, then
  // outwards, so something a game update moved a little is found after one or two reads. Hands each chunk to Visit until
  // it returns true. Chunks overlap by Overlap bytes so a pattern across a boundary is seen whole; pages that can't be
  // read are left zero. Uses NtReadVirtualMemory, which goes through the Wine server: fine for a few searches, too slow
  // for every compiled block.
  template<typename VisitFn>
  ScanResult ScanNear(uint64_t Center, uint64_t Begin, uint64_t End, size_t Overlap, VisitFn&& Visit) {
    constexpr uint64_t ChunkSize = 64 * 1024;
    constexpr uint64_t PageSize = 4096;
    Begin = std::max(Begin, Center > SearchRadius ? Center - SearchRadius : 0);
    End = std::min(End, Center + SearchRadius);
    if (Begin >= End) {
      return ScanResult::NotFound;
    }
    Center = std::clamp(Center, Begin, End - 1);
    fextl::vector<uint8_t> Buffer(ChunkSize + Overlap);
    bool Unreadable = false;
    auto VisitChunk = [&](uint64_t Chunk) {
      const uint64_t From = std::max(Chunk, Begin);
      const uint64_t To = std::min(Chunk + ChunkSize + Overlap, End);
      const size_t Size = To - From;
      if (!Read(From, Buffer.data(), Size)) {
        memset(Buffer.data(), 0, Size);
        for (uint64_t Page = From & ~(PageSize - 1); Page < To; Page += PageSize) {
          const uint64_t PageFrom = std::max(Page, From);
          const uint64_t PageTo = std::min(Page + PageSize, To);
          if (!Read(PageFrom, Buffer.data() + (PageFrom - From), PageTo - PageFrom)) {
            Unreadable = true;
          }
        }
      }
      return Visit(From, Buffer.data(), Size);
    };
    const uint64_t First = Center & ~(ChunkSize - 1);
    const uint64_t Below = (First - (Begin & ~(ChunkSize - 1))) / ChunkSize;
    const uint64_t Above = (((End - 1) & ~(ChunkSize - 1)) - First) / ChunkSize;
    for (uint64_t Step = 0; Step <= std::max(Below, Above); ++Step) {
      if (Step <= Above && VisitChunk(First + Step * ChunkSize)) {
        return ScanResult::Found;
      }
      if (Step && Step <= Below && VisitChunk(First - Step * ChunkSize)) {
        return ScanResult::Found;
      }
    }
    return Unreadable ? ScanResult::Unreadable : ScanResult::NotFound;
  }

  // Checks the round tables the game's MD5 transform at Transform reads; NativeMD5.c hard-codes the standard ones, so
  // they must match. The 2026-10 update reordered the three tables, so their places are read from the transform's code.
  // Remembers the answer for the first transform whose tables it could read; Unreadable leaves the target unchecked, and
  // PreCompile tries again. Reads with NtReadVirtualMemory.
  std::mutex MD5TablesLock;
  uint64_t MD5TablesTransform {};
  bool MD5TablesStandard {};
  uint32_t MD5TablesUnreadable {};

  CheckResult CheckMD5Tables(uint64_t Transform) {
    std::scoped_lock Guard {MD5TablesLock};
    if (MD5TablesTransform) {
      return MD5TablesTransform == Transform && MD5TablesStandard ? CheckResult::Match : CheckResult::Mismatch;
    }
    const auto Unreadable = [&](const char* What, uint64_t Address) {
      if (MD5TablesUnreadable++ == 0) {
        FEX::Windows::StatsLogNote(fextl::fmt::format("native md5: can't read {} at {:X} yet, tries again later", What, Address));
      }
      return CheckResult::Unreadable;
    };
    uint8_t Code[sizeof(MD5TransformSignature)];
    if (!Read(Transform, Code, sizeof(Code))) {
      return Unreadable("md5block's code", Transform);
    }
    if (!Matches(MD5TransformPattern, Code, sizeof(Code))) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("native md5: md5 calls {:X}, which isn't md5block's code, md5 stays emulated: {}",
                                                    Transform, Hex(Code, sizeof(Code))));
      return CheckResult::Mismatch;
    }
    const uint64_t Base = Transform + MD5TransformImageBase + 4 + Rel32At(Code, MD5TransformImageBase);
    const uint64_t Shifts = Base + Rel32At(Code, MD5TransformShifts);
    const uint64_t Order = Base + Rel32At(Code, MD5TransformOrder);
    const uint64_t Sines = Base + Rel32At(Code, MD5TransformSines) + 4;
    uint8_t GameShifts[64], GameOrder[64];
    uint32_t GameSines[64];
    if (!Read(Shifts, GameShifts, sizeof(GameShifts)) || !Read(Order, GameOrder, sizeof(GameOrder)) || !Read(Sines, GameSines, sizeof(GameSines))) {
      return Unreadable("md5block's round tables", Shifts);
    }
    const bool ShiftsOk = memcmp(GameShifts, MD5Shifts, sizeof(MD5Shifts)) == 0;
    const bool OrderOk = memcmp(GameOrder, MD5Order, sizeof(MD5Order)) == 0;
    const bool SinesOk = memcmp(GameSines, MD5Sines, sizeof(MD5Sines)) == 0;
    MD5TablesTransform = Transform;
    MD5TablesStandard = ShiftsOk && OrderOk && SinesOk;
    FEX::Windows::StatsLogNote(
      MD5TablesStandard ?
        fextl::fmt::format("native md5: md5block reads standard MD5 round tables (shifts {:X}, order {:X}, sines {:X})", Shifts, Order, Sines) :
        fextl::fmt::format("native md5: md5block's round tables are not standard MD5 (shifts {:X} {}, order {:X} {}, sines {:X} {}), MD5 "
                           "stays emulated",
                           Shifts, ShiftsOk ? "ok" : "differ", Order, OrderOk ? "ok" : "differ", Sines, SinesOk ? "ok" : "differ"));
    return MD5TablesStandard ? CheckResult::Match : CheckResult::Mismatch;
  }

  // Checks the parts of a target beyond its code: the round tables of the MD5 transform it is or calls.
  CheckResult CheckExtra(Target& T) {
    if (T.Group != Group::MD5) {
      return CheckResult::Match;
    }
    const uint64_t RIP = T.GuestRIP.load(std::memory_order_relaxed);
    if (T.Stub == &NativeMD5TransformRedirect) {
      return CheckMD5Tables(RIP);
    }
    int32_t Call;
    if (!Read(RIP + MD5UpdateCallTransform, &Call, sizeof(Call))) {
      return CheckResult::Unreadable;
    }
    return CheckMD5Tables(RIP + MD5UpdateCallTransform + 4 + Call);
  }

  // Opens the unpack cache once the game's inflate_fast is found, so other processes started with the same settings
  // (launchers, helpers) never open the cache files. Runs before the stub is handed out, so the stub sees the cache.
  void OpenUnpackCache() {
    if (!UnpackCacheRequested || UnpackCacheOpened.load(std::memory_order_acquire)) {
      return;
    }
    std::scoped_lock Guard {UnpackCacheLock};
    if (!UnpackCacheOpened.load(std::memory_order_relaxed)) {
      const char* MD5Cache = FEX::Windows::Switch("FEX_NATIVE_MD5_CACHE");
      NativeUnpackCache::Init(MD5Enabled && !(MD5Cache && MD5Cache[0] == '0'));
      UnpackCacheOpened.store(true, std::memory_order_release);
    }
  }

  // Arms a target whose code was found.
  uintptr_t Check(Target& T) {
    const auto Result = CheckExtra(T);
    if (Result == CheckResult::Unreadable) {
      MD5Pending.store(true, std::memory_order_relaxed);
      return 0;
    }
    if (Result != CheckResult::Match) {
      T.State.store(TargetState::Rejected, std::memory_order_release);
      return 0;
    }

    if (T.Stub == &NativeInflateFastRedirect) {
      OpenUnpackCache();
    }

    auto Expected = TargetState::Unchecked;
    if (T.State.compare_exchange_strong(Expected, TargetState::Armed, std::memory_order_acq_rel)) {
      FEX::Windows::StatsLogNote(fextl::fmt::format("{}: {} at {:X} matched, runs natively from now on", GroupName(T.Group), T.Name,
                                                    T.GuestRIP.load(std::memory_order_relaxed)));
    }
    return T.State.load(std::memory_order_acquire) == TargetState::Armed ? reinterpret_cast<uintptr_t>(T.Stub) : 0;
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
      ImageEnd = FEXNativeTextEnd;
      return;
    }
    ImageEnd = Base + Nt.OptionalHeader.SizeOfImage;
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

  // D2R's main thread waits for a frame's parallel tasks in a loop that never sleeps (addresses below are from the last
  // known version; the code is found wherever an update puts it):
  //
  //   do { JobSystem()->Queue->TryRunJob(priority 2) } while (finished task bits != all tasks)
  //
  // TryRunJob (0x1407B7A90) takes the queue's spinlock (xchg + pause), looks at the priority 2 list and releases the
  // lock. With nothing to run, the loop retakes the lock every few hundred nanoseconds. A worker that wants to queue a
  // job (0x1407B75A0) has to win the same lock between two of those, and on these cores it can lose for hundreds of
  // milliseconds: build 41 logs show the worst frames (190-570 ms) as Main spinning here while one worker spends the
  // whole time in that lock. The loop asks for the job system singleton (0x1407B7940, an MSVC thread-safe static) right
  // before each TryRunJob. Running that getter natively lets the wait loop's call pause for a moment when the priority 2
  // list is empty, so the lock stays free for the workers; every other caller just gets the object back.
  namespace JobWait {
    // Where the last known version had the getter and the wait loop's call of it: the call is searched for starting
    // there (moved by as much as the getter moved).
    constexpr uint32_t KnownGetterRVA = 0x7B7940;
    constexpr uint32_t KnownCallSiteRVA = 0x876948;
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
    // The getter's RIP-relative displacements and call targets: its init guard (read at 0x1a, 0x27, 0x32 and 0xa5), the
    // object it returns (0xb1) and the object's fields, the CRT's init and atexit helpers.
    constexpr uint8_t GetterRel32[] = {0x1a, 0x27, 0x2c, 0x32, 0x3d, 0x45, 0x4c, 0x53, 0x5a, 0x6b,
                                       0x72, 0x7c, 0x84, 0x8b, 0x92, 0x99, 0x9e, 0xa5, 0xaa, 0xb1};
    // Each guard reference: the displacement's offset and the end of its instruction.
    constexpr std::pair<uint8_t, uint8_t> GuardRefs[] = {{0x1a, 0x1e}, {0x27, 0x2b}, {0x32, 0x37}, {0xa5, 0xa9}};
    constexpr std::pair<uint8_t, uint8_t> ObjectRef {0xb1, 0xb5};
    constexpr uint8_t CallSiteCode[] = {
      0xe8, 0x03, 0x11, 0xf4, 0xff, 0x41, 0xb1, 0xff, 0x45, 0x8b, 0xc4, 0x48, 0x8b, 0x90, 0xa0, 0x01, 0x00, 0x00, 0xe8, 0x41, 0x12,
      0xf4, 0xff, 0x8b, 0x97, 0xe8, 0x60, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0xc0, 0x5e, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x48,
      0xd3, 0xe0, 0xff, 0xc8, 0x3b, 0xd0, 0x75, 0xce,
    };
    constexpr uint8_t CallSiteRel32[] = {0x01, 0x13}; // call getter; call TryRunJob (which ends at 0x17)
    constexpr uint8_t TryRunCode[] = {
      0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x42, 0x10, 0x4c, 0x8b, 0xda, 0x48, 0x87, 0x42, 0x10, 0x48, 0x85, 0xc0, 0x74, 0x18, 0xf3,
      0x90, 0x48, 0x8b, 0x42, 0x10, 0x48, 0x85, 0xc0, 0x75, 0xf5, 0x48, 0x8d, 0x42, 0x10, 0x48, 0x87, 0x42, 0x10, 0x48, 0x85, 0xc0,
      0x75, 0xe8, 0x4d, 0x63, 0xc0, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x49, 0x83, 0xf8, 0x02, 0x7f, 0x30, 0xba, 0x02, 0x00, 0x00, 0x00,
      0x49, 0x8d, 0x4b, 0x38,
    };

    constexpr Pattern Getter {GetterCode, sizeof(GetterCode), GetterRel32, sizeof(GetterRel32)};
    constexpr Pattern CallSite {CallSiteCode, sizeof(CallSiteCode), CallSiteRel32, sizeof(CallSiteRel32)};
    // Searches for the wait loop when its page couldn't be read, at most every 2 seconds.
    constexpr uint32_t MaxCallSiteScans = 10;

    bool Enabled {};
    uint64_t BackoffMicroseconds {};
    std::atomic<uint64_t> GetterAddress {}; // where the getter's code was found, 0 until then
    uint64_t GuardAddress {};               // its init guard: 0 = not constructed, -1 = being constructed, else epoch
    std::mutex CheckLock;
    bool CallSiteDone {};
    uint32_t CallSiteScans {};
    uint64_t LastCallSiteScan {};
    std::atomic<TargetState> State {TargetState::Unchecked};
    std::atomic<bool> Pending {};      // waiting for the object to be constructed or the code to be readable
    std::atomic<uint64_t> NextRetry {}; // tick of the next PreCompile check while Pending
    uint32_t Retries {};
    void (*Invalidate)(uint64_t Address, uint64_t Size) {};
  } // namespace JobWait

  // D2R's main thread starts every frame in one function (0x1408764C0 in the 2026-10 version) that first reads the clock:
  // lea rcx, [rsp+0x20]; call [QueryPerformanceCounter]. Where that call returns to marks a frame start, so the stats
  // log can time every frame without touching the game's code (Module.S, ExitFunctionEC). FEX_FRAMETIMES=0 turns it off.
  namespace FrameClock {
    constexpr uint8_t Code[] = {
      0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9, 0x4c, 0x89, 0xa4, 0x24, 0xb8,
      0x00, 0x00, 0x00, 0x48, 0x8d, 0x4c, 0x24, 0x20, 0xff, 0x15, 0xbe, 0x93, 0xd5, 0x00, 0x48, 0x8b, 0x5c, 0x24, 0x20, 0xff,
      0x15, 0xe3, 0x73, 0x49, 0x01, 0x33, 0xd2, 0x48, 0x8d, 0x2d, 0xd2, 0x5f, 0x48, 0x01, 0x4c, 0x8b, 0xc0, 0x33, 0xf6, 0x48,
      0x8b, 0xc3, 0x49, 0xf7, 0xf0, 0x44, 0x8d, 0x66, 0x02, 0x48, 0x8b, 0xc8, 0x48, 0x69, 0xc2, 0x00, 0xca, 0x9a, 0x3b, 0x33,
      0xd2, 0x49, 0xf7, 0xf0, 0x48, 0x8b, 0xd8, 0x48, 0x69, 0xc1, 0x00, 0xca, 0x9a, 0x3b, 0x48, 0x03, 0xd8,
    };
    // call [QueryPerformanceCounter]; call [the counter frequency]; lea rbp, [...]
    constexpr uint8_t Rel32[] = {0x1e, 0x29, 0x32};
    constexpr Pattern Function {Code, sizeof(Code), Rel32, sizeof(Rel32)};
    constexpr uint64_t ClockCallReturn = 0x22;
    constexpr uint32_t KnownRVA = 0x8764C0;

    bool Enabled {};
    std::atomic<uint64_t> Address {};
  } // namespace FrameClock

  uint64_t JobWaitObject {}; // what the getter returns, from its code
  uint64_t JobWaitReturn {}; // the wait loop's return address from its call of the getter; 0 if not found

  uint64_t NowTicks() {
    uint64_t Ticks;
    __asm volatile("mrs %[Res], CNTVCT_EL0" : [Res] "=r"(Ticks));
    return Ticks;
  }

  // Finds the wait loop's call of the getter: the call, then the constant part of the loop, then a call of code that is
  // TryRunJob. Its return address tells the wait loop's calls apart from everyone else's.
  ScanResult FindWaitLoop(uint64_t Getter) {
    const uint64_t Expected = ImageBase + JobWait::KnownCallSiteRVA + (Getter - (ImageBase + JobWait::KnownGetterRVA));
    constexpr size_t TailOffset = 5, TailSize = 14; // 41 b1 ff ... e8: everything between the two calls
    return ScanNear(
      Expected, FEXNativeTextBegin, FEXNativeTextEnd, sizeof(JobWait::CallSiteCode), [&](uint64_t At, const uint8_t* Data, size_t Size) {
        for (size_t i = 0; i + sizeof(JobWait::CallSiteCode) <= Size; ++i) {
          if (Data[i] != 0xe8 || memcmp(Data + i + TailOffset, JobWait::CallSiteCode + TailOffset, TailSize) != 0) {
            continue;
          }
          const uint64_t Call = At + i;
          if (Call + 5 + Rel32At(Data, i + 1) != Getter || !Matches(JobWait::CallSite, Data + i, sizeof(JobWait::CallSiteCode))) {
            continue;
          }
          const uint64_t TryRun = Call + 0x17 + Rel32At(Data, i + 0x13);
          uint8_t Code[sizeof(JobWait::TryRunCode)];
          if (!Read(TryRun, Code, sizeof(Code)) || memcmp(Code, JobWait::TryRunCode, sizeof(Code)) != 0) {
            continue;
          }
          JobWaitReturn = Call + 5;
          FEX::Windows::StatsLogNote(fextl::fmt::format("native jobwait: frame wait loop calls the getter at {:X} (last known at {:X}), "
                                                        "TryRunJob at {:X}",
                                                        Call, ImageBase + JobWait::KnownCallSiteRVA, TryRun));
          return true;
        }
        return false;
      });
  }

  CheckResult CheckJobWait() {
    std::scoped_lock Lock {JobWait::CheckLock};
    // Not constructed yet: the getter would have to run the constructor, which only the guest code can do. Once it is,
    // MSVC stores the global init epoch there, which counts up from INT_MIN.
    int32_t Guard {};
    if (!Read(JobWait::GuardAddress, &Guard, sizeof(Guard)) || Guard == 0 || Guard == -1) {
      return CheckResult::Unreadable;
    }
    if (!JobWait::CallSiteDone) {
      const uint64_t Now = NowTicks();
      if (JobWait::CallSiteScans && Now - JobWait::LastCallSiteScan < 2 * TickFrequency) {
        return CheckResult::Unreadable;
      }
      JobWait::LastCallSiteScan = Now;
      const auto Result = FindWaitLoop(JobWait::GetterAddress.load(std::memory_order_acquire));
      if (Result == ScanResult::Unreadable && ++JobWait::CallSiteScans < JobWait::MaxCallSiteScans) {
        return CheckResult::Unreadable;
      }
      JobWait::CallSiteDone = true;
      if (Result != ScanResult::Found) {
        // The getter still runs natively for everyone, as it did for the other callers in build 42; only the wait
        // loop's pause and its jobwait line need the loop.
        FEX::Windows::StatsLogNote(fextl::fmt::format("native jobwait: frame wait loop not found within {} MB{}, the getter runs natively "
                                                      "for every caller, no jobwait line",
                                                      SearchRadius >> 20, Result == ScanResult::Unreadable ? " (some pages unreadable)" : ""));
      }
    }
    return CheckResult::Match;
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
                                                      JobWait::GetterAddress.load(std::memory_order_relaxed), JobWait::BackoffMicroseconds));
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
  const char* Zlib = FEX::Windows::Switch("FEX_NATIVE_ZLIB");
  const char* MD5 = FEX::Windows::Switch("FEX_NATIVE_MD5");
  ZlibEnabled = Zlib && Zlib[0] == '1';
  MD5Enabled = MD5 && MD5[0] == '1';
  ImageBase = MainImageBase;
  InvalidateCode = Invalidate;
  for (auto& T : Targets) {
    if (StatsLog) {
      FEX::Windows::StatsLogNameNative(reinterpret_cast<uint64_t>(T.Entry), fextl::fmt::format("fex:{}", T.Name));
    }
  }
  uint64_t Frequency {};
  __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
  TickFrequency = Frequency ? Frequency : 1;

  {
    const char* JobWaitSwitch = FEX::Windows::Switch("FEX_JOBWAIT");
    const char* Backoff = FEX::Windows::Switch("FEX_JOBWAIT_BACKOFF");
    JobWait::Enabled = !(JobWaitSwitch && JobWaitSwitch[0] == '0') && Invalidate;
    JobWait::BackoffMicroseconds = Backoff && Backoff[0] ? std::min<uint64_t>(strtoull(Backoff, nullptr, 10), 1000) : 0;
    JobWait::Invalidate = Invalidate;
    JobWaitBackoff = JobWait::BackoffMicroseconds ? std::max<uint64_t>(JobWait::BackoffMicroseconds * TickFrequency / 1000000, 1) : 0;
    JobWaitEpisodeGap = std::max<uint64_t>(TickFrequency / 5000, 1); // 200 us
    if (StatsLog) {
      FEX::Windows::StatsLogNameNative(reinterpret_cast<uint64_t>(&NativeJobSystemGetTarget), "fex:jobsystem");
    }
  }

  FindText(MainImageBase);
  if (StatsLog && ZlibEnabled) {
    FEXNativeZlibTrackStreams = 1;
  }
  if (const char* FrameTimes = FEX::Windows::Switch("FEX_FRAMETIMES"); StatsLog && !(FrameTimes && FrameTimes[0] == '0')) {
    FrameClock::Enabled = true;
  }
  if (FrameClock::Enabled) {
    FEX::Windows::StatsLogSetFrameMarks(FEXFrameMarks, FrameMarkRing);
  }
  Searching.store(ZlibEnabled || MD5Enabled || JobWait::Enabled || FrameClock::Enabled, std::memory_order_relaxed);

  // On unless set to 0.
  auto DefaultOn = [](const char* Name) {
    const char* Value = FEX::Windows::Switch(Name);
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
  if (StatsLog) {
    FEX::Windows::StatsLogNote(FrameClock::Enabled ? "frames: on, FEX_FRAMETIMES=0 turns it off" : "frames: off (FEX_FRAMETIMES=0)");
  }
  FEX::Windows::StatsLogNote(JobWait::Enabled ? fextl::fmt::format("native jobwait: on, pause {} us (FEX_JOBWAIT_BACKOFF), FEX_JOBWAIT=0 turns it off",
                                                                   JobWait::BackoffMicroseconds) :
                                                "native jobwait: off (FEX_JOBWAIT=0)");
  if (FEXNativeZlibTrackStreams) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("native zlib: tracking streams, code range {:X}-{:X}", FEXNativeTextBegin, FEXNativeTextEnd));
  }
  if (const char* Cache = FEX::Windows::Switch("FEX_UNPACK_CACHE"); Cache && Cache[0] == '1') {
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

namespace {
  // Checks the MD5 targets waiting for readable round tables again, and drops the emulated code of those that now match.
  void RetryMD5() {
    const uint64_t Now = NowTicks();
    uint64_t Due = MD5NextRetry.load(std::memory_order_relaxed);
    if (Now < Due || !MD5NextRetry.compare_exchange_strong(Due, Now + TickFrequency / 20, std::memory_order_relaxed)) {
      return;
    }
    // Only this thread got past the exchange for this retry.
    MD5Pending.store(false, std::memory_order_relaxed);
    const bool GiveUp = ++MD5Retries > 2000;
    for (auto& T : Targets) {
      const uint64_t RIP = T.GuestRIP.load(std::memory_order_acquire);
      if (T.Group != Group::MD5 || !RIP || T.State.load(std::memory_order_acquire) != TargetState::Unchecked) {
        continue;
      }
      if (GiveUp) {
        T.State.store(TargetState::Rejected, std::memory_order_release);
        FEX::Windows::StatsLogNote(fextl::fmt::format("native md5: the round tables never became readable, {} stays emulated", T.Name));
      } else if (Check(T) && InvalidateCode) {
        InvalidateCode(RIP, 1);
      }
    }
  }
} // namespace

void PreCompile() {
  if (MD5Pending.load(std::memory_order_relaxed)) {
    RetryMD5();
  }
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
  JobWait::Invalidate(JobWait::GetterAddress.load(std::memory_order_relaxed), 1);
}

uintptr_t Lookup(uint64_t GuestRIP) {
  if (JobWait::Enabled && GuestRIP == JobWait::GetterAddress.load(std::memory_order_acquire)) {
    return LookupJobWait();
  }
  for (auto& T : Targets) {
    if (T.GuestRIP.load(std::memory_order_acquire) != GuestRIP) {
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

namespace {
  std::atomic<uint32_t> SearchedBlocks {};

  fextl::string NotFound() {
    fextl::string Names;
    for (auto& T : Targets) {
      if (Enabled(T.Group) && !T.GuestRIP.load(std::memory_order_relaxed)) {
        Names += fextl::fmt::format("{}{}", Names.empty() ? "" : ", ", T.Name);
      }
    }
    if (JobWait::Enabled && !JobWait::GetterAddress.load(std::memory_order_relaxed)) {
      Names += fextl::fmt::format("{}job system getter", Names.empty() ? "" : ", ");
    }
    if (FrameClock::Enabled && !FrameClock::Address.load(std::memory_order_relaxed)) {
      Names += fextl::fmt::format("{}frame function", Names.empty() ? "" : ", ");
    }
    return Names;
  }

  void FoundOne(const char* Group, const char* Name, uint64_t Address, uint64_t KnownAddress) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("{}: {} found at {:X} (last known at {:X})", Group, Name, Address, KnownAddress));
    if (NotFound().empty() && Searching.exchange(false, std::memory_order_relaxed)) {
      FEX::Windows::StatsLogNote(
        fextl::fmt::format("native: found everything after {} compiled blocks", SearchedBlocks.load(std::memory_order_relaxed)));
    }
  }

  // Copies guest code inside the compiler's fault recovery: a read of a page the protector keeps inaccessible restarts
  // the compile with that page recorded as FaultPage, and the next try leaves it alone.
  bool CopyCode(uint64_t Address, void* Buffer, size_t Size, uint64_t FaultPage) {
    if (FaultPage && FaultPage >= (Address & ~uint64_t {4095}) && FaultPage < Address + Size) {
      return false;
    }
    memcpy(Buffer, reinterpret_cast<const void*>(Address), Size);
    return true;
  }

  uintptr_t FoundGetter(uint64_t GuestRIP, const uint8_t* Code) {
    // Every reference to the init guard must point at the same place.
    uint64_t Guard {};
    for (const auto& [Field, End] : JobWait::GuardRefs) {
      const uint64_t Ref = GuestRIP + End + Rel32At(Code, Field);
      if (Guard && Ref != Guard) {
        return 0;
      }
      Guard = Ref;
    }
    {
      std::scoped_lock Lock {JobWait::CheckLock};
      const uint64_t Known = JobWait::GetterAddress.load(std::memory_order_relaxed);
      if (Known) {
        if (Known != GuestRIP) {
          return 0;
        }
      } else {
        JobWait::GuardAddress = Guard;
        JobWaitObject = GuestRIP + JobWait::ObjectRef.second + Rel32At(Code, JobWait::ObjectRef.first);
        JobWait::GetterAddress.store(GuestRIP, std::memory_order_release);
        FEX::Windows::StatsLogNote(fextl::fmt::format("native jobwait: guard at {:X}, job system at {:X}", Guard, JobWaitObject));
        FoundOne("native jobwait", "getter", GuestRIP, ImageBase + JobWait::KnownGetterRVA);
      }
    }
    return LookupJobWait();
  }
} // namespace

uintptr_t LookupByCode(uint64_t GuestRIP, uint64_t FaultPage) {
  if (!Searching.load(std::memory_order_relaxed) || GuestRIP < FEXNativeTextBegin || GuestRIP + MaxPatternSize > FEXNativeTextEnd) {
    return 0;
  }
  if (SearchedBlocks.fetch_add(1, std::memory_order_relaxed) + 1 == MaxSearchedBlocks) {
    Searching.store(false, std::memory_order_relaxed);
    FEX::Windows::StatsLogNote(
      fextl::fmt::format("native: stopped looking after {} compiled blocks, not found: {}", MaxSearchedBlocks, NotFound()));
    return 0;
  }

  // Read what the page holds first: common prologues match the first bytes of a pattern, and reading on into the next
  // page for those could fault where the decoder would never have read.
  uint8_t Code[MaxPatternSize];
  size_t Have = std::max(PrefixSize, std::min<size_t>(MaxPatternSize, 4096 - (GuestRIP & 4095)));
  if (!CopyCode(GuestRIP, Code, Have, FaultPage)) {
    return 0;
  }
  auto Found = [&](const Pattern& P) {
    if (!Matches(P, Code, std::min(P.Size, Have))) {
      return false;
    }
    if (P.Size > Have) {
      if (!CopyCode(GuestRIP, Code, P.Size, FaultPage)) {
        return false;
      }
      Have = P.Size;
    }
    return Matches(P, Code, P.Size);
  };
  for (auto& T : Targets) {
    if (!Enabled(T.Group) || T.GuestRIP.load(std::memory_order_acquire) || !Found(T.Code)) {
      continue;
    }
    uint64_t Expected = 0;
    if (!T.GuestRIP.compare_exchange_strong(Expected, GuestRIP, std::memory_order_acq_rel)) {
      // Another thread found it first; a second copy of the same code stays emulated.
      return Expected == GuestRIP ? Lookup(GuestRIP) : 0;
    }
    FoundOne(GroupName(T.Group), T.Name, GuestRIP, ImageBase + T.KnownRVA);
    return Check(T);
  }
  if (JobWait::Enabled && !JobWait::GetterAddress.load(std::memory_order_acquire) && Found(JobWait::Getter)) {
    return FoundGetter(GuestRIP, Code);
  }
  if (FrameClock::Enabled && !FrameClock::Address.load(std::memory_order_acquire) && Found(FrameClock::Function)) {
    uint64_t Expected = 0;
    if (FrameClock::Address.compare_exchange_strong(Expected, GuestRIP, std::memory_order_acq_rel)) {
      __atomic_store_n(&FEXFrameMarkReturn, GuestRIP + FrameClock::ClockCallReturn, __ATOMIC_RELEASE);
      FEX::Windows::StatsLogNote(
        fextl::fmt::format("frames: timing every frame from the clock read at {:X}", GuestRIP + FrameClock::ClockCallReturn - 6));
      FoundOne("frames", "frame function", GuestRIP, ImageBase + FrameClock::KnownRVA);
    }
  }
  return 0;
}
} // namespace NativeRedirect

// The job system getter, run in place of the game's once its object exists (see JobWait). Every caller gets the
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
