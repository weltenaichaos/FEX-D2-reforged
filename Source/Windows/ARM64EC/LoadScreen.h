// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Build 94: loading screens told by how little the GPU is asked to draw (FEX_LOAD_LIGHT, FEX_LOAD_LIGHT_MS).
//
// Up to build 93 FEX took "no new frame for 250 ms" for a loading screen: the code buffer was renewed there
// (CodeBuffer.h), the learned game data and unpack cache lists were read in there and left the main thread's reads there
// out (GameFiles.h, NativeUnpackCache.cpp). But D2R draws most of its loading screen at full speed while its other
// threads load: only the step into a load stalls a frame. In the build 93 run the 5 s loading screen at 6:23 ran at
// 40 FPS with no frame over 72 ms, so none of that happened there. The code buffer, last renewed when the game was
// entered, filled in play at 8:35 instead (every thread reloaded its code, frames of 44-96 ms), and the main thread's
// unpacks in that loading screen were learned as play. Build 92's loading screens ran smooth for up to 16 s too.
//
// What tells them apart is the GPU work: a loading screen frame records ~7-9 push descriptor updates (vkd3d's root
// constant buffers, about one per draw: the frame is a picture and a progress bar), the menus ~33-37, the title screens
// 5-18, while every second of play in the builds 88-93 logs recorded 190 or more per frame (mostly 1,000-2,500; seconds
// that were part loading screen, part play, 44-99).
// FEX's Turnip build counts them in the FEX link (TurnipChannel.h, 'PushTotal'). A thread looks every 50 ms: frames
// that came with fewer than FEX_LOAD_LIGHT updates each, for FEX_LOAD_LIGHT_MS or longer, are a loading screen
// ("GPU-light") until a frame with more comes.
//
//   FEX_LOAD_LIGHT=N     push descriptor updates per frame below which frames are a loading screen's (D2R default 64);
//                        0 or off = only "no frame for 250 ms" (builds 84-93)
//   FEX_LOAD_LIGHT_MS=N  how long frames must stay that light (default 500, 100-5000)
//
// Without FEX's Turnip build (or with FEX_TU_CHANNEL=0) nothing counts the updates, and only frame gaps count. With the
// stats log on, a '# load: GPU-light' line names each such stretch when it is seen and when it ends.
namespace LoadScreen {
// Call on the process's first thread, in process init, after the Turnip channel is set up (SetupEnvironmentVariableValues);
// Wanted: something uses loading screens (the code buffer renewal, the learned lists), so the frame clock runs.
void Init(bool StatsLog, bool Wanted);

// Whether frames are coming GPU-light now (a loading screen that still draws frames).
bool LightNow();

// Whether frame Frame (a FEXFrameMarks[0] value taken while it ran: it began at mark Frame - 1) was in a GPU-light
// stretch: 1 yes, 0 no, -1 not decided yet (the frames around it were light but not yet for FEX_LOAD_LIGHT_MS).
int FrameLight(uint64_t Frame);
} // namespace LoadScreen
