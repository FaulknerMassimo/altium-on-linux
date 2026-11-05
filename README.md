# Altium Designer on Linux

Run Altium Designer 26 on Linux with Wine through [Bottles](https://usebottles.com), with
no Windows VM. This guide takes you from a fresh Linux install to signing in to Altium:
eight steps, about 30 minutes plus the download. A script applies the two dozen fixes Wine
needs (installer, sign-in, fonts, menus, opening a PCB, drawing, freezes), so you only
copy a few commands.

Not affiliated with Altium. You need your own Altium account and license; nothing from
Altium is redistributed here.

**What works:** the online installer, sign-in through your browser, on-demand licenses,
opening projects, the PCB editor in 2D and 3D, and the schematic editor. Rough edges are
listed in [known issues](docs/known-issues.md).

## Before you start

- **Linux desktop** with working graphics drivers (Vulkan). Tested on Arch with Hyprland
  and Intel graphics; other distributions and desktops should work but are untested
  ([details](#tested-on)).
- **Altium account** with a license (a student license works).
- **About 15 GB of free disk space** and a good internet connection: the installer
  downloads several GB.
- **Hyprland users:** step 4 adds window rules you need. Other desktops skip it.

## Step 1. Install Bottles

Install Bottles from Flathub (the scripts expect the Flatpak version, not a distribution
package):

```bash
flatpak install flathub com.usebottles.bottles
```

Start Bottles once and let it finish its first-run download, then leave it open for step 3.

## Step 2. Install the build tools and download this repository

The fixes include a few small Windows and Linux programs that are built on your machine
(with clang; no Windows SDK needed). Install the tools:

```bash
# Arch, Manjaro, EndeavourOS
sudo pacman -S --needed git clang lld llvm python curl unzip
# Debian, Ubuntu, Mint, Pop!_OS
sudo apt install git clang lld llvm python3 python3-venv curl unzip
# Fedora
sudo dnf install git clang lld llvm python3 curl unzip
```

Then download this repository into your home folder:

```bash
git clone https://github.com/FaulknerMassimo/altium-on-linux.git ~/altium-on-linux
```

## Step 3. Create the bottle

Everything was tested with the **soda-11.0-7** runner (a Proton-based Wine 11). Bottles
may pick a newer one by default, so install this one first:

1. In Bottles, open the menu (☰, top right) → **Preferences** → **Runners**.
2. Expand **Soda** and click the download button next to **soda-11.0-7**.

Then create the bottle:

1. Click **+** (top left) to create a new bottle.
2. **Name:** `Altium` (the scripts use this name).
3. **Environment:** **Gaming**.
4. **Runner:** **soda-11.0-7**.
5. Click **Create** and wait until it is done (Bottles installs DXVK, vkd3d-proton and a few
   dependencies).

Now **close Bottles** completely. The script in step 5 refuses to run while Bottles is
open, because Bottles would overwrite the settings it changes.

Prefer a terminal? Once the runner is downloaded, this creates the same bottle:

```bash
flatpak run --command=bottles-cli com.usebottles.bottles new --bottle-name Altium --environment gaming --runner soda-11.0-7
```

## Step 4. Hyprland only: add the window rules

Add this line at the end of `~/.config/hypr/hyprland.lua` (Hyprland 0.56 or newer, Lua
config):

```lua
dofile(os.getenv("HOME") .. "/altium-on-linux/config/hyprland/altium.lua")
```

Hyprland reloads its config when you save. [`config/hyprland/altium.lua`](config/hyprland/altium.lua)
floats Altium's dialogs and tiles its main window, keeps Wine from warping your cursor and
Altium's popups from closing, moves the orange glow Altium draws behind its dialogs out of
the way (otherwise it covers the installer's Sign In dialog), makes an external monitor
X11's primary so Altium opens its windows there, and resyncs Wine's monitor list when
monitors change.

<details>
<summary>Older Hyprland with <code>hyprland.conf</code></summary>

Write the same rules in your version's `windowrule` syntax: `focus_on_activate off` and
`no_follow_mouse` matched on class `steam_app_altium`; `float` matched on that class and a
title *not* matching `.*Altium Designer.*\(\d+\.\d+.*` (the main window); and
`exec-once = xrandr --output <external monitor> --primary`. The glow fix needs the Lua
config: without it, drag the orange box aside (Super + drag) to reach the dialog under it.
</details>

## Step 5. Prepare the bottle

With Bottles closed:

```bash
cd ~/altium-on-linux
scripts/install.sh
```

Altium is not installed yet, so this first run prepares the bottle for the installer: the
sign-in fix for the installer, fonts, environment variables and registry settings. It
ends with:

```
==> Checking
    ok   tabtip.exe disabled
    ok   WPF software rendering
    ok   SHFolder proxy override
    ok   real display modes
    ok   Segoe UI font
    ok   start.exe shim (installer sign-in)
    ok   SteamAppId
    ok   present-wait layer
    ok   fd guard, XRandR and zero-size filters

The bottle is ready for the Altium Designer installer. Next: run the installer from
Bottles (README step 6), untick "Run Altium Designer" at the end, then run this
script again.
```

(`scripts/install.sh --dry-run` shows what it would do; `--help` lists the options. It backs
up anything it replaces and can be run again at any time.)

## Step 6. Install Altium Designer

1. Download the installer from [altium.com/products/downloads](https://www.altium.com/products/downloads)
   (sign in, then **Download**). You get `AltiumDesignerSetup_<version>.exe`, about 30 MB.
2. Open Bottles, click the **Altium** bottle, then **Run Executable…** and pick the file
   you downloaded.
3. Go through the installer:
   - **Welcome:** click **Next**.
   - **License Agreement:** accept it and click **Next**.
   - **Sign In:** click **Sign In**. Your browser opens Altium's sign-in page; sign in
     there, then come back to the installer. You have 5 minutes; if the time runs out,
     click **Please try again**.
   - **Design Functionality:** choose what you need (PCB Design is always installed) and
     click **Next**.
   - **Destination Folders:** keep the defaults.
   - **Customer Experience Improvement Program:** your choice.
   - **Ready To Install:** click **Next**. The installer downloads and installs Altium;
     this takes a while.
4. On **Installation Complete**, **untick "Run Altium Designer"** and click **Finish**.
   Altium should not start before step 7. (If it starts anyway, it opens in Times New Roman
   with half its fixes missing; close it.)

## Step 7. Apply the fixes

Close Bottles again, and run the script a second time:

```bash
cd ~/altium-on-linux
scripts/install.sh
```

Now that Altium is installed, it also installs the fixes that go into Altium's folder,
switches Altium to sign in through your browser, and adds **Altium Designer** to the
bottle's programs. Every line of the check should say `ok`:

```
==> Checking
    ok   tabtip.exe disabled
    ok   WPF software rendering
    ok   SHFolder proxy override
    ok   real display modes
    ok   Segoe UI font
    ok   start.exe shim (installer sign-in)
    ok   cmd.exe shim (Altium sign-in)
    ok   SHFolder.dll proxy
    ok   browser sign-in
    ok   SteamAppId
    ok   present-wait layer
    ok   fd guard, XRandR and zero-size filters

Done. Start Altium from Bottles.
```

## Step 8. Start Altium and sign in

1. Open Bottles, click the **Altium** bottle, and start **Altium Designer** from its
   programs (▶). The first start takes a minute or two.
2. Altium is already signed in with the account you used in the installer, and without a
   license in use it opens on **License Management**: click **Use License** next to yours.
3. If Altium ever says you are not signed in, click **Sign In** (top right): your browser
   opens Altium's sign-in page, and once you have signed in there, Altium picks it up.
   License Management is also in the account menu (top right).

To start Altium from your application launcher, use **Add Desktop Entry** in the program's
⋮ menu in Bottles.

You're done. The rest of this page is for when something goes wrong, and for the curious.

## Updating Altium

Update from inside Altium (**Extensions and Updates**) or with a newer installer, then
close Altium and run `scripts/install.sh` again. A new major version installs into a new
folder (`AD27`); the script always targets the newest one.

To update the fixes, `git pull` in `~/altium-on-linux` and run `scripts/install.sh` again.

## Troubleshooting

| Problem | What to do |
|---|---|
| `install.sh`: "the bottle is running" | Close Altium and the installer. If Wine processes are left over: `flatpak run --command=bottles-cli com.usebottles.bottles stop -b Altium` |
| `install.sh`: "close the Bottles window first" | Bottles can keep running in the background: `flatpak kill com.usebottles.bottles` |
| A line of the check says `MISSING` | Run `scripts/install.sh` again and read the error above the check |
| Installer: Sign In does nothing, or "Time to login has expired" | Step 5 was skipped or failed: close the installer, run `scripts/install.sh`, start the installer again |
| No browser opens at all (installer or Altium) | Wine opens links with your desktop's default browser (`xdg-open`); make sure one is set |
| Hyprland: orange box over a dialog, dialogs behind the main window, cursor jumping | The window rules (step 4) are not loaded; check the `dofile` line and the path of your clone |
| Hyprland: empty white box next to the installer | Step 5 was skipped: close the installer, run `scripts/install.sh`, start it again |
| Altium in Times New Roman | The font step failed (it downloads Microsoft's Selawik font from GitHub); run `scripts/install.sh` again |
| Altium does not start, hangs with no window, or shows `Runtime error 217` | Processes from an earlier session are left over: stop the bottle (first row) and start Altium again |
| Altium freezes now and then | See [known issues](docs/known-issues.md); the logs are in `~/.var/app/com.usebottles.bottles/data/bottles/bottles/Altium/altium-on-linux/logs/` |

Everything known to still go wrong is in [docs/known-issues.md](docs/known-issues.md), and
[docs/debugging.md](docs/debugging.md) has the tools to dig further.

## Using the offline installer instead

Altium also offers an offline installer (`OfflineSetup_Altium_Designer_Public_*.zip`,
about 3 GB, from the Downloads page while signed in). Follow the same steps, but in step 6
extract the zip and give Bottles access to the extracted folder first. Bottles' file
picker only exposes the file you pick, and the offline installer needs the folders next to
it:

```bash
flatpak override --user --filesystem="$HOME/Downloads/OfflineSetup_Altium_Designer_Public_26_10_1" com.usebottles.bottles
```

Then pick `Installer.Exe` in that folder under **Run Executable…**. Its sign-in step uses the
same helper as the online installer; earlier, before the `start.exe` shim existed, running
the installer a second time skipped it.

## Undoing the fixes

Delete `cmd.exe`, `SHFolder.dll` and `dxvk.conf` from Altium's folder (or restore the
`.bak` copies), restore `drive_c/windows/syswow64/start.exe.bak` to `start.exe`, and remove
the `shfolder` value under `HKCU\Software\Wine\AppDefaults\X2.EXE\DllOverrides` and the bottle
variables `VK_ADD_IMPLICIT_LAYER_PATH` and `LD_PRELOAD`. The other changes are harmless to
keep. Or delete the bottle.

# Reference

## Status

| Area | State |
|---|---|
| Online installer, including its sign-in | works |
| Offline installer | works (see [above](#using-the-offline-installer-instead)) |
| Sign-in through the browser, on-demand license | works |
| Home page, menus, Preferences | work |
| PCB editor, 2D and 3D | works |
| Schematic editor | opens and redraws; text ([§12](docs/how-it-works.md#12-schematic-text-broken-and-redraws-slow-direct2d-text)) and symbols drawn with arcs, such as inductors ([§15](docs/how-it-works.md#15-blank-schematic-symbols-inductors-direct2d-arcs)), fixed; editing works |
| PCB and schematic views showing the previous frame | fixed ([§11](docs/how-it-works.md#11-pcb-and-schematic-views-one-frame-behind-vulkan-present-race)), without capping the PCB view's redraw rate (over 200 redraws/s); the schematic redraws about 50 times a second on a large sheet ([§18](docs/how-it-works.md#18-slow-schematic-redraws-a-direct3d-state-swap-per-shape)) |
| Heads-up display in the PCB view | fixed ([§14](docs/how-it-works.md#14-heads-up-display-frozen-in-the-pcb-view-layered-child-windows)); drawn over black rather than blended with the board |
| Two monitors with different scaling | Altium's windows open on the external monitor ([§17](docs/how-it-works.md#17-windows-split-across-monitors-hyprland-mixed-scaling)); moving them to the laptop screen, or starting Altium from it, still misplaces them ([known issues](docs/known-issues.md#windows-misplaced-on-the-other-monitor-mixed-scaling)) |
| Closing the lid, switching a monitor off and on | Altium follows the new layout ([§19](docs/how-it-works.md#19-altium-drawn-offset-after-a-monitor-goes-away-and-comes-back)); needs the Hyprland hook |
| Freezes now and then (a Wine thread loses its pipes to wineserver) | cause found: under fsync, Wine closes a random fd each time a thread ends; the fd guard skips those closes ([§20](docs/how-it-works.md#20-freezes-wine-threads-lose-their-pipes-to-wineserver)); not yet confirmed over long sessions |
| A monitor stuck at 0x0 (a dock reconnecting) | hidden from Wine, which divided by its width and lost Altium's windows ([§21](docs/how-it-works.md#21-a-monitor-stuck-at-0x0-altium-shows-no-window-or-loses-them-all)) |
| Unplugging a dock while Altium runs | Altium's windows move onto a remaining monitor ([§24](docs/how-it-works.md#24-altium-disappears-when-a-monitor-is-unplugged-a-window-on-no-monitor)); X errors about outputs and windows that went away no longer end Wine's processes ([§25](docs/how-it-works.md#25-wine-processes-exit-after-a-monitor-goes-away-unexpected-x-errors)) |
| Starting Altium | the hang now and then on start (no window) is fixed ([§29](docs/how-it-works.md#29-altium-hangs-on-start-altiums-ucrtbasedll-and-wines-loader-lock)) |
| Quitting Altium | after a long session, it sometimes stays open with `No License` in the title: [known issues](docs/known-issues.md#altium-does-not-exit-after-file--exit) |

### Tested on

| | |
|---|---|
| Altium Designer | 26.11.1 (online installer) and 26.10.1 (offline installer), student (on-demand) license |
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
| 2 | Dialogs hidden behind the main window, ghost copies (Hyprland) | float rule for Altium's windows (`steam_app_altium`) except the main window, which tiles |
| 3 | Offline installer finds no packages | Flatpak access to the setup folder |
| 4 | Freezes with black windows and a ~55 MB/s memory leak | disable Proton's `tabtip.exe` |
| 5 | Sign-in box stays blank | browser sign-in (`System.UnifiedLogin`) |
| 6 | Browser sign-in opens no browser | `cmd.exe` shim (Wine `start.exe` overflows on long URLs) |
| 7 | Cursor jumps to window centres (Hyprland) | `focus_on_activate = false` |
| 8 | Whole UI in Times New Roman | Microsoft's Selawik, renamed to "Segoe UI" |
| 9 | File menu / right-click: `External exception C06D007F` | `SHFolder.dll` proxy supplies `InheritWindowMonitor` |
| 10 | Opening a PCB closes Altium (`GnuTLS error: Decryption has failed`) | `SHFolder.dll` proxy does raw RSA for the Simbeor license check |
| 11 | PCB and schematic views one frame behind: a click selects, but the highlight shows up only with the next redraw | Vulkan layer waits until the X server has copied each frame before Wine copies the view; `dxvk.conf`: `dxgi.syncInterval = 0` |
| 12 | Schematic text with missing strokes and underscores, slow redraws | `SHFolder.dll` proxy routes Direct2D text through DirectWrite bitmaps |
| 13 | Black box around part of the splash screen | `SHFolder.dll` proxy keeps Wine from cutting transparent pixels out of layered windows |
| 14 | PCB heads-up display frozen in the view, a live copy in the corner of the screen on every workspace | `SHFolder.dll` proxy draws layered child windows into their parent |
| 15 | Schematic symbols drawn with arcs (inductors) blank | `SHFolder.dll` proxy implements Direct2D arcs |
| 16 | Error dialog `Control 'View…' has no parent window`, now and then | `SHFolder.dll` proxy clears a panel's stale focus pointer instead of creating a window for it |
| 17 | Two monitors at different scales (Hyprland): splash split across them, Messages panel on the other screen | external monitor made X11's primary, so Wine and Altium put their windows there |
| 18 | Schematic redraws slow (24 a second on a large sheet) | `SHFolder.dll` proxy skips Wine's Direct3D state swaps around every Direct2D shape |
| 19 | After a monitor goes away and comes back (closed lid, monitor off): Altium drawn offset, dialogs off-screen, a monitor lost | registry: `EmulateModeset=N`; Hyprland hook resyncs Wine's monitor list (`scripts/refresh-displays.sh`) |
| 20 | Altium freezes now and then: no redraws, no input (`wine client error … Bad file descriptor` earlier in the log) | fd guard (`LD_PRELOAD`) skips Wine's own closes of fds it does not own (under fsync, each ending thread closes a random fd), keeps other code off the pipes Wine's threads use to talk to wineserver, and logs both |
| 21 | A monitor stuck at 0x0 (dock reconnecting): Altium starts with no window or loses all its windows, `explorer.exe` at 100% CPU | XRandR filter (`LD_PRELOAD`) hides outputs whose CRTC is 0x0 from Wine |
| 22 | Popups close as soon as the cursor leaves them, such as the PCB editor's list for picking one of several objects (Hyprland, focus follows mouse) | `no_follow_mouse` for `steam_app_altium` |
| 23 | Altium crashes now and then when a schematic is first drawn (`System.AccessViolationException`) | `SHFolder.dll` proxy holds DXVK's allocator lock while DXVK imports a new swapchain's images (a race fixed upstream after DXVK 3.1.1) |
| 24 | Unplugging the monitor Altium is on (a dock): Altium disappears | `SHFolder.dll` proxy moves Altium's windows left on no monitor onto the nearest one; `refresh-displays.sh` does it for every program (`onscreen.exe`) |
| 25 | `explorer.exe`, then Altium, exit after a monitor goes away (`X Error of failed request: BadRROutput`) | XRandR filter drops X errors about outputs and windows that no longer exist instead of letting Xlib end the process |
| 26 | Online installer: Sign In opens no browser, then "Time to login has expired" | `start.exe` shim in the bottle's `syswow64` folder (the installer signs in from a 32-bit helper the `cmd.exe` shim cannot reach) |
| 27 | Orange box over the installer's dialogs, hiding the Sign In dialog (Hyprland) | Hyprland hook sends the dialog's glow window to the back |
| 28 | Empty white 800x800 box next to the installer (Hyprland) | zero-size filter (`LD_PRELOAD`) keeps 0x0 windows unmapped, which Hyprland would resize to 800x800 |
| 29 | Altium hangs now and then on start: no window, or only the splash | `SHFolder.dll` proxy turns off the thread start and end calls of Altium's own `ucrtbase.dll`, which take a lock that deadlocks with Wine's loader lock |

[docs/how-it-works.md](docs/how-it-works.md) explains each one: symptom, root cause,
evidence and fix.

## What `install.sh` changes

| Where | What | Fix |
|---|---|---|
| `Program Files\Altium\AD26\cmd.exe` | shim: opens `cmd /c start <url>` links directly, passes everything else to the real `cmd.exe` | 6 |
| `Program Files\Altium\AD26\SHFolder.dll` | proxy: forwards to `shell32`, supplies missing `user32` functions, raw RSA for Simbeor, schematic text, layered windows, Direct2D arcs, stale panel focus, Direct3D state swaps, DXVK allocator lock, windows left on no monitor | 9, 10, 12, 13, 14, 15, 16, 18, 23, 24 |
| `Program Files\Altium\AD26\dxvk.conf` | `dxgi.syncInterval = 0` | 11 |
| `windows\syswow64\start.exe` | shim for 32-bit programs: opens `start <url>` links directly, passes everything else to Wine's 64-bit `start.exe` (Wine's own copy kept as `start.exe.bak`) | 26 |
| `<bottle>/altium-on-linux/vulkan/` | Vulkan layer (Linux library and manifest) that waits for each present | 11 |
| `<bottle>/altium-on-linux/fdguard/` | fd guard and XRandR filter (Linux libraries, preloaded into the bottle's 64-bit Wine processes); log to `<bottle>/altium-on-linux/logs/fd-guard.log` and `xrandr-filter.log` | 20, 21, 25 |
| `<bottle>/altium-on-linux/zerosize/` | zero-size filter, 64- and 32-bit (preloaded into every Wine process of the bottle); logs to `<bottle>/altium-on-linux/logs/zero-size.log` | 28 |
| `windows\Fonts\segoeui-selawik*.ttf` | Selawik renamed to "Segoe UI", built locally from Microsoft's release | 8 |
| `HKCU\Software\Wine\DllOverrides` | `tabtip.exe` disabled | 4 |
| `HKCU\Software\Wine\AppDefaults\X2.EXE\DllOverrides` | `shfolder` = `native,builtin` | 9, 10, 12, 13, 14, 15 |
| `HKCU\Software\Wine\X11 Driver` | `EmulateModeset` = `N` | 19 |
| `HKCU\Software\Microsoft\Avalon.Graphics` | WPF software rendering | 4 |
| `HKCU\Software\Altium\Altium Designer {…}\…\AltiumPortal\Account` | `UnifiedLogin` = `1` | 5 |
| `HKLM\…\Fonts`, `HKCU\Control Panel\Desktop` | register the fonts, RGB font smoothing | 8 |
| `bottle.yml` | `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1`, `SteamAppId=altium`, `VK_ADD_IMPLICIT_LAYER_PATH=<bottle>/altium-on-linux/vulkan`, `LD_PRELOAD=<bottle>/altium-on-linux/fdguard/altium_fd_guard.so:<bottle>/altium-on-linux/fdguard/altium_xrandr_filter.so:<bottle>/altium-on-linux/zerosize/$LIB/altium_zero_size.so`; an **Altium Designer** program entry | 1, 2, 11, 20, 21, 25, 28 |

The shims are freestanding C (no C runtime, no Windows headers), the Vulkan layer is plain
C without Vulkan headers, and the fd guard and the XRandR and zero-size filters are plain
C; `scripts/build.sh` builds them all. Each source file explains what it does and why.

## Debugging

[docs/debugging.md](docs/debugging.md) covers the tools and the traps:

| Script | Use |
|---|---|
| `scripts/debug-launch.sh` | start Altium with Wine/DXVK logging, optionally opening a document |
| `scripts/watchdog.sh` | kill Altium above a memory limit, log its memory |
| `scripts/backtrace.sh` | native stacks of every thread (`winedbg`) |
| `scripts/dotnet-stacks.sh` | managed .NET stacks (`dotnet-dump`), for freezes |
| `scripts/wininfo.sh` | Wine's monitors and Altium's windows as Wine sees them |
| `scripts/refresh-displays.sh` | resync Wine's monitor list after a monitor change and move windows left on no monitor (run by the Hyprland hook) |
| `scripts/scan-missing-imports.py` | Windows functions Altium delay-loads that Wine lacks |
| `scripts/redraw-bench.sh` | time a view's redraws by posting scroll steps to it |
| `scripts/profile.py` | sampling profiler for Altium's UI thread (modules, functions, system calls) |
| `tests/test_rsa.py` | checks the raw RSA code against Python on random keys |
| `tests/test_arc.py` | checks the Direct2D arc code on random arcs |
| `tests/test_fdguard.py` | checks the fd guard on the host, with stand-in `ntdll.so` and `crypt32.so` |
| `tests/test_fdguard_wine.py` | checks the fd guard in a Wine process of the running bottle, and reproduces Wine's fsync close bug |
| `tests/test_xrandrfilter.py` | checks the XRandR filter on the host, with a stand-in libXrandr and libX11 |
| `tests/test_xrandrfilter_wine.py` | reproduces, in a throwaway prefix, Wine's divide by zero on a 0x0 monitor and the fatal `BadRROutput` after an output goes away, and checks the filter |
| `tests/test_zerosize.py` | checks the zero-size filter, 64- and 32-bit, on the host with a stand-in libX11 |
| `tests/test_layered_wine.py` | checks the image the `SHFolder.dll` proxy hands to Wine for layered windows, from top-down and bottom-up bitmaps, in a throwaway prefix |
| `tests/test_dxvk_alloc_wine.py` | reproduces DXVK's allocator race (swapchains recreated while threads create buffers) with the bottle's DXVK in a throwaway prefix, and checks the proxy's lock |

## Repository layout

```
config/            dxvk.conf, Hyprland rules
docs/              how-it-works, known-issues, debugging
fonts/             rename_to_segoeui.py (Selawik -> "Segoe UI")
registry/          .reg files imported by install.sh
scripts/           build, install, debugging tools (lib.sh is shared)
src/cmdshim/       cmd.exe shim (Altium's browser sign-in)
src/startshim/     start.exe shim for 32-bit programs (the installer's browser sign-in)
src/shfolderproxy/ SHFolder.dll proxy (user32 fallbacks, raw RSA, Direct2D text, layered windows, Direct2D arcs, VCL focus,
                   Direct3D state swaps, DXVK allocator lock)
src/presentwait/   Vulkan layer that waits for each present (Linux)
src/fdguard/       LD_PRELOAD library that protects Wine's server pipes (Linux)
src/xrandrfilter/  LD_PRELOAD library that hides 0x0 monitors from Wine (Linux)
src/zerosize/      LD_PRELOAD library that keeps 0x0 windows unmapped (Linux, 64- and 32-bit)
src/tools/         wininfo.exe, clearex.exe, postmsg.exe (diagnostics), displayfix.exe, onscreen.exe (monitor list)
tests/             raw RSA, Direct2D arc, fd guard, XRandR and zero-size filter tests, Wine reproductions
```

## License

[MIT](LICENSE). Altium Designer is proprietary and not included; you need your own
license. The Selawik fonts are built locally from Microsoft's release under the SIL
Open Font License and are not distributed here.
