#!/bin/bash
# Lists where dlls/ntdll/unix/virtual.c takes the memory lock (virtual_mutex), as
# "line function", for reading patch 0006's notes (__wine_d2r_vm_lock: the line field).
#   vm-lock-lines.sh <virtual.c as compiled> [ntdll.so built from it]
# Give the source after every patch (ours and GameNative's Android patches, which are
# applied at configure time). With ntdll.so, only the places really built into it are
# listed (its section d2r_vm_sites holds their lines; code under #if that isn't compiled
# drops out), and a built line the source can't name is reported on stderr.
src=$1 so=${2:-}
built=
if [ -n "$so" ]; then
  built=$(python3 -I "$(dirname "$0")/elf-section-u32.py" "$so" d2r_vm_sites) || exit 1
  built=$(echo "$built" | sort -un | tr '\n' ' ')
fi
awk -v built="$built" '
  BEGIN { n = split(built, b, " "); for (i = 1; i <= n; i++) want[b[i]] = 1 }
  /^[A-Za-z_][^;#]*[A-Za-z0-9_] *\(/ && !/;[ \t]*$/ && !/^(static )?(const )?struct [^(]*=/ {
    s = $0; sub(/ *\(.*/, "", s); k = split(s, a, /[ \t*]+/); fn = a[k]
  }
  /server_enter_uninterrupted_section\( *&virtual_mutex|mutex_lock\( *&virtual_mutex/ {
    if (!n || (NR in want)) { print NR, fn; seen[NR] = 1 }
  }
  END { for (l in want) if (!(l in seen)) print "built line " l " is no lock place in the source" > "/dev/stderr" }
' "$src"
