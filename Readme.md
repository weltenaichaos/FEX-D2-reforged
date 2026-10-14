> ## This fork: Diablo II: Resurrected and Overwatch 2 on GameNative
>
> *Written by the fork maintainer (Dmage22), not the FEX project. The upstream FEX readme follows below.*
>
> Small compatibility fixes so Blizzard's protected launchers run under Wine ARM64EC on Android. No anti-cheat changes. Use at your own risk.
>
> - **Download:** [Releases](https://github.com/Dmage22/FEX-D2/releases) → `fexcore-2610-d2rfix27.wcp` (skip `-debug` and `maple` builds). Source: branch [`d2r/clean`](https://github.com/Dmage22/FEX-D2/tree/d2r/clean).
> - **Also needed:** Proton with the matching Wine fixes ([proton-wine#47](https://github.com/GameNative/proton-wine/pull/47) or [`proton-11.0-2-arm64ec-6`](https://github.com/Dmage22/proton-wine/releases)).
> - **Use ARM64EC DXVK/VKD3D builds** where you can. GameNative's default ones are x86-64 and run emulated; native builds noticeably cut CPU load and shader-compile time (Overwatch went from ~5 min to ~30 s to load). Our DXVK builds: [Dmage22/dxvk releases](https://github.com/Dmage22/dxvk/releases).
> - Tested on a Galaxy Z Fold 8 (Snapdragon 8 Elite Gen 5), headless Steam.
>
> **FEXCore preset (both games)**
>
> | Setting | Value |
> |---|---|
> | TSO / Vector TSO | on / on |
> | Memcpy/Set TSO, Half-barrier TSO | off |
> | Multiblock | on |
> | SMC checks | `mtrack` |
> | Environment | `FEX_DISKCACHE=1` |
>
> Keep Wine debug channels off while playing. Keep SMC checks on `mtrack`: `d2rfix27` fixes the freeze that earlier builds only avoided with `none` (details in the test data page).
>
> **Per game**
>
> | | Diablo II: Resurrected | Overwatch 2 |
> |---|---|---|
> | Steam mode | headless | headless |
> | DX wrapper | VKD3D | DXVK `3.1.1-gplasync-arm64ec-dm` (async) |
> | Graphics driver | Wrapper + Turnip | Wrapper + Turnip |
> | Environment | optional: `VKD3D_CONFIG=no_upload_hvv,nodxr`, `MESA_SHADER_CACHE_MAX_SIZE=4096MB` | `DXVK_CONFIG=dxgi.maxDeviceMemory=2048;dxgi.maxSharedMemory=2048;dxvk.enableGraphicsPipelineLibrary=False`, `MESA_SHADER_CACHE_MAX_SIZE=4096MB` |
> | In game | low textures, 30 fps cap | low textures, 30 fps cap |
> | Notes | | large multiplayer fights are CPU-bound and may dip |
>
> **D2R test data:** memory use, clock throttling and freeze runs on the Fold 8, in [docs/d2r-test-data.md](docs/d2r-test-data.md).

---

[中文](https://github.com/FEX-Emu/FEX/blob/main/docs/Readme_CN.md)
# FEX: Emulate x86 Programs on ARM64
FEX allows you to run x86 applications on ARM64 Linux devices, similar to qemu-user and box64.
It offers broad compatibility with both 32-bit and 64-bit binaries, and it can be used alongside Wine/Proton to play Windows games.

It supports forwarding API calls to host system libraries like OpenGL or Vulkan to reduce emulation overhead.
An experimental code cache helps minimize in-game stuttering as much as possible.
Furthermore, a per-app configuration system allows tweaking performance per game, e.g. by skipping costly memory model emulation.
We also provide a user-friendly FEXConfig GUI to explore and change these settings.

## Prerequisites
FEX requires ARMv8.0-a or newer hardware, with minimum extensions FEAT_FP and FEAT_CRC32.

It has been tested with the following Linux distributions, though others are likely to work as well:

- Arch Linux
- Fedora Linux
- openSUSE
- Ubuntu 22.04/24.04/24.10/25.04

An x86-64 RootFS is required and can be downloaded using our `FEXRootFSFetcher` tool for many distributions.
For other distributions you will need to generate your own RootFS (our [wiki page](https://wiki.fex-emu.com/index.php/Development:Setting_up_RootFS) might help).

## Quick Start
### For Ubuntu 22.04, 24.04, 24.10 and 25.04
Execute the following command in the terminal to install FEX through a PPA.

```sh
curl --silent https://raw.githubusercontent.com/FEX-Emu/FEX/main/Scripts/InstallFEX.py | python3
```

This command will walk you through installing FEX through a PPA, and downloading a RootFS for use with FEX.

### For other Distributions
Follow the guide on the official FEX-Emu Wiki [here](https://wiki.fex-emu.com/index.php/Development:Setting_up_FEX).

### Navigating the Source
See the [Source Outline](docs/SourceOutline.md) for more information.
