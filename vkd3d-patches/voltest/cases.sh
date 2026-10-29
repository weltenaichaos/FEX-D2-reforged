#!/bin/bash
# Runs voltest over the 16 table sizes used to check FEX's versions of D2R's compute shader 83e2eb4a6bf5f03b
# (VKD3D fex30/fex31) against the game's shader on lavapipe.
#   ./cases.sh ORIG.spv NEW.spv   (voltest built next to this script)
# Covers A = W (one slot per i), A a multiple of W, W not dividing A (slot steps of 2 and 4), A coprime to W (8 x 8
# slots), volumes smaller than the output (VOL_WHD), both address modes and mip levels 0-2.
cd "$(dirname "$0")"
O=$1
NEW=$2
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
run() { timeout 600 ./voltest "$O" "$NEW" "$@" 2>&1 | tail -1 | sed 's/^/  /'; }
run 32 64 64 1 0
run 32 32 32 1 0
run 32 64 64 0 1
run 32 24 24 1 0
run 32 33 7 1 0
run 32 13 5 1 1
run 16 16 16 1 0
run 8 8 3 0 0
run 40 40 40 1 0
run 16 64 64 2 0 32
run 32 48 64 1 0
run 32 16 64 1 0
VOL_WHD=24,32,16 run 32 64 64 1 0
VOL_WHD=40,16,8 run 32 64 32 1 1
VOL_WHD=8,8,8 run 32 64 64 1 0
run 32 96 64 1 0
