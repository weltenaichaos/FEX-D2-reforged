// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

namespace FEX::Windows {
// Keeps the process's threads in a narrow priority band, puts them on the right cores, and lifts the wineserver.
//
// Priorities. Wine on Linux maps Windows priorities onto nice values across the whole -19..19 range when RLIMIT_NICE
// allows it (it does on Android): IDLE becomes nice 19 (about 1.5% of a core next to a busy normal thread), LOWEST
// nice 6, HIGHEST nice -5, TIME_CRITICAL nice -19, and a HIGH priority class puts every thread at nice -13, above the
// wineserver. Under CPU contention a low priority thread holding a lock others need, or a wineserver starved by the
// game's own threads, stalls everyone. Desktop Linux never sees this: RLIMIT_NICE is 0 there, so Wine leaves every
// thread at nice 0. FEX_PRIO=normal gives the same picture on Android.
//
//   FEX_PRIO=keep    leave priorities alone (upstream; the default outside D2R)
//   FEX_PRIO=normal  priority class normal, every thread at NORMAL (nice 0; Wine still gives the first thread -2); the
//                    D2R default
//   FEX_PRIO=clamp   priority class normal, thread base priorities clamped to BELOW_NORMAL..ABOVE_NORMAL (nice 3..-2)
//
// Cores. In the build 48 log D2R's threads all had cores 3-7 (the container's setting on the Thor) except its main
// thread, which Wine reported free to run on every core, the slow A510s included. The main thread does most of each
// frame's work alone, and in fights the stall probe saw threads wait up to 26 ms for a CPU, so it gets the fastest core
// (the Cortex-X3) to itself:
//
//   FEX_MAINCORE=fast  the main thread alone on the fastest core; the game's other threads, FEX's threads and the
//                      wineserver keep their cores minus that one (the D2R default). Falls back to "big" when the
//                      container doesn't allow the fastest core.
//                      The main thread's Linux cores are also checked 4 times a second and set back when
//                      something outside Wine changed them (build 50 lost the pin to Android or the launcher after 34
//                      seconds). CPUs the kernel pauses for a while (core control, heat) only get a "# cores: CPUs ...
//                      paused" line: the thread's own mask is unchanged then.
//   FEX_MAINCORE=big   the main thread on the same cores as the game's other threads
//   FEX_MAINCORE=off   leave affinities alone
//   FEX_MAINCORE_WAIT=N  with "fast": while the main thread waits N ms or more a second for its one core (login and
//                      loading, where the wineserver and other threads share that core), it may also use the game's
//                      other big cores until it waits under 10 ms a second for 5 seconds (longer if it keeps bouncing).
//                      D2R default 40; "off" or 0 keeps it on its core at all times.
//   FEX_MAINCORE_OTHERS=user  with "fast": every other thread of the app's Linux user stays off the main thread's core
//                      too, checked every 2 seconds at the Linux level: Wine's other programs (steam.exe, winedevice.exe,
//                      explorer.exe ...), the launcher's own process and this process's threads Wine doesn't list (the D2R
//                      default, build 76). A thread loses only that core; one that could run nowhere else gets the game's
//                      other cores. "# cores: <name> (pid N): ..." lines name what moved.
//                      Build 75 timed each slow game frame: in play the main thread waited for its core 7 ms per slow frame
//                      (up to 28 ms), nearly always inside SetEvent (7.5 of the frame's 2 ms samples where it waited 10 ms or
//                      more, 0.2 elsewhere), while threads other than it and the game's used ~25% of that core. Of the
//                      user's other programs only steam.exe's CPU use followed those waits (correlation 0.75-0.97 over 5 s
//                      windows in the build 74, fex14, fex15 and build 75 runs; winedevice, pulseaudio and the wineserver
//                      0.6 or less). Each frame the game calls the Steam client, a round trip to steam.exe through shared
//                      memory and events; the event wakes a steam.exe thread, Linux may start it on the waking thread's core,
//                      and the main thread then waits there until Steam is done with that core.
//   FEX_MAINCORE_OTHERS=process  only this process's threads Wine doesn't list
//   FEX_MAINCORE_OTHERS=off  other programs may run on the main thread's core (build 75 and earlier)
//   FEX_HELPER_CORES=little  the stats log thread (a 2ms sampler), this guard, the rumble sender (FEXRumble) and the
//                      file cache dropper (FEXFileCache) on the slowest cores, off the game's (the D2R default), when the
//                      process may use them. The stall probe, disk cache and unpack cache threads stay on the game's cores.
//
// Wineserver. Every file, pipe, controller and window message request of every Wine process goes through the one
// wineserver thread; under load the build 48 probe saw single round trips take up to 27 ms.
//
//   FEX_SERVER_NICE=N  run the wineserver at Linux nice N (D2R default -10; "off" leaves it alone)
//
// Loader threads. In the fex10 run (FEX build 61) one thread of D2R's (tid 576) ran almost only code in a private
// allocation next to D2R_loader.dll (the game's protection/loader code), not in D2R.exe or any loaded module. In 36% of
// the play time it was busy: a CreateEventW / WaitForMultipleObjects / CloseHandle loop at ~0.65 of a big core, plus ~0.25
// of a core of extra wineserver work. In those phases calm play had 3 times as many frames of 40 ms or more (0.25 a
// second against 0.08), fights 7 times as many (4.0 against 0.6), and the main thread waited twice as long for a CPU. It
// ran on the same big cores as the game's workers and the wineserver.
//
// Build 62 counted every sample outside an image view as loader code. D2R's protection remaps D2R.exe into a view that is
// no longer an image, so it moved three of the game's worker threads (their code is in D2R.exe) to the slow cores, and the
// game got slower everywhere (calm play 5 times as many frames of 40 ms or more, the main thread at 62-78% of its core
// instead of 40-49%, spinning in the job wait). Since build 63 each code sample (4 a second, the guest code of the JIT
// block the thread last entered) counts as:
//   game code        in the main exe's range as loaded (read at process start, before the remap), or in an x64 DLL
//   protection code  in committed memory that is neither: D2R_loader.dll's remapped code and what it decrypts
//   neither          in an ARM64EC DLL (Wine's, VKD3D, DXVK): a thread in a Wine call (a wait, CreateEventW) was last in
//                    that function's x64 entry stub, which says nothing about its own code; or unreadable
//
//   FEX_LOADER_CORES=little  every thread that runs at 25% of a core or more (over 2 second rounds), with at least 6
//                      protection code samples and 3 times as many as game code samples over its busy rounds (up to 4,
//                      then it isn't looked at for a minute), goes to the slowest cores (0-2 on the Thor) for good, at the
//                      Linux level: GameNative's container lets Wine use cores 3-7 only, but Linux lets the process use
//                      all 8. Its Linux cores are checked 4 times a second and set back when something changed them (the
//                      D2R default).
//   FEX_LOADER_CORES=find    find and log them, leave them where they are
//   FEX_LOADER_CORES=off     leave them where Wine puts them
//
// Game threads. In the build 64 log the guard moved a second thread (tid 784) to the slowest cores at 2:37. Its samples
// were mostly protection code, but it runs the game's own code: the protection replaced instructions in D2R.exe with
// int3, and each one is an exception (~800 a second, nearly all of the run's) that the protection's handler resolves
// and continues, so most of the thread's time is in that handler. On the slow cores it needed twice the CPU for the same
// work, and in its busy phase at 3:12-3:24 it took a whole slow core, right before the area change where the game
// rubber banded. The loader thread (tid 584) raises few exceptions (its NtContinue time was 1/65 of tid 784's).
//
//   FEX_LOADER_GAME_EXCEPTIONS=N  a thread that raises N or more exceptions in the main exe's code within a 2 second
//                      round is a game thread: it is never moved to the slowest cores, and moved back if it was (the D2R
//                      default 400; 0 turns this off)
//
// Each loader thread found gets a "# loader: ..." line, and every second a "loader" line with how long it ran and how
// long it waited for a CPU; game threads found this way are on it too, with their exceptions in the game's code a
// second. With the stats log, a loader thread's calls get a "loader calls" line too (see ARM64EC/LoaderWatch.h).
//
// FEX's own threads (named FEX*) keep their priorities. Checked every 2 seconds, since a game can change priorities and
// affinities at any time. Each change is written to the FEX_STATSLOG log as a "# prio ...", "# cores ..." or
// "# server ..." line.
void StartPriorityGuard(std::string_view AppName);

// Every emulated thread registers as it starts and unregisters as it ends (ARM64EC ThreadInit/ThreadTerm), so the guard
// knows its Linux tid and can see which guest code it runs. BlockHeader points at the thread's InlineJITBlockHeader,
// GameExceptions at its count of exceptions raised in the main exe's code; the guard reads them only while the thread
// is registered, and GuardThreadEnded waits for any such read to finish.
void GuardThreadStarted(uint32_t TID, int LinuxTid, uint64_t Teb, const volatile uint64_t* BlockHeader, const volatile uint64_t* GameExceptions);
void GuardThreadEnded(uint32_t TID);

// Called on the guard's thread for every loader thread it finds (see FEX_LOADER_CORES), with its TEB, and again with Teb 0
// when it ended. Set before StartPriorityGuard.
void SetLoaderThreadHook(void (*Hook)(uint32_t TID, uint64_t Teb));
} // namespace FEX::Windows
