#!/usr/bin/env python3
"""Check src/zerosize/zerosize.c (keeps 0x0 windows unmapped) on the host.

Builds the filter as scripts/build.sh does (no libc headers or libraries), a stand-in
libX11 (tests/zerosize_fakex11.c) and tests/test_zerosize.c, which calls it the way
winex11.so does. With the filter preloaded, a managed top-level 1x1 window must not be
mapped until it is 2x2 or larger, by XMoveResizeWindow or by XConfigureWindow changing one
dimension at a time; an override-redirect window, a child window and a larger window must
be mapped as usual; a held window's withdraw must be swallowed and its destroy must go
through. Each hold and late map must be logged. ALTIUM_ZERO_SIZE=off must leave every call
alone. Runs for x86_64, and for i686 when 32-bit programs can be linked here.

    tests/test_zerosize.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "clang")
FLAGS = ["-O2", "-Wall", "-Wextra", "-Werror"]

FILTERED = """map 1x1 managed
map 1x1 override-redirect
  X: map 2
map 400x300
  X: map 3
map 1x1 child
  X: map 4
map again while 1x1
resize to 640x480
  X: move-resize 1 640x480
  X: map 1
unmap after the map
  X: unmap 1
map 1x1 managed, withdraw, destroy
  X: destroy 5
map 1x1 managed, widen, then heighten
  X: configure 6
  X: configure 6
  X: map 6
"""
UNFILTERED = """map 1x1 managed
  X: map 1
map 1x1 override-redirect
  X: map 2
map 400x300
  X: map 3
map 1x1 child
  X: map 4
map again while 1x1
  X: map 1
resize to 640x480
  X: move-resize 1 640x480
unmap after the map
  X: unmap 1
map 1x1 managed, withdraw, destroy
  X: map 5
  X: withdraw 5
  X: destroy 5
map 1x1 managed, widen, then heighten
  X: map 6
  X: configure 6
  X: configure 6
"""
CASES = {
    "filter": ({}, FILTERED, [r"kept unmapped window 0x1 \(1x1\) in test_zerosize", r"mapped, now window 0x1 \(640x480\)",
                              r"kept unmapped window 0x5 \(1x1\)", r"kept unmapped window 0x6 \(1x1\)",
                              r"mapped, now window 0x6 \(50x40\)"]),
    "off": ({"ALTIUM_ZERO_SIZE": "off"}, UNFILTERED, []),
}


def build(tmp, arch):
    m = "-m64" if arch == "x86_64" else "-m32"
    lib, fake, exe = (os.path.join(tmp, f"{n}-{arch}") for n in ("altium_zero_size.so", "libX11-fake.so", "test_zerosize"))
    subprocess.run([CC, m, "-shared", "-fPIC", "-nostdlib", "-fvisibility=hidden", "-fno-stack-protector", *FLAGS,
                    "-o", lib, os.path.join(ROOT, "src/zerosize/zerosize.c")], check=True)
    try:
        subprocess.run([CC, m, "-shared", "-fPIC", *FLAGS, "-o", fake, os.path.join(ROOT, "tests/zerosize_fakex11.c")],
                       check=True, capture_output=True)
        subprocess.run([CC, m, *FLAGS, "-o", exe, os.path.join(ROOT, "tests/test_zerosize.c"), fake,
                        f"-Wl,-rpath,{tmp}"], check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        if arch == "x86_64":
            raise
        print(f"skip {arch}: cannot link 32-bit programs here ({e.stderr.decode().strip().splitlines()[-1]})")
        return None
    return lib, exe


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for arch in ("x86_64", "i686"):
            built = build(tmp, arch)
            if not built:
                continue
            lib, exe = built
            for name, (env, want, want_log) in CASES.items():
                log = os.path.join(tmp, f"{name}-{arch}.log")
                run_env = {k: v for k, v in os.environ.items() if not k.startswith("ALTIUM_ZERO_SIZE")}
                run_env.update(env, LD_PRELOAD=lib, ALTIUM_ZERO_SIZE_LOG=log)
                res = subprocess.run([exe], env=run_env, capture_output=True, text=True, timeout=30)
                text = open(log).read() if os.path.exists(log) else ""
                problems = []
                if res.returncode:
                    problems.append(f"exit code {res.returncode}")
                if res.stdout != want:
                    problems.append(f"stdout {res.stdout!r}, wanted {want!r}")
                for pattern in want_log:
                    if len(re.findall(pattern, text)) != 1:
                        problems.append(f"log lacks {pattern!r}")
                if len(text.splitlines()) != len(want_log):
                    problems.append(f"log has {len(text.splitlines())} lines, wanted {len(want_log)}")
                if problems:
                    failures += 1
                    print(f"FAIL {arch} {name}: " + "; ".join(problems))
                    print("    " + text.replace("\n", "\n    ").rstrip())
                else:
                    print(f"ok   {arch} {name}")
    print("all passed" if not failures else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
