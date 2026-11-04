# D2R Companion (test app)

Android app for the AYN Thor's bottom screen. It mirrors the top screen (where
D2R runs in GameNative) with Android's screen capture and reads item text with
on-device text recognition (ML Kit, bundled, offline).

It only ever sees pixels, like a screenshot: no game memory, no injection, no
Battle.net traffic.

Test build features:
- **Read now / Read in 3 s**: read all text on the game screen, colored by D2R item quality.
- **Watch for drops**: every 2 s, log new magic, rare, set, unique and rune labels that name a known item.
- **Drop history**: every logged drop is saved in the app (`drops.tsv`: time, session, run, quality, name, item, raw read); *Undo last drop* removes a wrong hit.
- **Run counter** (while watching): a run starts when the life globe comes back after a loading screen and at least the *run gap* (10-60 s) away from the game. *+1 run / -1 run* fix the count by hand.
- **Save shot**: PNG, found text boxes, recognized text, on-screen log and the drop history to `Download/D2RCompanion`.
- **Self-update**: on start it checks the GitHub releases for a newer `d2r-companion-<build>` and offers to install it.

Built by `.github/workflows/build-companion-apk.yml`; releases are tagged `d2r-companion-<run>`.
