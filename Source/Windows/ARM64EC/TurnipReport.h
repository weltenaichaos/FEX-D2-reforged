// SPDX-License-Identifier: MIT
#pragma once

// Reports from FEX's Turnip build (build 69, turnip-patches/ in this repo) through the FEX link (Common/TurnipChannel.h).
// Once the driver attached, a '# turnip: driver' note says which build it is, whether the descriptor fix is honored and
// which GPU counters KGSL gave it. Every second:
//   turnip: cmdbufs n | descriptor updates u | push p | passes sampled s small m
//     (with FEX_TU_DESC=1/alt instead: desc <on|off> this second | cmdbufs off a on b | sets per update off x on y
//      (stock 8) | push n reused r%)
//   turnip pass WxH c<colors>/a<attachments> fmt<VkFormat of attachment 0> <direct|tiled>: n passes, draws, gpu ms
//     (us/draw) at MHz | shader cores: busy (of 6), waves per busy core, cores stalled, cores starved by the front end
//     | fs waves and pixels per draw, wave life | wave cycles split | tex latency | per cycle: draw overlap (dual fs)
//     and front end / command processor stalls | contexts per draw; then every counter per draw
//     (one line for each of the 5 pass kinds with the most GPU time that second, the rest summed on 'turnip pass
//      other'; build 70 counters, see EnvironmentVariablesHandling.cpp)
//   (build 72, driver fex3: '<direct> clear 3d|2d' after the kind for direct passes that clear depth at their start, and
//    '| units per cycle: pc waits for cp, pc waits for draw setup, vertex fetch, triangle setup, rasterizer, depth
//    pre-test, render backend, 2d engine' before the contexts)
//   turnip: depth clear <3d|2d> this second | depth clears 2d engine n (Mpx) 3d draw m (Mpx)   (driver fex3 and later)
// With FEX_TU_DESC=alt it also flips the descriptor fix on and off each second, with FEX_TU_DEPTH_CLEAR=alt the depth
// clear path (every two seconds when both are alt).
namespace TurnipReport {
// Call in process init, when the stats log is on.
void Init();
} // namespace TurnipReport
