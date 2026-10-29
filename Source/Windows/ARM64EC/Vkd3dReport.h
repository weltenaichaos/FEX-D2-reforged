// SPDX-License-Identifier: MIT
#pragma once

// Reports from FEX's vkd3d-proton build (build 55, vkd3d-patches/ in this repo). Its d3d12core.dll exports two text
// buffers that it rewrites once a second: vkd3d_fex_pass_report (GPU time per render pass, FEX_VKD3D_PASSES) and
// vkd3d_fex_present_report (each frame's way from the game's Present() to the window system and the queue threads' time,
// FEX_VKD3D_TIMELINE). With the stats log on, their lines go into every per-second block ('gpu ...', 'present ...',
// 'queue ...'); their '#' notes (driver, swapchain setup) are printed once each. Once the game has loaded d3d12core.dll,
// a note says whether it is FEX's build and which of its features are on, and a '# cap' note whether GameNative's frame
// cap layer (liblsfg-vk-layer.so, which sleeps in vkQueuePresentKHR) is loaded and what its config caps. Build 56 adds
// the 'frame start' (frame start pacing) and 'cap' (cap pre-sleep) lines.
// Build 67 (VKD3D fex13) also writes out the export vkd3d_fex_shader_copy: copies of the shaders FEX_VKD3D_SHADER_COPY
// names, their PSOs and the driver's compiled code, at most 192 KB of log a second, as 'shader copy N begin/end' notes
// around '  sc N' lines (base64, or '|' text lines with '+' continuations).
namespace Vkd3dReport {
// Call on the process's first thread, in process init, when the stats log is on.
void Init();
// Without the stats log: only the cap's frame time hand-off to VKD3D (FEX_VKD3D_CAP_GRID), on a thread of its own.
void InitWithoutLog();
} // namespace Vkd3dReport
