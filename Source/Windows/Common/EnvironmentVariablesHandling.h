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
//                            FEX_VKD3D_SHADER_COPY=0 turns it off (the D2R default from build 76: the shader study is
//                            done, and the copies were 37% of a log).
//   FEX_VKD3D_LATENCY_RESYNC=1 VKD3D_FEX_LATENCY_RESYNC=1 (the D2R default, VKD3D fex16; =0 sets it to 0, since that
//                            VKD3D build also turns it on by itself while frame start pacing is on). D2R's frame latency
//                            handle holds 2 credits: the game takes one before each frame (WaitForSingleObjectEx with a
//                            1 s timeout) and vkd3d hands one back once that frame's present is done. In the b74 and b77
//                            logs that wait timed out during a 1.6 s hitch at startup; the frame the game then ran
//                            without a credit left one too many for the whole session, so 3 frames were in flight (only
//                            vkd3d's own latency of 3 in Present() held the game) and frames took ~100 ms from start to
//                            screen with no hold-back instead of ~67, ~50 at best. Each hand-out now compares the game's
//                            Present() calls with the credits it took; once the game runs ahead, vkd3d holds back that
//                            many hand-outs (the next frame starts one frame later, once). 8 such actions within 60 s
//                            stop it. A 'frame credits (resync)' line shows how far ahead the game was (-1 or 0 = as
//                            set, 0 or 1 = one ahead) and the hand-outs held back.
//   FEX_VKD3D_MEMORY_AHEAD=1 VKD3D_FEX_MEMORY_AHEAD=1 (the D2R default, build 80, VKD3D fex17). D2R keeps creating
//                            D3D12 heaps while it is played (in the b79 log GPU memory grew from 3.1 to 3.8 GB in 95 s
//                            and was never given back), each with its own vkAllocateMemory, which on Turnip/KGSL is
//                            kernel zeroing and mapping on the calling thread: ~0.15 ms per MB. Twice the main thread
//                            made 96 MB of heaps in one frame (~14 ms in ID3D12Device::CreateHeap, the frame came late,
//                            and the frame start pacing then held back little for ~8 s), and a 256 MB heap on a worker
//                            made Android reclaim memory while the main thread waited for its jobs. vkd3d now makes
//                            spares of each kind of request it saw (memory type, size, flags; 4-64 MB) on a background
//                            thread and hands one out instead of allocating; each kind keeps as many as its biggest
//                            burst, up to a quarter of VKD3D_FEX_MEMORY_AHEAD_MB (default 256 MB) for all together. A
//                            'gpu memory ahead' line shows spares taken, requests that found none, the spares made and
//                            how long the allocations made when asked took.
//   FEX_VKD3D_CBV_SIZE=1 VKD3D_FEX_CBV_SIZE=1 (the D2R default, build 98, VKD3D fex29). vkd3d's shader converter
//                            (dxbc-spirv) declares a constant buffer that a shader indexes with a register as 64 KiB,
//                            the D3D maximum, whatever size the shader declares. Turnip can't keep that in the GPU's
//                            constant registers, so every read stays a memory load through the buffer's descriptor;
//                            D2R's lighting pixel shaders index two root constant buffers this way (90 and 16 vectors
//                            declared), which go through vkd3d's root constant buffer set, pushed again for nearly
//                            every draw. With the declared size (if 256 vectors or less) the buffer fits, and with
//                            FEX_TU_CP_UBO the driver loads it before the draw. An index past the declared end reads
//                            other constants instead of buffer memory (the D3D compiler only makes such indices for
//                            out-of-range array reads). Part of vkd3d's shader cache key, so a change rebuilds it. The
//                            '# vkd3d' line names it.
//   FEX_VKD3D_SHADER_SWAP=1 VKD3D_FEX_SHADER_SWAP=1 (the D2R default, build 108, VKD3D fex30; with the switch at 0 FEX
//                            sets 0, as vkd3d swaps unless 0). D2R's compute shader 83e2eb4a6bf5f03b blurs a 3D
//                            lighting table: every cell sums 2 x A x B filtered samples of one volume, 8,192 per cell,
//                            30 ms per run on the Adreno 740 with the texture unit busy throughout, in bursts of up to 19
//                            runs when the game enters an area (b107: 34 of the 40 frames of 40 ms or more in 147 s of
//                            play). Most samples repeat across a group of cells, so FEX's version (same bindings, push
//                            constants and constant buffer) samples each distinct value once per group into shared
//                            memory and sums from there; results match up to float rounding. vkd3d swaps it in only when
//                            its translation of the game's shader is the SPIR-V the FEX version was written for, and
//                            never takes that shader from a pipeline cache. A '# shader swap' note in the gpu report
//                            gives the counts; the '# vkd3d' line names the switch. The 'turnip dispatch' lines show the
//                            FEX version as its own compute shader (other instruction count).
//                            Build 109 (VKD3D fex31): the value is passed on. alt (the D2R default from build 109; also
//                            what VKD3D fex31 does when unset): the pipeline gets two FEX versions, 4 cells per GPU
//                            thread (local size 4x4x8) and 2 cells per thread (8x4x8), and each bind takes the next one in
//                            turn; the 'turnip dispatch' lines tell them apart by local size. 1: 4 cells per thread only.
//                            2: 2 cells per thread only. fex30's version (1 cell per thread, 8x8x8) took 12-13 ms per run
//                            in b108, held back by its instructions per product (hwave wait 49%).
//                            VKD3D fex32 (with FEX 109, no FEX change): b109's 4-cell version took 8.5 ms per run, held
//                            back by its shared memory reads (one per product). Both fex32 versions sum 8 cells per
//                            thread and keep the last s2 values in registers: 1 = 4 x by 2 y (local size 2x4x8, 3 reads
//                            per 8 products), 2 = 8 x in a row (1x8x8, 2 reads per 8 products); alt takes turns.
//                            VKD3D fex33 (FEX 109 kept): 8 in a row won (2.2 ms per run vs 2.7) and is the only 83e2
//                            version. D2R's ground clutter compute shader 0e21a3b5088ba1af (1152 x 1152 grid, 1.0-1.2
//                            ms per frame) gets FEX's version too: its on-screen test and mask texture test, which came
//                            after two layers of density noise (~330 instructions), now come first with their own early
//                            returns; nothing before the old test has side effects, so the result is the same. It is
//                            an edit script applied to vkd3d's own translation (checked by hash before and after). alt:
//                            binds take turns between FEX's version and the game's own (the 'turnip dispatch' lines
//                            tell them apart by instruction count, cs2224 = the game's); 1 = FEX's version; 2 = the
//                            game's own.
//                            Build 110: 1 (fex33's run: clutter 0.66 ms per frame in town with FEX's version vs 1.14
//                            with the game's own, 1.16 vs 1.19 outdoors; never slower).
//   FEX_VKD3D_CLEAR_MEMO=1 VKD3D_FEX_CLEAR_MEMO=1 (the D2R default from build 110, VKD3D fex34; with the switch at 0 FEX
//                            sets 0, as vkd3d remembers unless 0). D2R clears its second 4096 x 4096 shadow map in a
//                            render pass of its own every frame, 0.55-0.69 ms of GPU time, though nothing draws into it
//                            (fex33's 'gpu big clear passes' line: the image's next use is always a barrier, then the
//                            lighting pass reads it). A whole one-plane depth image of 4M+ pixels in memory of its own
//                            gets a slot in a small GPU buffer the first time such a clear of it gets its own render pass;
//                            0 = the image still holds that clear's value. The clear then runs as vkCmdClearAttachments
//                            in a load/store pass under conditional rendering on the slot (the GPU skips it while the
//                            slot is 0) and sets the slot to 0 after. Every render pass with the image as a writable depth
//                            attachment, other clear, copy, resolve or discard of it sets the slot to 1 first (once per
//                            command list until the next such clear). Clears with another value or while the game's own
//                            predication is on are done in full. Needs FEX_VKD3D_FOLD_CLEARS (on). alt = on and off by
//                            turns every second (with the stats log). The 'gpu clear memo' line counts the clears the
//                            GPU may skip, the ones done in full and why, and the marks; the skipped clear passes show as
//                            their own 'clear-pass ... dLS' kind in the pass lines (done in full: 'dCS').
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
//   FEX_TU_NOPREAMBLE=0  leave IR3_SHADER_DEBUG alone (the D2R default from build 69 to 103)
//   FEX_TU_NOPREAMBLE=alt  (the D2R default in build 104 only, 0 from build 105: no gain) leave IR3_SHADER_DEBUG alone; through FEX's link FEX's Turnip
//                        (fex13 and later) compiles the big fragment shaders (D2R's lighting shaders, 1,024+ instructions
//                        with a preamble of 32+) a second time without preamble, with their own constant layout, and
//                        pipelines use that binary in every other second (TurnipReport flips the mode bit; 'np on/off'
//                        pass kinds, 'turnip: second binary' lines). Build 103: lighting waves spend 25-30% of their life
//                        in HWAVE_WAIT, no other pass does; their 137-instruction early preamble runs once per draw
//                        before the draw's waves may start
//   FEX_TU_NOPREAMBLE=lit  that second binary always
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
//   FEX_TU_COUNTERS=1      reserve 24 GPU counters through KGSL and record them around big passes (the D2R default up
//                          to build 75, 0 in builds 76-101, 1 again in build 102; build 107: a compute list for the
//                          dispatches (busy, occupancy, launches, compute instructions and threads, every big wave
//                          state, memory atomics, stores and address locks, stalls on the L2 cache and texture unit,
//                          texture latency); build 102 a lighting pass list: every wave
//                          state, wave life, texture instructions, texture unit busy and latency, shader cores unable
//                          to send texture requests, L1 and L2 cache misses for textures; build 103: every wave
//                          state counter (float/special/integer math, csp, nops, waits, fetch, flow control, input
//                          load, emit, end, join, input, output, half-wave waits, idle) with wave life and occupancy;
//                          build 74: GPU cycles, state contexts, and where the command processor's time per draw goes
//                          (busy, register writes and bunches, state loads, draw packets, waits for command or draw
//                          state data, syncs, waits for idle, packets and dwords read), shader front end stalls and
//                          draw overlap, shader core occupancy/starvation and pixels, primitive control starved by
//                          the command processor, render backend busy and depth writes waiting for memory); 0 = none
//   FEX_TU_SAMPLE_DRAWS=N  passes with at least N draws are timed and counted (default 100; the D2R default 0 in builds
//                          76-101: each sampled pass paid two GPU waits for idle; 100 again in build 102)
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
//   FEX_TU_PREAMBLE_LOADS=1  (the D2R default from build 92, driver fex6 and later) shader preambles issue their memory
//                          reads first, the constant buffer copies (ldc.k) right after them, and store each value
//                          once. The early preamble runs as one thread ahead of each draw's pixels, so nothing hides
//                          its waits: stock Turnip gave the lighting shaders 5 separate waits (the 11 copies last, an
//                          ordering rule kept them behind every constant store) and stored 24 values up to four
//                          times (122 stores for 58 values); fex6 makes it 2 waits and 174-184 instead of 243-253
//                          instructions. Register reads are only moved up when all of a preamble's reads fit in the
//                          registers at once (else as stock). Read once at device creation and part of the shader
//                          keys, so shaders are compiled anew once. 0 = stock order.
//   FEX_TU_LOOP_PREFETCH=1  (the D2R default from build 96, driver fex7 and later) fragment shader loops that walk a
//                          list (for each light: entry = list[i], then the light's data at entry) read the next entry
//                          while the current one is worked on, carried to the next iteration, so the data reads and
//                          the next entry's read share one memory wait instead of two waits one after the other. The
//                          read before the loop is skipped when the loop doesn't run; the one read past the last entry
//                          is bounds-checked by the descriptor and never used. Only read-only buffer loads whose
//                          address is made from the loop counter and values from outside the loop. Read once at
//                          device creation and part of the shader keys, so shaders are compiled anew once. 0 = stock.
//   FEX_TU_DEPTH_CLEAR=alt (the D2R default in build 72, driver fex3 and later) a depth/stencil attachment cleared at the
//                          start of a direct (sysmem) pass is cleared by one 3D draw through the depth unit (depth
//                          test ALWAYS, like vkCmdClearAttachments inside a pass) instead of the 2D engine, in every
//                          other second, so one run compares both. Build 71's pass meter: each 4096x4096 D16 shadow
//                          map spends ~1.1 ms in its clear before the first draw (~25 pixels per GPU clock, the same
//                          rate as the 2D engine's color clears), two maps plus a 2048x2048 one per frame. 1 = always
//                          the 3D draw, 0 = stock. Without the stats log nothing flips it, so alt stays on.
//                          Build 73 measured it: ~0.55 ms less per 4096x4096 map (2.25 -> 1.69 ms at the same load),
//                          but the 3D clear still ran the (empty) clear fragment shader on every pixel, all shader
//                          cores busy for ~0.5 ms. From build 74 alt keeps the 3D draw always and runs it without
//                          fragment shader ('fast Z', driver fex4 and later: FS_DISABLE as for depth-only pipelines,
//                          the depth from the vertex shader) in every other second; fastz = always without fragment
//                          shader. Build 74 measured fast Z: no gain (1.19 vs 1.19 ms per 4096 map), so from build 75
//                          the D2R default is 1 (the 3D draw always, no alternation).
//   FEX_TU_BUDGET_CACHE=1  (the D2R default from build 77, driver fex5 and later) the memory budget query
//                          (vkGetPhysicalDeviceMemoryProperties2 with VK_EXT_memory_budget) reads /proc/meminfo at most
//                          every 250 ms and reuses the last available memory in between; stock Turnip reads the whole
//                          file on every query. Build 76: D2R's main thread in that call for 3% of every second of play
//                          (~1 ms of every frame), as DXGI's QueryVideoMemoryInfo asks for the budget each frame.
//                          0 = stock. Works without the stats log too (it is part of the mode set before the game).
//   FEX_TU_CP_UBO=1        (the D2R default from build 98, driver fex9 and later, a7xx without consts via preamble like
//                          the a740) the ranges of push descriptor set UBOs that graphics shaders read (vkd3d's D3D12
//                          root constant buffers) are loaded into the shader's constants by the GPU's command
//                          processor before each draw (CP_LOAD_STATE6 from the address in the pushed descriptor),
//                          instead of by the shader's preamble through the descriptor. A draw whose shaders then no
//                          longer read the push set through descriptors keeps the previous descriptor set state when
//                          only the push set changed: no new bindless base and no invalidation of all 8 descriptor
//                          sets for each draw. D2R pushes new root constant buffers for nearly every draw; the fex28
//                          probe measured ~1.4 us more GPU time per long-shader draw when they change. Barriers
//                          before graphics uniform reads also make the command processor wait for earlier writes.
//                          The 'turnip' line's 'descriptor updates' then drop below 'push'. Read once at device
//                          creation and part of the shader keys, so shaders are compiled anew once. 0 = stock.
//   FEX_TU_UBO_TABLE=1     (the D2R default from build 99, driver fex10 and later, only with FEX_TU_CP_UBO) root
//                          constant buffers that shaders still read from memory, because they index more than fits the
//                          constants (D2R's skinning vertex shaders: bone matrices, cb2 with 1536 vectors), are read
//                          through the shader's own UBO table instead of the root set's descriptor. The driver writes
//                          the table entry from the pushed descriptor before each draw (CP_LOAD_STATE6 ST6_UBO). Those
//                          shaders then no longer read the root set through descriptors, so their draws keep the
//                          descriptor set state like the others. Build 98: draws that skipped the descriptor set change
//                          got 2.4x cheaper where the GPU front end was the limit (a 350-draw main pass at every start
//                          1.72-2.39 -> 0.79 ms), but shadow passes got slower (1.98 -> 2.35 us/draw): 76% of their
//                          draws use skinning vertex shaders, which still changed it for every draw and paid the
//                          constant loads on top. Read once at device creation and part of the shader keys. 0 = as
//                          build 98.
//   FEX_TU_SWITCH=alt      (the D2R default in build 100, driver fex11 and later, a7xx) at a graphics pipeline switch the
//                          driver only sends what changed, in every other second so one run compares both: a state
//                          block made of register writes whose bytes equal the block the GPU ran at the last draw keeps
//                          that block's address (the command processor skips it), and for pipelines without
//                          tessellation/geometry shaders only the vertex/fragment stage whose shader changed is reset
//                          (SP_UPDATE_CNTL with that stage's bit) and sent again with its config registers. Stock Turnip
//                          runs ~16 blocks again at every switch, most of them byte-identical (each pipeline has its own
//                          copy of its fixed-function state), and resets every shader stage, so an unchanged vertex
//                          shader or the empty fragment shader shared by depth-only pipelines is sent again too. Build
//                          99: each switch cost ~1.5 us of GPU time in the shadow passes and ~3.5 us in the depth
//                          prepass. 1 = always, 0 = stock. Without the stats log nothing flips it, so alt stays on.
//                          Build 101: off by default (build 100: blocks per switch 15.8 -> 4.1 but no GPU time saved).
//   FEX_TU_NOALIAS=alt     (the D2R default in build 101 only, 0 from build 102: no gain; driver fex12 and later, a7xx)
//                          the driver also compiles fragment
//                          shaders with 1,024+ instructions and 64+ alias.tex helpers (D2R's lighting shaders) without
//                          alias.tex, and pipelines use those binaries in every other second so one run compares both.
//                          alias.tex lets a texture fetch read its coordinates from scattered registers; each alias is an
//                          instruction of its own (17% of the lighting shaders). Without them the coordinates are moved
//                          next to each other by movs: 11-12% fewer instructions, 32 instead of 24 registers. The main
//                          pass costs about one lighting shader wave per draw, so its time should follow. The second
//                          binary is kept only when the pipeline state built from the shader is the same. Read once at
//                          device creation and part of the shader keys (one-time recompile). 1 = always, 0 = stock.
//                          Without the stats log nothing flips it, so alt stays on.
//   FEX_TU_WAVE_PAIR=alt   (the D2R default from build 105, driver fex14 and later) big fragment shaders (1,024+
//                          instructions, D2R's lighting shaders) run with another wave setup each second, in turn:
//                          0 stock, 1-3 SP_PS_CNTL_1.WAVE_PAIR_MODE 1, 2, 3 (a7xx; the driver writes 0, the values are
//                          not documented), 4 a second binary compiled as 128-pixel waves (ir3 keeps them at 64; same
//                          instructions and registers). Build 104: lighting waves spend 25-30% of their life in
//                          HWAVE_WAIT with or without preamble; a Qualcomm patent (US11954758, dynamic wave pairing)
//                          describes 64-pixel waves running as pairs that part at texture and branch blocks and join
//                          again, the one that got ahead waiting. A list of digits (e.g. 04) rotates only those, one
//                          digit = that setup always, 0 = stock. With setup 4 in the list, the second binary's mode bit
//                          follows the rotation (FEX_TU_NOALIAS / FEX_TU_NOPREAMBLE then only change how that binary is
//                          compiled). Read once at device creation (one-time recompile of those shaders). Without the
//                          stats log nothing rotates, so the first setup in the list stays.
//                          Build 106: default 0 (build 105: 128-pixel waves +12% slower, pair modes no reliable gain);
//                          only read when FEX_TU_DRAW_SETUP=0.
//   FEX_TU_DRAW_SETUP=4    (the D2R default from build 107; alt in build 106; driver fex15 and later, a7xx) big
//                          fragment shaders (1,024+ instructions, D2R's lighting shaders) run with another draw setup
//                          each second, in turn:
//                          0 stock, 4 a second binary with no preamble at all (push constants loaded by the command
//                          processor before each draw, the fragment stage not reading the shared constant registers),
//                          5 a third binary with one constant load per root constant buffer instead of one per range,
//                          6 SP_PS_CNTL_1.DEFER_WAVE_ALLOC_DIS 0, 7 EVICT_BUF_MODE 0 (the driver writes 1 and 1; not
//                          documented). Build 105: about one lighting draw's waves are on the shader cores at a time
//                          although there is room for more, so the next draw doesn't start into free slots; these are
//                          the per-draw setup steps that could hold it. A list of digits (e.g. 045) rotates only those
//                          (1-3 = FEX_TU_WAVE_PAIR's pair modes), one digit = that setup always, 0 = off. With setup 4 in
//                          the list, the second binary's mode bit follows the rotation. Read once at device creation
//                          (one-time recompile of those shaders). Without the stats log nothing rotates, so the first
//                          setup in the list stays. Build 107: 4 always (build 106: the binary with no preamble raised
//                          lighting draw overlap 10% and cut the lighting pass about 5%; the others did not help).
//   FEX_TU_CS_ATOMICS=alt  (the D2R default in build 107 only, driver fex16 and later) compute shaders whose memory atomics
//                          hit one address for the whole wave get a second binary that does each such atomic once per
//                          wave (one lane adds the wave's total, every lane gets its own slot back), as AMD's, Intel's
//                          and Apple's Mesa drivers do and Turnip didn't. In D2R that is the ground clutter shader
//                          (0e21a3b5088ba1af, a 1152x1152 grid, about 1.2 ms a frame, the biggest compute job): every
//                          item it places appends itself to one list with an atomic add on one counter. alt = that
//                          binary in every other second, so one run compares both ways (needs the stats log, without it
//                          alt stays on); 1 = always; 0 = stock. Read once at device creation (one-time recompile of
//                          compute shaders). With FEX_TU_SAMPLE_DRAWS above 0, every direct dispatch of 16 or more
//                          workgroups is timed and counted too ('turnip dispatch' lines; two GPU waits for idle each).
//                          Build 108: 0 (b107: that binary was 5.9% slower; atomics fell from 2457 to 514 per run, but
//                          only ~2,500 items are placed per run, so the counter never held the shader up).
//   FEX_TU_CONST_KEEP=alt  (the D2R default from build 110, driver fex17 and later, needs FEX_TU_CP_UBO's command
//                          processor loads) root constant buffer ranges whose buffer address didn't change stay in the
//                          shaders' constants instead of being loaded again with every draw. In the lighting pass about
//                          4 of the GPU's 12 shader cores sit idle between draws while the next draw is set up; every D2R
//                          draw changes one or two root constant buffers, and the driver then loaded all of the draw's
//                          ranges again (~100 vec4 per lighting draw, ~16 of them changed). Build 106 saw more constants
//                          per draw mean less overlap between draws. The driver keeps unchanged ranges in a draw state
//                          group of their own that the command processor skips while it is the same, and loads only the
//                          changed ones with the draw (host check: 640 -> 184 vec4 over 8 D2R-like draws). alt = in
//                          every other second, so one run compares both ways on the 'turnip pass ... keep on/off' lines
//                          (without the stats log alt stays on); 1 = always; 0 = stock. The 'constants kept' line counts
//                          the loads per constant state build either way.
// The stats log's 'turnip' lines (ARM64EC/TurnipReport.cpp) report them.
void SetupEnvironmentVariableValues(HMODULE NtDll);
}
