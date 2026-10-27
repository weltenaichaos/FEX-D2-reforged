// SPDX-License-Identifier: MIT
/*
$info$
category: code buffer ~ Thread shared code buffer management
tags: backend|shared
$end_info$
*/
#pragma once

#include <FEXCore/fextl/memory.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FEXCore {
struct GuestToHostMap;
}

namespace FEXCore::CPU {
// What a block of code from the disk cache was loaded for: the guest code's address, the hash and size of the guest
// bytes it translates, and its host size. Equal keys mean the same translation of the same bytes at the same address,
// so code placed for one key can serve it again (GuestToHostMap::FindPlaced).
struct PlacedCodeKey {
  uint64_t GuestRIP;
  uint64_t HashLow;
  uint64_t HashHigh;
  uint32_t GuestSize;
  uint32_t HostSize;

  bool operator==(const PlacedCodeKey&) const = default;
  uint64_t Hash() const {
    uint64_t H = GuestRIP * 0x9E3779B97F4A7C15ULL;
    H ^= HashLow + 0x632BE59BD9B4E019ULL + (H << 6) + (H >> 2);
    H ^= HashHigh + 0x85EBCA77C2B2AE63ULL + (H << 6) + (H >> 2);
    H ^= (uint64_t {GuestSize} << 32 | HostSize) + (H << 6) + (H >> 2);
    return H;
  }
};

// What a translation was made from (build 95, FEX_JIT_REUSE): the guest address it starts at, a 128-bit hash of the
// decoded guest code (every decoded block's address, size, instruction count, decode result and whether its code checks
// its own bytes, then the bytes themselves), the decoded size, and Flags (TranslatedFlag*). Equal keys decode to the same
// instructions with the same checks, so they translate to the same host code, and a translation made for one key can
// serve it again (GuestToHostMap::FindTranslated).
struct TranslatedCodeKey {
  uint64_t GuestRIP;
  uint64_t HashLow;
  uint64_t HashHigh;
  uint32_t GuestSize;
  uint32_t Flags;

  bool operator==(const TranslatedCodeKey&) const = default;
};
// TranslatedCodeKey::Flags
constexpr uint32_t TranslatedFlag64Bit = 1U << 0;      // decoded in 64-bit mode
constexpr uint32_t TranslatedFlagTrapFlag = 1U << 1;   // EFLAGS.TF was set: the code checks it before each instruction
constexpr uint32_t TranslatedFlagPatchable = 1U << 2;  // the disk cache was on: guest values went in as patchable literals
constexpr uint32_t TranslatedFlagSelfCheck = 1U << 3;  // some of its blocks check their own bytes (on a hot SMC page)

struct CodeBuffer {
  fextl::unique_ptr<GuestToHostMap> LookupCache;

  CodeBuffer(size_t Size, bool ShouldBeNamed);
  CodeBuffer(const CodeBuffer&) = delete;
  CodeBuffer& operator=(const CodeBuffer&) = delete;
  CodeBuffer(CodeBuffer&& oth) = delete;
  CodeBuffer& operator=(CodeBuffer&&) = delete;

  ~CodeBuffer();

  // Blocks placed in this buffer, and how many of them were loaded from the disk cache (for the frontend's notes).
  std::atomic<uint32_t> Blocks {};
  std::atomic<uint32_t> CachedBlocks {};

  // Atomically allocate a fixed size buffer out of the current allocated codebuffer.
  // Lockless because it's just a linear allocator.
  struct CodeBufferAllocation {
    const uint8_t* BufferBase;
    uint8_t* BufferAllocationOffset;
  };

  CodeBufferAllocation AtomicAllocateBuffer(size_t Size) {
    Size = FEXCore::AlignUp(Size, 16);
    LOGMAN_THROW_A_FMT(reinterpret_cast<uintptr_t>(CodeBufferOffset.load()) % 16 == 0, "Buffer needs to always be 16B aligned!");

    auto ExpectedOffset = CodeBufferOffset.load(std::memory_order_relaxed);
    auto DesiredOffset = ExpectedOffset + Size;

    if (DesiredOffset > CodeBufferEnd) {
      // Couldn't fit.
      return {};
    }

    while (!CodeBufferOffset.compare_exchange_strong(ExpectedOffset, DesiredOffset)) {
      DesiredOffset = ExpectedOffset + Size;

      if (DesiredOffset > CodeBufferEnd) {
        // Couldn't fit.
        return {};
      }
    }

    // Managed to fit.
    return {
      .BufferBase = Ptr,
      .BufferAllocationOffset = ExpectedOffset,
    };
  }

  // Returns the total number of bytes available for storing code
  size_t UsableSize() const {
    return AllocatedSize - FEXCore::Utils::FEX_PAGE_SIZE;
  }

  // Returns the full size of the buffer, including the guard page.
  size_t TotalAllocationSize() const {
    return AllocatedSize;
  }

  // Returns the num of bytes currently allocated from the allocator.
  size_t AllocatedSpaceUsed() const {
    return CodeBufferOffset - Ptr;
  }

  // Trivially reset the allocator.
  void Reset() {
    CodeBufferOffset = Ptr;
  }

  // Returns the base of the buffer.
  uint8_t* GetBufferBase() const {
    return Ptr;
  }

private:
  uint8_t* Ptr;
  uint8_t* CodeBufferEnd;
  size_t AllocatedSize; // including guard page; see UsableSize()

  // Code buffer allocation information.
  std::atomic<uint8_t*> CodeBufferOffset {};
};

/**
 * A manager that coordinates access to the CodeBuffer used for compiling new code across threads.
 *
 * The CodeBuffer is managed as a partially persistent data structure:
 * - Exactly one CodeBuffer is now designated as "active", which means data can be appended to it
 * - Lossy modifications to the active CodeBuffer will not invalidate any data in use by other threads (which is what enables save CodeBuffer sharing across threads)
 * - Instead, such lossy modifications trigger a new "version" of the data in the modifying thread. Old versions of the CodeBuffer persist as read-only data for use by the other threads.
 * - The other threads can update their version of the CodeBuffer. This will decrease the reference count and eventually trigger deallocation of the old version
 */
class SharedCodeBufferManager {
public:
  SharedCodeBufferManager();
  virtual ~SharedCodeBufferManager() = default;

  // Get the CodeBuffer that was most recently allocated.
  // This is the only CodeBuffer that data may be written to.
  fextl::shared_ptr<CodeBuffer> GetLatest();

  // Allocate a new CodeBuffer with geometric growth up to an internal maximum.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartLargerCodeBuffer();

  // Allocate a new CodeBuffer with maximum internal size.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartMaximalCodeBuffer();

  // Why a buffer was started: the first one, the latest one was full, a renewal the frontend asked for (see
  // RequestRenewal), or the start size changed before any code was placed (UseMaximal).
  enum class StartReason : uint8_t { First, Full, Renewed, Resized };
  struct StartInfo {
    StartReason Reason;
    uint32_t Number;         // buffers started so far, this one included
    uint64_t Size;           // this buffer's usable size
    uint64_t PrevSize;       // the buffer it replaced: usable size, bytes used and blocks placed (0 for the first)
    uint64_t PrevUsed;
    uint32_t PrevBlocks;
    uint32_t PrevCachedBlocks;
  };
  virtual void OnCodeBufferAllocated(const std::shared_ptr<CodeBuffer>&, const StartInfo&) {};

  // Placing disk cache code again that is still in the buffer. A guest that changes the protection of its code pages
  // (or rewrites them with the same bytes) makes FEX drop the blocks there; when the same code runs again it is found
  // in the disk cache and, without this, copied into the buffer once more while the old copy stays as waste.
  void SetReusePlaced(bool Enable) {
    ReusePlaced.store(Enable, std::memory_order_relaxed);
  }
  bool ReusePlacedCode() const {
    return ReusePlaced.load(std::memory_order_relaxed);
  }

  // For the frontend's log: the latest buffer's number, usable size and bytes used, and counts since the process
  // started: blocks placed after translating them and after loading them from the disk cache, disk cache blocks served by
  // code already in the buffer (and the bytes not copied), and blocks dropped from the lookup maps by invalidations.
  struct Use {
    uint32_t Number;
    uint64_t Size;
    uint64_t Used;
    uint64_t Translated;
    uint64_t Loaded;
    uint64_t Reused;
    uint64_t ReusedBytes;
    uint64_t Dropped;
    // Build 94: bytes placed after translating / loading, and the placements of LargeMin bytes or more.
    uint64_t TranslatedBytes;
    uint64_t LoadedBytes;
    uint64_t Large;
    uint64_t LargeBytes;
    // Build 95: translations mapped again (count, bytes, how many of them check their own bytes), translations at an
    // address translated before from other bytes, and self-check failures that dropped what was kept for an address.
    uint64_t ReusedTranslated;
    uint64_t ReusedTranslatedBytes;
    uint64_t ReusedSelfCheck;
    uint64_t TranslatedChanged;
    uint64_t TranslatedForgotten;
  };
  Use GetUse() const;
  // A thread placed Size bytes of code in Buffer (Loaded: from the disk cache, else just translated) for the guest code
  // at GuestRIP (GuestSize bytes of it; 0 when not known).
  void NotePlaced(const CodeBuffer* Buffer, bool Loaded, size_t Size, uint64_t GuestRIP, uint32_t GuestSize);
  // Build 94: placement Index (0 = the first of LargeMin bytes or more) if it is still in the ring and written.
  struct LargePlacement {
    uint64_t Size;
    uint64_t GuestRIP;
    uint32_t GuestSize;
    bool Loaded;
  };
  static constexpr size_t LargeMin = 64 * 1024;
  bool GetLarge(uint64_t Index, LargePlacement& Out) const;
  void NoteReused(size_t Bytes) {
    ReusedTotal.fetch_add(1, std::memory_order_relaxed);
    ReusedBytesTotal.fetch_add(Bytes, std::memory_order_relaxed);
  }
  void NoteDropped(size_t Blocks) {
    DroppedTotal.fetch_add(Blocks, std::memory_order_relaxed);
  }

  // Build 95 (FEX_JIT_REUSE): mapping a translation again that is still in the current buffer, when the guest code it was
  // made from comes back the same after its blocks were dropped (a guest that writes into its code pages, or changes
  // their protection, without changing the code). Without this, the same code is translated once more each time, and the
  // old translation stays in the buffer as waste.
  void SetReuseTranslated(bool Enable) {
    ReuseTranslated.store(Enable, std::memory_order_relaxed);
  }
  bool ReuseTranslatedCode() const {
    return ReuseTranslated.load(std::memory_order_relaxed);
  }
  // A translation mapped again (Bytes: its size); SelfCheck: one of its blocks checks its own bytes.
  void NoteReusedTranslated(size_t Bytes, bool SelfCheck) {
    ReusedTranslatedTotal.fetch_add(1, std::memory_order_relaxed);
    ReusedTranslatedBytesTotal.fetch_add(Bytes, std::memory_order_relaxed);
    if (SelfCheck) {
      ReusedSelfCheckTotal.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Translating at an address translated before in this buffer, but from other bytes (or other checks): Older says the
  // two last translations kept there were both different.
  void NoteTranslatedChanged() {
    TranslatedChangedTotal.fetch_add(1, std::memory_order_relaxed);
  }
  // A translation that checks its own bytes found them changed, and the translations kept for its address were dropped.
  void NoteTranslatedForgotten() {
    TranslatedForgottenTotal.fetch_add(1, std::memory_order_relaxed);
  }

  // Start at the largest size instead of growing to it: every growth step starts a new buffer, which drops all code.
  void UseMaximal();

  // The next code allocation of any thread within ValidMs starts a fresh buffer when the current one is at least
  // MinUsedPercent used (TakeRenewal says so). A frontend asks at a moment where reloading code is hidden, such as a
  // loading screen, so the buffer doesn't run full (and drop everything) in the middle of play. A later request replaces
  // an earlier one.
  void RequestRenewal(uint32_t MinUsedPercent, uint32_t ValidMs);
  // Called by a thread about to place code in Current, its reference to the latest buffer: whether to start a fresh one
  // instead. Only one thread takes a request.
  bool TakeRenewal(const CodeBuffer& Current);

private:
  fextl::shared_ptr<CodeBuffer> Latest;

  fextl::shared_ptr<CodeBuffer> AllocateNew(size_t Size, StartReason Reason);

  // MinUsedPercent << 56 | the cycle counter it expires at (low 56 bits); 0 = no request.
  std::atomic<uint64_t> RenewRequest {};
  // Set by TakeRenewal, read by the AllocateNew that follows it.
  std::atomic<bool> RenewTaken {};
  std::atomic<uint32_t> Started {};

  std::atomic<bool> ReusePlaced {};
  std::atomic<bool> ReuseTranslated {};
  std::atomic<uint64_t> ReusedTranslatedTotal {};
  std::atomic<uint64_t> ReusedTranslatedBytesTotal {};
  std::atomic<uint64_t> ReusedSelfCheckTotal {};
  std::atomic<uint64_t> TranslatedChangedTotal {};
  std::atomic<uint64_t> TranslatedForgottenTotal {};
  // The latest buffer, only compared against (never followed: the log thread reads these without the buffer's lock).
  std::atomic<const CodeBuffer*> LatestRaw {};
  std::atomic<uint64_t> LatestSize {};
  std::atomic<uint64_t> LatestUsed {};
  std::atomic<uint64_t> TranslatedTotal {};
  std::atomic<uint64_t> LoadedTotal {};
  std::atomic<uint64_t> ReusedTotal {};
  std::atomic<uint64_t> ReusedBytesTotal {};
  std::atomic<uint64_t> DroppedTotal {};
  std::atomic<uint64_t> TranslatedBytesTotal {};
  std::atomic<uint64_t> LoadedBytesTotal {};
  std::atomic<uint64_t> LargeBytesTotal {};
  // Placements of LargeMin bytes or more, for the log (CodeBuffer.cpp's '# code buffer' lines).
  struct LargeSlot {
    std::atomic<uint64_t> Seq; // index + 1 once written
    LargePlacement Info;
  };
  static constexpr size_t LargeRing = 64;
  LargeSlot LargeSlots[LargeRing];
  std::atomic<uint64_t> LargeCount {};

  bool NameJITBuffers {true};
};
} // namespace FEXCore::CPU
