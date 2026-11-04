#!/usr/bin/env bash
# Apply the Altium-on-Wine fixes to a Bottles (Flatpak) bottle. Run it twice: before
# installing Altium (it prepares the bottle for the installer) and after (it adds the
# fixes that go into Altium's folder). Safe to re-run. See README.md for each fix.
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
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
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

# install_file SRC DEST: copy, keeping a different existing file as DEST.bak (Altium and
# Wine ship none of the files replaced here, or only Wine's stub, so it is the original or
# a copy from an earlier run).
install_file() {
    if [ -f "$2" ] && ! cmp -s "$1" "$2"; then
        run cp -p "$2" "$2.bak"
    fi
    run cp "$1" "$2"
}

load_bottle_dir "$BOTTLE"
if find_altium "$ALTIUM_DIR"; then
    say "Bottle '$BOTTLE' (runner $RUNNER), Altium in Program Files/Altium/$ALTIUM_DIR"
else
    [ -z "$ALTIUM_DIR" ] || die "Altium Designer not found in $AD (check --altium-dir)"
    AD=
    say "Bottle '$BOTTLE' (runner $RUNNER), Altium not installed yet: preparing the bottle for the installer"
fi
if [ -n "$(bottle_pids)" ]; then
    [ "$DRY" = 1 ] || die "the bottle is running: close Altium or its installer (and wait a few seconds for Wine to exit)"
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

if [ -n "$AD" ]; then
    say "Installing cmd.exe shim, SHFolder.dll proxy and dxvk.conf into $ALTIUM_DIR"
    for f in build/cmd.exe build/SHFolder.dll config/dxvk.conf; do
        install_file "$ROOT/$f" "$AD/$(basename "$f")"
    done
fi

# 32-bit cmd.exe runs `start` as syswow64\start.exe: the installer's sign-in (Authorizer.exe,
# 32-bit) opens the browser through it. Wine keeps a file that is not one of its stubs when
# it updates the prefix.
say "Installing the start.exe shim for 32-bit programs (the installer's sign-in)"
install_file "$ROOT/build/start.exe" "$BOTTLE_DIR/drive_c/windows/syswow64/start.exe"

# Implicit Vulkan layer for the bottle only, found through VK_ADD_IMPLICIT_LAYER_PATH (set below).
say "Installing the present-wait Vulkan layer into $STAGE/vulkan"
run mkdir -p "$STAGE/vulkan"
run cp "$ROOT/build/VkLayer_altium_present_wait.so" "$ROOT/src/presentwait/VkLayer_altium_present_wait.json" "$STAGE/vulkan/"

# Preloaded into the bottle's Wine processes (LD_PRELOAD, set below): the fd guard keeps
# stray closes, Wine's own among them, off Wine's server pipes, which froze Altium (logs to
# $STAGE/logs/fd-guard.log); the XRandR filter hides 0x0 monitors, on which Wine divides
# by zero (logs to $STAGE/logs/xrandr-filter.log). Both are 64-bit, like Altium. The
# zero-size filter keeps 0x0 windows unmapped, which Hyprland would blow up to 800x800
# (logs to $STAGE/logs/zero-size.log); the installer is 32-bit, so it comes in both, and
# the dynamic loader picks the process's own through $LIB, which in the Bottles Flatpak
# runtime is lib/x86_64-linux-gnu or lib/i386-linux-gnu ($PLATFORM would depend on the CPU).
say "Installing the fd guard, the XRandR filter and the zero-size filter into $STAGE"
run mkdir -p "$STAGE/fdguard" "$STAGE/logs"
run cp "$ROOT/build/altium_fd_guard.so" "$ROOT/build/altium_xrandr_filter.so" "$STAGE/fdguard/"
for arch in x86_64 i386; do
    run mkdir -p "$STAGE/zerosize/lib/$arch-linux-gnu"
    run cp "$ROOT/build/${arch/i386/i686}/altium_zero_size.so" "$STAGE/zerosize/lib/$arch-linux-gnu/"
done
# shellcheck disable=SC2016 # $LIB is for the dynamic loader
PRELOAD=$STAGE/fdguard/altium_fd_guard.so:$STAGE/fdguard/altium_xrandr_filter.so:$STAGE/zerosize/'$LIB'/altium_zero_size.so

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

# "Altium Designer {GUID}" keys of the installations: the installer creates one (the online
# installer stores its sign-in there), Altium creates it on its first start otherwise.
mapfile -t install_keys < <(grep -oE '^\[Software\\\\Altium\\\\Altium Designer \{[0-9A-Fa-f-]+\}' "$BOTTLE_DIR/user.reg" \
    | sed -E 's/^\[Software\\\\Altium\\\\//' | sort -u)
if [ -n "$AD" ] && [ "${#install_keys[@]}" -gt 0 ]; then
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
quiet flatpak run --command=bottles-cli "$APP" edit -b "$BOTTLE" --env-var VK_ADD_IMPLICIT_LAYER_PATH="$STAGE/vulkan"
quiet flatpak run --command=bottles-cli "$APP" edit -b "$BOTTLE" --env-var LD_PRELOAD="$PRELOAD"

# Bottles lists a bottle's programs from their Start Menu shortcuts, but Altium's points at
# X2.exe and the file is X2.EXE, which a case-sensitive file system does not match.
if [ -n "$AD" ]; then
    programs=$(flatpak run --command=bottles-cli "$APP" -j programs -b "$BOTTLE" 2>/dev/null || true)
    if ! python3 -c 'import json, sys
sys.exit(0 if any(p.get("path") == sys.argv[1] for p in json.loads(sys.stdin.read() or "[]")) else 1)' \
        "$AD/X2.EXE" <<< "$programs" 2>/dev/null; then
        say "Adding Altium Designer to the bottle's programs"
        quiet flatpak run --command=bottles-cli "$APP" add -b "$BOTTLE" -n "Altium Designer" -p "$AD/X2.EXE"
    fi
fi

if [ "$DRY" = 0 ]; then
    say "Checking"
    check() { if grep -qF "$2" "$3"; then echo "    ok   $1"; else echo "    MISSING $1"; fi; }
    check_file() { if cmp -s "$2" "$3"; then echo "    ok   $1"; else echo "    MISSING $1"; fi; }
    check "tabtip.exe disabled" '"tabtip.exe"=""' "$BOTTLE_DIR/user.reg"
    check "WPF software rendering" '"DisableHWAcceleration"=dword:00000001' "$BOTTLE_DIR/user.reg"
    check "SHFolder proxy override" '"shfolder"="native,builtin"' "$BOTTLE_DIR/user.reg"
    check "real display modes" '"EmulateModeset"="N"' "$BOTTLE_DIR/user.reg"
    [ "$FONTS" = 0 ] || check "Segoe UI font" '"Segoe UI (TrueType)"="segoeui-selawik.ttf"' "$BOTTLE_DIR/system.reg"
    check_file "start.exe shim (installer sign-in)" "$ROOT/build/start.exe" "$BOTTLE_DIR/drive_c/windows/syswow64/start.exe"
    if [ -n "$AD" ]; then
        check_file "cmd.exe shim (Altium sign-in)" "$ROOT/build/cmd.exe" "$AD/cmd.exe"
        check_file "SHFolder.dll proxy" "$ROOT/build/SHFolder.dll" "$AD/SHFolder.dll"
        [ "${#install_keys[@]}" -eq 0 ] || check "browser sign-in" '"UnifiedLogin"="1"' "$BOTTLE_DIR/user.reg"
    fi
    check "SteamAppId" 'SteamAppId: altium' "$BOTTLE_DIR/bottle.yml"
    check "present-wait layer" "VK_ADD_IMPLICIT_LAYER_PATH: $STAGE/vulkan" "$BOTTLE_DIR/bottle.yml"
    check "fd guard, XRandR and zero-size filters" "LD_PRELOAD: $PRELOAD" "$BOTTLE_DIR/bottle.yml"
fi

echo
if [ -z "$AD" ]; then
    echo "The bottle is ready for the Altium Designer installer. Next: run the installer from"
    echo "Bottles (README step 6), untick \"Run Altium Designer\" at the end, then run this"
    echo "script again."
    exit 0
fi
if [ "${#install_keys[@]}" -eq 0 ]; then
    echo "Altium's settings are not in the registry yet, so browser sign-in could not be switched"
    echo "on. Start Altium once, close it, and run this script again (or enable"
    echo "System.UnifiedLogin under Preferences > System > General > Advanced)."
fi
echo "Done. Start Altium from Bottles."
