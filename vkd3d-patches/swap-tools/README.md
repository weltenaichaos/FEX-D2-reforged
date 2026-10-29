# Shader swap tools (VKD3D fex30 and later)

Used to make `libs/vkd3d/fex_shader_swap_spv.h` in the vkd3d tree (patches 0030-0033).

- `genswaphdr.py OUT.h DIR file.comp:array ...` compiles FEX's own compute shaders (`.comp` next to the header) with
  `glslangValidator -V --target-env vulkan1.3` and writes them as C arrays.
- `clutpatch.py IN.spvasm OUT.spvasm BLOCK F T` reorders one block of a `spirv-dis --raw-id` listing: the instructions
  that compute the boolean `F` and then `T` move to the block's start, each followed by an early `OpReturn` when it is
  false. It refuses blocks with side effects (stores, atomics, barriers) and calls of functions that have any.
- `genedit.py ORIG.spv NEW.spv ARRAY OUT.h 'comment'` appends an edit script that turns ORIG into NEW by copying runs of
  ORIG's words and inserting only the words that are new, with both FNV-1a 64 hashes in the comment.

VKD3D fex33, D2R's ground clutter shader 0e21a3b5088ba1af (vkd3d's translation from the shader copy in a FEX stats log,
`VKD3D_FEX_CBV_SIZE=1`, SPIR-V hash c77bcaec5decff13):

    spirv-dis --raw-id orig.spv -o orig_raw.spvasm
    python3 clutpatch.py orig_raw.spvasm new_raw.spvasm %3057 %728 %773
    spirv-as --preserve-numeric-ids --target-env spv1.6 new_raw.spvasm -o new.spv
    spirv-val --target-env vulkan1.3 --scalar-block-layout new.spv
    python3 genswaphdr.py fex_shader_swap_spv.h . fex_shader_swap_83e2_row.comp:fex_swap_83e2_row_comp
    python3 genedit.py orig.spv new.spv fex_edit_0e21 fex_shader_swap_spv.h '...'

`%3057` is the block after the coarse mask test, `%728` the on-screen test, `%773` the mask texture test (result
3a50174863afb494).
