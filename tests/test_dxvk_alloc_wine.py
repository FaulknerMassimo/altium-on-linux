#!/usr/bin/env python3
"""Check the SHFolder.dll proxy's lock on DXVK's allocator imports (how-it-works §22).

Builds the proxy and tests/dxvk_alloc_wine.c (a Windows program) and runs the program in a
throwaway prefix of the bottle's runner (inside the Bottles sandbox; the bottle and a running
Altium are not touched), with the bottle's own DXVK d3d11.dll and dxgi.dll next to it. The
program loads the proxy, then DXVK, checks that the proxy hooked DXVK's importImageResource and
importBufferResource, then recreates a swapchain over and over (DXVK's command thread imports
the new images each time) while two threads create and release buffers. With the proxy it must
run to the end; DXVK's log shows how many swapchains it made.

With --control, runs it once more without the proxy, which is expected to crash (the pool's
free list gets corrupted), though a race may take longer than the run to show up: the result
is reported, not required.

    tests/test_dxvk_alloc_wine.py [--bottle NAME] [--seconds N] [--control]
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
                          '"$RUNNER_DIR" "$BOTTLE_DIR"', "_", bottle], capture_output=True, text=True,
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
    libs = []
    for dll in ("kernel32", "user32"):
        subprocess.run(["llvm-dlltool", "-m", "i386:x86-64", "-d", f"{ROOT}/tests/dxvk_alloc_wine_{dll}.def",
                        "-l", f"{work}/{dll}.lib"], check=True)
        libs.append(f"{work}/{dll}.lib")
    subprocess.run(["clang", *PE_FLAGS, "-c", f"{ROOT}/tests/dxvk_alloc_wine.c", "-o", f"{work}/dxvk_alloc_wine.obj"],
                   check=True)
    subprocess.run(["lld-link", "/nologo", "/entry:start_entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
                    f"/out:{work}/dxvk_alloc_wine.exe", f"{work}/dxvk_alloc_wine.obj", *libs], check=True)
    subprocess.run([f"{ROOT}/scripts/build.sh"], check=True, capture_output=True, env={**os.environ, "OUT": f"{work}/build"})
    shutil.copy(f"{work}/build/SHFolder.dll", f"{work}/SHFolder.dll")


def run(runner, prefix, work, proxy, seconds):
    log_dir = f"{work}/logs-{'proxy' if proxy else 'control'}"
    shutil.rmtree(log_dir, ignore_errors=True)
    os.makedirs(log_dir)
    env = [f"DXVK_TEST_PROXY_WIN={winpath(work + '/SHFolder.dll') if proxy else ''}", f"DXVK_TEST_SECONDS={seconds}",
           f"DXVK_LOG_PATH={winpath(log_dir)}", "WINEDLLOVERRIDES=mscoree,mshtml=;d3d11,dxgi=n;shfolder=n"]
    start = time.time()
    res = sandbox(runner, prefix, env, f"{runner}/bin/wine", winpath(f"{work}/dxvk_alloc_wine.exe"),
                  timeout=seconds + 60)
    took = time.time() - start
    swapchains = 0
    for name in os.listdir(log_dir):
        swapchains += open(f"{log_dir}/{name}", errors="replace").read().count("Actual swapchain properties")
    return res, took, swapchains


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--control", action="store_true", help="also run without the proxy (expected to crash)")
    args = ap.parse_args()
    stage, runner, bottle_dir = bottle_paths(args.bottle)
    work = os.path.join(stage, "dxvk-test")
    prefix = os.path.join(work, "prefix")
    os.makedirs(work, exist_ok=True)
    build(work)
    for dll in ("d3d11.dll", "dxgi.dll"):
        shutil.copy(f"{bottle_dir}/drive_c/windows/system32/{dll}", f"{work}/{dll}")
    if not os.path.exists(f"{prefix}/system.reg"):
        print("making the test prefix (wineboot)...")
        res = sandbox(runner, prefix, [], f"{runner}/bin/wine", "wineboot", "-i", timeout=300)
        if res.returncode:
            sys.exit(f"wineboot failed ({res.returncode}): {res.stderr[-2000:]}")
    failures = 0
    for proxy in (True, False) if args.control else (True,):
        res, took, swapchains = run(runner, prefix, work, proxy, args.seconds)
        out = res.stdout.replace("\r", "")
        name = "with the proxy" if proxy else "without the proxy"
        print(f"{name} ({took:.0f} s, exit {res.returncode}, {swapchains} swapchains made):")
        print("    " + "\n    ".join(out.strip().split("\n")))
        passed = res.returncode == 0 and "result: PASS" in out
        if proxy:
            ok = passed and "locked by the proxy" in out and swapchains > 100
            print(("ok   " if ok else "FAIL ") + name)
            failures += not ok
        else:
            print("     " + ("the race did not show up in this run" if passed else "crashed or failed, as expected"))
    print("all passed" if not failures else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
