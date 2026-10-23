// SPDX-License-Identifier: MIT
#pragma once

// Reports from FEX's vkd3d-proton build (build 55, vkd3d-patches/ in this repo). Its d3d12core.dll exports two text
// buffers that it rewrites once a second: vkd3d_fex_pass_report (GPU time per render pass, FEX_VKD3D_PASSES) and
// vkd3d_fex_present_report (each frame's way from the game's Present() to the window system and the queue threads' time,
// FEX_VKD3D_TIMELINE). With the stats log on, their lines go into every per-second block ('gpu ...', 'present ...',
// 'queue ...'); their '#' notes (driver, swapchain setup) are printed once each. Once the game has loaded d3d12core.dll,
// a note says whether it is FEX's build and which of its features are on.
namespace Vkd3dReport {
// Call on the process's first thread, in process init, when the stats log is on.
void Init();
} // namespace Vkd3dReport
