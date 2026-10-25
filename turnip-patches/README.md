# turnip-patches

Patches on top of Mesa `mesa-26.3.0-rc2` (b34c71d9) for FEX's Turnip build, applied in name
order by `.github/workflows/build-turnip-fex.yml`, which packages an adrenotools driver zip for
GameNative.

- `0001-fex-link-descriptor-fix-counters.patch` (fex1, build 69): the FEX link
  (`TU_FEX_CHANNEL`, layout in `src/freedreno/vulkan/tu_fex.h`, mirrored in
  `Source/Windows/Common/TurnipChannel.h`).
  - Descriptor fix (A7XX, mode bit 0): in direct (sysmem) render passes a graphics descriptor
    update writes `SP_UPDATE_CNTL.GFX_BINDLESS` with only the sets whose address changed since
    the pass began (the pass start invalidates all 8); tiled passes and binning keep 0xff.
  - Push reuse (mode bit 1): a graphics push descriptor update with the same bytes as the
    previous one keeps the previous copy and address.
  - Counters: reserves the GPU counters FEX lists through KGSL and records them with
    `CP_ALWAYS_ON_COUNTER` around render passes with enough draws into a ring FEX reads.
  - Statistics per mode (command buffers, updates, invalidated sets, pushes reused).

Host check: build with `-Dfreedreno-kmds=msm,kgsl -Dtools=freedreno,drm-shim`, run a small
Vulkan program on `libfreedreno_noop_drm_shim.so` (`FD_GPU_ID=740`) with `FD_RD_DUMP=full`, and
decode the dump with `cffdump`.
