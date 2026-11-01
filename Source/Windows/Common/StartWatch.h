// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace FEX::Windows {
// Build 128 (FEX_START_WATCH): why a D2R start sometimes ends after a black screen with D2R and Steam both gone. In the
// failed start of 2026-11-01 (log fexstats-D2R.exe-b126-215523-504) the game ran normally for about a second, then the
// process was gone with no exit record: it didn't end itself, something ended it from outside (a TerminateProcess from
// another process, or the launcher closing Wine). In good starts the next thing at that point is Steam putting its
// overlay into the game.
//
// For every start this writes a small file D:\fexstart-D2R.exe-b<N>-<time>-<pid>.txt (in both the logging and the clean
// build) with:
//   - this process's parent and every process of this Linux user (Wine's programs such as steam.exe, the wineserver,
//     the launcher) at the start, then every process start and end of that user to the millisecond for the first
//     FEX_START_WATCH_S seconds (default 120), and a heartbeat each second, so the last lines of a failed start say
//     who went away first and when this process stopped;
//   - the end of Steam's own logs (C:\Program Files (x86)\Steam\logs: console_log, bootstrap_log, connection_log, ...):
//     at the start, which holds the previous Steam session (a failed start's), and again after 60 s.
// Every line is written at once (no buffering), so the file is complete up to the moment the process ended.
// FEX_START_WATCH=<folder> sets the folder, 0 turns it off.
void StartStartWatch(std::string_view AppName);
} // namespace FEX::Windows
