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
//   FEX_VKD3D_SKIP_FRESH_CLEARS=1 VKD3D_FEX_SKIP_FRESH_CLEARS=1 (the D2R default, build 64, VKD3D fex11). New D3D12
//                            memory has to read as zero. vkd3d places small buffers (<2 MB) and textures/heaps (<8 MB) in
//                            16/32 MB blocks of its own and zero-fills each such placement on the GPU (a chain of 64 KB
//                            fills on Turnip, and all of the game's queues wait for it). Block pages nothing was placed in
//                            since the driver zeroed the block are skipped. Suspected cause of the ~1 s of 50-55 ms frames
//                            after each area load (build 63 log); the 'gpu memory' and 'gpu transfers' lines count it.
//   FEX_VKD3D_PRESENT_DRAWN=1 VKD3D_FEX_PRESENT_DRAWN=1 (the D2R default, build 66, VKD3D fex12). GameNative's Vulkan
//                            wrapper sends a presented image to its X server right after vkQueuePresentKHR submits (its
//                            X11 WSI only waits for the GPU in MAILBOX mode), and the X server draws the window from that
//                            AHardwareBuffer without a fence; KGSL has no implicit sync between the two processes. An image
//                            handed over before the GPU got to the frame's copy into it shows what it held before: the
//                            frame from 4 presents earlier, or part of it. In the build 65 log fights handed every frame
//                            over ~0.5 ms after Present() while the GPU finished it ~41-44 ms later (calm scenes had it
//                            ~5 ms ahead, the cap held those presents). vkd3d now waits, without the VkQueue, until the
//                            frame done thread saw the copy's fence before it calls vkQueuePresentKHR (at most 200 ms).
//                            The 'screen handoff' line counts the frames the GPU wasn't done with and the wait.
//   FEX_VKD3D_SHADER_COPY=<hashes> VKD3D_FEX_SHADER_COPY=<hashes> (the D2R default lists 9 lighting pixel shaders,
//                            build 67, VKD3D fex13). Changes nothing in the game. Once build 66 fixed the hitches,
//                            fights were GPU-bound (45-48 ms frames, 23-29 FPS); about 20 ms of such a frame is the main
//                            pass, where uber pixel shaders of ~7,000 instructions and ~370 texture reads take 12-29 us
//                            a draw (fex9 'gpu draws' lines). vkd3d copies each listed shader's D3D bytecode, its PSO
//                            (targets, depth, blend, root signature mapping) and serialized root signature, and for the
//                            first four also the SPIR-V it gives the driver, the PSO's other stages and the driver's
//                            compiled code and statistics (the driver compiles those four pipelines once with its
//                            internal representations kept). The stats log writes the copies out as 'shader copy'
//                            blocks (see ARM64EC/Vkd3dReport.cpp) so the cost can be taken apart off the device.
//                            FEX_VKD3D_SHADER_COPY=0 turns it off.
//
// Turnip, the Vulkan driver behind GameNative's wrapper (build 68). Its shader compiler (ir3) moves work that is the
// same for every pixel of a draw into a "preamble": before the draw's pixels are shaded, the first wave on each shader
// core runs it on one thread, and its results go into the constant file the other waves read, so they wait for it. In
// D2R's lighting pixel shaders (build 67 copies) that preamble is ~720 instructions with 6-14 memory reads that each
// wait for the one before, plus ~13 constant uploads, and the driver could not run it ahead ("Early preamble = false":
// it uses branches and normal registers). The main pass draws ~1,000 small objects in fights, and its time follows
// the draw count, not the shaded pixels: ~14.6 us more per draw (b67: 238 draws 5.4 ms, 963 draws 15.9 ms at
// ~0.85M pixels each); a draw timed alone takes 23-33 us even with 64 vertices or fewer (fex9 'gpu draws').
//
//   FEX_TU_NOPREAMBLE=1  add nopreamble to IR3_SHADER_DEBUG in the Unix environment (the D2R default in build 68 only,
//                        it changed nothing: b68 13.3-14.1 us/draw like b67): the per-draw
//                        work runs in each wave instead (from constants the short remaining preamble uploads in one
//                        go), so a draw's pixels no longer wait on the chain of reads. Every shader is compiled anew
//                        the first time (the flag is part of the driver's cache key).
//   FEX_TU_NOPREAMBLE=0  leave IR3_SHADER_DEBUG alone (the D2R default from build 69)
//
// FEX's Turnip build (build 69, turnip-patches/, Common/TurnipChannel.h). Stock Turnip writes SP_UPDATE_CNTL with all 8
// bindless sets on every descriptor update, which drops the GPU's descriptor caches of every set; vkd3d keeps the root
// constant buffers in a push descriptor set and pushes it before nearly every draw, so each draw of a fight's main pass
// (~1,000 draws, ~14 us each in b67/b68) starts with cold descriptor caches for the texture heap too. FEX allocates a
// link block and names it to the driver with TU_FEX_CHANNEL=<hex address> (Unix environment, __wine_set_unix_env):
//   FEX_TU_CHANNEL=1       set up the link (the D2R default); 0 = none, the driver behaves like stock Turnip
//   FEX_TU_DESC=0          stock descriptor handling (the D2R default since build 70: build 69 measured no gain).
//                          1 = in direct (sysmem) passes invalidate only the sets whose address changed since the
//                          pass began, and an identical push keeps its old copy; alt = on in every other second, so
//                          one run compares both ways (needs the stats log, without it alt stays on)
//   FEX_TU_COUNTERS=1      reserve 24 GPU counters through KGSL and record them around big passes (the D2R default;
//                          build 70: GPU cycles, state contexts, command processor idle/stall, shader front end
//                          stalls and draw overlap, shader core occupancy/starvation, wave life, wave cycle split,
//                          texture latency); 0 = none
//   FEX_TU_SAMPLE_DRAWS=N  passes with at least N draws are timed and counted (default 100)
//   FEX_TU_EARLY_PREAMBLE=1  (the D2R default from build 71, driver fex2 and later) push descriptor sets count as written,
//                          so the lighting shaders' reads of vkd3d's root constant buffers can run in the early
//                          preamble, ahead of the draw's pixel waves, as Qualcomm's driver does for D3D11 games. Build
//                          70 counted ~12k GPU cycles per main pass draw for a ~9.7k cycle wave, and the shaders'
//                          preambles (~730 instructions, ~108 memory reads, 47 branches) ran inside each draw's first
//                          wave ('Early preamble = false'): with descriptor buffers stock Turnip treats every binding
//                          as possibly unwritten, so no read through it may run before the draw. With the switch the
//                          preamble is ~250 branch-free instructions and early ('Early preamble = true' in the shader
//                          copies); bindings not pushed since the push set's layout changed read as empty (null
//                          descriptors). The driver reads it once at device creation; it is part of the shaders' cache
//                          key, so shaders are compiled anew once. 0 = stock.
// The stats log's 'turnip' lines (ARM64EC/TurnipReport.cpp) report both.
void SetupEnvironmentVariableValues(HMODULE NtDll);
}
