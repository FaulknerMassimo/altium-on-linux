#!/usr/bin/env python3
"""Check src/xrandrfilter/xrandrfilter.c (hides 0x0 outputs from Wine) on the host.

Builds the filter, a stand-in libXrandr (tests/xrandrfilter_fake.c) and
tests/test_xrandrfilter.c, which looks up XRRGetOutputInfo the way winex11.so does
(dlopen, then dlsym on the handle). With the filter preloaded, the output whose CRTC has
a 0x0 mode must read as disconnected, and be logged; the others, including one without a
CRTC, must not change, and every CRTC info the filter asks for must be freed. Lookups that
depend on the caller (RTLD_NEXT) must still see the program, not the filter.
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

CASES = {
    "filter": ({}, "DP-1 connected crtc 11\nDP-4 disconnected crtc 0\nDP-5 connected crtc 0\nDP-6 disconnected crtc 0\n"
                   "crtc infos freed 2\n",
               [(r"hid output DP-4 from test_xrandrfi.* its CRTC is 0x0 at 0,0", 1)]),
    "off": ({"ALTIUM_XRANDR_FILTER": "off"},
            "DP-1 connected crtc 11\nDP-4 connected crtc 12\nDP-5 connected crtc 0\nDP-6 disconnected crtc 0\n"
            "crtc infos freed 0\n", []),
}
TAIL = "RTLD_NEXT finds the preloaded dlsym 1, RTLD_DEFAULT strlen 1\n"


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        lib, fake, exe = (os.path.join(tmp, n) for n in ("altium_xrandr_filter.so", "libXrandr-fake.so", "test_xrandrfilter"))
        subprocess.run([CC, "-shared", "-fPIC", "-fvisibility=hidden", *FLAGS, "-o", lib,
                        os.path.join(ROOT, "src/xrandrfilter/xrandrfilter.c")], check=True)
        subprocess.run([CC, "-shared", "-fPIC", *FLAGS, "-o", fake, os.path.join(ROOT, "tests/xrandrfilter_fake.c")],
                       check=True)
        subprocess.run([CC, *FLAGS, "-o", exe, os.path.join(ROOT, "tests/test_xrandrfilter.c")], check=True)
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
