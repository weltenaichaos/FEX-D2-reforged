// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/HLE/SyscallHandler.h>

namespace FEXCore::Context {
class Context;
}

// FEX's buffer for translated code (FEX_CODE_BUFFER_START, FEX_CODE_BUFFER_RENEW). All threads place the code they
// translate, or load from the disk cache, in one shared buffer that is only ever appended to. When it is full FEX starts
// a new one and drops everything in the old one, so every thread translates or loads all the code it runs again. Upstream
// starts at 16 MB and doubles up to 128 MB, dropping everything at each step.
//
// In the build 83 log the main thread loaded 5,301 blocks from the disk cache in one second at 6:21 (89 ms of its time,
// frames of 58-78 ms over the next 11 s), and every game worker thread started loading at the same moment, with no file
// reads and no area change. That is what a full buffer looks like. D2R's code is ~1.8 KB a block, and at that size the
// fourth start (16 + 32 + 64 + 128 MB) comes after ~135,000 blocks: where that run was at 6:21. Builds 80-83 also all
// reloaded at second 6, at ~60,000 blocks, the third start. D2R places ~100,000 blocks before play (start, menus, the
// first load), each area load several thousand more, and calm play ~40 a second, so the buffer fills again in play.
//
//   FEX_CODE_BUFFER_START=max   start at 128 MB (the D2R default; upstream does this when it writes its own code cache)
//   FEX_CODE_BUFFER_START=grow  16 MB, doubling (upstream)
//   FEX_CODE_BUFFER_RENEW=N     at a loading screen (no new frame for FEX_CODE_BUFFER_LOAD_MS), start a fresh buffer if
//                               the current one is N% used or more (D2R default 50), at most once a load: the reload then
//                               falls in the load and the first seconds of the new area, which load new code anyway,
//                               instead of a random moment of play. "off" or 0 never does.
//   FEX_CODE_BUFFER_RENEW_EARLY=N  the same for the first 3 loading screens (start-up, menus, entering the game; D2R
//                               default 10): the buffer then holds mostly start-up and menu code. In the build 83 run it
//                               would have been ~25-45% used when the game was entered, and the area change at 3:01 was
//                               seamless (no loading screen), so with 50% alone it would still have filled in play.
//   FEX_CODE_BUFFER_LOAD_MS=N   how long without a frame counts as a loading screen (default 250; play frames stay
//                               under 100 ms). Build 94: frames that ask the GPU for little count too (LoadScreen.h,
//                               FEX_LOAD_LIGHT): D2R draws most of a loading screen at full speed, and in the build 93
//                               run the buffer filled in play after a 5 s loading screen at 40 FPS went unseen.
//   FEX_CODE_REUSE=1            (D2R default) code loaded from the disk cache whose blocks were dropped since (the guest
//                               changed or rewrote its pages) and that comes back byte for byte the same is mapped again
//                               from the copy already in the buffer, instead of placing another copy. In the build 84 run
//                               calm play placed ~78 blocks a second (~8% of the buffer a minute), nearly all from the
//                               disk cache, more than D2R's whole working set every 3 minutes. 0 = upstream.
//
// With the stats log on, a "# code buffer" line names every new buffer (why, how full the old one was, how many blocks
// it held and how many of those came from the disk cache) and every loading screen seen, and every 10 s says how full the
// buffer is, what was placed in it and served again from it, and how many blocks were dropped by what (protection
// changes by their new protection, write faults, frees, unmaps, instruction cache flushes, file reads). Build 94 adds
// the megabytes placed each way and the placements of 64 KB or more by guest address (how many, how big, translated or
// from the disk cache, how much guest code): in calm play of the build 93 run the buffer grew by megabytes in windows
// that placed only a few hundred small blocks.
namespace CodeBufferPolicy {
// Call on the process's first thread, in process init, right after InitCore and before any code is translated.
void Init(FEXCore::Context::Context* CTX, bool StatsLog);

// Whether the frame clock (NativeRedirect.cpp) should run: a loading screen is told by the time since the last frame.
bool WantsFrameMarks();

// The main exe's range, so the log names big placements' guest code as exe+offset (build 94).
void SetMainImage(uint64_t Base, uint64_t Size);

// From the syscall handler's OnCodeBufferStarted: records it for the log (runs under FEX's code locks, so it only stores).
void NoteStarted(const FEXCore::HLE::SyscallHandler::CodeBufferStart& Info);
} // namespace CodeBufferPolicy
