# FAQ: Diablo II: Resurrected on Android

Short answers to common questions. Setup steps are in [d2r-thor-setup.md](d2r-thor-setup.md), the background in
[d2r-thor-findings.md](d2r-thor-findings.md), every fix's on/off switch in [d2r-switches.md](d2r-switches.md).

## What is this, and who is it for?

It makes the **Steam version of Diablo II: Resurrected** run smoothly on **Android** in **GameNative**. It replaces four
parts of GameNative's setup with versions changed and measured for D2R: FEX (runs the game's x86 code on ARM),
VKD3D (DirectX 12 to Vulkan), the Turnip graphics driver, and the frame generation layer. It is made and tested on the
**AYN Thor**.

## Does it work on my device?

Only the AYN Thor (Snapdragon 8 Gen 2, Adreno 740) has been tested. Some things to know before trying another device:

- The FEX builds are compiled for ARMv8.4-a CPUs. Chips older than the Snapdragon 8 Gen 1 may not run them.
- FEX picks the fastest core and the little cores by their top clock speeds, so its thread placement should adapt to
  other chips, but it has only been tuned on the Thor's layout (one fast core, four big, three little).
- The Turnip build has only run on the Adreno 740.

If you try it on another device, please open an issue with a log either way. That helps more than anything else.

## Which version of the game?

The Steam version, started through GameNative. Others have not been tested. The FEX fixes find the game's code by its
shape, so game updates have kept working so far.

## Clean or logging FEX version?

Play with the **clean** version (`2610-d2rfix28-thor-clean`). It writes nothing, unless the game crashes, in which case
it leaves one small `fexcrash` file. Use the **logging** version (`2610-d2rfix28s-thor`) when something goes wrong
and you want to report it: it writes a detailed log of every session. Both have the same fixes and can be installed side
by side.

## Do I need Lossless Scaling?

Only for frame generation. GameNative's frame generation uses `Lossless.dll` from Lossless Scaling on Steam, so you
need to own it there. With it, the game runs at a steady 30 FPS and you see 60.

Without frame generation you see only the real frames. In earlier tests that was about 40 FPS in most play, with
GameNative's FPS limit at 40 and the screen at 120 Hz, and around 35 in the biggest fights. At 60 Hz a limit of 40
rounds down to 30, so keep the screen at 120 Hz.

## Why not 60 real frames?

The graphics chip is the limit in fights. Every draw D2R makes has a fixed cost on the Adreno 740, so a busy fight with
thousands of draws per frame takes longer than 16.7 ms on the GPU no matter the resolution. A lot of that cost was cut
(see the findings page), but 60 real frames in fights are out of reach on this chip. Frame generation from 30 gives a
steady 60 on screen.

## Why does it stutter the first time I enter an area?

FEX translates the game's code the first time it runs and keeps the result in its code cache. New areas run new code,
so the first visits stutter. It gets better the more you play. Back up `AppData\Local\fex-emu\DiskCache` from the
container's C: drive once it is well filled; if the cache is ever reset, put it back. A game patch changes the code, so
part of the cache is rebuilt after one.

## How much storage does it use?

FEX keeps two caches in the container's `AppData\Local\fex-emu`: `DiskCache` (translated code) and `UnpackCache`
(unpacked game data, so loading screens don't unpack the same data again; at most 16 GB). The logging version also
writes several MB of log per minute of play to `D:\`. The clean version writes no log.

## Where is `D:\`?

`D:` is a drive GameNative maps to a folder on your device. The container's drive settings show which folder.

## Is it safe to play online?

This is an unofficial project, not affiliated with Blizzard. It changes no game files and doesn't touch Blizzard's
protection. FEX does run a few of the game's routines (data unpacking, checksums, one wait loop) as native ARM code
with the same results, and VKD3D swaps two of the game's shaders for faster ones that draw the same. Play online at your
own risk.

## After a GameNative update, smearing or crashes came back

GameNative copies its own frame generation layer back when it updates. Copy ours over it again (step 3.3 of the setup
guide) and check that the file is 2,755,712 bytes.

## Can I use a newer Proton, or other FEX, VKD3D or Turnip versions?

Use GameNative's **Proton 11.0-1 (arm64ec)**; 11.0-2 crashes D2R. The four parts of a release are made to work
together. FEX's switches turn on the fixes inside our VKD3D, Turnip and layer, so with other versions of those parts
their fixes are simply missing.

## Can I turn a fix off?

Yes. Every fix has a switch, set as an environment variable in the container. See [d2r-switches.md](d2r-switches.md).
Please mention any switch you changed when you report a problem.

## Does it help other games?

The fixes are switched on for `D2R.exe` only. For every other program FEX behaves like upstream FEX with Dmage22's
launcher fixes. Other games have not been tested with these builds.

## How do I report a problem?

Open an [issue on GitHub](https://github.com/weltenaichaos/FEX-D2-reforged/issues/new/choose). The form asks for your
device, the release, which FEX version you used, and a log: switch to the logging version, play until the problem shows
up, and attach `D:\fexstats-D2R.exe-b<N>-<time>-<pid>.txt` (zip it if it is large). If the clean version crashed,
attach the `D:\fexcrash-D2R.exe-...txt` file instead.
