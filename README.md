# Altium Designer on Linux (Wine + Bottles)

Run Altium Designer 26 on Linux with Wine through [Bottles](https://usebottles.com),
without a Windows VM. Out of the box it fails at about a dozen points: the installer,
sign-in, menus, fonts, opening a PCB. This repository collects the fixes, the small
Windows shims that make some of them possible, a script that applies them, and the
debugging tools that found them.

It runs: you can sign in, get an on-demand license, open projects, and work in the
PCB editor in 2D and 3D. It is not polished yet; see the status table.

Not affiliated with Altium. You need your own Altium license; nothing from Altium is
redistributed here.

## Status

| Area | State |
|---|---|
| Offline installer | works (see [step 4](#4-install-altium)) |
| Sign-in through the browser, on-demand license | works |
| Home page, menus, Preferences | work |
| PCB editor, 2D and 3D | works |
| Schematic editor | untested |
| PCB view redraw after a layer switch | one step behind; fix in testing ([§11](docs/how-it-works.md#11-pcb-view-one-step-behind-experimental)) |
| Heads-up display in the PCB view | dark box; press Shift+H to hide it |
| Dragging floating panels across monitors with different scaling | jitters |
| Loading a project | occasionally freezes (seen twice) |

The full list, with what is known about each, is in [docs/known-issues.md](docs/known-issues.md).

### Tested on

| | |
|---|---|
| Altium Designer | 26.10.1, offline installer, student (on-demand) license |
| Bottles | 67.0 (Flatpak), "Gaming" environment, Windows 10 |
| Runner | soda-11.0-7 (Proton-based Wine 11.0), DXVK 3.1, vkd3d-proton 3.0.1 |
| Desktop | Omarchy (Arch), Hyprland 0.56 with the Lua config, XWayland |
| GPU | Intel Arc (Meteor Lake), Mesa 26.1 |

Other runners, desktops and GPUs are untested. Two fixes rely on the runner being
Proton-based (`tabtip.exe` and the `steam_app_<SteamAppId>` window class); the
Hyprland rules only matter on Hyprland.

## What breaks, and the fix

| # | Without the fix | Fix |
|---|---|---|
| 1 | Installer: `Cannot get symbol u_charsToUChars from libicuuc Error: 127` | `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1` |
| 2 | Dialogs hidden behind the main window, ghost copies (Hyprland) | float rule for `steam_app_altium` |
| 3 | Offline installer finds no packages | Flatpak access to the setup folder |
| 4 | Freezes with black windows and a ~55 MB/s memory leak | disable Proton's `tabtip.exe` |
| 5 | Sign-in box stays blank | browser sign-in (`System.UnifiedLogin`) |
| 6 | Browser sign-in opens no browser | `cmd.exe` shim (Wine `start.exe` overflows on long URLs) |
| 7 | Cursor jumps to window centres (Hyprland) | `focus_on_activate = false` |
| 8 | Whole UI in Times New Roman | Microsoft's Selawik, renamed to "Segoe UI" |
| 9 | File menu / right-click: `External exception C06D007F` | `SHFolder.dll` proxy supplies `InheritWindowMonitor` |
| 10 | Opening a PCB closes Altium (`GnuTLS error: Decryption has failed`) | `SHFolder.dll` proxy does raw RSA for the Simbeor license check |
| 11 | PCB view one step behind | `dxvk.conf`: `dxgi.syncInterval = 0` (experimental) |

[docs/how-it-works.md](docs/how-it-works.md) explains each one: symptom, root cause,
evidence and fix.

## Setup

### 1. Requirements

- [Bottles](https://flathub.org/apps/com.usebottles.bottles) from Flathub (the scripts
  assume the Flatpak).
- To build the shims: `clang`, `lld` and `llvm` (for `llvm-dlltool`). No MinGW or
  Windows SDK. Arch: `sudo pacman -S clang lld llvm`; Debian/Ubuntu:
  `sudo apt install clang lld llvm`.
- `python3`, `curl`, `unzip`.
- The Altium Designer **offline installer** (`OfflineSetup_Altium_Designer_Public_*.zip`
  from your Altium account), extracted.

### 2. Create the bottle

In Bottles: **Create new bottle**, name `Altium`, environment **Gaming**, runner
**soda** (tested with soda-11.0-7). Bottles installs DXVK, vkd3d-proton and a few
dependencies (core fonts, `d3dx9`, `d3dcompiler_47`, Wine Mono, Gecko).

Close Bottles, then set two environment variables. The first is needed by the
installer, the second names Altium's windows for window rules:

```bash
flatpak run --command=bottles-cli com.usebottles.bottles edit -b Altium --env-var DOTNET_SYSTEM_GLOBALIZATION_USENLS=1
flatpak run --command=bottles-cli com.usebottles.bottles edit -b Altium --env-var SteamAppId=altium
```

(Or add them under the bottle's **Settings > Environment Variables**.)

### 3. Hyprland only: window rules

Add [`config/hyprland/altium.lua`](config/hyprland/altium.lua) to
`~/.config/hypr/hyprland.lua`. It floats Altium's windows (tiling hides dialogs behind
the main window) and stops Wine's activate requests from warping the cursor. On older
Hyprland versions with `hyprland.conf`, write the same two rules in your version's
`windowrule` syntax: `float` and `focus_on_activate off`, matched on class
`steam_app_altium`.

### 4. Install Altium

Bottles' file picker only gives Wine access to the file you pick, but the offline
installer needs the folders next to it. Grant access to the extracted folder first:

```bash
flatpak override --user --filesystem="$HOME/Downloads/OfflineSetup_Altium_Designer_Public_26_10_1" com.usebottles.bottles
```

Then in Bottles: open the bottle, **Run Executable**, pick `Installer.Exe` in that
folder, and install as usual. If the installer asks you to sign in and no browser
opens, cancel and run it again; the second run skipped the sign-in step for us (see
[known issues](docs/known-issues.md#installer-sign-in-opens-no-browser)).

### 5. Start Altium once

Start Altium from Bottles once and close it. This creates the registry key the browser
sign-in setting goes into. Altium may complain or misbehave on this first run; that is
expected.

### 6. Apply the fixes

With Altium and the Bottles window closed:

```bash
git clone <this repository> altium-on-linux && cd altium-on-linux
scripts/install.sh            # --dry-run to see what it would do; --help for options
```

It refuses to run while the bottle is running, backs up anything it replaces, can be
re-run, and ends with a check:

```
==> Checking
    ok   tabtip.exe disabled
    ok   WPF software rendering
    ok   SHFolder proxy override
    ok   Segoe UI font
    ok   browser sign-in
    ok   SteamAppId
```

### 7. Sign in

Start Altium from Bottles, open **License Management** and click **Sign In**. Your
browser opens Altium's sign-in page; once you are signed in there, Altium picks it up
and you can use your license.

In the PCB editor, press **Shift+H** to hide the heads-up display until it is fixed.

## What `install.sh` changes

| Where | What | Fix |
|---|---|---|
| `Program Files\Altium\AD26\cmd.exe` | shim: opens `cmd /c start <url>` links directly, passes everything else to the real `cmd.exe` | 6 |
| `Program Files\Altium\AD26\SHFolder.dll` | proxy: forwards to `shell32`, supplies missing `user32` functions, raw RSA for Simbeor | 9, 10 |
| `Program Files\Altium\AD26\dxvk.conf` | `dxgi.syncInterval = 0` | 11 |
| `windows\Fonts\segoeui-selawik*.ttf` | Selawik renamed to "Segoe UI", built locally from Microsoft's release | 8 |
| `HKCU\Software\Wine\DllOverrides` | `tabtip.exe` disabled | 4 |
| `HKCU\Software\Wine\AppDefaults\X2.EXE\DllOverrides` | `shfolder` = `native,builtin` | 9, 10 |
| `HKCU\Software\Microsoft\Avalon.Graphics` | WPF software rendering | 4 |
| `HKCU\Software\Altium\Altium Designer {…}\…\AltiumPortal\Account` | `UnifiedLogin` = `1` | 5 |
| `HKLM\…\Fonts`, `HKCU\Control Panel\Desktop` | register the fonts, RGB font smoothing | 8 |
| `bottle.yml` | `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1`, `SteamAppId=altium` | 1, 2 |

The shims are freestanding C (no C runtime, no Windows headers) built with
`scripts/build.sh`; each source file explains what it does and why.

**After updating Altium** to a new major version (a new `AD27` folder), run
`scripts/install.sh` again: it targets the newest `AD*` folder.

**To undo:** delete `cmd.exe`, `SHFolder.dll` and `dxvk.conf` from Altium's folder (or
restore the `.bak` copies) and the `shfolder` value under `AppDefaults\X2.EXE`; the
other changes are harmless to keep.

## Debugging

[docs/debugging.md](docs/debugging.md) covers the tools and the traps:

| Script | Use |
|---|---|
| `scripts/debug-launch.sh` | start Altium with Wine/DXVK logging, optionally opening a document |
| `scripts/watchdog.sh` | kill Altium above a memory limit, log its memory |
| `scripts/backtrace.sh` | native stacks of every thread (`winedbg`) |
| `scripts/dotnet-stacks.sh` | managed .NET stacks (`dotnet-dump`), for freezes |
| `scripts/wininfo.sh` | Altium's windows as Wine sees them |
| `scripts/scan-missing-imports.py` | Windows functions Altium delay-loads that Wine lacks |
| `tests/test_rsa.py` | checks the raw RSA code against Python on random keys |

## Repository layout

```
config/          dxvk.conf, Hyprland rules
docs/            how-it-works, known-issues, debugging
fonts/           rename_to_segoeui.py (Selawik -> "Segoe UI")
registry/        .reg files imported by install.sh
scripts/         build, install, debugging tools (lib.sh is shared)
src/cmdshim/     cmd.exe shim
src/shfolderproxy/  SHFolder.dll proxy (user32 fallbacks, raw RSA)
src/tools/       wininfo.exe, clearex.exe (diagnostics)
tests/           raw RSA test
```

## License

[MIT](LICENSE). Altium Designer is proprietary and not included; you need your own
license. The Selawik fonts are built locally from Microsoft's release under the SIL
Open Font License and are not distributed here.
