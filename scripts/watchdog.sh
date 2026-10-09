#!/usr/bin/env bash
# Kill Altium if its memory use passes a limit, and log its memory every few seconds.
# Run it next to Altium while testing something that might leak: a runaway leak
# (~55 MB/s before the tabtip.exe fix) can otherwise fill the RAM and freeze the desktop.
#
#   scripts/watchdog.sh [--bottle NAME] [--limit-gb 6] [--interval 3]
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
LIMIT_GB=6
INTERVAL=3
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --limit-gb) LIMIT_GB=$2; shift 2 ;;
        --interval) INTERVAL=$2; shift 2 ;;
        -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
limit_kb=$((LIMIT_GB * 1024 * 1024))

say "Waiting for Altium (limit ${LIMIT_GB} GB)"
until pid=$(altium_pid) && [ -n "$pid" ]; do sleep 2; done
say "Watching PID $pid"
while kill -0 "$pid" 2>/dev/null; do
    rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    printf '%s  %6d MB\n' "$(date +%T)" "$((rss / 1024))"
    if [ "${rss:-0}" -gt "$limit_kb" ]; then
        say "Over ${LIMIT_GB} GB: killing Altium (PID $pid)"
        kill -KILL "$pid"
        break
    fi
    sleep "$INTERVAL"
done
say "Altium is gone"
