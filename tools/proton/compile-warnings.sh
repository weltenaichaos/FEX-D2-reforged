#!/bin/bash
# Lists the compiler warnings of a build log that point at undefined behaviour an optimizer
# can turn into wrong code (use of uninitialized values, falling off the end of a non-void
# function, out of bounds indexes, bad shifts, unsequenced changes, returning stack memory):
# unique "file:line:col: warning: ... [-Wflag]" lines, sorted, then a count per flag.
#   compile-warnings.sh <build log>
# Information only: stock Wine has a few of these that are harmless. It matters when the
# build flags change (GameNative's Android patches had only been built unoptimized before d2r7).
FLAGS='sometimes-uninitialized|uninitialized|conditional-uninitialized|return-type|array-bounds|array-bounds-pointer-arithmetic|shift-count-overflow|shift-count-negative|shift-negative-value|shift-overflow|unsequenced|return-stack-address|dangling|null-dereference|static-in-inline'
list=$(grep -aoE "[^ ]+:[0-9]+:[0-9]+: warning: .*\[-W($FLAGS)\]" "$1" | sed -E 's|^(\.\./)+||' | sort -u)
[ -n "$list" ] && echo "$list"
echo "-- $(printf '%s' "$list" | grep -c .) unique warnings of these kinds (of $(grep -acE ':[0-9]+:[0-9]+: warning: ' "$1") warning lines in the log)$( [ -n "$list" ] && echo ', by flag:')"
[ -n "$list" ] && echo "$list" | grep -oE '\[-W[a-z-]+\]$' | sort | uniq -c | sort -rn
exit 0
