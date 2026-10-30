// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
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
//                      other big cores until it waits under 10 ms a second for 5 seconds (longer if it keeps bouncing),
//                      and from build 93 for that long at most: then it tries its own core again either way (build 92
//                      log: widened at login, it never came back and ran 94% of play on the slower cores). While the
//                      kernel has its core paused (core control), it uses the game's other big cores and goes back
//                      as soon as the core is in use again. D2R default 40; "off" or 0 keeps it on its core at all
//                      times (paused or not, as up to build 92).
//   FEX_MAINCORE_ESCAPE=N  with "fast" (build 117, D2R default 4): a watcher thread (FEXMainWatch, on the slowest cores)
//                      reads the main thread's wait for a CPU every 4 ms; when it waited N ms or more within the last 8 ms
//                      the thread moves to the game's other big cores (not its own) for 100 ms, doubling up to 1.6 s while
//                      it has to leave again within 1 s of coming back (back to 100 ms after 10 s at home), then returns.
//                      A 'main escape' line shows each second with activity. FEX_MAINCORE_WAIT's once-a-second widening is
//                      off then (its "core paused" handling stays). Build 116 log: in play Android's memory reclaim
//                      (kswapd) or a root process ran on the main thread's core 7 for up to a second; Android's CPU groups
//                      put that work beside the app's whole group, so the main thread's nice -10 counted for nothing there
//                      and it got about half the core: it waited 30-115 ms within single frames (48-156 ms frames). The
//                      once-a-second widening reacted a second late and didn't help: allowed the other big cores too, the
//                      kernel kept it on core 7 (98%), waiting 278 ms that second. 0 or "off" = no watcher.
//                      Build 118: the build 117 watcher saw each wait only once it had ended (the kernel adds a wait to
//                      run_delay when the thread gets a CPU back), so the 23-93 ms waits that made slow frames in play
//                      ran their full length before it moved. It now checks every 2 ms whether the thread waits right
//                      now: state "R" at two checks in a row and its exact CPU time (its thread CPU clock) unchanged in
//                      between. Once it has been ready to run without a CPU for N ms it moves, still waiting, and runs on
//                      another core at once. The 'main escape' line adds how long the waits it left lasted in all and
//                      how soon it ran after the move. Without the stat file or the CPU clock it works as in build 117.
//   FEX_MAINCORE_ESCAPE_NICE=-20  its nice while away (D2R default -20, above the game's job workers at -10 there: on
//                      those cores they are in the same CPU group, so nice counts); "off" leaves it at FEX_MAIN_NICE.
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
//                      Build 77: also the threads /proc doesn't show. Android hides processes that aren't dumpable, such as
//                      the launcher's own app process (GameNative's Java process: it puts the game's frames on the screen
//                      and reads the controller), even from their own user, so build 76's sweep never saw them. Android
//                      groups every process of the app's user in cgroup v2 (/sys/fs/cgroup/uid_<user>/pid_<process>), and a
//                      group's cgroup.threads lists all its threads; those /proc doesn't show get every core but the main
//                      thread's ("# cores: a process hidden from /proc (pid N; ...) (group pid_N): ..."). In build 76 the
//                      main thread still waited ~40 ms a second for its core while ~17% of that core went to other threads,
//                      and the user's CPU use that no /proc process accounts for was ~0.14 of a core.
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
// Main thread. Wine runs it at nice -2 (D2R asks for TIME_CRITICAL, FEX_PRIO=normal puts it back to NORMAL, and Wine adds
// one for the foreground). In the build 82 log other programs (an app of another Android user and root's processes) took
// up to 87% of the main thread's core for 6 seconds at 4:58: the main thread waited 110-190 ms a second for a CPU, was
// eased onto the game's other big cores where the workers keep them ~90% busy, and 17 of the run's 58 frames of 40 ms or
// more came in those seconds. Linux shares a core between busy threads by their nice: at -2 against a nice-0 program the
// main thread gets ~61% of it, at -10 ~90%. (A short task that wakes up still runs first; nice limits only the ones
// that keep running.)
//
//   FEX_MAIN_NICE=N    keep the main thread at Linux nice N, checked every 250 ms (D2R default -10, as the wineserver;
//                      "off" leaves it to Wine). '# prio: main thread ... nice' notes say when it was set.
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
//                      then it isn't looked at for a minute), or (build 81) with at least 3 protection code samples and
//                      none in the game's code over 4 busy rounds (a thread with some protection samples and none of the
//                      game's is looked at again after 10 s), goes to the slowest cores (0-2 on the Thor) for good, at the
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
// Build 81: the loader thread gives few samples of its own code, since most of its time is in Wine calls. Over 4 busy
// rounds it had 3-7 protection samples and no game samples in every log from build 73 to 80; the game's workers had 0-1
// protection samples and 6-25 game samples. With 6 required, build 80 found it only at 1:29 (4 at 0:17), build 76 at
// 2:51 (3, then 5). Until then it ran at ~0.3-0.4 of a big core next to the game's workers: other tasks on the game's
// cores 0.75 of a core against 0.22-0.26 once it was moved, the main thread waited 34-44 ms a second for a CPU (24-26
// after), and build 80 had 13 frames of 40 ms or more in those 45 s of play against 2 in the next 139 s.
//
// Each loader thread found gets a "# loader: ..." line, and every second a "loader" line with how long it ran and how
// long it waited for a CPU; game threads found this way are on it too, with their exceptions in the game's code a
// second. With the stats log, a loader thread's calls get a "loader calls" line too (see ARM64EC/LoaderWatch.h).
//
// Job workers (build 87). D2R runs each frame's parallel tasks on a job system: the main thread queues them, worker
// threads run them, and the main thread polls the queue until they are done (the frame wait loop, see
// ARM64EC/NativeRedirect.cpp). In the build 86 log busy scenes had the main thread polling ~12 ms a frame (~380 ms a
// second in the busiest seconds, 22% of its samples in the frame function that holds the loop) while each of the 4
// workers ran 200-250 ms a second and waited 85-150 ms a second for a CPU (calm play 110 ms run, 35-50 ms waited). D2R
// itself asks for HIGHEST for its workers (Wine: nice -5); FEX_PRIO=normal has put them at nice 0 since build 48, under
// the main thread and the wineserver (both -10) and level with every other program's threads.
//
//   FEX_WORKER_NICE=alt  the workers at nice -10 in every other 10 s window (seconds 1-10, 21-30, ... of the stats log,
//                      the same windows as its frames10s lines), at nice 0 (FEX_PRIO=normal; else their own) in the
//                      others: one run compares both. "alt-5" uses -5. The D2R default in build 87.
//   FEX_WORKER_NICE=N    keep them at nice N (the D2R default -10 from build 88: in build 87's comparison the raised
//                      workers waited 42% less for a CPU, lost it 80% less often, the main thread polled 20% less for
//                      their jobs, and play had a quarter fewer slow game frames; the wineserver waited 86 ms a second
//                      for a CPU against 54, with no change in frame times or lag)
//   FEX_WORKER_NICE=off  leave them to Wine and FEX_PRIO
//
// A worker is a thread other than the main thread that calls the job system's getter (every queue operation does); loader
// threads (FEX_LOADER_CORES) are left out. Their nice is checked every 250 ms. With the stats log, a "# workers: ..."
// line names each one found, a "workers" line every second gives their CPU time, their wait for a CPU, how many times
// they got one (runs) and the average wait for each, how many times they went to sleep (slept) or lost their CPU while
// they could run (preempted), and their job system calls; a "# workers: seconds A-B ..." line closes each window with
// its averages, the main thread's polling time, and the totals of each half so far.
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

// Where the game's job workers come from (ARM64EC/NativeRedirect.cpp): fills Out with up to Max threads, other than the
// main thread, that called the job system, by TEB, with their calls so far, and MainSpinNs with the main thread's time
// spent polling the job queue in its frame wait loop so far. Returns how many. Runs on the guard's and the log's threads.
struct JobWorker {
  uint64_t Teb;
  uint64_t Calls;
};
using JobWorkerSource = size_t (*)(JobWorker* Out, size_t Max, uint64_t* MainSpinNs);
void SetJobWorkerSource(JobWorkerSource Source);

// Called on the guard's thread for every loader thread it finds (see FEX_LOADER_CORES), with its TEB, and again with Teb 0
// when it ended. Set before StartPriorityGuard.
void SetLoaderThreadHook(void (*Hook)(uint32_t TID, uint64_t Teb));
} // namespace FEX::Windows
