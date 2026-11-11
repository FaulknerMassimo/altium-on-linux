#!/usr/bin/env python3
"""Check the SHFolder.dll proxy's fix for llPDFLib's PolyDraw handlers (how-it-works §30).

Builds the proxy and tests/polydraw_wine.c (a Windows program) and runs the program in a
throwaway prefix of the bottle's runner (inside the Bottles sandbox; the bottle and a running
Altium are not touched). The program maps Altium's own WorkspaceManager.dll, ItemManager.dll and
Advpcb.dll from the bottle (without their imports or DllMain), finds the EMR_POLYDRAW16 and
EMR_POLYDRAW handlers in each, loads the proxy, and runs copies of every handler with its calls
into the rest of llPDFLib sent to stubs:

- as Altium ships it, a 2-point line, as Wine's GDI+ records one, never returns (the freeze)
- as the proxy leaves it, lines, closed polygons, circles and mixed figures, stroked and inside a
  path, give exactly the MoveTo, LineTo and CurveTo calls PolyDraw means

    tests/test_polydraw_wine.py [--bottle NAME]
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
MODULES = ("WorkspaceManager.dll", "ItemManager.dll", "Advpcb.dll")


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
    subprocess.run(["llvm-dlltool", "-m", "i386:x86-64", "-d", f"{ROOT}/tests/polydraw_wine_kernel32.def",
                    "-l", lib], check=True)
    subprocess.run(["clang", *PE_FLAGS, "-c", f"{ROOT}/tests/polydraw_wine.c", "-o", f"{work}/polydraw_wine.obj"],
                   check=True)
    subprocess.run(["lld-link", "/nologo", "/entry:start_entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
                    f"/out:{work}/polydraw_wine.exe", f"{work}/polydraw_wine.obj", lib], check=True)
    subprocess.run([f"{ROOT}/scripts/build.sh"], check=True, capture_output=True,
                   env={**os.environ, "OUT": f"{work}/build"})
    shutil.copy(f"{work}/build/SHFolder.dll", f"{work}/proxy.dll")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    args = ap.parse_args()
    stage, runner, altium = bottle_paths(args.bottle)
    work = os.path.join(stage, "polydraw-test")
    prefix = os.path.join(work, "prefix")
    os.makedirs(work, exist_ok=True)
    build(work)
    modules = [f"{altium}/System/{name}" for name in MODULES]
    missing = [m for m in modules if not os.path.exists(m)]
    if missing:
        sys.exit("not found: " + ", ".join(missing))
    if not os.path.exists(f"{prefix}/system.reg"):
        print("making the test prefix (wineboot)...")
        res = sandbox(runner, prefix, [], f"{runner}/bin/wine", "wineboot", "-i", timeout=300)
        if res.returncode:
            sys.exit(f"wineboot failed ({res.returncode}): {res.stderr[-2000:]}")

    env = [f"POLYDRAW_TEST_MODULES={';'.join(winpath(m) for m in modules)}",
           f"POLYDRAW_TEST_PROXY_WIN={winpath(work + '/proxy.dll')}"]
    start = time.time()
    res = sandbox(runner, prefix, env, f"{runner}/bin/wine", winpath(f"{work}/polydraw_wine.exe"), timeout=180)
    out = res.stdout.replace("\r", "").strip()
    print(out)
    print(f"({time.time() - start:.0f} s, exit {res.returncode})")
    if "result: PASS" not in out:
        print(res.stderr[-3000:])
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
