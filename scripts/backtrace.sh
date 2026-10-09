#!/usr/bin/env bash
# Native backtraces of every thread of the running Altium, via winedbg. Altium is
# paused while winedbg is attached (a few seconds) and resumes when it detaches.
# For a freeze, look at the first thread (the UI thread); for managed (.NET) frames,
# which show up here only as module+offset, use scripts/dotnet-stacks.sh.
#
#   scripts/backtrace.sh [--bottle NAME] [--out FILE]
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
OUT=${OUT:-$ROOT/build/backtrace-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"

winpid=$(altium_winpid)
[ -n "$winpid" ] || die "Altium is not running in bottle '$BOTTLE'"
say "Attaching winedbg to X2.EXE (Windows PID 0x$winpid)"
printf 'attach 0x%s\nbt all\ndetach\nquit\n' "$winpid" | in_running_bottle WINEDEBUG=-all "$RUNNER_DIR/bin/wine" winedbg > "$OUT" 2>&1 || true

say "Wrote $OUT ($(grep -c 'Backtracing for thread' "$OUT") threads). Modules on each thread's stack:"
awk '/Backtracing for thread/ { if (t) print t ": " mods; t = $4; mods = ""; next }
     / in [A-Za-z0-9_.]+ / { m = $0; sub(/.* in /, "", m); sub(/ .*/, "", m); if (index(mods, m) == 0) mods = mods " " m }
     END { if (t) print t ": " mods }' "$OUT" | cut -c1-160
