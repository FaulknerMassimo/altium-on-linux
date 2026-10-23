#!/usr/bin/env python3
"""Check the image the SHFolder.dll proxy hands to Wine for layered windows (how-it-works §13).

Builds the proxy and tests/layered_wine.c (a Windows program) and runs the program in a
throwaway prefix of the bottle's runner (inside the Bottles sandbox; the bottle and a
running Altium are not touched). The program puts itself between user32 and win32u before
loading the proxy, then calls UpdateLayeredWindow with per-pixel alpha from top-down and
bottom-up DIB sections, whole and in part, with and without pixels of alpha 0, and checks
what reaches Wine: the same image the right way up, with alpha 0 replaced by alpha 1, or the
caller's own bitmap when there is nothing to replace. No window is created.

--proxy PATH tests that SHFolder.dll instead of a fresh build (the installed one, or an older
build to see a test fail). The prefix (<bottle>/altium-on-linux/layered-test) is made on the
first run (wineboot, about a minute) and kept.

    tests/test_layered_wine.py [--bottle NAME] [--proxy PATH]
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "scripts/lib.sh")
APP = "com.usebottles.bottles"
PE_FLAGS = ["--target=x86_64-pc-windows-msvc", "-O2", "-ffreestanding", "-fno-builtin", "-mno-stack-arg-probe",
            "-fno-stack-protector", "-Wall", "-Wextra", "-Werror"]


def bottle_paths(bottle):
    out = subprocess.run(["bash", "-c", f'source {LIB}; load_bottle "$1"; printf "%s\\n%s\\n" "$STAGE" "$RUNNER_DIR"',
                          "_", bottle], capture_output=True, text=True, check=True).stdout.split("\n")
    return out[0], out[1]


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
    for dll in ("kernel32", "user32", "gdi32"):
        subprocess.run(["llvm-dlltool", "-m", "i386:x86-64", "-d", f"{ROOT}/tests/layered_wine_{dll}.def",
                        "-l", f"{work}/{dll}.lib"], check=True)
        libs.append(f"{work}/{dll}.lib")
    subprocess.run(["clang", *PE_FLAGS, "-c", f"{ROOT}/tests/layered_wine.c", "-o", f"{work}/layered_wine.obj"],
                   check=True)
    subprocess.run(["lld-link", "/nologo", "/entry:start_entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
                    f"/out:{work}/layered_wine.exe", f"{work}/layered_wine.obj", *libs], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    ap.add_argument("--proxy", help="SHFolder.dll to test instead of a fresh build")
    args = ap.parse_args()
    stage, runner = bottle_paths(args.bottle)
    work = os.path.join(stage, "layered-test")
    prefix = os.path.join(work, "prefix")
    os.makedirs(work, exist_ok=True)
    build(work)
    if args.proxy:
        shutil.copy(args.proxy, f"{work}/SHFolder.dll")
    else:
        subprocess.run([f"{ROOT}/scripts/build.sh"], check=True, capture_output=True,
                       env={**os.environ, "OUT": f"{work}/build"})
        shutil.copy(f"{work}/build/SHFolder.dll", f"{work}/SHFolder.dll")
    if not os.path.exists(f"{prefix}/system.reg"):
        print("making the test prefix (wineboot)...")
        res = sandbox(runner, prefix, [], f"{runner}/bin/wine", "wineboot", "-i", timeout=300)
        if res.returncode:
            sys.exit(f"wineboot failed ({res.returncode}): {res.stderr[-2000:]}")
    # Wine would load its builtin shfolder.dll for a path that names a file of the same name.
    env = [f"LAYERED_PROXY_WIN={winpath(work + '/SHFolder.dll')}", "WINEDLLOVERRIDES=mscoree,mshtml=;shfolder=n"]
    res = sandbox(runner, prefix, env, f"{runner}/bin/wine", winpath(f"{work}/layered_wine.exe"), timeout=60)
    print(res.stdout.replace("\r", ""), end="")
    if res.returncode and "all passed" not in res.stdout:
        print(f"exit {res.returncode}" + (": " + res.stderr[-1000:] if res.returncode not in (0, 1) else ""))
    return 0 if res.returncode == 0 and "all passed" in res.stdout else 1


if __name__ == "__main__":
    sys.exit(main())
