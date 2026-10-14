#!/usr/bin/env python3
"""Check the fd guard (src/fdguard/fdguard.c) in a real Wine process of the bottle.

Builds the guard, tests/fdguard_poke.c (a native library that closes every pipe in the
process when told to) and tests/fdguard_wine.c (a Windows program), and runs the program
in the bottle's running Wine session with both libraries preloaded. The program starts
and ends 320 threads, and four that live past the guard's 5 s grace for new pipes (their
exits are checked against their TEB). Then the poke closes every pipe while a worker
thread keeps talking to wineserver; the worker, the main thread and new threads must
still get through, and the guard's log must show no complaint about Wine's own closes.
FRESH_MS=0 in the environment checks every close of Wine's, even those right after a
pipe is made, which the guard normally lets through.

With --control, runs it once more without the guard, which must break (the program fails
or hangs and is killed after 60 s): shows that the poke does what a stray close does.

Then the alert test, in the session's own sync mode: the poke library puts a /dev/null
canary on every free fd number, and 200 threads each make one alertable wait and end.
Under fsync each such exit closes the fd numbered by the thread's fsync index (Wine's
alert_fd bug); with the guard no canary may be closed, and the guard must log at least
200 skipped closes (more when the 20 threads still waiting at exit get to theirs). With --control, the same run without the guard must lose canaries (under
fsync; other sync modes do not have the bug).

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


def session_sync(bottle):
    """The running session's WINEFSYNC/WINEESYNC/WINENTSYNC settings, from wineserver."""
    out = bash('load_bottle "$1"; tr "\\0" "\\n" < /proc/$(wineserver_pid)/environ', bottle,
               capture_output=True, text=True).stdout
    return [line for line in out.split("\n") if line.split("=")[0] in ("WINEFSYNC", "WINEESYNC", "WINENTSYNC")]


def run(bottle, runner, work, guard, timeout, extra_env=()):
    for name in ("trigger", "done", "result", "guard.log", "ready", "check"):
        if os.path.exists(f"{work}/{name}"):
            os.remove(f"{work}/{name}")
    preload = (f"{work}/altium_fd_guard.so:" if guard else "") + f"{work}/fdguard_poke.so"
    env = [f"LD_PRELOAD={preload}", f"ALTIUM_FD_GUARD_LOG={work}/guard.log", "ALTIUM_FD_GUARD_FRESH_MS=" + os.environ.get("FRESH_MS", "5000"),
           f"FDPOKE_TRIGGER={work}/trigger", f"FDPOKE_DONE={work}/done",
           f"FDPOKE_TRIGGER_WIN={winpath(work + '/trigger')}", f"FDPOKE_DONE_WIN={winpath(work + '/done')}",
           f"FDPOKE_RESULT_WIN={winpath(work + '/result')}",
           f"FDPOKE_READY={work}/ready", f"FDPOKE_CHECK={work}/check",
           f"FDPOKE_READY_WIN={winpath(work + '/ready')}", f"FDPOKE_CHECK_WIN={winpath(work + '/check')}",
           "WINEDEBUG=-all", *extra_env]
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

        sync = session_sync(args.bottle)
        fsync = "WINEFSYNC=1" in sync
        alert_env = ["FDPOKE_MODE=alert", *sync]
        for guard in (True, False) if args.control else (True,):
            start = time.time()
            code, out, done, log = run(args.bottle, runner, work, guard, 120, alert_env)
            name = "alert test " + ("with the guard" if guard else "without the guard")
            print(f"{name} ({' '.join(sync) or 'no fsync/esync/ntsync'}, {time.time() - start:.1f} s, exit {code}):")
            print("    " + (out.strip() or "(no output)").replace("\n", "\n    ") + "\n    " + done)
            closed = int(done.split()[2]) if done.startswith("canaries:") else -1
            skipped = log.count("thread-exit close of an fsync index")
            if guard:
                print(f"    guard log: {skipped} skipped closes of an fsync index")
                if code != 0 or closed != 0 or (fsync and skipped < 200):
                    failures += 1
                    print(f"FAIL {name}")
                    if log:
                        print("    " + "\n    ".join(log.strip().split("\n")[:20]))
                else:
                    print(f"ok   {name}")
            elif not fsync:
                print(f"skip {name}: the session does not use fsync, which has the bug")
            elif closed > 0:
                print(f"ok   {name}: thread exits closed {closed} unrelated fds")
            else:
                failures += 1
                print(f"FAIL {name}: no canary was closed, so the test proves nothing")
        print("all passed" if not failures else f"{failures} failed")
        return 1 if failures else 0
    finally:
        if not os.environ.get("KEEP"):
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
