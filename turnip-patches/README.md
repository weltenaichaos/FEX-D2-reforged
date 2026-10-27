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

- `0005-fex5-memory-budget-cache.patch` (fex5, build 77): mode bit 4 `TU_FEX_MODE_BUDGET_CACHE`.
  - `tu_get_budget_memory` (the `VK_EXT_memory_budget` part of
    `vkGetPhysicalDeviceMemoryProperties2`) gets the available system memory from
    `tu_fex_available_memory`: with the mode bit, `/proc/meminfo` is read at most every 250 ms
    and the last `MemAvailable` reused in between (stock reads the file on every call). The
    channel is looked up once on the first query (it runs on a physical device, maybe before any
    device exists). Why: D3D12 games ask DXGI for the video memory budget every frame
    (`QueryVideoMemoryInfo`); build 76's log has D2R's main thread inside
    `vkGetPhysicalDeviceMemoryProperties2` for 3% of every second of play (~1 ms of every frame).
  - Statistics: `budget_reads` at 0x1a4 (was `reserved1[0]`), `budget_queries` and `budget_ns`
    (time spent getting the available memory, file reads included) at 0x298 (were `reserved2`);
    status bit 0x400. Host check (noop drm shim): 10 us per query reading the file, 0.05 us cached.

- `0006-fex6-preamble-loads-first.patch` (fex6, build 92): compile bit `TU_FEX_COMPILE_PREAMBLE_LOADS`
  (0x2), read once per device into `ir3_compiler_options::fex_preamble_loads_first` and carried in
  `tu_shader_key::fex_preamble_loads` so shader hashes change with it.
  - `copy_ubo_to_uniform` puts the ldc.k copies of UBO ranges at the start of the preamble (stock:
    the end). `ir3_sched_add_deps` orders every ldc.k after all earlier stc (both write consts), so
    at the end they could only go last.
  - `ir3_sched` (pre-RA), preamble blocks: ldc.k/ldg.k and their a1.x writes first; register loads
    first only when all of the block's register loads fit in 28 components at once and the running
    estimate of live values stays within 32 (the early preamble may only use the 32 shared
    registers; a preamble short of registers anyway keeps the stock order).
  - `ir3_postsched`, preamble blocks: ready register loads first (same budget, only when they need
    no nops or sync of their own), then, once the first (sy) consumer is placed, the ldc.k chain
    with at least 3 other instructions between a1.x writes and reads when there are any.
  - `ir3_nir_fex_dedupe_preamble_stores` (before `ir3_nir_lower_preamble`): after CSE, a second
    `store_preamble` of the same 32-bit or 1-bit def is dropped and the main shader's
    `load_preamble` of its slot reads the first slot.
  - Why: the a7xx early preamble runs as one wave ahead of each draw's pixel waves. In D2R's 4
    lighting shaders it had 5 (sy) waits (the 11 ldc.k last, then a final wait) and 122 stc for 58
    values; with fex6 2 waits and 58 stc, 243-253 -> 174-184 preamble instructions, still early.
  - Status bit 0x800.

Host check (fex6): `eptest/` with `EPTEST_FEX=1` (fex2 only) and `EPTEST_FEX=3` (fex2 + fex6) on the
4 lighting shaders: (sy) count, stc count, 'Early preamble'; the main shader's code differs only in
the preamble const slots it reads, and every merged slot was stored from the same register in the
fex2 build. A preamble-heavy test vertex shader (matrix products, not early) compiles the same as
without fex6.
