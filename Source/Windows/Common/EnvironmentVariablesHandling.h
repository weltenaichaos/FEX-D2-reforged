// SPDX-License-Identifier: MIT
#pragma once
#include <winternl.h>

namespace FEX::Windows {
// Environment values FEX sets for the process before the game starts. Call after InitSwitches.
//
// vkd3d-proton (build 52). On a GPU with a single Vulkan queue family (Adreno), vkd3d-proton maps the game's direct,
// compute and copy queues onto one Vulkan queue. While two kinds of them were active in the last second it then
// "staggers" submissions: after each batch of command buffers its queue thread waits on the CPU until the previous batch
// has finished on the GPU, and it resolves the game's cross-queue fence waits on the CPU too. In D2R (it streams through a
// copy queue) the build 51 log showed vkd3d's queue thread blocked 80-95% of each frame in that wait (a KGSL timeline
// wait), and the GPU idle 11% of the time at its top clock in heavy scenes (36 ms frames at 680 MHz, busy 89%). vkd3d's
// VKD3D_CONFIG flag no_staggered_submit submits each ExecuteCommandLists in one go and leaves the waits to the GPU;
// upstream already sets it for Unreal Engine games and others.
//
//   FEX_VKD3D_NOSTAGGER=1  add no_staggered_submit to VKD3D_CONFIG in the process environment (the D2R default)
//   FEX_VKD3D_NOSTAGGER=0  leave VKD3D_CONFIG alone
//
// FEX's own vkd3d-proton build (build 55, vkd3d-patches/ in this repo, a GameNative VKD3D component) has features
// these switch on through its environment variables. A variable that is already set is left alone, and another
// vkd3d-proton ignores them. The stats log says which build the game loaded (see ARM64EC/Vkd3dReport.h).
//
//   FEX_VKD3D_FOLD_CLEARS=1  VKD3D_FEX_FOLD_CLEARS=1 (the D2R default). D3D12 clears are commands of their own; vkd3d
//                            turns each full clear made while no render pass runs into a render pass that only clears,
//                            which on a tiled GPU (Adreno) writes the whole image to memory, and the next pass then reads
//                            it all back. With this on, such a clear waits and becomes the clear load op of the next pass
//                            that draws to the same view in the same layout, covering it whole. Anything else that may
//                            touch the image first gets the clear as it would have been.
//   FEX_VKD3D_PASSES=1       VKD3D_FEX_PASSES=1 (the D2R default): GPU time per render pass and command list.
//   FEX_VKD3D_TIMELINE=1     VKD3D_FEX_TIMELINE=1 (the D2R default): each frame's steps from the game's Present() to the
//                            window system, and the queue threads' time.
//   FEX_VKD3D_CAP_PRESLEEP=1 VKD3D_FEX_PRESENT_PRESLEEP=1 (the D2R default, build 56). GameNative's FPS limit sleeps in
//                            its Vulkan layer's vkQueuePresentKHR, ~32 ms of each frame at 30 FPS, while vkd3d holds the
//                            VkQueue; on Adreno the game's compute and copy queues share it and waited (build 55: compute
//                            queue signals up to 34 ms, ~600 ms a second). Once presents are held at a steady period, vkd3d
//                            sleeps until 4 ms before the cap's next due time without the VkQueue.
//   FEX_VKD3D_FRAME_START=1  VKD3D_FEX_FRAME_START=1 (the D2R default, build 56). D2R keeps 2 frames in flight; at the
//                            cap each finished frame waited ~23 ms for the queue thread, so the game read input ~2 frames
//                            before the screen showed it. vkd3d holds back the frame latency release the game waits on
//                            before each frame by the smallest spare time of the last 64 frames minus a margin; a frame
//                            that came late because of it grows the margin by 3 ms, 20 s without one shrink it by 1 ms.
void SetupEnvironmentVariableValues(HMODULE NtDll);
}
