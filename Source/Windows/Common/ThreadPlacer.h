// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
// Opt-in thread placement for big.LITTLE devices. Once per second:
//   sticky mode (default): every game thread stays on the big cores; only threads idle (<0.3% of a core) for 30 seconds in
//     a row and FEX's own helper threads go to the little cores. An idle thread returns to the big cores above 2%. Frame
//     threads (vkd3d*, dxvk*, wine_vk*, audio*) never leave the big cores.
//   load mode (FEX_PIN_MODE=load): the FEX_PIN_HEAVY busiest threads get the big cores, everything else FEX_PIN_REST.
//
//   FEX_THREADPIN=1            enables it (off by default)
//   FEX_PIN_MODE=sticky|load   see above (default sticky; FEX_PIN_HEAVY, FEX_PIN_REST and FEX_PIN_FAST apply to load mode)
//   FEX_PIN_HEAVY=<n>          how many of the busiest threads stay on the big cores (default 4: main + 3 workers)
//   FEX_PIN_BIG=<cpu list>     big cores (default 3-7)
//   FEX_PIN_LITTLE=<cpu list>  little cores (default 0-2)
//   FEX_PIN_REST=little|all    where the other threads go: little cores only (default) or any core
//   FEX_PIN_FAST=<cpu>         optional: the single busiest thread gets this core to itself (e.g. 7, the X3)
//   FEX_PIN_APP=<exe name>     optional: only this process is placed as above (e.g. D2R.exe); all threads of every other
//                              FEX process (Steam, launchers) go to the little cores, unless FEX_PIN_OTHERS=off
//
// The container must allow all cores (GameNative: CPU cores 0-7) for the little cores to be reachable.
// Every decision is written to the FEX_STATSLOG log as "# pin ..." lines.
void StartThreadPlacer(std::string_view AppName);
} // namespace FEX::Windows
