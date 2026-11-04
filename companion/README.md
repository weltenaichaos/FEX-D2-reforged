# D2R Companion (test app)

Android app for the AYN Thor's bottom screen. It mirrors the top screen (where
D2R runs in GameNative) with Android's screen capture and reads item text with
on-device text recognition (ML Kit, bundled, offline).

It only ever sees pixels, like a screenshot: no game memory, no injection, no
Battle.net traffic.

Test build features:
- **Read now / Read in 3 s**: read all text on the game screen, colored by D2R item quality.
- **Watch for drops**: every 2 s, log new unique, set and rune labels.
- **Save shot**: PNG + recognized text to `Download/D2RCompanion`.

Built by `.github/workflows/build-companion-apk.yml`; releases are tagged `d2r-companion-<run>`.
