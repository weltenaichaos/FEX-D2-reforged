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
NTSO=$LIB/aarch64-unix/ntdll.so
WS=$1/bin/wineserver
fail=0

ok()  { echo "OK   $1: $2"; }
bad() { echo "FAIL $1: $2"; fail=1; }

# release stores of zero (dmb then str xzr) in one function of an ARM64X dll
release_clears() {
  $OBJDUMP -d --no-show-raw-insn --disassemble-symbols="$2" "$1" 2>/dev/null |
    awk '$2 == "dmb" { d = 1; next } { if (d && $2 == "str" && $3 ~ /^xzr/) c++; d = 0 } END { print c + 0 }'
}

exports() { $OBJDUMP -p "$1" 2>/dev/null | grep -qw "$2"; }

# value of a 32-bit data export
export_u32() { python3 -I "$(dirname "$0")/pe-export-u32.py" "$1" "$2" 2>/dev/null; }

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

# 0004: wakes skip lists without waiters; the export says 3 and the off switch is there
check_0004() {
  local f v
  for f in "$NT" "$NT32"; do
    v=$(export_u32 "$f" __wine_d2r_wait_lists)
    if [ "$v" = 3 ]; then ok 0004 "${f#$LIB/} __wine_d2r_wait_lists = 3"; else bad 0004 "${f#$LIB/} __wine_d2r_wait_lists = '$v', not 3"; fi
    if utf16_in "$f" WINE_D2R_WAKE_FAST; then ok 0004 "${f#$LIB/} reads WINE_D2R_WAKE_FAST"; else bad 0004 "${f#$LIB/} lacks WINE_D2R_WAKE_FAST"; fi
  done
}

ascii_in() { grep -qaF "$2" "$1" 2>/dev/null; }

# 0005: unix ntdll keeps pipe/device/socket I/O out of the memory lock; off switch is there
check_0005() {
  if ascii_in "$NTSO" WINE_D2R_VM_IO; then ok 0005 "${NTSO#$LIB/} reads WINE_D2R_VM_IO"; else bad 0005 "${NTSO#$LIB/} lacks WINE_D2R_VM_IO"; fi
}

# 0006: memory lock holder notes in PE ntdll's __wine_d2r_vm_lock, filled by the unix side
check_0006() {
  local f
  for f in "$NT" "$NT32"; do
    if exports "$f" __wine_d2r_vm_lock; then ok 0006 "${f#$LIB/} exports __wine_d2r_vm_lock"; else bad 0006 "${f#$LIB/} lacks __wine_d2r_vm_lock"; fi
  done
  if ascii_in "$NTSO" __wine_d2r_vm_lock; then ok 0006 "${NTSO#$LIB/} looks up __wine_d2r_vm_lock"; else bad 0006 "${NTSO#$LIB/} never looks up __wine_d2r_vm_lock"; fi
  if ascii_in "$NTSO" WINE_D2R_VM_NOTES; then ok 0006 "${NTSO#$LIB/} reads WINE_D2R_VM_NOTES"; else bad 0006 "${NTSO#$LIB/} lacks WINE_D2R_VM_NOTES"; fi
  # the shipped line table (VM_LINES) must list exactly the lines the binary notes (every
  # lock place leaves its line in section d2r_vm_sites), and each of them must be a lock
  # place in the compiled source (VM_SOURCE: virtual.c after all patches)
  local built table l miss=
  built=$(python3 -I "$(dirname "$0")/elf-section-u32.py" "$NTSO" d2r_vm_sites 2>&1 | sort -n | tr '\n' ' ')
  table=$( [ -s "${VM_LINES:-}" ] && cut -d' ' -f1 "$VM_LINES" | sort -n | tr '\n' ' ')
  if [ -z "$table" ]; then bad 0006 "no line table given (VM_LINES)"
  elif [ "$built" = "$table" ]; then ok 0006 "line table matches the $(echo $built | wc -w) lock places built into ${NTSO#$LIB/}"
  else bad 0006 "line table doesn't match ${NTSO#$LIB/}: built '$built', table '$table'"; fi
  if [ -s "${VM_SOURCE:-}" ]; then
    for l in $table; do
      sed -n "${l}p" "$VM_SOURCE" | grep -qE 'server_enter_uninterrupted_section\( *&virtual_mutex|mutex_lock\( *&virtual_mutex' || miss="$miss $l"
    done
    if [ -z "$miss" ]; then ok 0006 "every table line takes the lock in the compiled virtual.c"; else bad 0006 "table lines without a lock in the compiled virtual.c:$miss"; fi
  else bad 0006 "no compiled virtual.c given (VM_SOURCE)"; fi
}

# 0007: NtContinue/NtContinueEx to x64 code enter the emulator directly; off switch is there.
# Stock code only converts the context to ARM64 and makes the syscall; the fast path calls
# dispatch_emulation, or has it inlined (then it calls context_arm_to_x64 itself).
check_0007() {
  local s n
  for s in 'NtContinue' 'NtContinueEx'; do
    n=$($OBJDUMP -d --no-show-raw-insn --disassemble-symbols="#$s\$hp_target" "$NT" 2>/dev/null | grep -cE 'dispatch_emulation|context_arm_to_x64')
    if [ "$n" -ge 1 ]; then ok 0007 "#$s enters the emulator directly"; else bad 0007 "#$s never enters the emulator (stock syscall only)"; fi
  done
  if utf16_in "$NT" WINE_D2R_FAST_CONTINUE; then ok 0007 "${NT#$LIB/} reads WINE_D2R_FAST_CONTINUE"; else bad 0007 "${NT#$LIB/} lacks WINE_D2R_FAST_CONTINUE"; fi
}

# 0008: anonymous events kept for reuse (off switch), request counters in PE ntdll's
# __wine_d2r_server_calls, and the shipped request name table (REQ_NAMES, made from the
# compiled server_protocol.h) has as many lines as the build has request types
check_0008() {
  local f n t
  for f in "$NT" "$NT32"; do
    if exports "$f" __wine_d2r_server_calls; then ok 0008 "${f#$LIB/} exports __wine_d2r_server_calls"; else bad 0008 "${f#$LIB/} lacks __wine_d2r_server_calls"; fi
  done
  if ascii_in "$NTSO" WINE_D2R_EVENT_REUSE; then ok 0008 "${NTSO#$LIB/} reads WINE_D2R_EVENT_REUSE"; else bad 0008 "${NTSO#$LIB/} lacks WINE_D2R_EVENT_REUSE"; fi
  if ascii_in "$NTSO" WINE_D2R_CALL_COUNTS; then ok 0008 "${NTSO#$LIB/} reads WINE_D2R_CALL_COUNTS"; else bad 0008 "${NTSO#$LIB/} lacks WINE_D2R_CALL_COUNTS"; fi
  if ascii_in "$NTSO" __wine_d2r_server_calls; then ok 0008 "${NTSO#$LIB/} looks up __wine_d2r_server_calls"; else bad 0008 "${NTSO#$LIB/} never looks up __wine_d2r_server_calls"; fi
  n=$(python3 -I "$(dirname "$0")/elf-section-u32.py" "$NTSO" d2r_server_reqs 2>/dev/null)
  t=$( [ -s "${REQ_NAMES:-}" ] && wc -l < "$REQ_NAMES")
  if [ -n "$n" ] && [ "$n" = "$t" ]; then ok 0008 "request name table has the $n request types of ${NTSO#$LIB/}"
  else bad 0008 "request name table has '${t:-no file}' lines, ${NTSO#$LIB/} has '${n:-no section}' request types"; fi
}

# calls from one function of an ELF binary to any of the given functions
calls_to() {
  $OBJDUMP -d --no-show-raw-insn --disassemble-symbols="$2" "$1" 2>/dev/null | grep -cE "\bbl?\b.*<($3)(@plt)?>"
}

# 0009: completion port waits in the process (off switch), both NtRemoveIoCompletion calls
# wait in the process, the server resets a port's sync once its queue is empty (stock: once,
# in remove_completion), and PE ntdll exports __wine_d2r_server_calls_ext
check_0009() {
  local f s n
  for f in "$NT" "$NT32"; do
    if exports "$f" __wine_d2r_server_calls_ext; then ok 0009 "${f#$LIB/} exports __wine_d2r_server_calls_ext"; else bad 0009 "${f#$LIB/} lacks __wine_d2r_server_calls_ext"; fi
  done
  if ascii_in "$NTSO" WINE_D2R_COMPLETION_WAIT; then ok 0009 "${NTSO#$LIB/} reads WINE_D2R_COMPLETION_WAIT"; else bad 0009 "${NTSO#$LIB/} lacks WINE_D2R_COMPLETION_WAIT"; fi
  if ascii_in "$NTSO" __wine_d2r_server_calls_ext; then ok 0009 "${NTSO#$LIB/} looks up __wine_d2r_server_calls_ext"; else bad 0009 "${NTSO#$LIB/} never looks up __wine_d2r_server_calls_ext"; fi
  for s in NtRemoveIoCompletion NtRemoveIoCompletionEx; do
    n=$(calls_to "$NTSO" "$s" 'd2r_cp_remove|inproc_wait|esync_wait_objects')
    if [ "${n:-0}" -ge 1 ]; then ok 0009 "$s waits in the process"; else bad 0009 "$s never waits in the process (stock server path only)"; fi
  done
  n=$(calls_to "$WS" completion_wait_satisfied reset_sync)
  if [ "${n:-0}" -ge 1 ]; then ok 0009 "wineserver's completion_wait_satisfied resets the port's sync"; else bad 0009 "wineserver's completion_wait_satisfied never resets the port's sync"; fi
  n=$(calls_to "$WS" req_remove_completion reset_sync)
  if [ "${n:-0}" -ge 2 ]; then ok 0009 "wineserver's remove_completion resets the sync of an empty port too"; else bad 0009 "wineserver's remove_completion resets the sync '${n:-0}' times (want 2)"; fi
}

# function(s) $2 of ELF $1 build the set_timer marker 0xd2d2f4e5 (the high half of the argument;
# mov #0xf4e5 + movk #0xd2d2, lsl #16 or #48), or the file holds it as data (a literal pool)
marker_in() {
  local d
  d=$($OBJDUMP -d --no-show-raw-insn --disassemble-symbols="$2" "$1" 2>/dev/null)
  if printf '%s\n' "$d" | grep -qE '#0xf4e5' && printf '%s\n' "$d" | grep -qE '#0xd2d2, lsl #(16|48)'; then return 0; fi
  python3 -I -c 'import sys; sys.exit(0 if open(sys.argv[1],"rb").read().count(bytes.fromhex("e5f4d2d2")) else 1)' "$1"
}

# 0010: anonymous timer reuse (off switch); NtCreateTimer can take a kept timer (it locks the
# table of fresh handles, stock never does); NtSetTimer and the wineserver's set_timer know
# the marker that puts a reused timer back to a new timer's state
check_0010() {
  local n
  if ascii_in "$NTSO" WINE_D2R_TIMER_REUSE; then ok 0010 "${NTSO#$LIB/} reads WINE_D2R_TIMER_REUSE"; else bad 0010 "${NTSO#$LIB/} lacks WINE_D2R_TIMER_REUSE"; fi
  n=$(calls_to "$NTSO" NtCreateTimer 'd2r_tm_take|d2r_tm_note|server_enter_uninterrupted_section')
  if [ "${n:-0}" -ge 1 ]; then ok 0010 "NtCreateTimer can take a kept timer"; else bad 0010 "NtCreateTimer never looks at the kept timers (stock)"; fi
  if marker_in "$NTSO" NtSetTimer,d2r_tm_set; then ok 0010 "NtSetTimer sends the fresh-timer marker"; else bad 0010 "NtSetTimer has no fresh-timer marker"; fi
  if marker_in "$WS" req_set_timer,d2r_timer_fresh; then ok 0010 "wineserver's set_timer knows the fresh-timer marker"; else bad 0010 "wineserver's set_timer has no fresh-timer marker (stock)"; fi
}

# 0011: the wineserver keeps a thread table (request handler, memfd, and the places that change
# a thread's values write its entry: stock has none of these); ntdll asks for it, maps it and
# fills PE ntdll's __wine_d2r_threads; off switch
check_0011() {
  local f s n
  for f in "$NT" "$NT32"; do
    if exports "$f" __wine_d2r_threads; then ok 0011 "${f#$LIB/} exports __wine_d2r_threads"; else bad 0011 "${f#$LIB/} lacks __wine_d2r_threads"; fi
  done
  if ascii_in "$NTSO" WINE_D2R_THREAD_TABLE; then ok 0011 "${NTSO#$LIB/} reads WINE_D2R_THREAD_TABLE"; else bad 0011 "${NTSO#$LIB/} lacks WINE_D2R_THREAD_TABLE"; fi
  if ascii_in "$NTSO" __wine_d2r_threads; then ok 0011 "${NTSO#$LIB/} looks up __wine_d2r_threads"; else bad 0011 "${NTSO#$LIB/} never looks up __wine_d2r_threads"; fi
  n=$(calls_to "$NTSO" d2r_threads_init 'mmap|mmap64|wine_server_receive_fd')
  if [ "${n:-0}" -ge 2 ]; then ok 0011 "${NTSO#$LIB/} receives and maps the table"; else bad 0011 "${NTSO#$LIB/} d2r_threads_init receives/maps '${n:-0}' times (want 2)"; fi
  if ascii_in "$WS" wine-d2r-threads; then ok 0011 "wineserver makes the table's memfd"; else bad 0011 "wineserver has no table memfd (stock)"; fi
  n=$(calls_to "$WS" req_d2r_get_thread_table 'd2r_thread_update|d2r_threads_create|send_client_fd')
  if [ "${n:-0}" -ge 1 ]; then ok 0011 "wineserver handles d2r_get_thread_table"; else bad 0011 "wineserver has no d2r_get_thread_table handler"; fi
  for s in create_thread set_thread_priority set_thread_affinity set_thread_disable_boost kill_thread; do
    n=$(calls_to "$WS" "$s" 'd2r_thread_update|d2r_thread_gone|d2r_thread_removed')
    if [ "${n:-0}" -ge 1 ]; then ok 0011 "wineserver's $s writes the thread's entry"; else bad 0011 "wineserver's $s never writes the thread's entry"; fi
  done
}

# 0012: timer reuse window 10 s (its switch, and the default in the data), registry key and value
# notes from NtOpenKeyEx and NtQueryValueKey, and the export's size word says 3,728 bytes
check_0012() {
  local s n
  if ascii_in "$NTSO" WINE_D2R_TIMER_LIFE_MS; then ok 0012 "${NTSO#$LIB/} reads WINE_D2R_TIMER_LIFE_MS"; else bad 0012 "${NTSO#$LIB/} lacks WINE_D2R_TIMER_LIFE_MS"; fi
  if python3 -I -c 'import sys; sys.exit(0 if open(sys.argv[1],"rb").read().count(bytes.fromhex("00e40b5402000000")) else 1)' "$NTSO"; then
    ok 0012 "${NTSO#$LIB/} holds the 10 s timer window"; else bad 0012 "${NTSO#$LIB/} has no 10 s timer window"; fi
  for s in NtOpenKeyEx NtQueryValueKey; do
    n=$(calls_to "$NTSO" "$s" d2r_reg_note)
    if [ "${n:-0}" -ge 1 ]; then ok 0012 "$s notes its call for FEX"; else bad 0012 "$s never notes its call (stock)"; fi
  done
  n=$($OBJDUMP -d --no-show-raw-insn "$NTSO" 2>/dev/null | grep -cE 'mov\s+w[0-9]+, #0xe90\b')
  if [ "${n:-0}" -ge 1 ]; then ok 0012 "${NTSO#$LIB/} writes the export size 3,728"; else bad 0012 "${NTSO#$LIB/} never writes the export size 3,728"; fi
}

# 0013: event reuse window 10 s: its switch, and the window is a variable in ntdll.so's data
# whose default is 10 s (d2r6-d2r10 had a 10 ms constant in the code and no such variable)
check_0013() {
  local v
  if ascii_in "$NTSO" WINE_D2R_EVENT_LIFE_MS; then ok 0013 "${NTSO#$LIB/} reads WINE_D2R_EVENT_LIFE_MS"; else bad 0013 "${NTSO#$LIB/} lacks WINE_D2R_EVENT_LIFE_MS"; fi
  v=$(python3 -I "$(dirname "$0")/elf-symbol-u64.py" "$NTSO" d2r_ev_life_ns 2>/dev/null)
  if [ "$v" = 10000000000 ]; then ok 0013 "${NTSO#$LIB/} event reuse window defaults to 10 s"
  else bad 0013 "${NTSO#$LIB/} event reuse window is '${v:-not a variable}' ns (want 10000000000)"; fi
}

# 0014: SetupAPI shortcuts for device lists (both PE builds of setupapi.dll read the switch;
# stock has no such string)
check_0014() {
  local f
  for f in "$LIB/aarch64-windows/setupapi.dll" "$LIB/i386-windows/setupapi.dll"; do
    if ascii_in "$f" WINE_D2R_SETUPAPI_FAST; then ok 0014 "${f#$LIB/} reads WINE_D2R_SETUPAPI_FAST"; else bad 0014 "${f#$LIB/} lacks WINE_D2R_SETUPAPI_FAST"; fi
  done
}

for p in "$PATCHES"/*.patch; do
  n=$(basename "$p"); n=${n%%-*}
  if declare -F "check_$n" >/dev/null; then "check_$n"; else bad "$n" "no check for $(basename "$p") in check-patches.sh"; fi
done
exit $fail
