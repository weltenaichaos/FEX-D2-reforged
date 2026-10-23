// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
// Keeps the process's threads in a narrow priority band.
//
// Wine on Linux maps Windows priorities onto nice values across the whole -19..19 range when RLIMIT_NICE allows it (it
// does on Android): IDLE becomes nice 19 (about 1.5% of a core next to a busy normal thread), LOWEST nice 6, HIGHEST
// nice -5, TIME_CRITICAL nice -19, and a HIGH priority class puts every thread at nice -13, above the wineserver. Under
// CPU contention a low priority thread holding a lock others need, or a wineserver starved by the game's own threads,
// stalls everyone.
//
// Desktop Linux never sees this: RLIMIT_NICE is 0 there, so Wine leaves every thread at nice 0. FEX_PRIO=normal gives
// the same picture on Android.
//
//   FEX_PRIO=keep    leave priorities alone (upstream; the default outside D2R)
//   FEX_PRIO=normal  priority class normal, every thread at NORMAL (nice 0; Wine still gives the first thread -2); the
//                    D2R default
//   FEX_PRIO=clamp   priority class normal, thread base priorities clamped to BELOW_NORMAL..ABOVE_NORMAL (nice 3..-2)
//
// FEX's own threads (named FEX*) are left alone. Checked every 2 seconds, since a game can set priorities at any time.
// Each change is written to the FEX_STATSLOG log as a "# prio ..." line.
void StartPriorityGuard(std::string_view AppName);
} // namespace FEX::Windows
