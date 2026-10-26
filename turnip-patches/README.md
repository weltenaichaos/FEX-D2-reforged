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

- `0003-fex3-depth-clear-3d.patch` (fex3, build 72): mode bit 2 `TU_FEX_MODE_DEPTH_CLEAR_3D`.
  - In `tu_emit_subpass_begin_sysmem` a depth/stencil attachment cleared at the start of the
    subpass (the subpass's own single-sampled D16/X8D24/D24S8/D32 attachment, one layer, no
    multiview, FDM, predication, custom resolve or unresolve) is not cleared by the 2D engine
    (`clear_sysmem_attachment`, r2d) but left for `tu_fex_clear_depth_3d`, which
    `tu_emit_subpass_begin` calls once the depth buffer state is emitted: the
    `vkCmdClearAttachments` sysmem path (`tu_clear_sysmem_attachments`, a RECTLIST draw with
    depth test ALWAYS and depth/stencil writes) under `CP_COND_REG_EXEC RENDER_MODE|SYSMEM`.
    Writes go through the depth unit and its cache with UBWC like the app's own depth writes,
    so the CCU color->depth flush and the wait for idle after the 2D clear are not needed. LRZ
    is left alone: the pass already cleared it to the same value. Tiled (GMEM) passes clear in
    GMEM as before. Why: on a740 (a7xx gen2, no `has_generic_clear`) the 2D engine fills ~25
    pixels per GPU clock, ~1.1 ms for each of D2R's two 4096x4096 D16 shadow maps per frame.
  - Statistics `depth_clears[2]` / `depth_clear_px[2]` at 0x268 (were reserved), status bit
    0x100, slot mode bit 0x80 (`TU_FEX_SLOT_DEPTH_CLEAR`: the pass clears depth at its start).

- `0004-fex4-depth-clear-fastz.patch` (fex4, build 74): mode bit 3 `TU_FEX_MODE_DEPTH_CLEAR_FASTZ`.
  - `tu_fex_clear_depth_3d` sets `cmd->state.fex.clear_fs_disable` around its
    `tu_clear_sysmem_attachments` call; there, a clear without color (`clear_rts == 0`) on a7xx
    writes `RB_RENDER_CNTL`, `GRAS_SU_RENDER_CNTL` and `SP_RENDER_CNTL` with `FS_DISABLE` after
    `r3d_common`, as `tu_calc_disable_fs` does for depth-only pipelines: no fragment shader waves,
    the depth comes from the clear vertex shader's position, and the depth unit can use its Z-only
    rate. The next draw re-emits those registers with its draw states (the clear disabled them all).
    Why: build 73 counted 16.8 million fragment shader pixels per 4096x4096 shadow map clear, every
    shader core busy for its ~0.5 ms.
  - Statistics `depth_clears_fastz` / `depth_clear_px_fastz` at 0x288 (were reserved), status bit
    0x200; the slot mode carries bit 3.

Host check (fex4): `cltest` mode 12 shows `FS_DISABLE` in all three registers before the clear's
`DI_PT_RECTLIST` draw, and the app's color draw after it re-emits them without it; mode 4 keeps
`FS_DISABLE` off for the clear.

Host check (fex3): `cltest/` records a D16 depth-only pass and a color + D24S8 pass with depth
clears on the noop drm shim; with mode 0 `cffdump` shows `CP_BLIT` for both depth clears, with
mode 4 a `DI_PT_RECTLIST` draw after `RB_DEPTH_BUFFER_INFO` (UBWC `LOSSLESSCOMPEN`) with
`RB_DEPTH_CNTL` Z_WRITE_ENABLE / ZFUNC_ALWAYS, color writes off, stencil untouched.

Host check (fex2): `eptest/` compiles a D2R lighting shader's SPIR-V with
vkd3d's descriptor buffer layouts on the noop drm shim and prints the pipeline statistics
('Early preamble', 'Preamble Instruction Count'), with and without the FEX link.

Host check: build with `-Dfreedreno-kmds=msm,kgsl -Dtools=freedreno,drm-shim`, run a small
Vulkan program on `libfreedreno_noop_drm_shim.so` (`FD_GPU_ID=740`) with `FD_RD_DUMP=full`, and
decode the dump with `cffdump`.
