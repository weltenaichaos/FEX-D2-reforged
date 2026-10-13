Everything needed to run Diablo II Resurrected (Steam) on the AYN Thor in GameNative, as of 2026-10-13: 60 FPS shown (30 real + frame gen 2x).

## Files
| File | What it is |
|---|---|
| `fexcore-2610-d2rfix28-thor-clean.wcp` | FEX build 24 for normal play (no stats log). Maps the disk cache file for faster cache loads. Uses the same translation settings as build 15, so an existing FEX disk cache stays valid. |
| `fexcore-2610-d2rfix28s-thor.wcp` | The same FEX build with the optional stats log (`FEX_STATSLOG=<folder>`, `FEX_STATSLOG_APP=D2R.exe`), for diagnosis only. |
| `liblsfg-vk-layer.so` | Fixed Lossless Scaling frame gen layer (lsfg-d2r-6). Fixes the D2R crash, the 12/15 FPS locks and late-frame stutter. Copy it over GameNative's copy in `Z:\home\xuser-<container>\.local\lib\` (2,641,968 bytes). |
| `vkd3d-proton-3.0b-arm64ec.wcp` | vkd3d-proton 3.0b built as native ARM64EC (about +5 FPS). |

## Settings
- FEX: thor-clean, CPU cores 3-7, `FEX_DISKCACHE=1`
- DX12: the ARM64EC vkd3d above, DXVK 2.7.1 dxgi; do **not** set `VKD3D_DISABLE_EXTENSIONS`
- Frame gen: 2x, flow scale 0.25, Performance Mode on, Mailbox
- GameNative FPS limiter: 30, set before launch
- In game: VSync off, frame cap unlimited

## Notes
- A GameNative update re-copies its own frame gen layer; copy `liblsfg-vk-layer.so` back afterwards.
- Back up `AppData\Local\fex-emu\DiskCache` once it is well filled, so a reset doesn't bring the stutter back.
- Full write-up: setup, findings and optimization ideas are in the project doc.

