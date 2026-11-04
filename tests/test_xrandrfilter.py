#!/usr/bin/env python3
"""Check src/xrandrfilter/xrandrfilter.c (hides 0x0 outputs from Wine) on the host.

Builds the filter, a stand-in libXrandr (tests/xrandrfilter_fake.c) and
tests/test_xrandrfilter.c, which looks up XRRGetOutputInfo the way winex11.so does
(dlopen, then dlsym on the handle). With the filter preloaded, the output whose CRTC has
a 0x0 mode must read as disconnected, and be logged; the others, including one without a
CRTC, must not change, and every CRTC info the filter asks for must be freed. Lookups that
depend on the caller (RTLD_NEXT) must still see the program, not the filter.

The driver is linked to a stand-in libX11 (tests/xrandrfilter_fakex11.c) and sets an error
handler: RandR's errors for resources that are gone, and BadWindow from mapping or
unmapping, must be dropped and logged; every other error must reach the handler, and
XSetErrorHandler() must return the handlers the program set, never the filter.
ALTIUM_XRANDR_FILTER=off must leave everything alone.

    tests/test_xrandrfilter.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "clang")
FLAGS = ["-O2", "-Wall", "-Wextra", "-Werror", "-g"]

HANDLERS = ("RandR queried 1\nprevious handler is the default 1\n{wine}"
            "previous handler is wine's 1\nother handler: error 3 request 18.0\n"
            "previous handler is the other 1\ndefault handler: error 147 request 140.9\n")
WINE_ALL = "".join(f"wine handler: error {e}\n" for e in ("147 request 140.9", "148 request 140.20", "150 request 140.33",
                                                         "151 request 140.9", "3 request 10.0", "3 request 8.0",
                                                         "3 request 18.0", "8 request 10.0"))
WINE_FILTERED = "wine handler: error 151 request 140.9\nwine handler: error 3 request 18.0\nwine handler: error 8 request 10.0\n"
CASES = {
    "filter": ({}, "DP-1 connected crtc 11\nDP-4 disconnected crtc 0\nDP-5 connected crtc 0\nDP-6 disconnected crtc 0\n"
                   "crtc infos freed 2\n" + HANDLERS.format(wine=WINE_FILTERED),
               [(r"hid output DP-4 from test_xrandrfi.* its CRTC is 0x0 at 0,0", 1),
                (r"dropped X error 14[78] \(request 140\.(9|20), resource 0x123\) in test_xrandrfi.*a RandR resource", 2),
                (r"dropped X error 150 .*a RandR resource that no longer exists", 1),
                (r"dropped X error 3 \(request (10|8)\.0, .*a window that no longer exists", 2),
                (r"dropped", 5)]),
    "off": ({"ALTIUM_XRANDR_FILTER": "off"},
            "DP-1 connected crtc 11\nDP-4 connected crtc 12\nDP-5 connected crtc 0\nDP-6 disconnected crtc 0\n"
            "crtc infos freed 0\n" + HANDLERS.format(wine=WINE_ALL), []),
}
TAIL = "RTLD_NEXT finds the preloaded dlsym 1, RTLD_DEFAULT strlen 1\n"


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        lib, fake, fake_x11, exe = (os.path.join(tmp, n) for n in ("altium_xrandr_filter.so", "libXrandr-fake.so",
                                                                   "libX11-fake.so", "test_xrandrfilter"))
        subprocess.run([CC, "-shared", "-fPIC", "-fvisibility=hidden", *FLAGS, "-o", lib,
                        os.path.join(ROOT, "src/xrandrfilter/xrandrfilter.c")], check=True)
        subprocess.run([CC, "-shared", "-fPIC", *FLAGS, "-o", fake, os.path.join(ROOT, "tests/xrandrfilter_fake.c")],
                       check=True)
        subprocess.run([CC, "-shared", "-fPIC", *FLAGS, "-o", fake_x11,
                        os.path.join(ROOT, "tests/xrandrfilter_fakex11.c")], check=True)
        subprocess.run([CC, *FLAGS, "-o", exe, os.path.join(ROOT, "tests/test_xrandrfilter.c"), fake_x11,
                        f"-Wl,-rpath,{tmp}"], check=True)
        for name, (env, want, want_log) in CASES.items():
            log = os.path.join(tmp, name + ".log")
            run_env = {k: v for k, v in os.environ.items() if not k.startswith("ALTIUM_XRANDR_FILTER")}
            run_env.update(env, LD_PRELOAD=lib, ALTIUM_XRANDR_FILTER_LOG=log)
            res = subprocess.run([exe, fake], env=run_env, capture_output=True, text=True, timeout=30)
            text = open(log).read() if os.path.exists(log) else ""
            problems = []
            if res.returncode:
                problems.append(f"exit code {res.returncode}")
            if res.stdout != want + TAIL:
                problems.append(f"stdout {res.stdout!r}, wanted {want + TAIL!r}")
            for pattern, count in want_log:
                found = len(re.findall(pattern, text))
                if found != count:
                    problems.append(f"log has {found} x {pattern!r}, wanted {count}")
            if not want_log and text:
                problems.append("log not empty")
            if problems:
                failures += 1
                print(f"FAIL {name}: " + "; ".join(problems))
                print("    " + text.replace("\n", "\n    ").rstrip())
            else:
                print(f"ok   {name}")
    print("all passed" if not failures else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
