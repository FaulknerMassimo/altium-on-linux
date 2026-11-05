#!/usr/bin/env python3
"""Check the SHFolder.dll proxy's fix for the start-up deadlock with Altium's ucrtbase.dll (how-it-works §29).

Builds the proxy and tests/ucrt_thread_wine.c (a Windows program) and runs the program in a
throwaway prefix of the bottle's runner (inside the Bottles sandbox; the bottle and a running
Altium are not touched), with Altium's own ucrtbase.dll, copied from the bottle, next to it.
With the CRT's locale lock held (twice, as .NET's host held it when Altium hung), a thread
starts and uses the CRT, then the main thread calls LoadLibrary; then a thread with CRT data
ends, and the main thread calls LoadLibrary again. Both must return:

- with the proxy loaded before ucrtbase.dll (its load notification turns the thread calls off)
- with the proxy loaded after it (its DllMain does)

Without the proxy (the control, always run) the first LoadLibrary never returns: the new
thread holds Wine's loader lock in ucrtbase's DLL_THREAD_ATTACH and waits for the locale lock.
That run is ended after --timeout seconds.

    tests/test_ucrt_thread_wine.py [--bottle NAME] [--timeout S]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "scripts/lib.sh")
APP = "com.usebottles.bottles"
PE_FLAGS = ["--target=x86_64-pc-windows-msvc", "-O2", "-ffreestanding", "-fno-builtin", "-mno-stack-arg-probe",
            "-fno-stack-protector", "-Wall", "-Wextra", "-Werror"]


def bottle_paths(bottle):
    out = subprocess.run(["bash", "-c", f'source {LIB}; load_bottle "$1"; printf "%s\\n%s\\n%s\\n" "$STAGE" '
                          '"$RUNNER_DIR" "$AD"', "_", bottle], capture_output=True, text=True,
                         check=True).stdout.split("\n")
    return out[0], out[1], out[2]


def winpath(path):
    return "Z:" + path.replace("/", "\\")


def sandbox(runner, prefix, env, *cmd, timeout):
    """Run cmd in a new Bottles sandbox with the test prefix; then end its wineserver."""
    script = ('export WINEPREFIX="$1" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" "${@:3}"; '
              'timeout -s KILL "$2" "${CMD[@]}"; code=$?; "$RUNNER/bin/wineserver" -k 2>/dev/null; exit $code')
    full = ["flatpak", "run", "--command=bash", APP, "-c",
            f'RUNNER="{runner}"; CMD=({" ".join(repr(c) for c in cmd)}); ' + script,
            "_", prefix, str(timeout), *env]
    return subprocess.run(full, capture_output=True, text=True, timeout=timeout + 60)


def build(work):
    lib = f"{work}/kernel32.lib"
    subprocess.run(["llvm-dlltool", "-m", "i386:x86-64", "-d", f"{ROOT}/tests/ucrt_thread_wine_kernel32.def",
                    "-l", lib], check=True)
    subprocess.run(["clang", *PE_FLAGS, "-c", f"{ROOT}/tests/ucrt_thread_wine.c", "-o", f"{work}/ucrt_thread_wine.obj"],
                   check=True)
    subprocess.run(["lld-link", "/nologo", "/entry:start_entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
                    f"/out:{work}/ucrt_thread_wine.exe", f"{work}/ucrt_thread_wine.obj", lib], check=True)
    subprocess.run([f"{ROOT}/scripts/build.sh"], check=True, capture_output=True,
                   env={**os.environ, "OUT": f"{work}/build"})
    shutil.copy(f"{work}/build/SHFolder.dll", f"{work}/proxy.dll")


def run(runner, prefix, work, order, timeout):
    env = [f"UCRT_TEST_ORDER={order}", f"UCRT_TEST_PROXY_WIN={winpath(work + '/proxy.dll')}",
           "WINEDLLOVERRIDES=mscoree,mshtml=;ucrtbase=n,b"]
    start = time.time()
    res = sandbox(runner, prefix, env, f"{runner}/bin/wine", winpath(f"{work}/ucrt_thread_wine.exe"), timeout=timeout)
    took = time.time() - start
    out = res.stdout.replace("\r", "").strip()
    print(f"{order} ({took:.0f} s, exit {res.returncode}):")
    if out:
        print("    " + "\n    ".join(out.split("\n")))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    ap.add_argument("--timeout", type=int, default=20)
    args = ap.parse_args()
    stage, runner, altium = bottle_paths(args.bottle)
    work = os.path.join(stage, "ucrt-test")
    prefix = os.path.join(work, "prefix")
    os.makedirs(work, exist_ok=True)
    build(work)
    shutil.copy(f"{altium}/ucrtbase.dll", f"{work}/ucrtbase.dll")
    if not os.path.exists(f"{prefix}/system.reg"):
        print("making the test prefix (wineboot)...")
        res = sandbox(runner, prefix, [], f"{runner}/bin/wine", "wineboot", "-i", timeout=300)
        if res.returncode:
            sys.exit(f"wineboot failed ({res.returncode}): {res.stderr[-2000:]}")
    failures = 0

    out = run(runner, prefix, work, "none", args.timeout)
    ok = "Altium's copy" in out and "calling LoadLibrary" in out and "attach: LoadLibrary returned" not in out
    print(("ok   " if ok else "FAIL ") + "without the proxy, a thread start deadlocks the loader")
    failures += not ok

    for order in ("proxy-first", "ucrt-first"):
        out = run(runner, prefix, work, order, args.timeout)
        ok = "result: PASS" in out
        print(("ok   " if ok else "FAIL ") + f"with the proxy ({order}), threads start and end freely")
        failures += not ok

    print("all passed" if not failures else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
