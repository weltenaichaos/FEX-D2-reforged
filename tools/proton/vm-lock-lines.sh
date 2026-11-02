#!/bin/bash
# Lists where dlls/ntdll/unix/virtual.c takes the memory lock (virtual_mutex), as
# "line function", for reading patch 0006's notes (__wine_d2r_vm_lock: the line field).
#   vm-lock-lines.sh <patched dlls/ntdll/unix/virtual.c>
awk '
  /^[A-Za-z_][^;#]*[A-Za-z0-9_] *\(/ && !/;[ \t]*$/ && !/^(static )?(const )?struct [^(]*=/ {
    s = $0; sub(/ *\(.*/, "", s); n = split(s, a, /[ \t*]+/); fn = a[n]
  }
  /server_enter_uninterrupted_section\( *&virtual_mutex|mutex_lock\( *&virtual_mutex/ { print NR, fn }
' "$1"
