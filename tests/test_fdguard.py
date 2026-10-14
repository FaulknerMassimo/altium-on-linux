#!/usr/bin/env python3
"""Check src/fdguard/fdguard.c (keeps other code from closing Wine's server pipes) on the host.

Builds the guard, a stand-in for Wine's ntdll.so (tests/fdguard_ntdll.c, built under that
name) and tests/test_fdguard.c, then runs each scenario with the guard preloaded. A stray
close (by code outside ntdll.so) of a pipe ntdll.so made must be refused and logged with a
backtrace; dup2/dup3 onto one must fail with EBUSY; close_range and closefrom must skip
them, and so must the first pipe ntdll.so receives over a socket (the first thread's
request pipe, sent by wineserver) but no later one. ntdll.so's own closes must go through: silently when the pipe is new or the thread's
own (its TEB holds it), logged when it is another thread's or the TEB is borrowed. A fd
whose pipe went away unseen, a forked child, and ALTIUM_FD_GUARD=log/off are left alone.

    tests/test_fdguard.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "clang")
FLAGS = ["-O2", "-Wall", "-Wextra", "-Werror", "-g"]

# scenario: (extra environment, expected stdout, log lines expected (regular expression, count))
CASES = {
    "stray": ({}, "stray: close 0, open 1, pipe 1\n", [("refused stray close of fd", 1), (r"\(close\)\n.*#\d+ test_fdguard\+", 1)]),
    "stray/log": ({"ALTIUM_FD_GUARD": "log"}, "stray: close 0, open 0, pipe 0\n", [("let through stray close of", 1)]),
    "stray/off": ({"ALTIUM_FD_GUARD": "off"}, "stray: close 0, open 0, pipe 0\n", []),
    "fresh": ({}, "fresh: close 0, open 0\n", []),
    "own-exit": ({"ALTIUM_FD_GUARD_FRESH_MS": "0"}, "own-exit: close 0 0, open 0 0\n", []),
    "other-thread": ({"ALTIUM_FD_GUARD_FRESH_MS": "0"}, "other-thread: close 0, open 0\n",
                     [("ntdll.so, not its own thread's pipe: close of", 1), (r"\(Wine 0333\)", 1)]),
    "borrowed-teb": ({"ALTIUM_FD_GUARD_FRESH_MS": "0"}, "borrowed-teb: close 0, open 0\n",
                     [("ntdll.so, not its own thread's pipe: close of", 1), (r"\(Wine 0222\)", 1)]),
    "stale": ({}, "stale: same number 1, close 0, open 0\n", []),
    "dup": ({}, "dup: dup2 -1 EBUSY, dup3 -1 EBUSY, pipes 1 1\ndup: dup2 onto a plain fd 1\n",
            [("refused stray dup2 onto", 1), ("refused stray dup3 onto", 1)]),
    "close-range": ({}, "close-range: 0, pipes 1 1 1 1, plain 0 0 0\nclosefrom: pipes 1 1 1 1, plain 0\n",
                    [("refused stray close_range of", 8)]),
    "received": ({}, "received: first close 0 open 1, later close 0 open 0\n", [("refused stray close of fd", 1)]),
    "fork": ({}, "fork: child closed 1, parent open 1\n", []),
}


def build(tmp):
    guard = os.path.join(tmp, "altium_fd_guard.so")
    lib = os.path.join(tmp, "ntdll.so")
    exe = os.path.join(tmp, "test_fdguard")
    subprocess.run([CC, "-shared", "-fPIC", "-fvisibility=hidden", *FLAGS, "-o", guard,
                    os.path.join(ROOT, "src/fdguard/fdguard.c")], check=True)
    # Calls, not tail jumps, as in Wine's ntdll.so: the guard goes by the return address.
    subprocess.run([CC, "-shared", "-fPIC", "-fno-optimize-sibling-calls", *FLAGS, "-o", lib,
                    os.path.join(ROOT, "tests/fdguard_ntdll.c")], check=True)
    subprocess.run([CC, *FLAGS, "-pthread", "-o", exe, os.path.join(ROOT, "tests/test_fdguard.c"), lib,
                    "-Wl,-rpath," + tmp], check=True)
    return guard, exe


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        guard, exe = build(tmp)
        for name, (env, want, want_log) in CASES.items():
            log = os.path.join(tmp, name.replace("/", "-") + ".log")
            run_env = {k: v for k, v in os.environ.items() if not k.startswith("ALTIUM_FD_GUARD")}
            run_env.update(env, LD_PRELOAD=guard, ALTIUM_FD_GUARD_LOG=log)
            res = subprocess.run([exe, name.split("/")[0]], env=run_env, capture_output=True, text=True, timeout=30)
            text = open(log).read() if os.path.exists(log) else ""
            problems = []
            if res.returncode:
                problems.append(f"exit code {res.returncode}")
            if res.stdout != want:
                problems.append(f"stdout {res.stdout!r}, wanted {want!r}")
            for pattern, count in want_log:
                found = len(re.findall(pattern, text, re.MULTILINE))
                if found != count:
                    problems.append(f"log has {found} x {pattern!r}, wanted {count}")
            if not want_log and text:
                problems.append("log not empty")
            if text and res.stderr.count("fd-guard") != text.count("fd-guard"):
                problems.append("stderr and log file differ")
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
