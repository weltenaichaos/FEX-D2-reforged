// SPDX-License-Identifier: MIT
#pragma once

// Controller rumble (FEX_RUMBLE). In fights D2R's render workers call XInputSetState for every change of the rumble
// strength. Wine's xinput sends each change to the controller before returning: it takes the controller's lock (the same
// one its input thread needs for every input report), then a HID output report goes through the wineserver to
// winedevice.exe's HID driver and on to the launcher's controller code, and comes back the same way, every hop a thread
// that has to get a CPU. In the build 48 and build 52 fights every one of the game's threads (even the wineserver at nice
// -10, and FEX's 4 ms sleeper) waited 3-5 s a second for a CPU, and the render workers spent up to 10% (build 52) and
// 38% (build 48) of a second inside XInputSetState, three at a time, while the main thread waited for their jobs. Outside
// fights the game doesn't rumble, and none of that happens.
//
//   FEX_RUMBLE=async  the game's XInputSetState calls return at once; a FEX thread (FEXRumble) hands the latest strength
//                     to Wine's XInputSetState when it changed, at most once every FEX_RUMBLE_MS per controller (the D2R
//                     default). The first call for a controller, and every call while the last one failed (no controller
//                     there), still goes to Wine on the game's thread, so the game sees the same results.
//   FEX_RUMBLE=off    the same, but the controller always gets strength 0 (no rumble)
//   FEX_RUMBLE=keep   the game calls Wine's XInputSetState itself (upstream; the default outside D2R)
//   FEX_RUMBLE_MS=N   shortest time between two rumble changes sent to one controller (default 50; 10-1000)
//
// Only the game's own (x64) calls are taken over: Module.S ExitFunctionEC runs FEXNativeRumbleSetState in place of
// XInputSetState once this finds xinput's entry. With the stats log on, a "rumble" line says every second how many calls
// the game made, how many changed the strength, how many changes went to the controller and how long Wine took for them.
namespace Rumble {
// Call on the process's first thread, in process init, after the switches are read.
void Init(bool StatsLog);
} // namespace Rumble
