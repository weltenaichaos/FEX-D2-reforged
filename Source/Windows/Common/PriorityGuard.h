// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
// Keeps the process's threads in a narrow priority band, puts them on the right cores, and lifts the wineserver.
//
// Priorities. Wine on Linux maps Windows priorities onto nice values across the whole -19..19 range when RLIMIT_NICE
// allows it (it does on Android): IDLE becomes nice 19 (about 1.5% of a core next to a busy normal thread), LOWEST
// nice 6, HIGHEST nice -5, TIME_CRITICAL nice -19, and a HIGH priority class puts every thread at nice -13, above the
// wineserver. Under CPU contention a low priority thread holding a lock others need, or a wineserver starved by the
// game's own threads, stalls everyone. Desktop Linux never sees this: RLIMIT_NICE is 0 there, so Wine leaves every
// thread at nice 0. FEX_PRIO=normal gives the same picture on Android.
//
//   FEX_PRIO=keep    leave priorities alone (upstream; the default outside D2R)
//   FEX_PRIO=normal  priority class normal, every thread at NORMAL (nice 0; Wine still gives the first thread -2); the
//                    D2R default
//   FEX_PRIO=clamp   priority class normal, thread base priorities clamped to BELOW_NORMAL..ABOVE_NORMAL (nice 3..-2)
//
// Cores. In the build 48 log D2R's threads all had cores 3-7 (the container's setting on the Thor) except its main
// thread, which Wine reported free to run on every core, the slow A510s included. The main thread does most of each
// frame's work alone, and in fights the stall probe saw threads wait up to 26 ms for a CPU, so it gets the fastest core
// (the Cortex-X3) to itself:
//
//   FEX_MAINCORE=fast  the main thread alone on the fastest core; the game's other threads, FEX's threads and the
//                      wineserver keep their cores minus that one (the D2R default). Falls back to "big" when the
//                      container doesn't allow the fastest core.
//                      The main thread's Linux cores are also checked 4 times a second and set back when
//                      something outside Wine changed them (build 50 lost the pin to Android or the launcher after 34
//                      seconds).
//   FEX_MAINCORE=big   the main thread on the same cores as the game's other threads
//   FEX_MAINCORE=off   leave affinities alone
//   FEX_MAINCORE_WAIT=N  with "fast": while the main thread waits N ms or more a second for its one core (login and
//                      loading, where the wineserver and other threads share that core), it may also use the game's
//                      other big cores until it waits under 10 ms a second for 5 seconds (longer if it keeps bouncing).
//                      D2R default 40; "off" or 0 keeps it on its core at all times.
//   FEX_HELPER_CORES=little  the stats log thread (a 2ms sampler) and this guard on the slowest cores, off the game's
//                      (the D2R default), when the process may use them. The stall probe, disk cache and unpack cache
//                      threads stay on the game's cores.
//
// Wineserver. Every file, pipe, controller and window message request of every Wine process goes through the one
// wineserver thread; under load the build 48 probe saw single round trips take up to 27 ms.
//
//   FEX_SERVER_NICE=N  run the wineserver at Linux nice N (D2R default -10; "off" leaves it alone)
//
// FEX's own threads (named FEX*) keep their priorities. Checked every 2 seconds, since a game can change priorities and
// affinities at any time. Each change is written to the FEX_STATSLOG log as a "# prio ...", "# cores ..." or
// "# server ..." line.
void StartPriorityGuard(std::string_view AppName);
} // namespace FEX::Windows
