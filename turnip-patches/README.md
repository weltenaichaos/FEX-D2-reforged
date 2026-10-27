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

- `0007-fex7-loop-prefetch.patch` (fex7, build 96): compile bit `TU_FEX_COMPILE_LOOP_PREFETCH` (0x4),
  read once per device into `ir3_compiler_options::fex_loop_prefetch` and carried in
  `tu_shader_key::fex_loop_prefetch` so shader hashes change with it.
  - `ir3_nir_fex_loop_prefetch` (new file, run in `ir3_nir_lower_variant` before the cleanup
    optimize loop that precedes `ir3_nir_opt_preamble`, fragment shaders only): in a loop with one
    back edge and no continue construct, a scalar 32-bit `load_ssbo` that is read-only and
    reorderable (not volatile/coherent), in one of the loop's own top-level blocks, whose address is
    made only of this loop's header phis, values from outside the loop, ALU and descriptor handles
    (`bindless_resource_ir3`), is read one iteration ahead: a copy with the phis' entry values goes
    before the loop, a copy with the phis' next values (their back-edge sources, which must be made
    the same way) goes where the load was, and a new header phi carries the value. Loads whose value
    is used after the loop are left alone. When the loop starts with `if (c) break` and `c` is made
    the same way, the copy before the loop is inside `if (!c)` (empty lists read nothing). The copy
    in the last iteration reads one entry past the list; the descriptor's bounds check makes a read
    past the buffer return 0, and the value is never used.
  - Why: D2R's lighting shaders walk light and decal lists (`idx = list[(cluster << 8) + i]`, then 3-4
    texel buffer fetches at `idx`): two dependent memory round trips per light, and the GPU keeps
    about two draws' pixel work in flight, so the waves' latency sets the cost of each of the ~1,400
    main pass draws in fights (build 95: 7.95 us a draw). Now the next entry's read goes out with the
    light data's reads and shares their (sy) wait.
  - Statistics: `loop_prefetch_loads` / `loop_prefetch_shaders` at 0x1a8 (were `reserved1`), counted
    by fresh compiles only (`ir3_shader_variant::fex_loop_prefetches`). Status bit 0x1000.

Host check (fex7): `eptest/` with `EPTEST_FEX=3` (fex2 + fex6) and `EPTEST_FEX=7` (+ fex7) on the 4
lighting shaders: 4 loops each moved, Registers used 24 and Max Waves 8 unchanged, early preamble
kept, NIR validation on (debugoptimized build); in the ir3 code the light loop's data fetches and the
next entry's `isam` are issued together before one (sy) (stock: `isam` entry, (sy), data fetches,
(sy)). `eptest/looptests/*.frag` (glslangValidator -V) are loop shapes for the pass: t1 for-loop
(moved, guarded), t2 do-while whose entry is used after the loop (left), t3 do-while (moved, no guard),
t4 nested lists (both moved), t5 conditional step (left), t6 continue (NIR folds it: moved), t7 break
in the body (moved).

- `0008-fex8-color-clear-3d-cs-unroll.patch` (fex8, with VKD3D fex27): two trims, both on by default
  with FEX's link (no FEX build needed), each with a Unix environment variable to turn it off.
  - Color clears: with `TU_FEX_MODE_DEPTH_CLEAR_3D` (FEX_TU_DEPTH_CLEAR=1, the default since build
    75), a color attachment cleared at the start of a direct (sysmem) pass joins the 3D clear draw
    (`tu_fex_clear_3d`, renamed from `tu_fex_clear_depth_3d`): one `tu_clear_sysmem_attachments` draw
    for the pass's depth/stencil and color clears, after the subpass state is emitted, inside
    `CP_COND_REG_EXEC RENDER_MODE|SYSMEM`. Same limits as fex3 (single sample, one layer, no
    multiview, per-layer areas, FDM, predication, custom resolves, unresolves), and E5B9G9R9 stays on
    the 2D engine. Stock clears every such attachment with `CP_BLIT` (2D engine, ~25 pixels a clock
    on a740) followed by `CCU_CLEAN_COLOR`, `CCU_INVALIDATE_COLOR` and a wait for idle.
  - Why: D2R clears ~14 color targets a frame this way (main pass 2, ~10 one-draw full-screen passes,
    the 1280x720 RGBA16F UI pass, ...), ~11 Mpx a frame; build 73 showed the 3D draw clears depth
    about twice as fast as the 2D engine.
  - `TU_FEX_MODE_DEPTH_CLEAR_FASTZ` (0x8) now means "color clears on the 2D engine" (depth stays a 3D
    draw, with the fragment shader): fast Z gave nothing in build 74, and FEX_TU_DEPTH_CLEAR=2 now
    compares the color clear both ways a second each. `depth_clears_fastz` / `depth_clear_px_fastz`
    count the color clears done by the 3D draw (FEX's line: "of them fast z N (Mpx)").
    `TU_FEX_COLOR_CLEAR=0` turns it off. Status bit 0x2000.
  - Compute loop unrolling: `ir3_compiler::fex_cs_nir_options` is the compiler's NIR options with
    `max_unroll_iterations_aggressive = 128` (as on AMD), used by `nir_opt_loop_unroll` in
    `ir3_optimize_loop` for compute shaders when `ir3_compiler_options::fex_cs_unroll`: a loop with
    an exact trip count whose body is one block with loads that depend on the iteration
    (`can_pipeline_loads`) is unrolled up to 128 iterations (stock limit 32 x 26 cost), so its loads
    go out together. Carried in `tu_shader_key::fex_cs_unroll`. `TU_FEX_CS_UNROLL=0` turns it off.
    Status bit 0x4000.
  - Why: D2R's compute shader 66a6aa1d0c34c002 (4x4 groups of 8x8) runs 16 x 32 iterations of two
    buffer reads per thread; each iteration waited for memory (0.23 ms of every frame on the GPU).

Host check (fex8): `cltest/` (mode 4: case B's RGBA8 color clear and D24S8 depth clear are one
`DI_PT_RECTLIST` draw with `RB_MRT[0].CONTROL COMPONENT_ENABLE = 0xf` and `ZFUNC_ALWAYS`, no `CP_BLIT`,
no CCU flush or wait for idle after it; mode 12: color by `CP_BLIT`, depth by the draw; counts "fast z
1 (4096 px)" in mode 4). Compute (scratchpad tool cstest, D2R's 17 compute shaders from build 97):
only 66a6aa1d0c34c002 changes, 82 -> 768 instructions, 5 -> 12 registers, 16 waves kept, loops 2 -> 1,
32 `isam` per outer iteration with 9 (sy) instead of one (sy) per read.
