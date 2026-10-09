#!/usr/bin/env python3
"""List Windows functions that Altium delay-loads but Wine does not export.

A delay-loaded function is resolved on first call; if Wine lacks it, the call raises
"External exception C06D007F" (procedure not found) or C06D007E (DLL not found) at a
random-looking moment, e.g. when a menu opens. This finds them ahead of time.

    scripts/scan-missing-imports.py [--bottle NAME] [--all]

Scans Altium's program folder, its extensions in ProgramData and the WebView2 runtime,
and compares each delay import against the exports of the bottle runner's Wine DLLs
(lib/wine/x86_64-windows). --all also lists imports from DLLs Wine does not ship
(mostly Altium's own optional libraries, which is normally fine).

Needs pefile (`pip install pefile`). llvm-readobj cannot be used: it fails on Altium's
Delphi binaries ("RVA ... for debug directory not found").
"""
import argparse
import os
import re
import sys
from pathlib import Path

try:
    import pefile
except ImportError:
    sys.exit("needs pefile: python3 -m venv build/venv && build/venv/bin/pip install pefile")

DATA = Path.home() / ".var/app/com.usebottles.bottles/data/bottles"
DELAY = pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]
EXPORT = pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]


def runner_dir(bottle_dir):
    match = re.search(r"^Runner: (.+)$", (bottle_dir / "bottle.yml").read_text(), re.M)
    if not match:
        sys.exit(f"no Runner in {bottle_dir / 'bottle.yml'}")
    return DATA / "runners" / match.group(1) / "lib/wine/x86_64-windows"


class WineExports:
    def __init__(self, wine_dir):
        self.files = {f.name.lower(): f for f in wine_dir.iterdir()}
        self.cache = {}

    def get(self, dll):
        """Set of exported names, or None if Wine does not ship the DLL."""
        if dll not in self.cache:
            path = self.files.get(dll)
            names = None
            if path:
                pe = pefile.PE(str(path), fast_load=True)
                pe.parse_data_directories(directories=[EXPORT])
                names = set()
                if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
                    names = {s.name.decode() for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
            self.cache[dll] = names
        return self.cache[dll]


def delay_imports(path):
    try:
        pe = pefile.PE(str(path), fast_load=True)
        if pe.FILE_HEADER.Machine != 0x8664:  # x64 only; Altium is 64-bit
            return
        pe.parse_data_directories(directories=[DELAY])
    except pefile.PEFormatError:
        return
    for entry in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        dll = entry.dll.decode(errors="replace").lower()
        for imp in entry.imports:
            if imp.name:
                yield dll, imp.name.decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bottle", default="Altium")
    parser.add_argument("--all", action="store_true", help="also list DLLs Wine does not ship")
    args = parser.parse_args()

    bottle = DATA / "bottles" / args.bottle
    c = bottle / "drive_c"
    roots = [c / "Program Files/Altium", c / "ProgramData/Altium",
             c / "Program Files (x86)/Microsoft/EdgeWebView"]
    wine = WineExports(runner_dir(bottle))

    found = set()
    for root in roots:
        for path in root.rglob("*"):
            if path.suffix.lower() not in (".dll", ".exe", ".bpl") or not path.is_file():
                continue
            for dll, name in delay_imports(path):
                exports = wine.get(dll)
                if exports is None:
                    if args.all:
                        found.add(("no-dll", path.name, f"{dll}!{name}"))
                elif name not in exports:
                    found.add(("MISSING", path.name, f"{dll}!{name}"))
    for kind, module, func in sorted(found):
        print(f"{kind:8} {module:40} {func}")
    if not found:
        print("no missing delay-loaded functions")


if __name__ == "__main__":
    main()
