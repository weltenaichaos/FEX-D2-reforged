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
//    '| units per cycle: ...' before the contexts; build 74, driver fex4: 'clear fastz' = the 3D clear ran without
//    fragment shader, and '| command processor per draw: busy, register writes, register bunches, state loads, draw
//    packets, waits for command data, waits for draw state data, sync waits, waits for idle (GPU cycles) | packets,
//    dwords' before the contexts; units per cycle are only the ones still counted: pc waits for cp, render backend,
//    depth writes wait for memory)
//   turnip: depth clear <fastz|3d|2d> this second | depth clears 2d engine n (Mpx) 3d draw m (Mpx), of them fast z k
//     (Mpx)   (driver fex3 and later; fast z fex4 and later)
// With FEX_TU_DESC=alt it also flips the descriptor fix on and off each second, with FEX_TU_DEPTH_CLEAR=alt the depth
// clear's fast Z (build 74; build 72/73: 3D draw against 2D engine), every two seconds when both are alt.
namespace TurnipReport {
// Call in process init, when the stats log is on.
void Init();
} // namespace TurnipReport
