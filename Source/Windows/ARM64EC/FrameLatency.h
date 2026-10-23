// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Frame latency (FEX_FRAMELATENCY). In the build 49 log every D2R frame took the main thread's own work plus an almost fixed
// ~20 ms wait in WaitForSingleObjectEx, one after the other: when the main thread's work grew from 12 to 16 ms, frames grew
// from 33 to 37 ms while the wait stayed at ~20 ms. That is what a swapchain with a frame latency of one does: DXGI's
// frame latency handle (a semaphore that vkd3d-proton's swapchain thread releases once a presented frame is done) lets
// the game start frame N+1 only after frame N went through the GPU and the presentation path, so they never overlap.
// One frame is DXGI's default for swapchains made with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT.
//
// This watches which handles the main thread waits on (ExitFunctionEC in Module.S records each call to
// kernelbase!WaitForSingleObject(Ex) from it), and when one is a frame latency handle (a semaphore the main thread waits on
// at least 10 times a second for two seconds in a row, which it can only wait on: DXGI and vkd3d-proton hand it out with
// SYNCHRONIZE access only, and vkd3d-proton makes it with a maximum count of 16), releases it N-1 more times, the same as
// the game calling IDXGISwapChain2::SetMaximumFrameLatency(N): the main thread can then prepare the next frame while the
// GPU draws the last one.
//
//   FEX_FRAMELATENCY=2    one frame more in flight; 3 or 4 for more
//   FEX_FRAMELATENCY=off  leave the handle alone (also 0 or 1; the D2R default since build 51); the waits are still logged
//                         with the stats log on
//
// Build 50 result (D2R on the Thor): the handle was found and released, the main thread then ran a frame further ahead,
// and the ~20 ms wait moved from the handle into vkd3d-proton's Present (its own limit of the same depth, released by the
// same present completions). Heavy scenes stayed at 37-42 ms a frame: the stage after the CPU (GPU or present path) is
// the limit there, not the hand-off. One more frame in flight then only adds input lag, hence off by default.
//
// With the stats log on, every second gets a "waits main" line: the handles the main thread waited on, how often, where
// from, and how long it was sampled waiting in each. "# latency" lines say what was found and done.
namespace FrameLatency {
// Call on the process's first thread (the game's main thread), in process init.
void Init(uint64_t MainImageBase, bool StatsLog);

// Whether the frame clock (NativeRedirect.cpp) should run so frames can be counted, even without the stats log.
bool WantsFrameMarks();
} // namespace FrameLatency
