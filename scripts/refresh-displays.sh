#!/usr/bin/env bash
# Resync Wine's monitor list with X11 after monitors are plugged, unplugged or rearranged
# while the bottle runs (a closed lid, a monitor switched off and on): deletes Wine's stale
# display slot links (build/displayfix.exe) and makes a fresh process rebuild the list, then
# prints it. Without this, Altium can lose a monitor, see two primaries, and draw offset
# (docs/how-it-works.md §19). The Hyprland hook in config/hyprland/altium.lua runs it on
# every monitor change; it does nothing when the bottle isn't running.
#
#   scripts/refresh-displays.sh [--bottle NAME] [--quiet]
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
QUIET=0
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
[ -n "$(wineserver_pid)" ] || exit 0

# A lid or a monitor going off and on fires several monitor events within seconds: one run
# at a time, each one sees the latest layout.
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/altium-refresh-displays.lock"
flock 9

for tool in displayfix wininfo; do
    [ -f "$ROOT/build/$tool.exe" ] || "$ROOT/scripts/build.sh" >/dev/null
done
mkdir -p "$STAGE/tools"
cp "$ROOT/build/displayfix.exe" "$ROOT/build/wininfo.exe" "$STAGE/tools/"

show() { if [ "$QUIET" = 1 ]; then cat >/dev/null; else cat; fi; }
wine_running "$(winpath "$STAGE/tools/displayfix.exe")" | tr -d '\r' | show
# A new process finds no slot links, rebuilds the list from X11 and tells every other
# process (Altium, explorer) to reload it.
wine_running "$(winpath "$STAGE/tools/wininfo.exe")" | tr -d '\r' | grep -E '^(adapter|monitor) ' | show
