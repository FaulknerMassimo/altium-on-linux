#!/usr/bin/env python3
"""Sampling profiler for Altium's UI thread under Wine: where does the time go?

    scripts/profile.py [--bottle NAME] [--seconds 20] [--interval 2] [--tid TID]
                       [--focus MODULE] [--all] [--save FILE] [--load FILE] [-- COMMAND...]

Every --interval ms the thread is stopped for a moment with ptrace (Wine processes accept
any tracer) and its instruction pointer, the system call it is in and the stack words that
look like return addresses are recorded. The report counts samples by module and by
symbol, at the top of the stack ("self") and anywhere on it ("inclusive"), and by system
call. Only samples taken while the thread was running count, unless --all is given.

With a COMMAND after --, it samples while the command runs, e.g. during a redraw benchmark:
    scripts/profile.py -- scripts/redraw-bench.sh --runs 1

Symbols are the nearest exported symbol before the address (pefile for Windows DLLs,
nm -D for Unix libraries): code in an unexported function is listed under a neighbouring
export, so treat names as hints. The stack scan is heuristic and can count stale frames.
Wine's Unix side runs on its own stack: a sample taken there shows Unix frames (win32u.so,
ntdll.so, libc) but not the Windows frames that called them. .NET code appears as [anon].
"""
import argparse
import bisect
import collections
import ctypes
import ctypes.util
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

try:
    import pefile
except ImportError:
    pefile = None

DATA = Path.home() / ".var/app/com.usebottles.bottles/data/bottles"
PTRACE_GETREGS, PTRACE_DETACH, PTRACE_SEIZE, PTRACE_INTERRUPT = 12, 17, 0x4206, 0x4207
WALL = 0x40000000
STACK_BYTES = 256 * 1024
MAX_FRAMES = 64
NEAR_EXPORT = 0x2000  # farther than this from the previous export: do not borrow its name
SYSCALLS = {
    0: "read", 1: "write", 3: "close", 7: "poll", 9: "mmap", 10: "mprotect", 11: "munmap",
    13: "rt_sigaction", 14: "rt_sigprocmask", 15: "rt_sigreturn", 16: "ioctl", 17: "pread64",
    18: "pwrite64", 23: "select", 24: "sched_yield", 28: "madvise", 39: "getpid", 44: "sendto",
    45: "recvfrom", 46: "sendmsg", 47: "recvmsg", 72: "fcntl", 186: "gettid", 200: "tkill",
    202: "futex", 228: "clock_gettime", 232: "epoll_wait", 234: "tgkill", 257: "openat",
    262: "newfstatat", 270: "pselect6", 271: "ppoll", 281: "epoll_pwait", 435: "clone3",
    449: "futex_waitv",
}


class Regs(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in (
        "r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs eflags "
        "rsp ss fs_base gs_base ds es fs gs").split()]


def altium_pid(bottle_dir):
    """PID of X2.EXE running in the bottle (its WINEPREFIX is the bottle)."""
    want = f"WINEPREFIX={bottle_dir}".encode()
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if want not in (proc / "environ").read_bytes().split(b"\0"):
                continue
            argv0 = (proc / "cmdline").read_bytes().split(b"\0")[0]
        except OSError:
            continue
        if argv0.lower().endswith(b"\\x2.exe"):
            return int(proc.name)
    return None


def read_maps(pid):
    maps = []
    for line in open(f"/proc/{pid}/maps"):
        parts = line.split(None, 5)
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        maps.append((lo, hi, parts[1], int(parts[2], 16), parts[5].strip() if len(parts) > 5 else ""))
    return sorted(maps)


class Ranges:
    """Address lookup in a sorted list of (lo, hi) ranges."""

    def __init__(self, ranges):
        self.ranges = ranges
        self.starts = [lo for lo, _ in ranges]

    def find(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.ranges[i] if i >= 0 and addr < self.ranges[i][1] else None


def sample(pid, tid, interval, keep_going):
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    libc.ptrace.restype = ctypes.c_long
    libc.ptrace.argtypes = [ctypes.c_long, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    regs, status = Regs(), ctypes.c_int()
    samples, maps, refreshed = [], [], 0.0

    def read(addr, size):
        r = readable.find(addr)
        if not r:
            return b""
        try:
            return os.pread(mem, min(size, r[1] - addr), addr)
        except OSError:
            return b""

    def after_call(addr):
        """Does a call instruction end at addr (so addr is a return address)?"""
        b = read(addr - 7, 7)
        if len(b) < 7:
            return False
        if b[2] == 0xE8 or (b[1] == 0xFF and b[2] == 0x15):  # call rel32, call [rip+disp32]
            return True
        for back in (2, 3, 6, 7):  # call r/m64 (ff /2), with or without REX
            i = 7 - back
            if b[i] == 0xFF and (b[i + 1] >> 3) & 7 == 2:
                return True
            if i + 2 < 7 and b[i] & 0xF0 == 0x40 and b[i + 1] == 0xFF and (b[i + 2] >> 3) & 7 == 2:
                return True
        return False

    while keep_going():
        if time.monotonic() - refreshed > 1.0:  # DLLs keep loading; refresh the maps
            maps = read_maps(pid)
            executable = Ranges([(lo, hi) for lo, hi, perm, _, _ in maps if "x" in perm])
            readable = Ranges([(lo, hi) for lo, hi, perm, _, _ in maps if "r" in perm])
            refreshed = time.monotonic()
        try:
            state = open(f"/proc/{pid}/task/{tid}/stat").read().rsplit(")", 1)[1].split()[0]
        except OSError:
            break  # thread gone
        if libc.ptrace(PTRACE_SEIZE, tid, None, None) != 0:
            sys.exit(f"ptrace failed: {os.strerror(ctypes.get_errno())} (is a debugger attached?)")
        libc.ptrace(PTRACE_INTERRUPT, tid, None, None)
        libc.waitpid(tid, ctypes.byref(status), WALL)
        ok = libc.ptrace(PTRACE_GETREGS, tid, None, ctypes.byref(regs)) == 0
        stack = read(regs.rsp, STACK_BYTES) if ok else b""
        libc.ptrace(PTRACE_DETACH, tid, None, None)
        if ok:
            frames = []
            for (word,) in struct.iter_unpack("<Q", stack[: len(stack) // 8 * 8]):
                if word > 0x10000 and executable.find(word) and after_call(word):
                    frames.append(word)
                    if len(frames) >= MAX_FRAMES:
                        break
            syscall = regs.orig_rax if regs.orig_rax < 1 << 63 else -1
            samples.append([regs.rip, frames, state, syscall])
        time.sleep(interval)
    os.close(mem)
    return {"pid": pid, "tid": tid, "maps": read_maps(pid) if os.path.exists(f"/proc/{pid}") else maps,
            "samples": samples}


def pe_exports(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    exports = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    return [(s.address, s.name.decode() if s.name else f"#{s.ordinal}") for s in exports.symbols if s.address] if exports else []


def elf_exports(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return [(int(f[0], 16), f[2]) for f in (line.split() for line in out.splitlines()) if len(f) == 3 and f[1] in "TtWi"]


class Symbols:
    def __init__(self, profile):
        self.pid = profile["pid"]
        self.maps = sorted(tuple(m) for m in profile["maps"])
        self.starts = [m[0] for m in self.maps]
        self.base = {}  # load address of each file: lowest (mapping start - file offset)
        for lo, _, _, off, path in self.maps:
            if path.startswith("/") and lo - off < self.base.get(path, 1 << 64):
                self.base[path] = lo - off
        self.tables = {}

    def table(self, path):
        if path not in self.tables:
            # Libraries of the Flatpak runtime are only visible through the process's root.
            real = path if os.path.exists(path) else f"/proc/{self.pid}/root{path}"
            syms = []
            try:
                if path.lower().endswith((".dll", ".exe", ".drv", ".sys")):
                    syms = pe_exports(real) if pefile else []
                elif ".so" in os.path.basename(path) and shutil.which("nm"):
                    syms = elf_exports(real)
            except OSError:
                pass
            except Exception as e:  # pefile.PEFormatError and friends: no names for this file
                print(f"(no symbols for {path}: {e})", file=sys.stderr)
            syms.sort()
            self.tables[path] = (syms, [a for a, _ in syms])
        return self.tables[path]

    def path(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.maps[i][4] if i >= 0 and addr < self.maps[i][1] else None

    def module(self, addr):
        path = self.path(addr)
        if path is None:
            return "?"
        return os.path.basename(path) if path.startswith("/") else (path or "[anon]")

    def name(self, addr):
        path = self.path(addr)
        if not path or not path.startswith("/") or path not in self.base:
            return self.module(addr)
        mod, rva = os.path.basename(path), addr - self.base[path]
        syms, addrs = self.table(path)
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0 or rva - syms[i][0] > NEAR_EXPORT:
            return f"{mod} (unexported code)+{rva:#x}"
        return f"{mod}!{syms[i][1]}+{rva - syms[i][0]:#x}"

    def function(self, addr):
        """Like name() without the offset, so samples in one function add up."""
        return self.name(addr).split("+0x")[0]


def report(profile, include_all, focus, top):
    samples = profile["samples"]
    states = collections.Counter(s[2] for s in samples)
    print(f"{len(samples)} samples of thread {profile['tid']}: "
          + ", ".join(f"{n} {'running' if k == 'R' else 'sleeping' if k == 'S' else k}" for k, n in states.most_common()))
    if not include_all:
        samples = [s for s in samples if s[2] == "R"]
    if not samples:
        print("nothing to report")
        return
    sym = Symbols(profile)
    n = len(samples)
    focus = [f.lower() for f in focus]

    def table(title, counter):
        print(f"\n== {title}")
        for key, count in counter.most_common(top):
            print(f"{count:6d} {100 * count / n:5.1f}%  {key}")

    self_mod, self_fn, incl_mod, incl_fn, calls = (collections.Counter() for _ in range(5))
    def wanted(mod):
        return not focus or any(f in mod.lower() for f in focus)

    for rip, frames, _, syscall in samples:
        call = "-" if syscall < 0 else SYSCALLS.get(syscall, str(syscall))
        top_mod = sym.module(rip)
        # In a system call the instruction pointer sits in an unexported libc wrapper.
        top_fn = sym.function(rip) if syscall < 0 else f"{top_mod} [system call {call}]"
        self_mod[top_mod] += 1
        self_fn[top_fn] += 1
        calls[call] += 1
        mods, fns = {top_mod}, {top_fn} if wanted(top_mod) else set()
        for addr in frames:
            mod = sym.module(addr)
            mods.add(mod)
            if wanted(mod):
                fns.add(sym.function(addr))
        incl_mod.update(mods)
        incl_fn.update(fns)

    which = "running samples" if not include_all else "all samples"
    table(f"self (top of stack) by module, {which}", self_mod)
    table("self by function", self_fn)
    table("inclusive (anywhere on the stack) by module", incl_mod)
    table("inclusive by function" + (f" in {', '.join(focus)}" if focus else ""), incl_fn)
    table("system call at the time of the sample (- = none; read from ntdll.so/win32u.so = waiting for wineserver)", calls)


def main():
    argv = sys.argv[1:]
    command = argv[argv.index("--") + 1:] if "--" in argv else []
    argv = argv[: argv.index("--")] if "--" in argv else argv
    parser = argparse.ArgumentParser(usage=argparse.SUPPRESS, description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bottle", default="Altium")
    parser.add_argument("--seconds", type=float, default=20, help="how long to sample without a COMMAND")
    parser.add_argument("--interval", type=float, default=2, help="ms between samples")
    parser.add_argument("--tid", type=int, help="thread to sample (default: the UI thread, thread id = pid)")
    parser.add_argument("--focus", action="append", default=[], help="inclusive functions only from this module (repeatable)")
    parser.add_argument("--all", action="store_true", help="also count samples taken while the thread slept")
    parser.add_argument("--top", type=int, default=20, help="rows per table")
    parser.add_argument("--save", help="write the raw samples to this JSON file")
    parser.add_argument("--load", help="report on samples saved earlier instead of sampling")
    args = parser.parse_args(argv)

    if args.load:
        profile = json.load(open(args.load))
    else:
        pid = altium_pid(DATA / "bottles" / args.bottle)
        if not pid:
            sys.exit(f"Altium is not running in bottle '{args.bottle}'")
        tid = args.tid or pid
        if command:
            proc = subprocess.Popen(command)
            keep_going = lambda: proc.poll() is None
        else:
            end = time.monotonic() + args.seconds
            keep_going = lambda: time.monotonic() < end
        print(f"==> Sampling thread {tid} of X2.EXE (PID {pid}) every {args.interval:g} ms", file=sys.stderr)
        profile = sample(pid, tid, args.interval / 1000, keep_going)
        if command and proc.wait():
            print(f"==> {command[0]} exited with {proc.returncode}", file=sys.stderr)
        if args.save:
            json.dump(profile, open(args.save, "w"))
    if not pefile:
        print("(no pefile: Windows DLLs are shown without function names; see scripts/scan-missing-imports.py)", file=sys.stderr)
    report(profile, args.all, args.focus, args.top)


if __name__ == "__main__":
    main()
