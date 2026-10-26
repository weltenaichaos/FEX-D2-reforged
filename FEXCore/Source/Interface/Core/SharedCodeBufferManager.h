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

#include <cstddef>
#include <cstdint>

namespace FEXCore {
struct GuestToHostMap;
}

namespace FEXCore::CPU {
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

  bool NameJITBuffers {true};
};
} // namespace FEXCore::CPU
