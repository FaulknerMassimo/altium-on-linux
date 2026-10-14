#!/usr/bin/env python3
"""Check the XRandR filter (src/xrandrfilter/xrandrfilter.c) in real Wine processes.

Runs build/wininfo.exe, which lists Wine's display adapters and monitors, in a throwaway
prefix of the bottle's runner (inside the Bottles sandbox; the bottle and a running Altium
are not touched), with tests/xrandrfilter_zero.c preloaded: it makes one X11 output look
the way XWayland lists a monitor Hyprland could not bring up (connected, CRTC mode 0x0).
With the filter preloaded first, Wine must list the other monitor(s) and not the zeroed
one, and the filter must log it. With --control, the same run without the filter must
fail: Proton's win32u divides by the monitor's zero width (monitor_get_dpi) and the
desktop process faults over and over, so wininfo never finishes (killed after 60 s).

The zeroed output is the last connected one in `xrandr --query`, or --output NAME.
Needs two connected outputs. The prefix (<bottle>/altium-on-linux/xrandr-test) is made on
the first run (wineboot, about a minute) and kept.

    tests/test_xrandrfilter_wine.py [--bottle NAME] [--output NAME] [--control]
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "scripts/lib.sh")
APP = "com.usebottles.bottles"
SO_FLAGS = ["-shared", "-fPIC", "-O2", "-fvisibility=hidden", "-fno-stack-protector", "-U_FORTIFY_SOURCE",
            "-Wall", "-Wextra", "-Werror"]


def bottle_paths(bottle):
    out = subprocess.run(["bash", "-c", f'source {LIB}; load_bottle "$1"; printf "%s\\n%s\\n" "$STAGE" "$RUNNER_DIR"',
                          "_", bottle], capture_output=True, text=True, check=True).stdout.split("\n")
    return out[0], out[1]


def sandbox(runner, prefix, env, *cmd, timeout):
    """Run cmd in a new Bottles sandbox with the test prefix; then end its wineserver."""
    script = ('export WINEPREFIX="$1" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" "${@:3}"; '
              'timeout -s KILL "$2" "${CMD[@]}"; code=$?; "$RUNNER/bin/wineserver" -k 2>/dev/null; exit $code')
    full = ["flatpak", "run", "--command=bash", APP, "-c",
            f'RUNNER="{runner}"; CMD=({" ".join(repr(c) for c in cmd)}); ' + script,
            "_", prefix, str(timeout), *env]
    return subprocess.run(full, capture_output=True, text=True, timeout=timeout + 60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    ap.add_argument("--output")
    ap.add_argument("--control", action="store_true", help="also run without the filter (must fail)")
    args = ap.parse_args()
    connected = re.findall(r"^(\S+) connected", subprocess.run(["xrandr", "--query"], capture_output=True,
                                                               text=True).stdout, re.M)
    if len(connected) < 2:
        sys.exit(f"needs two connected outputs, xrandr lists {connected}")
    output = args.output or connected[-1]
    stage, runner = bottle_paths(args.bottle)
    work = os.path.join(stage, "xrandr-test")
    prefix = os.path.join(work, "prefix")
    os.makedirs(work, exist_ok=True)
    subprocess.run(["clang", *SO_FLAGS, "-o", f"{work}/altium_xrandr_filter.so",
                    f"{ROOT}/src/xrandrfilter/xrandrfilter.c"], check=True)
    subprocess.run(["clang", *SO_FLAGS, "-o", f"{work}/xrandrfilter_zero.so", f"{ROOT}/tests/xrandrfilter_zero.c"],
                   check=True)
    if not os.path.exists(f"{ROOT}/build/wininfo.exe"):
        subprocess.run([f"{ROOT}/scripts/build.sh"], check=True, capture_output=True)
    subprocess.run(["cp", f"{ROOT}/build/wininfo.exe", f"{work}/wininfo.exe"], check=True)
    wininfo = "Z:" + f"{work}/wininfo.exe".replace("/", "\\")
    if not os.path.exists(f"{prefix}/system.reg"):
        print("making the test prefix (wineboot)...")
        res = sandbox(runner, prefix, [], f"{runner}/bin/wine", "wineboot", "-i", timeout=300)
        if res.returncode:
            sys.exit(f"wineboot failed ({res.returncode}): {res.stderr[-2000:]}")
    failures = 0
    for with_filter in (True, False) if args.control else (True,):
        log = f"{work}/filter.log"
        if os.path.exists(log):
            os.remove(log)
        preload = (f"{work}/altium_xrandr_filter.so:" if with_filter else "") + f"{work}/xrandrfilter_zero.so"
        env = [f"LD_PRELOAD={preload}", f"XRANDR_ZERO_OUTPUT={output}", f"ALTIUM_XRANDR_FILTER_LOG={log}"]
        start = time.time()
        res = sandbox(runner, prefix, env, f"{runner}/bin/wine", wininfo, timeout=60)
        took = time.time() - start
        monitors = [line for line in res.stdout.replace("\r", "").split("\n") if line.startswith("monitor ")]
        logged = open(log).read() if os.path.exists(log) else ""
        name = "with the filter" if with_filter else "without the filter"
        print(f"{name} ({output} at 0x0, {took:.1f} s, exit {res.returncode}):")
        print("    " + ("\n    ".join(monitors) or "(no monitors listed)"))
        if logged:
            print("    filter log: " + logged.strip().split("\n")[0])
        if with_filter:
            ok = res.returncode == 0 and len(monitors) == len(connected) - 1 and f"hid output {output}" in logged
        else:
            ok = res.returncode != 0 or len(monitors) != len(connected) - 1
        print(("ok   " if ok else "FAIL ") + name + ("" if with_filter else ": Wine breaks on the 0x0 output"
                                                     if ok else ": Wine coped, so the test proves nothing"))
        failures += not ok
    print("all passed" if not failures else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
