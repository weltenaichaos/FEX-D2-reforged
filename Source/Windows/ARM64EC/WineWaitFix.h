// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/string.h>

// Builds 142-144: Wine's wait lists, fixed (FEX_WINE_WAIT_FIX).
//
// Every SRW lock, condition variable and WaitOnAddress in a Wine process waits and wakes through ntdll's
// RtlWaitOnAddress, RtlWakeAddressSingle and RtlWakeAddressAll. They keep each waiting thread in one of 256 lists, as a
// list entry on that thread's stack. In the Proton 11.0-1 GameNative ships (and Valve's proton_11.0) a waker marks the
// entry as woken (entry->addr = NULL) BEFORE it unlinks it, and a waiter that comes back on its own (a timeout, or an
// alert left over from an earlier wake) and sees the mark leaves at once, without the list's lock. The waker then
// unlinks an entry whose stack the waiter is already reusing, and the list points into stale stack: the next walk of it
// never ends, with the list's spin lock held, and every thread that waits or wakes on that list (or on another list the
// stale entry got linked into) spins forever. That is the freeze of the build 118 run 2 and build 139 logs: the main
// thread 95-100% in RtlWakeConditionVariable and never sleeping, the job workers 50-96% in SleepConditionVariableSRW,
// the network threads in UnsealMessage (schannel's handle table lock), no frames, until the game is closed. Wine
// itself fixed this upstream (unlink first, read the thread id, then clear the mark with release order, and the waiter
// reads it with acquire order); Proton's bleeding-edge branch has the fix, 11.0 does not.
//
//   FEX_WINE_WAIT_FIX=1  at process start (before any second thread exists) FEX points those three ntdll functions at
//                        its own copy of the fixed code, with its own 1024 lists; a list's lock spins briefly and then
//                        sleeps in the Linux kernel, so a thread pushed off its CPU while holding it no longer leaves
//                        the others spinning (the build 124 144 ms hitch had that cause). The D2R default.
//   FEX_WINE_WAIT_FIX=2  the same fixed order, but the list lock is a spin lock like Wine's (to compare)
//   FEX_WINE_WAIT_FIX=0  Wine's own code (as up to build 140)
//
// Before patching it checks that ntdll's code matches what it replaces: the native code of the three functions is
// found through the ARM64EC redirection table, Wine's list array through their code (the one
// address all three functions use whose 256 lists all look empty), and no code anywhere else in ntdll may use that
// array (so no copy of the functions was inlined elsewhere), and both wake functions must clear the woken mark in a
// known way: the buggy plain store (stock 11.0-1) or a fenced store after the unlink (upstream's fix, our own Proton
// d2r1). On a Proton with the fix FEX still swaps in for its sleeping list lock while Proton's lock still spins (build
// 143); our Proton d2r2 and later export __wine_d2r_wait_lists = 2 (both fixes), and then FEX stays out (build 144).
// All of this was tried on stock 11.0-1's and our d2r1 Proton's ntdll.dll (tools/wine/check-wait-fix.py). Any
// mismatch leaves Wine's code alone and says why in the '# wine waits' note, which also names a hash of the three
// functions' code. With the stats log on, a 'wine waits' line counts each second's waits and wakes.
namespace WineWaitFix {
// Call on the process's first thread in process init, right after the switches are read, before any other thread or
// wait exists. Does nothing outside Wine or with the switch off.
void Init();
// Writes the '# wine waits' note and starts the per-second line; call once the stats log takes notes.
void StartStats(bool StatsLog);
} // namespace WineWaitFix
