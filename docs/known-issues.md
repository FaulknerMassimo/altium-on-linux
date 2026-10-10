# Known issues

What still goes wrong, what is known about each problem, and where to dig next.
Contributions welcome: [debugging.md](debugging.md) has the tools.

## Heads-up display is a dark box

The heads-up display (the translucent box at the top-left of the PCB view with
coordinates, layer, snap and violations) shows as an empty dark rectangle.

- Wine reports the window (`TForm_HeadsUpTranslucent`) at the right place, as a
  layered child (`WS_EX_LAYERED`) of the D3D view `View_Graphical`
  (`scripts/wininfo.sh`).
- Hyprland lists a separate window with that title, parked off-screen (at -960,0 and
  later 0,0). It looks like the off-screen X window Wine creates for a child that is
  drawn with DirectX, managed by the compositor as a normal top-level instead of being
  composited back into the view.
- Clearing `WS_EX_LAYERED` at runtime (`build/clearex.exe`) did not change anything.

**Workaround:** press Shift+H in the PCB editor to turn the heads-up display off. The
same information is in the status bar.

## Panels jitter when dragged across monitors (mixed scaling)

Dragging a floating panel, especially between monitors, makes it jump back and forth;
once, after that, Altium reported `Control 'View…' has no parent window`. The same error
came back on a single laptop screen after resizing the main window twice from the
compositor (`hyprctl dispatch resizewindowpixel`) with a schematic open, so window
resizes may be enough to reproduce it.

Likely cause: with Hyprland's `xwayland:force_zero_scaling = true` (Omarchy's default)
and monitors with different scales, X11 and Hyprland disagree about coordinates. Here a
2880x1800 laptop at 1.5x and a 2560x1440 monitor at 1x:

| | Laptop | External monitor |
|---|---|---|
| X11 (`xrandr --listmonitors`) | 2880x1800 at x=0 | starts at x=2880 |
| Hyprland (logical) | 1920x1200 at x=0 | starts at x=1920 |

Windows that Altium positions itself (panels while dragging, the heads-up display)
land 960 px off, Altium corrects, and the two keep fighting. Toggling
`force_zero_scaling` with `hyprctl eval` did not update XWayland's layout until a
restart, so this is untested. Things to try: put the external monitor at x=0 (then its
X11 and logical coordinates match), or turn `force_zero_scaling` off and accept blurrier
X11 apps on the laptop screen.

## Intermittent freeze while a project loads

Seen twice, not on later runs: the PCB appears but nothing responds. Managed stacks
(`scripts/dotnet-stacks.sh`) showed the UI thread in

```
Altium.Har.Core.Project.HarnessReader.LoadDocumentsListParallel
  -> System.Threading.Tasks.Parallel.ForEach -> Task.Wait()
```

with no live thread-pool worker to run the tasks it waits for. One worker OS thread had
disappeared without .NET noticing (`clrthreads` still listed it, `winedbg` did not). The
project used schematic harness files (`.Harness`). Next step: catch it again with
`WINEDEBUG=+thread,+seh` to see which thread exits and how.

## Lost connection to wineserver

Seen once: the log shows `wine client error: write: Bad file descriptor` right after the
simulation modules (`SimModel.dll`) loaded and DXVK unloaded Proton's stand-in Intel
driver (`igd10iumd64.dll`); Altium then hung on the loader lock and exited 20 minutes
later. Cause unknown.

## Installer sign-in opens no browser

The installer's sign-in step ("use your browser to complete the sign in process")
starts `Authorizer.exe`, which runs `cmd /c start <url>` and hits the same `start.exe`
overflow as Altium (see [how-it-works.md §6](how-it-works.md#6-browser-sign-in-nothing-opens-wine-startexe-overflow)).
The `cmd.exe` shim does not help here: `Authorizer.exe` is 32-bit and runs from a
temporary folder the installer creates. Running the installer again skipped the
sign-in step and installed normally; sign in afterwards from Altium.

## Smaller things

- **Orange box** at the bottom-left of the PCB view, origin unknown.
- **"Not Signed In"** after starting Altium with a file on the command line; the license
  still works. Sign in from the account menu if needed.
- **Starting from Bottles once hung** at about 110 MB with no window; the process was gone
  before it could be inspected. A second start worked.
- **The embedded WebView2 sign-in** never renders; browser sign-in replaces it. The
  WebView2 Home page does render.
- **A newer runner may fix some of this and break other things:** upstream Wine has
  `GetPointerFrameInfoHistory` and a SymCrypt `bcrypt` (which should make the Simbeor
  workaround unnecessary) but still lacks `InheritWindowMonitor`. Runner-specific pieces
  here: the `steam_app_<SteamAppId>` window class and `tabtip.exe` are Proton features.

## Tried and not needed

| Tried | Result |
|---|---|
| Windows 7 mode for `msedgewebview2.exe` | already set by the runner; WebView2 sign-in still blank |
| Bottles virtual desktop | not applied to "Run executable" launches; awkward to use |
| `FontSubstitutes` Segoe UI -> Selawik | fixed only part of the UI; replaced by the renamed font |
| Disable TLS 1.3 in schannel | no effect; the PCB crash was `bcrypt`, not TLS |
| WineD3D (OpenGL) for Altium's Direct3D 11 | still one frame behind, and slower |
| `dxgi.syncInterval = 0` alone for the frame-behind views | PCB view mostly right, schematic still one frame behind; the present-wait layer fixes both ([how-it-works.md §11](how-it-works.md#11-pcb-and-schematic-views-one-frame-behind-vulkan-present-race)) |
| Faking an older Windows version for `InheritWindowMonitor` | the call has no version check |
