#!/bin/sh
# Boot the port hidden and drive the menus into Career > Tony Hawk > Warehouse.
# tools/into_level.sh NAME SECONDS "SHOT_TIMES" ["DUMP_TIMES"] ["EXTRA_PRESSES"]  (level is up from about 62 s)
P="A@8.5,START@20,START@23,START@26,A@30,A@33,A@36,A@39,A@42,A@46,A@52,A@56"
[ -n "$5" ] && P="$P,$5"
exec "$(dirname "$0")/autorun.sh" "$1" "$2" "$P" "$3" "${4:-}"
