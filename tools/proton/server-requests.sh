#!/bin/bash
# Lists the wineserver request types of a build as "number name", for reading patch 0008's
# counters (__wine_d2r_server_calls: count[number], time_ns[number]).
#   server-requests.sh <include/wine/server_protocol.h as compiled>
awk '
  /^enum request$/ { on = 1; next }
  on && /REQ_NB_REQUESTS/ { exit }
  on && /^ *REQ_[A-Za-z0-9_]+,/ { s = $1; sub(/^REQ_/, "", s); sub(/,$/, "", s); print n++, s }
' "$1"
