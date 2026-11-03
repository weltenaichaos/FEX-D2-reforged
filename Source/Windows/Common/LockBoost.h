// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include <FEXCore/fextl/string.h>

namespace FEX::Windows::LockBoost {
// Build 156 (FEX_LOCK_BOOST=<ms>, D2R default 4, 0 = off): a thread that holds FEX's code lock or memory lock for this
// long while another thread waits for it is raised to the main thread's priority (Linux nice -20) until it lets go, then
// put back to the nice it had. The waiter raises it (see WritePriorityMutex.h, LockBoostHooks); nothing runs while no
// one waits. In the b154 run the code lock was held 56, 59 and 111 ms while the main thread and the job workers waited
// to compile, and in each of those seconds a normal priority thread waited 79-172 ms for a CPU.
//
// Lock kinds, as the locks report them.
constexpr uint32_t KindCodeLock = 1;   // FEX's code invalidation lock, exclusive owners
constexpr uint32_t KindMemoryLock = 2; // FEX's memory/SMC lock (ThreadCreationMutex)

// Reads the switch and sets WritePriorityMutex::LockBoost. Once, in ProcessInit, while this is the only thread that can
// take either lock; before the locks' SetBoostKind. Returns whether it is on.
bool Init();

// The stats log's note at the start, and its per-second line ("" when nothing was raised that second).
fextl::string Note();
fextl::string Line();

// The Linux name of a thread of this process ("?" when it can't be read), for log lines.
fextl::string ThreadName(int Tid);
} // namespace FEX::Windows::LockBoost
