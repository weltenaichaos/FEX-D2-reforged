// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Runs native ARM64EC builds of hot guest functions in place of their emulated x86 code, see NativeRedirect.cpp.
namespace NativeRedirect {
// Reads FEX_NATIVE_ZLIB and FEX_NATIVE_MD5 and locates the functions relative to the main executable. With StatsLog,
// also tracks which zlib streams start and who asks for them. Call once before any guest code runs.
void Init(uint64_t MainImageBase, bool StatsLog);

// SyscallHandler::RedirectBlock: the host code to run in place of the guest code at GuestRIP, or 0.
uintptr_t Lookup(uint64_t GuestRIP);
} // namespace NativeRedirect
