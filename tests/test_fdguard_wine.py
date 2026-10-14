#!/usr/bin/env python3
"""Check the fd guard (src/fdguard/fdguard.c) in a real Wine process of the bottle.

Builds the guard, tests/fdguard_poke.c (a native library that closes every pipe in the
process when told to) and tests/fdguard_wine.c (a Windows program), and runs the program
in the bottle's running Wine session with both libraries preloaded. The program starts
and ends 320 threads, then the poke closes every pipe while a worker thread keeps talking
to wineserver; the worker, the main thread and new threads must still get through, and
the guard's log must show no complaint about Wine's own closes (thread exits).

With --control, runs it once more without the guard, which must break (the program fails
or hangs and is killed after 60 s): shows that the poke does what a stray close does.

Needs a running session of the bottle (Altium, or any program started in it), so the
test talks to the same wineserver and touches nothing else.

    tests/test_fdguard_wine.py [--bottle NAME] [--control]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "scripts/lib.sh")
SO_FLAGS = ["-shared", "-fPIC", "-O2", "-fvisibility=hidden", "-fno-stack-protector", "-U_FORTIFY_SOURCE",
            "-Wall", "-Wextra", "-Werror"]
PE_FLAGS = ["--target=x86_64-pc-windows-msvc", "-O2", "-ffreestanding", "-fno-builtin", "-mno-stack-arg-probe",
            "-fno-stack-protector", "-Wall", "-Wextra", "-Werror"]


def bash(script, *args, **kw):
    return subprocess.run(["bash", "-c", f"source {LIB}; " + script, "_", *args], **kw)


def bottle_paths(bottle):
    out = bash('load_bottle "$1"; printf "%s\\n%s\\n" "$STAGE" "$RUNNER_DIR"', bottle,
               capture_output=True, text=True, check=True).stdout.split("\n")
    return out[0], out[1]


def winpath(path):
    return "Z:" + path.replace("/", "\\")


def build(work):
    subprocess.run(["clang", *SO_FLAGS, "-o", f"{work}/altium_fd_guard.so", f"{ROOT}/src/fdguard/fdguard.c"], check=True)
    subprocess.run(["clang", "-shared", "-fPIC", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-o", f"{work}/fdguard_poke.so", f"{ROOT}/tests/fdguard_poke.c"], check=True)
    subprocess.run(["llvm-dlltool", "-m", "i386:x86-64", "-d", f"{ROOT}/tests/fdguard_wine_kernel32.def",
                    "-l", f"{work}/kernel32.lib"], check=True)
    subprocess.run(["clang", *PE_FLAGS, "-c", f"{ROOT}/tests/fdguard_wine.c", "-o", f"{work}/fdguard_wine.obj"], check=True)
    subprocess.run(["lld-link", "/nologo", "/entry:start_entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
                    f"/out:{work}/fdguard_wine.exe", f"{work}/fdguard_wine.obj", f"{work}/kernel32.lib"], check=True)


def run(bottle, runner, work, guard, timeout):
    for name in ("trigger", "done", "result", "guard.log"):
        if os.path.exists(f"{work}/{name}"):
            os.remove(f"{work}/{name}")
    preload = (f"{work}/altium_fd_guard.so:" if guard else "") + f"{work}/fdguard_poke.so"
    env = [f"LD_PRELOAD={preload}", f"ALTIUM_FD_GUARD_LOG={work}/guard.log",
           f"FDPOKE_TRIGGER={work}/trigger", f"FDPOKE_DONE={work}/done",
           f"FDPOKE_TRIGGER_WIN={winpath(work + '/trigger')}", f"FDPOKE_DONE_WIN={winpath(work + '/done')}",
           f"FDPOKE_RESULT_WIN={winpath(work + '/result')}",
           "WINEDEBUG=-all"]
    proc = subprocess.Popen(["bash", "-c", f'source {LIB}; load_bottle "$1"; shift; in_running_bottle "$@"', "_",
                             bottle, *env, f"{runner}/bin/wine", winpath(f"{work}/fdguard_wine.exe")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = None
    # Without the guard, the program's main thread can die while the others hang: the
    # launcher returns, the process stays. Kill the test program only; Wine cleans up.
    subprocess.run(["pkill", "-KILL", "-f", r"fdguard_wine\.exe"])
    proc.wait(timeout=30)
    out = open(f"{work}/result", errors="replace").read() if os.path.exists(f"{work}/result") else ""
    done = open(f"{work}/done").read().strip() if os.path.exists(f"{work}/done") else "(no poke)"
    log = open(f"{work}/guard.log").read() if os.path.exists(f"{work}/guard.log") else ""
    return code, out, done, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bottle", default="Altium")
    ap.add_argument("--control", action="store_true", help="also run without the guard (must break)")
    args = ap.parse_args()
    if bash('load_bottle "$1"; [ -n "$(wineserver_pid)" ]', args.bottle).returncode:
        sys.exit(f"the bottle {args.bottle} is not running: start Altium (or any program in it) first")
    stage, runner = bottle_paths(args.bottle)
    work = os.path.join(stage, "fdguard-test")
    os.makedirs(work, exist_ok=True)
    try:
        build(work)
        failures = 0
        start = time.time()
        code, out, done, log = run(args.bottle, runner, work, True, 120)
        print(f"with the guard ({time.time() - start:.1f} s, exit {code}, poke: {done}):")
        print("    " + out.strip().replace("\n", "\n    "))
        refused = log.count("refused stray")
        own = log.count("ntdll.so, not its own")
        print(f"    guard log: {refused} refused, {own} about Wine's own closes")
        if code != 0 or "result: PASS" not in out or not done.startswith("closed") or not refused or own:
            failures += 1
            print("FAIL with the guard")
            if log:
                print("    " + "\n    ".join(log.strip().split("\n")[:40]))
        else:
            print("ok   with the guard")
        if args.control:
            start = time.time()
            code, out, done, log = run(args.bottle, runner, work, False, 60)
            print(f"without the guard ({time.time() - start:.1f} s, exit {code}, poke: {done}):")
            print("    " + (out.strip() or "(no output)").replace("\n", "\n    "))
            if code == 0 and "result: PASS" in out:
                failures += 1
                print("FAIL without the guard: the poke did not break anything, so the test proves nothing")
            else:
                print("ok   without the guard it breaks")
        print("all passed" if not failures else f"{failures} failed")
        return 1 if failures else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
