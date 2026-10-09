#!/usr/bin/env bash
# Apply the Altium-on-Wine fixes to a Bottles (Flatpak) bottle in which Altium
# Designer is already installed. Safe to re-run. See README.md for what each fix does.
#
#   scripts/install.sh [--bottle NAME] [--altium-dir AD26] [--no-fonts] [--dry-run]
set -euo pipefail

# shellcheck source=scripts/lib.sh
. "$(dirname "$0")/lib.sh"

BOTTLE=Altium
ALTIUM_DIR=
FONTS=1
DRY=0

SELAWIK_URL=https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip
SELAWIK_SHA256=3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423

usage() {
    sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case $1 in
        --bottle) BOTTLE=$2; shift 2 ;;
        --altium-dir) ALTIUM_DIR=$2; shift 2 ;;
        --no-fonts) FONTS=0; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
done

run() { if [ "$DRY" = 1 ]; then printf '    + %s\n' "$*"; else "$@"; fi; }
quiet() {
    if [ "$DRY" = 1 ]; then printf '    + %s\n' "$*"; return; fi
    local log; log=$(mktemp)
    "$@" >"$log" 2>&1 || { cat "$log" >&2; rm -f "$log"; die "failed: $*"; }
    rm -f "$log"
}

load_bottle "$BOTTLE" "$ALTIUM_DIR"

say "Bottle '$BOTTLE' (runner $RUNNER), Altium in Program Files/Altium/$ALTIUM_DIR"
if [ -n "$(bottle_pids)" ]; then
    [ "$DRY" = 1 ] || die "the bottle is running: close Altium (and wait a few seconds for Wine to exit)"
    echo "    (the bottle is running; a real install would stop here)"
fi
if pgrep -f '/app/bin/bottles( |$)' >/dev/null; then
    [ "$DRY" = 1 ] || die "close the Bottles window first (it would overwrite the bottle's settings)"
    echo "    (Bottles is open; a real install would stop here)"
fi

# Run a Wine command in the bottle, inside the Bottles sandbox, and wait for wineserver.
wine_in_bottle() {
    # shellcheck disable=SC2016 # expanded by the inner bash, not here
    quiet flatpak run --command=bash "$APP" -c \
        'WINEPREFIX="$1" WINEDEBUG=-all "$2/bin/wine" "${@:3}" && WINEPREFIX="$1" "$2/bin/wineserver" -w' \
        _ "$BOTTLE_DIR" "$RUNNER_DIR" "$@"
}

say "Building the Windows binaries"
quiet "$ROOT/scripts/build.sh"

say "Installing cmd.exe shim, SHFolder.dll proxy and dxvk.conf into $ALTIUM_DIR"
# Altium ships none of these, so an existing copy is from an earlier run: keep it as .bak.
for f in build/cmd.exe build/SHFolder.dll config/dxvk.conf; do
    dest="$AD/$(basename "$f")"
    if [ -f "$dest" ] && ! cmp -s "$ROOT/$f" "$dest"; then
        run cp -p "$dest" "$dest.bak"
    fi
    run cp "$ROOT/$f" "$dest"
done

run mkdir -p "$STAGE"
run cp "$ROOT/registry/altium-fixes.reg" "$STAGE/"
regs=(altium-fixes.reg)

if [ "$FONTS" = 1 ]; then
    say "Building Segoe UI-named copies of Microsoft's Selawik font"
    cache=$ROOT/build/fonts
    zip=$cache/Selawik_Release.zip
    run mkdir -p "$cache/src" "$cache/out"
    if [ ! -f "$zip" ]; then run curl -fsSL -o "$zip" "$SELAWIK_URL"; fi
    if [ "$DRY" = 0 ]; then
        echo "$SELAWIK_SHA256  $zip" | sha256sum -c --quiet - || die "Selawik download checksum mismatch"
        [ -x "$ROOT/build/venv/bin/python" ] || python3 -m venv "$ROOT/build/venv"
        "$ROOT/build/venv/bin/python" -c 'import fontTools' 2>/dev/null || "$ROOT/build/venv/bin/pip" install -q fonttools
    fi
    run unzip -qo "$zip" 'selawk*.ttf' -d "$cache/src"
    quiet "$ROOT/build/venv/bin/python" "$ROOT/fonts/rename_to_segoeui.py" "$cache/src" "$cache/out"
    run cp "$cache/out/"segoeui-selawik*.ttf "$BOTTLE_DIR/drive_c/windows/Fonts/"
    run cp "$ROOT/registry/fonts.reg" "$STAGE/"
    regs+=(fonts.reg)
fi

# "Altium Designer {GUID}" keys of the installations that have already run once.
mapfile -t install_keys < <(grep -oE '^\[Software\\\\Altium\\\\Altium Designer \{[0-9A-Fa-f-]+\}' "$BOTTLE_DIR/user.reg" \
    | sed -E 's/^\[Software\\\\Altium\\\\//' | sort -u)
if [ "${#install_keys[@]}" -gt 0 ]; then
    for key in "${install_keys[@]}"; do
        name="unified-login-$(grep -oE '[0-9A-Fa-f-]{36}' <<< "$key").reg"
        if [ "$DRY" = 1 ]; then echo "    + write $STAGE/$name for $key"
        else sed "s/@INSTALL_KEY@/$key/" "$ROOT/registry/unified-login.reg.in" > "$STAGE/$name"; fi
        regs+=("$name")
    done
fi

say "Importing registry fixes: ${regs[*]}"
for reg in "${regs[@]}"; do
    wine_in_bottle regedit /S "$(winpath "$STAGE/$reg")"
done

say "Setting bottle environment variables"
quiet flatpak run --command=bottles-cli "$APP" edit -b "$BOTTLE" --env-var DOTNET_SYSTEM_GLOBALIZATION_USENLS=1
quiet flatpak run --command=bottles-cli "$APP" edit -b "$BOTTLE" --env-var SteamAppId=altium

if [ "$DRY" = 0 ]; then
    say "Checking"
    check() { if grep -qF "$2" "$3"; then echo "    ok   $1"; else echo "    MISSING $1"; fi; }
    check "tabtip.exe disabled" '"tabtip.exe"=""' "$BOTTLE_DIR/user.reg"
    check "WPF software rendering" '"DisableHWAcceleration"=dword:00000001' "$BOTTLE_DIR/user.reg"
    check "SHFolder proxy override" '"shfolder"="native,builtin"' "$BOTTLE_DIR/user.reg"
    [ "$FONTS" = 0 ] || check "Segoe UI font" '"Segoe UI (TrueType)"="segoeui-selawik.ttf"' "$BOTTLE_DIR/system.reg"
    [ "${#install_keys[@]}" -eq 0 ] || check "browser sign-in" '"UnifiedLogin"="1"' "$BOTTLE_DIR/user.reg"
    check "SteamAppId" 'SteamAppId: altium' "$BOTTLE_DIR/bottle.yml"
fi

echo
if [ "${#install_keys[@]}" -eq 0 ]; then
    echo "Altium has not run in this bottle yet, so browser sign-in could not be switched on."
    echo "Start Altium once, close it, and run this script again (or enable System.UnifiedLogin"
    echo "under Preferences > System > General > Advanced)."
fi
echo "Hyprland users: add config/hyprland/altium.lua to ~/.config/hypr/hyprland.lua."
echo "Done. Start Altium from Bottles."
