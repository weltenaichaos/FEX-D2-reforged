// SPDX-License-Identifier: MIT
#pragma once

// Reports from FEX's Turnip build (build 69, turnip-patches/ in this repo) through the FEX link (Common/TurnipChannel.h).
// Once the driver attached, a '# turnip: driver' note says which build it is, whether the descriptor fix is honored and
// which GPU counters KGSL gave it. Every second:
//   turnip: desc <on|off> this second | cmdbufs a/b | sets per update off x/on y | push n reused r%
//     (a/b = command buffers recorded with the fix off/on in that second, sets per update = descriptor sets a
//      graphics descriptor update invalidated, 8 = stock)
//   turnip pass <off|on>: n passes, draws, gpu ms (us/draw), then per draw: each counter, and derived ratios
//     (one line per mode seen among the passes the GPU finished that second)
// With FEX_TU_DESC=alt (the D2R default) it also flips the fix on and off each second.
namespace TurnipReport {
// Call in process init, when the stats log is on.
void Init();
} // namespace TurnipReport
