#!/usr/bin/env bash
# Managed (.NET) stacks of every thread of the running Altium, with Microsoft's
# dotnet-dump (downloaded once into build/tools). Use it when Altium freezes: winedbg
# shows .NET frames only as "coreclr+0x..." while this names every method.
#
#   scripts/dotnet-stacks.sh [--bottle NAME] [--out FILE] [--keep-dump]
#
# Under Wine, dotnet-dump's default "heap" dump comes out a few hundred KB and useless,
# so this takes a full dump (about as large as Altium's memory use, usually 2-3 GB) in
# the bottle and deletes it afterwards unless --keep-dump is given. dotnet-stack, the
# lighter tool, does not work: its trace parser needs tdh.dll!TdhGetEventInformation,
# which Wine does not implement.
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
OUT=
KEEP=0
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --keep-dump) KEEP=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
load_bottle "$BOTTLE"
OUT=${OUT:-$ROOT/build/dotnet-stacks-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")" "$ROOT/build/tools" "$STAGE/tools" "$STAGE/dumps"

tool=$ROOT/build/tools/dotnet-dump.exe
if [ ! -f "$tool" ]; then
    say "Downloading dotnet-dump (Microsoft, win-x64)"
    curl -fsSL -o "$tool" https://aka.ms/dotnet-dump/win-x64
fi
cp "$tool" "$STAGE/tools/"   # the sandbox cannot read this repository

winpid=$(altium_winpid)
[ -n "$winpid" ] || die "Altium is not running in bottle '$BOTTLE'"
pid=$((16#$winpid))
dump=$STAGE/dumps/x2-$(date +%Y%m%d-%H%M%S).dmp

# Altium is framework-dependent on the .NET runtime it ships in System\DotNet\Runtime.
dotnet_dump() {
    in_running_bottle WINEDEBUG=-all DOTNET_SYSTEM_GLOBALIZATION_USENLS=1 \
        "DOTNET_ROOT=C:\\Program Files\\Altium\\$ALTIUM_DIR\\System\\DotNet\\Runtime" \
        "$RUNNER_DIR/bin/wine" "$(winpath "$STAGE/tools/dotnet-dump.exe")" "$@"
}

say "Writing a full dump of X2.EXE (Windows PID $pid); this takes a minute"
dotnet_dump collect -p "$pid" --type Full -o "$(winpath "$dump")" | tail -1
say "Extracting managed stacks"
dotnet_dump analyze "$(winpath "$dump")" -c clrthreads -c 'clrstack -all' -c exit \
    | sed -E 's/ \[\/_\/src[^]]*\]//' > "$OUT"
if [ "$KEEP" = 1 ]; then say "Dump kept: $dump"; else rm -f "$dump"; fi

say "Wrote $OUT. Top frames of each thread running managed code:"
awk '/^OS Thread Id:/ { tid = $4; shown = 0; next }
     /^[0-9A-Fa-f]{16} [0-9A-Fa-f]{16} / && shown < 3 { sub(/^[0-9A-Fa-f]{16} [0-9A-Fa-f]{16} /, ""); if ($0 !~ /^\[/) { print tid "  " $0; shown++ } }' \
    "$OUT" | cut -c1-160
