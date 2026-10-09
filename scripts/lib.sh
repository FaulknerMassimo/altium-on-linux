# shellcheck shell=bash disable=SC2034 # variables set here are used by the sourcing scripts
# Shared helpers for the scripts in this directory (source it, don't run it).

APP=com.usebottles.bottles
DATA=$HOME/.var/app/$APP/data/bottles
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

say() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# Windows path (Z:\...) of a host path, for programs running inside the bottle.
winpath() { printf 'Z:%s' "${1//\//\\}"; }

# load_bottle NAME [ALTIUM_DIR]
# Sets BOTTLE_DIR, RUNNER, RUNNER_DIR, ALTIUM_DIR, AD (Altium's folder on the host)
# and STAGE (a folder inside the bottle that the Bottles sandbox can read and write;
# the sandbox has no access to this repository).
load_bottle() {
    BOTTLE_DIR=$DATA/bottles/$1
    [ -f "$BOTTLE_DIR/bottle.yml" ] || die "no Bottles bottle named '$1' in $DATA/bottles (use --bottle)"
    RUNNER=$(sed -n 's/^Runner: //p' "$BOTTLE_DIR/bottle.yml")
    RUNNER_DIR=$DATA/runners/$RUNNER
    [ -x "$RUNNER_DIR/bin/wine" ] || die "runner '$RUNNER' not found in $DATA/runners"
    local altium_root="$BOTTLE_DIR/drive_c/Program Files/Altium"
    ALTIUM_DIR=${2:-}
    if [ -z "$ALTIUM_DIR" ]; then
        ALTIUM_DIR=$(find "$altium_root" -maxdepth 2 -iname 'X2.EXE' -printf '%h\n' 2>/dev/null \
            | sort -V | tail -1 | xargs -r -d '\n' basename)
    fi
    AD="$altium_root/$ALTIUM_DIR"
    [ -n "$ALTIUM_DIR" ] && [ -f "$AD/X2.EXE" ] || die "Altium Designer not found under $altium_root (use --altium-dir)"
    STAGE=$BOTTLE_DIR/altium-on-linux
}

# PIDs of processes running in the bottle (their WINEPREFIX is the bottle).
bottle_pids() {
    local env pid
    for env in /proc/[0-9]*/environ; do
        pid=${env#/proc/}; pid=${pid%/environ}
        tr '\0' '\n' 2>/dev/null < "$env" | grep -qxF "WINEPREFIX=$BOTTLE_DIR" && echo "$pid"
    done
    return 0
}

# PID of the running Altium (X2.EXE) in the bottle, if any.
altium_pid() {
    local pid
    for pid in $(bottle_pids); do
        # argv[0] only: start.exe's command line also contains ...\X2.EXE
        if tr '\0' '\n' 2>/dev/null < "/proc/$pid/cmdline" | sed -n 1p | grep -qiE '\\X2\.EXE$'; then echo "$pid"; return; fi
    done
}

# Run a command inside the Flatpak sandbox of the bottle's running Wine session, so it
# talks to the same wineserver. (A second `flatpak run` gets its own /tmp and would
# start a second wineserver on the same prefix, which corrupts the registry.)
in_running_bottle() {
    local pids pid instance
    pids=$(bottle_pids)  # no `| head -1`: with pipefail its SIGPIPE would fail the caller
    pid=${pids%%$'\n'*}
    [ -n "$pid" ] || die "the bottle is not running"
    instance=$(sed -n 's/^instance-id=//p' "/proc/$pid/root/.flatpak-info" 2>/dev/null)
    [ -n "$instance" ] || die "cannot find the Flatpak sandbox of PID $pid"
    flatpak enter "$instance" env HOME="$HOME" WINEPREFIX="$BOTTLE_DIR" "$@"
}

# wine_running ARGS...: run "wine ARGS" in the bottle's running session.
wine_running() { in_running_bottle WINEDEBUG="${WINEDEBUG:--all}" "$RUNNER_DIR/bin/wine" "$@"; }

# Windows PID (hex, as winedbg prints it) of X2.EXE in the running session.
altium_winpid() {
    wine_running winedbg --command "info proc" 2>/dev/null | awk '!found && tolower($0) ~ /x2\.exe/ {print $1; found = 1}'
}
