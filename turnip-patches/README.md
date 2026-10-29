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


- `0009-fex9-cp-ubo-push-sets.patch` (fex9, with VKD3D fex29 and FEX build 98): root constant buffers
  loaded by the command processor, on by default (`TU_FEX_COMPILE_CP_UBO` 0x8, FEX_TU_CP_UBO=0 turns it
  off; a7xx without `load_shader_consts_via_preamble` only, status bit 0x8000).
  - ir3: `ir3_shader_options::fex_cp_ubo_sets` names the push descriptor sets of the layout (Turnip,
    graphics stages). UBO ranges of those sets are marked `ir3_ubo_range::fex_cp`; loads from them,
    also in the preamble, read the const file, and the preamble doesn't copy them
    (`copy_ubo_to_uniform`). `ir3_shader_variant::fex_bindless_sets` records which bindless sets the
    final shader still reads through descriptors (every `bindless_resource_ir3`, plus the
    framebuffer-read descriptor); it is kept in the shader cache.
  - Turnip: `tu_fex_emit_cp_ubos` (in `tu_emit_consts`) emits one `CP_LOAD_STATE6` `ST6_CONSTANTS`
    `SS6_INDIRECT` per marked range from the UBO address and size in the push set's CPU copy (UBO
    descriptor dwords 0-1), clamped to the UBO's size and the stage's constlen; push set changes dirty
    the consts. `tu_fex_desc_sets_skip`: when the only sets whose address changed since the last
    emitted descriptor set state are ones the program (binning variants included) doesn't read through
    descriptors, the draw keeps that state (no `SP_GFX_BINDLESS_BASE`, no `SP_UPDATE_CNTL`
    invalidation); a new program's descriptor preloads are still pointed to. Barriers whose
    destination is a graphics uniform read (outside render passes) also count as an indirect command
    read, so writes reach memory and the command processor waits for them before loading.
  - Why: D2R's lighting pixel shaders read ~100 vectors of vkd3d's root constant buffers (set 3, a
    push descriptor set vkd3d pushes for nearly every draw). Stock Turnip invalidates all 8 bindless
    descriptor sets at every push and the preamble fetches the ranges with dependent `ldc` loads;
    the fex28 probe measured ~1.4 us more per long-shader draw when the root constant buffers change.
  - VKD3D fex29 (`VKD3D_FEX_CBV_SIZE=1`): dxbc-spirv declares dynamically indexed constant buffers with
    their declared size (D2R: 90 and 16 vectors) instead of 4096, so their reads fit the const file
    and set 3 is no longer read through descriptors at all.

Host check (fex9): `eptest` on D2R's lighting shader 5ff3 with sized constant buffers: 9 ranges
loaded by the driver, preamble 184 -> 137 instructions and still early, sets still read through
descriptors 0x7 (set 3 gone). `drawtest` (3 draws, new root constant buffers each): 9
`CP_LOAD_STATE6_FRAG SS6_INDIRECT` const loads per draw at the per-draw addresses;
`SP_GFX_BINDLESS_BASE[3]` and `SP_UPDATE_CNTL` only for the first draw (fex8: every draw).

- `0010-fex10-ubo-table-push-sets.patch` (fex10, with FEX build 99): push set UBOs that shaders still
  read as UBOs go through the shader's own UBO table, on by default (`TU_FEX_COMPILE_UBO_TABLE` 0x10,
  only together with fex9's 0x8; FEX_TU_UBO_TABLE=0 turns it off; status bit 0x10000).
  - ir3: `ir3_nir_fex_ubo_table` runs right after `ir3_nir_lower_ubo_loads` (before descriptor
    prefetch), also on preamble functions. Each `load_ubo` left on a `fex_cp_ubo_sets` set (no known
    range, e.g. a dynamically indexed array) gets a non-bindless UBO index instead of its
    `bindless_resource_ir3`: one per (set, binding block), allocated like driver UBOs (index 0 stays
    free), at most 8 per stage and index < 16, recorded in `ir3_const_state::fex_ubo` (shared with the
    binning variant, which only looks entries up; kept in the shader cache). The shader code is
    otherwise the same (`ldc` without the bindless flag; one descriptor prefetch less).
  - Turnip: `tu_fex_emit_cp_ubos` also writes each entry with `CP_LOAD_STATE6` `ST6_UBO` `SS6_DIRECT`
    (the pushed descriptor's dwords 0-1, address and size in vec4, the table's format; zero if the
    descriptor is missing, like a null descriptor). The stage's `fex_bindless_sets` loses the push set,
    so `tu_fex_desc_sets_skip` applies to these draws too. Shader key bit `fex_ubo_table`.
  - Why: build 98's log: draws whose shaders read root constant buffers only through fex9's loads got
    2.4x cheaper where the GPU front end was the limit (a 350-draw main pass at every start 1.72-2.39
    -> 0.79 ms, prepass 0.95 -> 0.76 ms, 94% of descriptor changes skipped), but shadow passes got
    slower (1.98 -> 2.35 us/draw): 76% of their draws use skinning vertex shaders that index cb2 (1536
    vectors, too big for the const file), so they still changed the descriptor set state at every draw
    and paid the constant loads on top.

Host check (fex10): D2R's skinning vertex shader fb5d (DXBC from build 71, dxbc-spirv, root constant
buffers moved to set 3): sets still read through descriptors 0x8 -> 0x0, `ubo1=set3.2` in both the
draw and binning variants, 273 instructions either way, preamble 8 -> 7 (descriptor prefetch gone).
Pixel shaders with sized constant buffers (5ff3) unchanged; the unsized captures of 5ff3/fb5d/5b7f/0e15
lose set 3 too (0xf -> 0x7) with the same instruction counts. A test VS with a driver constant UBO puts
it at index 1 and the table entries at 2-3. `drawtest` (skinning VS + 5ff3, 3 draws, new root constant
buffers each): one `CP_LOAD_STATE6_GEOM ST6_UBO` per draw at the per-draw address (size 0x80 vec4) and
the descriptor set state only for the first draw (fex9: every draw).
- `0011-fex11-pipeline-switch.patch` (fex11, with FEX build 100): graphics pipeline switches send only
  the state that changed (`TU_FEX_MODE_SWITCH` 0x20 in `mode`, so FEX can switch it each second;
  FEX_TU_SWITCH=0 off, 1 on, 2/alt every other second; a7xx; status bit 0x20000, and 0x40000 when the
  channel extension for its counts is there).
  - Tracking (both modes, a7xx with the link): every draw state group set in `draw_cs` is noted in
    recording order (`tu_fex_sw_note`), and at each draw what was set becomes what the GPU has
    (`sw_exec`). A disable (render pass end, 3D clears/blits), a full re-emit, a secondary command
    buffer or anything not known resets it.
  - With the bit: a group made only of register writes (`pkt4`, `CP_CONTEXT_REG_BUNCH`, `CP_NOP`)
    whose bytes equal the group the GPU ran at the last draw is set to that group's address again, so
    the command processor skips it (`tu_fex_sw_pick`; pipeline-baked dynamic groups, the VPC and
    primitive mode groups, and groups re-made at draws such as the depth plane and dynamic state).
    `tu_draw_state::fex_map` keeps the CPU address of each state's bytes for the comparison.
  - For pipelines without tessellation/geometry shaders (and without dynamic descriptor offsets, the
    empty fragment shader of depth-only pipelines allowed), `tu_fex_sw_bind` replaces the bind: the
    program config group becomes the part all such programs share (`SP_MODE_CNTL`), and the vertex /
    fragment stage whose shader group changed since the last draw gets `SP_UPDATE_CNTL` with only its
    bit (plus `GFX_UAV`), its config registers (`SP_xS_CONST_CONFIG`, `SP_xS_CONFIG`) written directly
    and its shader group marked dirty. An unchanged stage is neither reset nor sent; the empty
    tessellation/geometry groups keep their address without the dirty flag. Anything else (or a stock
    bind since the last draw) takes the stock path.
  - Counts per command buffer, added to the channel extension at `vkEndCommandBuffer` (`ext_size`, the
    old `reserved0`, says FEX allocated it; `size` stays the old layout so fex10 and older still accept
    the block): binds, groups run/kept per mode, stage resets/kept, stock binds, groups re-made at draws
    run/kept.
  - Why: build 99's log, regressing pass time on draws and pipeline switches: each switch costs ~1.5 us
    in shadow passes and ~3.5 us in the prepass (31-44% of their draws switch; fights ~900 switches a
    frame in depth passes, ~2 ms). Stock re-runs ~16 groups per switch, most of them byte-identical, and
    resets every shader stage; build 73's counters had shadow draws waiting ~2,400 GPU cycles on
    context register writes.

Host check (fex11): `drawtest_sw2` (vkd3d-like layout, dynamic viewport/scissor/stencil reference/
topology, 8 pipelines: two vertex shaders, two fragment shaders, depth-only, depth compare variants)
recorded with the bit off and on, decoded with cffdump: the register state at every draw is the same
(~195 registers, all but `SP_UPDATE_CNTL`) in direct and tiled (binning + tile) passes and with
re-binds. Command processor groups run (sticky dirty model): switches with fragment shaders 16 ->
~4 groups per switch (state words -34%; Turnip links stages, so a different fragment shader usually
means a different vertex shader variant too), depth-only switches 361 -> 97 groups (-69% words), same
shaders with other depth state: only the depth and depth plane groups.

- `0012-fex12-noalias-alt.patch` (fex12, with FEX build 101): big fragment shaders get a second binary
  without `alias.tex` helper instructions, and FEX can pick either binary per second
  (`TU_FEX_COMPILE_NOALIAS_ALT` 0x20 in `compile`, `TU_FEX_MODE_NOALIAS` 0x40 in `mode`;
  FEX_TU_NOALIAS=0 off, 1 always the second binary, 2/alt every other second; a7xx; status bit 0x80000,
  and 0x100000 when the channel extension for its counts is there).
  - ir3: `ir3_shader_key::fex_noalias_tex` (copied to `ir3::fex_noalias_tex`) makes
    `ir3_create_alias_tex_regs` / `ir3_insert_alias_tex` do nothing for that variant, the same as
    `IR3_SHADER_DEBUG=noaliastex` but per shader.
  - Turnip: with the compile bit (part of the shader key, `tu_shader_key::fex_noalias_alt`), a fragment
    shader with at least 1,024 instructions and 64 `alias.tex` (category 7) instructions is compiled a
    second time with the key bit. The second binary is kept only when everything the pipeline builds
    from the shader is the same (thread size, early preamble, merged registers, no private memory,
    constants not longer, inputs/outputs/locations/interpolation, kill, sample mask, primitive id,
    samplers and bindless use); otherwise it is dropped and counted as refused. It is uploaded next to
    the stock binary, its own fragment stage draw state is made (`tu6_emit_variant`: base address,
    instruction size, register footprint, input/output registers), and it is stored in the pipeline cache
    with the shader. Thresholds can be changed with the Unix env `TU_FEX_NOALIAS_MIN=instrs,alias`.
  - Pipelines whose fragment shader has the second binary keep its draw state
    (`tu_program_state::fex_alt_fs_state`) unless the pipeline uses the reduced-constants binary
    (`ir3_trim_constlen`). At a pipeline bind in a command buffer begun with the mode bit, the
    fragment stage draw state is the second binary's; all other state is the pipeline's own.
  - Counts in the channel extension (`ext_size` >= 0x358): binds with the stock / second binary, shaders
    with a second binary and their totals for both binaries (instructions, nops, `alias.tex`, movs, full
    registers, ir3's (ss)/(sy) stall estimates), refused binaries, pipelines that can / cannot use it.
  - Why: build 100's log and build 70's counters: the main pass costs about one lighting shader wave per
    draw (~12k cycles per draw, about 2 draws in flight), so a shorter lighting shader should shorten the
    pass nearly 1:1. D2R's lighting shaders (5ff3, 5b7f, fb5d, 0e15) are 6,600-7,200 instructions with
    ~1,100-1,170 `alias.tex` helpers (17%). Without them: 11-12% fewer instructions (7120 -> 6277 for 5ff3;
    nops 1579 -> 1353, movs 180 -> 729, 24 -> 32 registers, which only lowers the wave limit 8 -> 6 where
    the game runs ~4). Whether an alias costs an issue cycle like a mov is not documented, hence both
    binaries and the per-second comparison.

Host check (fex12): `eptest` (EPTEST_FEX=0x3f EPTEST_EXT=0x358) on the four lighting shaders: all four get
the second binary, none refused. `drawtest_sw2` with the lighting shaders (two fragment shaders, plus a
small one below the threshold), mode bit off and on, with and without fex11's switch path, direct, tiled and
re-bind: the register state at every draw differs only in `SP_PS_BASE`, `SP_PS_INSTR_SIZE`,
`SP_PS_CNTL_0` (register footprint) and `SP_PS_OUTPUT[0].REG`; the small shader's draws are unchanged.

- `0013-fex13-nopreamble-alt.patch` (fex13, with FEX build 104): the second binary of big fragment
  shaders can be compiled without preamble (`TU_FEX_COMPILE_NOPREAMBLE_ALT` 0x40 in `compile`; picked
  with fex12's mode bit 0x40, also named `TU_FEX_MODE_ALT_FS`; FEX_TU_NOPREAMBLE=alt every other
  second, lit always; a7xx GPUs that load shader constants with the command processor, like the a740;
  status bit 0x200000).
  - ir3: `ir3_shader_key::fex_nopreamble` skips `ir3_nir_opt_preamble` and descriptor prefetch for that
    variant, like `IR3_SHADER_DEBUG=nopreamble` per shader. The push constant copy stays in a 6-instruction
    early preamble.
  - Turnip: with the compile bit (shader key `tu_shader_key::fex_nopreamble_alt`), a fragment shader with
    at least 1,024 instructions and a preamble of 32+ instructions (Unix env `TU_FEX_NOPREAMBLE_MIN`) gets
    the second binary with the key bit (with fex12's bit too if that is on). The fex12 checks still apply
    (same inputs/outputs, early preamble, thread size, constants not longer). Unlike fex12's binary, this
    one has its own constant layout (no preamble area, so the root constant buffer ranges and immediates
    sit lower): pipelines keep a pointer to it (`tu_program_state::fex_alt_fs_variant`) and a bind with
    the mode bit also copies its constant layout into `link[MESA_SHADER_FRAGMENT]`, which every per-draw
    constant upload reads.
  - Counts: the channel extension grows to 0x360 (`na_preamble[2]`: preamble instructions of both
    binaries); the fex12 counts need 0x358 as before.
  - Why: build 103 counted all 19 SP wave states. D2R's lighting waves spend 25-30% of their life in
    `SP_WAVE_HWAVE_WAIT_CYCLES` (about 1,500-2,300 cycles a wave, more in small draws); the depth prepass
    with equally small draws shows 3-4%, shadows 6-10%, full-screen passes 1%. What only the lighting
    draws have is a 137-instruction early preamble (no memory reads since fex9/fex10: about 70 one-value
    constant stores of products of root constants) that runs once per draw before its waves may start.
    Without it: 7120 -> 7181 instructions for 5ff3 (5b7f +47, fb5d -37, 0e15 +46), same 24 registers and
    8 waves, 164 -> 132 constants.

Host check (fex13): `eptest` (EPTEST_FEX=0x5f EPTEST_EXT=0x360) on the four lighting shaders: all get the
second binary (preamble 137 -> 6), none refused. `drawtest_sw2` with 0e15, mode bit off and on (direct):
register state per draw differs only in `SP_PS_BASE`, `SP_PS_INSTR_SIZE` and `SP_PS_OUTPUT[0].REG`; the
nine root constant buffer ranges and the immediates load from the same addresses to the second binary's
offsets (c4, c8, c12, c36, c40, c52, c56, c64, c72; immediates c84 instead of c116).

- `0014-fex14-wave-pair.patch` (fex14, with FEX build 105): other wave setups for big fragment shaders,
  picked per command buffer with FEX's arm bits (`TU_FEX_MODE_FS_ARM`, mode bits 8-10; FEX_TU_WAVE_PAIR=alt
  rotates them a second each).
  - Arms 1-3 (`TU_FEX_COMPILE_PAIR_ALT` 0x80 in `compile`, a7xx, status bit 0x400000): fragment shaders with
    at least 1,024 instructions get three more fragment stage draw states made from the same binary, with
    `SP_PS_CNTL_1.WAVE_PAIR_MODE` 1, 2 and 3 (`tu_shader::fex_pair_state`; stock Turnip and freedreno always
    write 0, and the values are not documented). Pipelines that use the stock binary keep them
    (`tu_program_state::fex_pair_fs_state`); a bind in a command buffer with arm 1-3 uses that state (not
    together with fex12's mode bit, whose binary may have another constant layout).
  - Arm 4 (`TU_FEX_COMPILE_W128_ALT` 0x100, a6xx/a7xx with double thread size, status bit 0x800000): the
    second binary of fex12/fex13 is compiled as 128-pixel waves (`ir3_shader_key::fex_wave128`: double
    thread size whenever the registers allow it, `regs * 2 <= reg_size_vec4`, instead of ir3's rule that
    keeps 32-bit shaders above a quarter of the register file at 64). `tu_fex_noalias_ok` then asks for a
    128-wide second binary of a 64-wide shader; the wave size is in the fragment stage's own draw state
    (`SP_PS_CNTL_0`, `SP_PS_WAVE_CNTL`). FEX sets fex12's mode bit with arm 4.
  - The arm a render pass was recorded with goes into its slot (`tu_fex_slot::sysmem` bits 4-6), so FEX's
    pass kinds split per arm. Counts in the channel extension (`ext_size` >= 0x3a8, status bit 0x1000000):
    `arm_binds[8]` (binds of pipelines with fex14 setups, by arm) at 0x360 and `pair_shaders` at 0x3a0.
  - Why: build 104's no-preamble binary changed nothing; D2R's lighting waves still spend 25-30% of their
    life in `SP_WAVE_HWAVE_WAIT_CYCLES`, and a lighting draw takes about one wave life (cycles per draw
    7,066, wave life 7,081). The wait grows with wave life (correlation 0.94) and the long sync state
    (0.85), not with issue load. A Qualcomm patent (US11954758, "dynamic wave pairing") describes
    a hardware wave slot running two 64-wide waves together that part at texture and branch blocks and
    join again: the one that got ahead waits for its partner. Pair mode 1-3 may change that pairing; a
    128-pixel wave has no partner.

Host check (fex14): `eptest` (EPTEST_FEX=0x19f EPTEST_EXT=0x3a8) with 5ff3 and 0e15: the 128-pixel binary
has the same instructions (7120 / 6591) and 24 registers, double thread size 1, max waves 8 -> 4 (same
pixels); with 0x1df (no preamble too) preamble 137 -> 6 as in fex13. `drawtest_sw14` with 0e15, 8 draws,
direct: arms 1, 2 and 3 differ from stock only in `SP_PS_CNTL_1.WAVE_PAIR_MODE`; arm 4 (mode 0x440) only
in the binary's address and `THREADSIZE = THREAD128` in `SP_PS_CNTL_0` and `SP_PS_WAVE_CNTL`; arm binds
count 8 in the right arm; without the compile bits nothing changes.

- `0015-fex15-draw-setup.patch` (fex15, with FEX build 106): other draw setups for big fragment shaders,
  picked with the same arm bits (FEX_TU_DRAW_SETUP=alt rotates them a second each). Build 105 found that
  about one D2R lighting draw's waves are on the GPU at a time (cycles per draw about one wave life, 5.9
  waves per busy core with room for 8), so these test what keeps the next draw from starting.
  - Arm 4 with `TU_FEX_COMPILE_NOPRE_ALL_ALT` 0x200 (a7xx without consts via preamble, status 0x2000000):
    the second binary has no preamble at all. Push constants are read from the const file where the
    command processor loads them before each draw (`ir3_shader_key::fex_push_by_driver`: push constant
    type per stage instead of the shared constant registers, `tu_fex_emit_push_by_driver`), and ir3 no
    longer makes an empty preamble when every UBO range is loaded by the driver. Build 104's binary kept a
    6-instruction push constant copy, so the early preamble still ran before every draw. FEX sets fex12's
    mode bit with arm 4; that bind also uses the binary's own program config (`fex_alt_config_state`).
  - Arm 5 with `TU_FEX_COMPILE_MERGE_ALT` 0x400 (needs fex9's driver-loaded ranges, status 0x4000000): a
    third binary (`ir3_shader_key::fex_merge_ranges`, `fex_merge_cp_ranges`) with one driver-loaded range
    per root constant buffer where the gaps fit, bound with its own program config and fragment constant
    layout (`tu_program_state::fex_alt2_*`). Pipelines whose constants would not fit keep the stock binary.
  - Arms 6 and 7 with `TU_FEX_COMPILE_CNTL_ALT` 0x800 (a7xx, status 0x8000000): two more fragment stage
    draw states from the same binary with `SP_PS_CNTL_1.DEFER_WAVE_ALLOC_DIS` 0 and `EVICT_BUF_MODE` 0
    (the driver always writes 1 and 1; not documented).
  - Counts in the channel extension (`ext_size` >= 0x3d0, status 0x10000000): `nopre_shaders` 0x3a8,
    `merge_shaders` 0x3ac, `merge_ranges[2]` 0x3b0, `merge_vec4[2]` 0x3b8, `cntl_shaders` 0x3c0,
    `setup_refused` 0x3c4, `merge_pipes[2]` 0x3c8. Pipeline binds per arm stay in fex14's `arm_binds`.

Host check (fex15): `eptest` (EPTEST_FEX=0xe1f EPTEST_EXT=0x3d0) with 5ff3: second binary preamble 137 -> 0,
7120 -> 7204 instructions, 24 registers; third binary 12 -> 6 driver-loaded ranges, 100 -> 152 vec4, constlen
164 -> 216, same instructions; refused 0. `drawtest_sw15` with 0e15 (9 ranges), direct: stock CONSTLEN 136 with
READ_IMM_SHARED_CONSTS and EARLYPREAMBLE; arm 4 (mode 0x440) CONSTLEN 104, neither flag, plus one
CP_LOAD_STATE6_FRAG with the push constants; arm 5 (0x500) CONSTLEN 192 and 5 loads; arms 6 and 7 change only
their bit. Mixed pipelines with dynamic state, rebinds, sysmem and gmem ran without asserts and counted binds
in the right arm.
