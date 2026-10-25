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
- `0002-fex2-early-preamble-push-sets.patch` (fex2, build 71): `tu_fex_channel::compile`
  (offset 0x18, was reserved; FEX writes it before the driver loads, the driver reads it once
  per device; fex1 ignores it).
  - `TU_FEX_COMPILE_PUSH_SPECULATE`: a push descriptor set layout created with
    `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` no longer counts as partially
    bound (stock: every descriptor buffer binding does), so `can_speculate_resource` marks UBO
    loads through it speculatable and ir3 can use the a7xx early preamble. vkd3d-proton keeps
    the D3D12 root constant buffers in such a set; D2R's lighting shaders went from a 729
    instruction preamble with 47 branches run inside the first wave to 253 branch-free
    instructions with `Early preamble = true`. The push set's host copy is zeroed when its
    layout changes, so bindings the app never pushed are null descriptors (UBO size 0, loads
    read 0) instead of leftover bytes. The flag is part of the layout hash (`partially_bound`),
    so shaders compile anew.
  - Status bit 0x80 tells FEX it was honored.

Host check (fex2): `eptest/` compiles a D2R lighting shader's SPIR-V with
vkd3d's descriptor buffer layouts on the noop drm shim and prints the pipeline statistics
('Early preamble', 'Preamble Instruction Count'), with and without the FEX link.

Host check: build with `-Dfreedreno-kmds=msm,kgsl -Dtools=freedreno,drm-shim`, run a small
Vulkan program on `libfreedreno_noop_drm_shim.so` (`FD_GPU_ID=740`) with `FD_RD_DUMP=full`, and
decode the dump with `cffdump`.
