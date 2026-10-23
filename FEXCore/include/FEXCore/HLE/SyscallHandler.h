// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <optional>

#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/fextl/string.h>

namespace FEXCore::Context {
class Context;
}

namespace FEXCore::Core {
struct InternalThreadState;
struct CpuStateFrame;
} // namespace FEXCore::Core

namespace FEXCore::HLE {
struct ExecutableRangeInfo {
  uint64_t Base;
  uint64_t Size;
  bool Writable;
};

class SyscallHandler;
class SourcecodeResolver;

class SyscallHandler {
public:
  virtual ~SyscallHandler() = default;

  virtual void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) = 0;

  virtual void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {}
  virtual void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {}
  virtual void MarkOvercommitRange(uint64_t Start, uint64_t Length) {}
  virtual void UnmarkOvercommitRange(uint64_t Start, uint64_t Length) {}
  virtual ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) = 0;
  virtual std::optional<ExecutableFileSectionInfo> LookupExecutableFileSection(Core::InternalThreadState* Thread, uint64_t GuestAddr) = 0;

  virtual void PreCompile() {}

  // Called after a block was installed from the disk cache, only while per-thread stats are being collected.
  virtual void OnDiskCacheHit(uint64_t GuestRIP) {}

  // Lets the frontend run its own host code in place of the guest code at GuestRIP. Called on every block lookup that
  // reaches the compiler, before the code caches are consulted; a non-zero result is jumped to instead of a compiled
  // block, with the same register state a compiled block would get. The result is not cached, but direct branches to it
  // are linked as usual.
  virtual uintptr_t RedirectBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
    return 0;
  }

  // Like RedirectBlock, but for a block about to be compiled (not found in this thread's lookup cache), before the disk
  // cache is consulted, inside the compiler's guest read fault recovery: the frontend may read the guest code at GuestRIP
  // directly. A read that faults restarts the compile with Thread->CompileFaultPage set to that page.
  virtual uintptr_t RedirectBlockByCode(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
    return 0;
  }

  virtual SourcecodeResolver* GetSourcecodeResolver() {
    return nullptr;
  }

  virtual void SleepThread(FEXCore::Context::Context* CTX, FEXCore::Core::CpuStateFrame* Frame) {}
};
} // namespace FEXCore::HLE
