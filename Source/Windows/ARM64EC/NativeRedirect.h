// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Runs native ARM64EC builds of hot guest functions in place of their emulated x86 code, see NativeRedirect.cpp.
namespace NativeRedirect {
// Reads FEX_NATIVE_ZLIB, FEX_NATIVE_MD5 and FEX_JOBWAIT* and starts looking for the functions in the main executable. With
// StatsLog, also tracks which zlib streams start and who asks for them. Invalidate drops compiled guest code in a range.
// Call once before any guest code runs.
void Init(uint64_t MainImageBase, bool StatsLog, void (*Invalidate)(uint64_t Address, uint64_t Size));

// SyscallHandler::PreCompile, with no FEX locks held: re-offers a redirect whose guest code was compiled before it could
// be checked.
void PreCompile();

// SyscallHandler::RedirectBlock: the host code to run in place of the guest code at GuestRIP, or 0.
uintptr_t Lookup(uint64_t GuestRIP);

// SyscallHandler::RedirectBlockByCode, inside the compiler's fault recovery: compares the code at GuestRIP with the
// functions not found yet, and returns the host code to run in its place if it is one of them. FaultPage is the page a
// read faulted on in this compile (0 if none), which is left alone.
uintptr_t LookupByCode(uint64_t GuestRIP, uint64_t FaultPage);
} // namespace NativeRedirect
