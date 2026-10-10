#!/usr/bin/env bash
# Time Altium's redraws without touching the keyboard or mouse: post mouse-wheel steps
# to a view (default the schematic view) and report the CPU time Altium's UI thread spent.
# Every step makes the view render again; half the steps scroll down and half back up,
# so the view ends where it started. Open the document and leave the view alone first.
#
#   scripts/redraw-bench.sh [--bottle NAME] [--class SchView] [--steps 10] [--delay 1500] [--runs 3]
#
#   --class   window class of the view (scripts/wininfo.sh lists Altium's windows)
#   --steps   wheel steps per run (rounded up to an even number)
#   --delay   ms between steps, long enough for one redraw to finish
#   --runs    number of timed runs
#
# Compare runs of one session with each other, or fresh sessions with fresh sessions: a
# session that had been running for an hour was twice as slow at the same redraws.
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
CLASS=SchView
STEPS=10
DELAY=1500
RUNS=3
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --class) CLASS=$2; shift 2 ;;
        --steps) STEPS=$2; shift 2 ;;
        --delay) DELAY=$2; shift 2 ;;
        --runs) RUNS=$2; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
pid=$(altium_pid)
[ -n "$pid" ] || die "Altium is not running in bottle '$BOTTLE'"
[ -f "$ROOT/build/postmsg.exe" ] && [ -f "$ROOT/build/wininfo.exe" ] || "$ROOT/scripts/build.sh" >/dev/null
mkdir -p "$STAGE/tools"
cp "$ROOT/build/postmsg.exe" "$ROOT/build/wininfo.exe" "$STAGE/tools/"

# First visible window of that class: "    <hwnd> pid=... vis=1 [x,y wxh] ... cls=<class> ..."
view=$(wine_running "$(winpath "$STAGE/tools/wininfo.exe")" | tr -d '\r' | grep -m1 " cls=$CLASS \"" || true)
[ -n "$view" ] || die "no visible $CLASS window; open a document of that kind first"
hwnd=$(awk '{print $1}' <<<"$view")
read -r x y w h < <(sed -E 's/.*\[(-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+)\].*/\1 \2 \3 \4/' <<<"$view")
# WM_MOUSEWHEEL: wParam high word = wheel delta (-120 down, +120 up), lParam = screen point.
lparam=$(printf '%04x%04x' $((y + h / 2)) $((x + w / 2)))
half=$(((STEPS + 1) / 2))
postmsg="$(winpath "$STAGE/tools/postmsg.exe")"

# The UI thread is the process's first thread (thread id = pid). utime + stime, in ticks.
ticks() { sed 's/.*) //' "/proc/$pid/task/$pid/stat" | awk '{print $12 + $13}'; }
now() { date +%s.%N; }
hz=$(getconf CLK_TCK)

say "Scrolling $CLASS $hwnd ($((2 * half)) steps, ${DELAY} ms apart, $RUNS runs)"
for run in $(seq 1 "$RUNS"); do
    # Altium's UI thread is not quite idle when left alone (timers, wineserver round
    # trips): measure that rate first and count only what the redraws add to it.
    before=$(ticks); sleep 2
    idle=$(($(ticks) - before))  # ticks per 2 s
    start=$(ticks); t0=$(now)
    wine_running "$postmsg" "$hwnd" 20a ff880000 "$lparam" "$half" "$DELAY"
    sleep "$(awk -v d="$DELAY" 'BEGIN {print d / 1000}')"
    wine_running "$postmsg" "$hwnd" 20a 00780000 "$lparam" "$half" "$DELAY"
    # Done once a whole second is back at the idle rate (or after 60 s).
    for _ in $(seq 1 60); do
        before=$(ticks); sleep 1
        [ $(($(ticks) - before)) -gt $((idle / 2 + 1)) ] || break
    done
    used=$(($(ticks) - start))
    awk -v r="$run" -v n=$((2 * half)) -v t="$used" -v hz="$hz" -v idle="$idle" -v secs="$(awk -v a="$t0" -v b="$(now)" 'BEGIN {print b - a}')" \
        'BEGIN {net = t / hz - idle / 2 / hz * secs; if (net < 0) net = 0
                printf "run %d: %d steps, %.2f s UI-thread CPU above idle (%.0f ms per step); idle %.0f%%, %.1f s total\n",
                       r, n, net, 1000 * net / n, 100 * idle / 2 / hz, t / hz}'
done
