#!/usr/bin/env bash
# Build the Windows binaries (64-bit PE) with clang + lld-link. No Windows SDK,
# MinGW or C runtime is needed: each source declares the few APIs it uses and
# links against import libraries generated from the .def files next to it.
#
#   scripts/build.sh            -> build/cmd.exe, build/SHFolder.dll,
#                                  build/wininfo.exe, build/clearex.exe
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src
OUT=${OUT:-$ROOT/build}
OBJ=$OUT/obj

for tool in clang lld-link llvm-dlltool; do
    command -v "$tool" >/dev/null || { echo "missing $tool (Arch: pacman -S clang lld llvm)" >&2; exit 1; }
done
mkdir -p "$OBJ"

CFLAGS=(--target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin
        -mno-stack-arg-probe -fno-stack-protector -Wall -Wextra -Werror)

# importlib <def file>: generate an x64 import library from a .def file.
importlib() {
    local lib
    lib=$OBJ/$(basename "${1%.def}").$(basename "$(dirname "$1")").lib
    llvm-dlltool -m i386:x86-64 -d "$1" -l "$lib"
    echo "$lib"
}

# link_exe <name> <entry> <source> <def files...>
link_exe() {
    local name=$1 entry=$2 src=$3; shift 3
    local libs=() def
    for def in "$@"; do libs+=("$(importlib "$def")"); done
    clang "${CFLAGS[@]}" -c "$src" -o "$OBJ/$name.obj"
    lld-link /nologo /Brepro /entry:"$entry" /subsystem:console /nodefaultlib /machine:x64 \
        /out:"$OUT/$name.exe" "$OBJ/$name.obj" "${libs[@]}"
}

link_exe cmd start_entry "$SRC/cmdshim/cmdshim.c" \
    "$SRC/cmdshim/kernel32.def" "$SRC/cmdshim/shell32.def"

clang "${CFLAGS[@]}" -c "$SRC/shfolderproxy/shfolderproxy.c" -o "$OBJ/shfolderproxy.obj"
lld-link /nologo /Brepro /dll /entry:DllMain /nodefaultlib /machine:x64 \
    /def:"$SRC/shfolderproxy/shfolder.def" /out:"$OUT/SHFolder.dll" "$OBJ/shfolderproxy.obj" \
    "$(importlib "$SRC/shfolderproxy/kernel32.def")" "$(importlib "$SRC/shfolderproxy/ntdll.def")"
rm -f "$OUT/SHFolder.lib" "$OUT/SHFolder.exp"

link_exe wininfo start_entry "$SRC/tools/wininfo.c" "$SRC/tools/user32.def" "$SRC/tools/kernel32.def"
link_exe clearex start_entry "$SRC/tools/clearex.c" "$SRC/tools/user32.def" "$SRC/tools/kernel32.def"

ls -l "$OUT"/*.exe "$OUT"/*.dll
