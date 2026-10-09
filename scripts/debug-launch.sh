#!/usr/bin/env bash
# Start Altium the way Bottles does, but with Wine/DXVK output in a log file.
#
#   scripts/debug-launch.sh [--bottle NAME] [--debug CHANNELS] [--log FILE] [DOCUMENT]
#
#   --debug   WINEDEBUG channels (default: fixme-all,warn+seh,+timestamp)
#             e.g. +loaddll (which DLLs load, and where), +bcrypt, +exec,+winebrowser
#             (link opening), +process (programs started), +thread
#   --log     log file (default: <bottle>/altium-on-linux/logs/altium-<time>.log)
#   DOCUMENT  a file to open on start (host or Windows path), e.g. a .PcbDoc; handy
#             for reproducing a problem without clicking through the UI.
#
# Runs in the foreground until Altium exits. Combine with scripts/watchdog.sh when
# chasing memory leaks.
set -euo pipefail
# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
CHANNELS=fixme-all,warn+seh,+timestamp
LOG=
DOC=
while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --debug) CHANNELS=$2; shift 2 ;;
        --log) LOG=$2; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) DOC=$1; shift ;;
    esac
done

load_bottle "$BOTTLE"
[ -z "$(bottle_pids)" ] || die "the bottle is already running; close Altium first"
mkdir -p "$STAGE/logs"
LOG=${LOG:-$STAGE/logs/altium-$(date +%Y%m%d-%H%M%S).log}
case $LOG in "$HOME"/.var/app/$APP/*) ;; *) die "the log must be inside $HOME/.var/app/$APP (the sandbox cannot write elsewhere)";; esac
if [ -n "$DOC" ] && [ -e "$DOC" ]; then DOC=$(winpath "$(realpath "$DOC")"); fi

# Same environment Bottles uses for this bottle (see `ps e` of a Bottles launch).
env_args=(
    WINEPREFIX="$BOTTLE_DIR" WINEARCH=win64 BOTTLE="$BOTTLE"
    WINEFSYNC=1 WINE_LARGE_ADDRESS_AWARE=1 STAGING_SHARED_MEMORY=1
    WINEDLLOVERRIDES="winemenubuilder=;easyanticheat_x86,easyanticheat_x64=b,n;beclient,beclient_x64=b,n"
    LD_LIBRARY_PATH="$RUNNER_DIR/lib:$RUNNER_DIR/lib/wine/x86_64-unix:$RUNNER_DIR/lib/wine/i386-unix"
    WINEDEBUG="$CHANNELS" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none
)
# The bottle's own environment variables (SteamAppId, DOTNET_..., ...).
while IFS= read -r line; do env_args+=("$line"); done < <(
    awk '/^Environment_Variables:/{on=1; next} on && /^    [A-Za-z_][A-Za-z0-9_]*: /{sub(/^    /,""); k=$1; sub(/:$/,"",k);
         $1=""; v=substr($0,2); gsub(/^'"'"'|'"'"'$/,"",v); print k"="v; next} on{exit}' "$BOTTLE_DIR/bottle.yml")

args=("C:\\Program Files\\Altium\\$ALTIUM_DIR\\X2.EXE")
[ -z "$DOC" ] || args+=("$DOC")

say "Starting Altium ($ALTIUM_DIR) with WINEDEBUG=$CHANNELS"
say "Log: $LOG"
# shellcheck disable=SC2016 # expanded by the inner bash
flatpak run --command=bash "$APP" -c 'cd "$1" && shift && exec env "$@"' _ "$AD" \
    "${env_args[@]}" "$RUNNER_DIR/bin/wine" start /wait /d "C:\\Program Files\\Altium\\$ALTIUM_DIR" "${args[@]}" \
    > "$LOG" 2>&1 || true
say "Altium exited. Log: $LOG"
