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
//   - build 132: this app's entries in Android's log (logd lets an app read its own user's entries): warnings and errors
//     of the launcher (GameNative) and crash lines, including those of a previous failed start, at the start and then
//     every 250 ms; and the launcher's own app process: in the build 131 failed start every process of the container
//     went away at once, which is what Android does when that process dies. (Build 129 copied Steam's logs instead; in
//     GameNative's container they are stale.)
//   - build 134: every log entry of the last 10 minutes (info level too) at the start and every entry in the first 30 s,
//     and the wineserver checked every 2 ms (in the build 133 failed start no warning or error came first).
//   - build 136: written through Linux directly (not Wine), so lines keep coming when a Wine lock or the wineserver
//     hangs; '## threads' blocks list every thread's state, kernel wait and system call once at 1 s and whenever the
//     process used under 100 ms CPU in a second during the first 30 s (a hang instead of an end).
// Every line is written at once (no buffering), so the file is complete up to the moment the process ended.
// FEX_START_WATCH=<folder> sets the folder, 0 turns it off.
void StartStartWatch(std::string_view AppName);
// A line for the start watch file now (time-stamped), from any thread; nothing when the start watch is off.
void StartWatchNote(std::string_view Text);
// Whether the start watch file is open (build 140: lets callers skip work for a note nobody writes).
bool StartWatchActive();
} // namespace FEX::Windows
