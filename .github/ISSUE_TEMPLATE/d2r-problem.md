---
name: D2R problem report
about: Something wrong with Diablo II Resurrected on Android (crash, hitch, lag, graphics, frame generation)
title: "[D2R] Short description of the problem"
labels: ''
assignees: ''

---

**What happened**
What you saw, and what you expected instead.

**When it happens**
Loading screen, town, fights, map open, after how many minutes, every time or once.

**Device and setup**
 - Device: [e.g. AYN Thor, Snapdragon 8 Gen 2]
 - GameNative version:
 - Release used: [e.g. TEST version 4]
 - FEX in the container: [clean `2610-d2rfix28-thor-clean` or logging `2610-d2rfix28s-thor`]
 - Proton: [should be 11.0-1 arm64ec]
 - Frame generation: [on/off; multiplier, FPS limit, flow scale]
 - Anything you set differently from the setup guide (environment variables, CPU cores, driver):

**Log**
Switch the container's FEX to the logging version (`2610-d2rfix28s-thor`), play until the problem shows up, and attach
`D:\fexstats-D2R.exe-b<N>-<time>-<pid>.txt` (zip it if it is large).
If the clean version crashed, attach `D:\fexcrash-D2R.exe-...txt` instead.

**Screenshots or video**
If it is something you can see on screen.
