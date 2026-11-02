#!/bin/bash
# Proves every proton-patches patch made it into the built binaries.
#   check-patches.sh <output dir with lib/wine> <patch dir>
# Each patch NNNN-*.patch needs a check_NNNN function below; a patch without one fails
# the build, so a new patch can't ship unchecked. OBJDUMP: an llvm-objdump that reads
# ARM64X PE files (llvm-mingw's).
set -u
OBJDUMP=${OBJDUMP:-llvm-objdump}
LIB=$1/lib/wine
PATCHES=$2
NT=$LIB/aarch64-windows/ntdll.dll
NT32=$LIB/i386-windows/ntdll.dll
fail=0

ok()  { echo "OK   $1: $2"; }
bad() { echo "FAIL $1: $2"; fail=1; }

# release stores of zero (dmb then str xzr) in one function of an ARM64X dll
release_clears() {
  $OBJDUMP -d --no-show-raw-insn --disassemble-symbols="$2" "$1" 2>/dev/null |
    awk '$2 == "dmb" { d = 1; next } { if (d && $2 == "str" && $3 ~ /^xzr/) c++; d = 0 } END { print c + 0 }'
}

exports() { $OBJDUMP -p "$1" 2>/dev/null | grep -qw "$2"; }

utf16_in() { python3 -I -c 'import sys; sys.exit(0 if open(sys.argv[1],"rb").read().count(sys.argv[2].encode("utf-16-le")) else 1)' "$1" "$2"; }

# 0001: wake functions clear a waiter's address with a release store after unlinking it
check_0001() {
  local s n
  for s in '#RtlWakeAddressSingle' '#RtlWakeAddressAll' RtlWakeAddressSingle RtlWakeAddressAll; do
    n=$(release_clears "$NT" "$s")
    if [ "$n" -ge 1 ]; then ok 0001 "$s has $n release clear(s)"; else bad 0001 "$s has no release clear (stock order)"; fi
  done
}

# 0002: parking list lock, its export for FEX and its off switch
check_0002() {
  local f
  for f in "$NT" "$NT32"; do
    if exports "$f" __wine_d2r_wait_lists; then ok 0002 "${f#$LIB/} exports __wine_d2r_wait_lists"; else bad 0002 "${f#$LIB/} lacks __wine_d2r_wait_lists"; fi
    if utf16_in "$f" WINE_D2R_WAIT_LOCK; then ok 0002 "${f#$LIB/} reads WINE_D2R_WAIT_LOCK"; else bad 0002 "${f#$LIB/} lacks WINE_D2R_WAIT_LOCK"; fi
  done
}

# 0003: xinput sends rumble from its update thread and reads outside the controller lock
check_0003() {
  local f
  for f in "$LIB"/aarch64-windows/xinput1_4.dll "$LIB"/aarch64-windows/xinput1_3.dll "$LIB"/i386-windows/xinput1_4.dll; do
    if exports "$f" __wine_d2r_xinput; then ok 0003 "${f#$LIB/} exports __wine_d2r_xinput"; else bad 0003 "${f#$LIB/} lacks __wine_d2r_xinput"; fi
    if utf16_in "$f" WINE_D2R_XINPUT; then ok 0003 "${f#$LIB/} reads WINE_D2R_XINPUT"; else bad 0003 "${f#$LIB/} lacks WINE_D2R_XINPUT"; fi
  done
  f=$LIB/aarch64-windows/xinput9_1_0.dll
  if exports "$f" __wine_d2r_xinput; then ok 0003 "${f#$LIB/} exports __wine_d2r_xinput"; else bad 0003 "${f#$LIB/} lacks __wine_d2r_xinput"; fi
}

for p in "$PATCHES"/*.patch; do
  n=$(basename "$p"); n=${n%%-*}
  if declare -F "check_$n" >/dev/null; then "check_$n"; else bad "$n" "no check for $(basename "$p") in check-patches.sh"; fi
done
exit $fail
