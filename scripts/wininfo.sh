#!/usr/bin/env bash
# List Altium's visible top-level windows and the main window's visible children as
# Wine sees them: class, title, rect (Windows coordinates), style, exstyle, owner,
# parent. Compare with `hyprctl clients` to spot windows the compositor misplaces.
#
#   scripts/wininfo.sh [--bottle NAME]
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
[ -f "$ROOT/build/wininfo.exe" ] || "$ROOT/scripts/build.sh" >/dev/null
mkdir -p "$STAGE/tools"
cp "$ROOT/build/wininfo.exe" "$STAGE/tools/"
wine_running "$(winpath "$STAGE/tools/wininfo.exe")" | tr -d '\r'
