# Known issues

What still goes wrong, what is known about each problem, and where to dig next.
Contributions welcome: [debugging.md](debugging.md) has the tools.

## Panels jitter when dragged across monitors (mixed scaling)

Dragging a floating panel, especially between monitors, makes it jump back and forth;
once, after that, Altium reported `Control 'View…' has no parent window`. That error has
other triggers too and is probably not caused by the scaling
([below](#control-view-has-no-parent-window)).

Likely cause: with Hyprland's `xwayland:force_zero_scaling = true` (Omarchy's default)
and monitors with different scales, X11 and Hyprland disagree about coordinates. Here a
2880x1800 laptop at 1.5x and a 2560x1440 monitor at 1x:

| | Laptop | External monitor |
|---|---|---|
| X11 (`xrandr --listmonitors`) | 2880x1800 at x=0 | starts at x=2880 |
| Hyprland (logical) | 1920x1200 at x=0 | starts at x=1920 |

Windows that Altium positions itself (panels while dragging) land 960 px off, Altium
corrects, and the two keep fighting. Toggling `force_zero_scaling` with `hyprctl eval` did
not update XWayland's layout until a restart, so this is untested. Things to try: put the
external monitor at x=0 (then its X11 and logical coordinates match), or turn
`force_zero_scaling` off and accept blurrier X11 apps on the laptop screen.

## `Control 'View…' has no parent window`

An error dialog, `Control 'View4296989184' has no parent window at 00000014003A377C`
(the number changes), that comes and goes. Seen:

- after closing the floating Messages panel that pops up when a project with warnings
  opens;
- while a schematic passed on the command line was loading, half a minute after start;
- after resizing the main window twice from the compositor (`hyprctl dispatch
  resizewindowpixel`) with a schematic open;
- once after dragging a floating panel across monitors.

Two later attempts to close the Messages panel, one through closing and reopening the
project, did not bring it back.

What happens, from the stack in a `warn+seh` log and from reading the running process
(Altium 26.10.1). X2.EXE ships no symbols, so these names come from Delphi's class
tables (VMTs) in the binary:

1. Altium's panels are plain `TCustomForm`s hosted inside the dock or floating windows.
   Closing the Messages panel hides its floating window and removes the panel's content
   (a `TViewComponent` named `View<address>`) from the form.
2. In the failing case that form's `ActiveControl` (and its focused control) still
   pointed at the removed view, which by then had neither a parent nor a window.
3. The next focus change sends that form `CM_DEACTIVATE`. `TCustomForm.Deactivate`
   walks up from the active control to its top-most parent and sends it the message.
   Asking a parentless control for its window handle makes VCL create the window,
   which needs a parent, so it raises (`TWinControl.CreateWnd`, `X2.EXE+3A377C`).

Why the form keeps the stale pointer is not known. VCL clears it when a control is hidden
or removed, and in two close cycles traced with gdb (breakpoints on
`SetFocusedControl`, `DefocusControl`, `RemoveControl` and the focus calls in
`user32`) it did. For both pointers to end up on the view, the view's own window must
have got keyboard focus after that clean-up, probably while its child windows were
being destroyed. When it hides a focused window, Wine moves the focus to that window's
parent. This is a guess; the error did not happen while the tracer was attached.

**Workaround:** close the dialog with **Continue** (not "Send and Close", which sends a
crash report); Altium keeps working.

Next steps: catch it under the tracer to see where the focus comes from. Or work around
it in the `SHFolder.dll` proxy: replace the VCL routine that sends
`CM_ACTIVATE`/`CM_DEACTIVATE` to the active control (`X2.EXE+3512D0`, found by
byte pattern) so that a detached control clears the form's stale fields instead of
being given a window. That is what VCL does when a control is removed normally.

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

The log shows `wine client error:<thread id>: write: Bad file descriptor` (or `read:`): the
pipe between a thread and wineserver was closed under it, and that thread dies. Altium then
hangs, at the latest when it is closed: the main window stays up after `WM_CLOSE`, and
threads wait for the loader lock and for an Altium critical section
(`RtlpWaitForCriticalSection ... wait timed out`).

- First seen right after the simulation modules (`SimModel.dll`) loaded and DXVK unloaded
  Proton's stand-in Intel driver (`igd10iumd64.dll`); Altium exited 20 minutes later.
- Seen three times in both runs of a test build of `SHFolder.dll` that imported
  `user32.dll` (for the heads-up display fix, [how-it-works §14](how-it-works.md#14-heads-up-display-frozen-in-the-pcb-view-layered-child-windows)):
  14 s after start, right after the PCB view's swapchain was created and before the HUD was
  first drawn, and twice about two minutes in, a second or so after a burst of Altium
  worker threads exited. Both runs hung on close. A run of the previous proxy (3.5 minutes)
  and a 12-minute run of the released fix, which looks `user32` up at run time, did not show
  it, and neither did the 18 runs logged before. How an import could close another thread's
  file descriptor is not known; this may be chance.

Next step: build the proxy with `user32.dll` imported again, start Altium with
`scripts/debug-launch.sh`, attach `strace -f -e trace=close,dup2,dup3` to X2.EXE and wait two
minutes, to see which code closes the pipe.

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
