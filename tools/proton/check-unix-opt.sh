#!/bin/bash
# Proves the unix side of a build is optimized (workflow env UNIX_OPT).
#   check-unix-opt.sh <output dir with bin and lib/wine>
# An unoptimized build (GameNative's script: CFLAGS without -O) emits an out-of-line copy of
# every tiny static inline helper from Wine's headers in each object that uses it
# (list_remove, HandleToULong, wine_server_obj_handle, ...); an optimized one inlines them
# all. Every unix binary must have none of them as local functions. NM: an llvm-nm that
# reads aarch64 ELF files.
set -u
NM=${NM:-llvm-nm}
OUT=$1
HELPERS='list_init|list_add_head|list_add_tail|list_remove|list_empty|list_head|list_next|HandleToLong|HandleToULong|ULongToHandle|LongToHandle|wine_server_obj_handle|wine_server_ptr_handle|wine_server_client_ptr'
fail=0
n=0

for b in "$OUT"/lib/wine/aarch64-unix/*.so "$OUT"/bin/wineserver "$OUT"/bin/wine "$OUT"/bin/wine-preloader; do
  [ -f "$b" ] || continue
  n=$((n + 1))
  c=$($NM "$b" 2>/dev/null | grep -cE " t ($HELPERS)$")
  if [ "$c" -gt 0 ]; then echo "FAIL unix-opt: ${b#$OUT/} has $c out-of-line header helpers (built unoptimized)"; fail=1; fi
done
if [ "$n" -lt 4 ]; then echo "FAIL unix-opt: only $n unix binaries found under $OUT"; fail=1
elif [ "$fail" = 0 ]; then echo "OK   unix-opt: none of the $n unix binaries has out-of-line header helpers (optimized)"; fi
exit $fail
