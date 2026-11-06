# Known issues

What still goes wrong, what is known about each problem, and where to dig next.
Contributions welcome: [debugging.md](debugging.md) has the tools.

## Windows misplaced on the other monitor (mixed scaling)

With monitors at different scales, Hyprland misplaces X11 windows that open or move on a
monitor other than the focused one
([how-it-works §17](how-it-works.md#17-windows-split-across-monitors-hyprland-mixed-scaling)).
Making the external monitor X11's primary keeps Altium's windows on it, which covers
working on the external monitor. Still broken:

- Starting Altium while the laptop has the focus: the splash and the main window ask for
  the external monitor and are moved onto the laptop, and the splash comes apart again.
- Moving Altium windows onto the laptop. A floating panel dragged between monitors jumps
  back and forth: Altium moves the panel itself while it is dragged, and each position on
  the other monitor lands 960 px off. A panel left on the laptop is shown 1.5 times too big,
  and Altium saves that size. Once, after such a drag, Altium reported
  `Control 'View…' has no parent window`; that error had other triggers too and is fixed
  ([how-it-works §16](how-it-works.md#16-control-view-has-no-parent-window-stale-active-control)).
- Using the laptop alone after working docked: dialog positions saved on the laptop while
  the external monitor was primary are negative. Whether Altium pulls such a dialog back
  on screen has not been checked.

The real fix belongs in Hyprland: convert an X11 position with the monitor it lies on (in
both directions), move the window to that monitor, and leave override-redirect windows
where they ask to be. Hyprland exports the functions involved
(`CHyprXWaylandManager::xwaylandToWaylandCoords` and `waylandToXWaylandCoords`,
`CWindow::onX11ConfigureRequest`, `CWindow::unmanagedSetGeometry`,
`CWindowTarget::desiredGeometry`), so a plugin could hook them; not attempted. Turning off
`force_zero_scaling` does not fix the splash (see §17).

## Altium does not exit after File > Exit

Seen five times (2026-10-10, -12, -23, -24 and 11-06), each after a session of an hour or more:
the documents close, the license is released (the title becomes `Altium Designer (26.x) -
No License`), no dialog is open (`scripts/wininfo.sh` lists only `TDocumentForm` and
`TApplication`), and the process stays, at a few percent CPU. Short sessions exit in a few
seconds: a project, a schematic and a PCB opened and closed, and 50 headless starts and
closes (debugging.md).

Backtraces of the UI thread stopped in a different module each time (`scriptingsystem.dll`,
`helpadvisor.dll`, `edmsinterface.dll`), but read with the code they are one place:
X2.EXE's idle handler (`x2+0x123fbad` in 26.11.1) calls each module's idle method, which
runs that module's own copy of VCL's `Application.Idle` (`DoMouseIdle`: `WindowFromPoint`,
`FindDragTarget`). So the UI thread is not stuck in a module: X2's main message loop is still
running, and idling. The exit waits for something that never happens (a background task, a
thread, a message); what, is not known.

The fifth time (2026-11-06, after a 19-hour session) was captured in full
(`build/exit-hang-20261106-1055.*`: managed and native stacks of every thread). It showed:

- No thread is stuck. Every .NET thread is an idle thread-pool worker, the X2 worker
  threads are idle file monitors and empty queues, and the UI thread runs X2's main loop
  (`TApplication.Run`, `x2+0x35bf34`). This is not a deadlock: the shutdown stopped and
  nothing told it to carry on.
- The shutdown stopped after the license was released and before the license manager was
  shut down. 74 minutes after File > Exit, a modal "Sign In" window (WPF,
  `Altium.Portal.Factory.UnifiedLogin`) appeared. It came from SecurityServerManager's
  lease timer (`x2+0x11dd2c0`: interval 20 min, renews any lease that is due, via
  `x2+0x11e19b0` and `AltiumPortalCache.dll!GetPortal`), which was still running and still
  held a lease. In a normal exit, `DXP_Shutdown.log` shows `FinalizeSecurityServerMgr` 1.5 s
  after "Closing Documents".
- `DXP_Shutdown.log` was not rewritten in this session, so possibly the shutdown sequence
  that it logs never started. To confirm, check when a normal exit writes that file.
- Cancel on the Sign In, then a second `WM_CLOSE` to the main window, changed nothing: Altium
  treats itself as already closing.
- Side finding: a thread-pool callback in X2 (`x2+0x10807a0`) walks every global atom from
  `0xC000` up, one wineserver call per atom. That likely explains why wineserver uses about
  12% CPU while Altium idles.

Next time, before ending it: managed stacks of every thread (`scripts/dotnet-stacks.sh`),
native ones (`scripts/backtrace.sh`, which can take minutes; killing `winedbg` while it is
attached kills Altium), and `scripts/wininfo.sh`. Nothing is left to save by then, so ending
the bottle's processes is safe:
`bash -c 'source scripts/lib.sh; load_bottle Altium; in_running_bottle "$RUNNER_DIR/bin/wineserver" -k'`.
A "Sign In" window that appears on its own while Altium hangs on exit is the lease timer;
Cancel it.

## An Altium session ended after the lid was closed

An Altium session ended about 20 seconds after the laptop lid was closed (its `X2.EXE`
was left a zombie, with the rest of that Wine session still running and no wineserver).
Its output was not logged. X errors about outputs and windows that went away, which ended
`explorer.exe` and Altium after a dock was unplugged, are now dropped
([how-it-works §25](how-it-works.md#25-wine-processes-exit-after-a-monitor-goes-away-unexpected-x-errors));
whether that was the cause here is not known. To find out, run Altium with
`scripts/debug-launch.sh` and close the lid or unplug a monitor.

A monitor that comes back at 0x0 (a dock reconnecting) is a different problem, fixed:
[how-it-works §21](how-it-works.md#21-a-monitor-stuck-at-0x0-altium-shows-no-window-or-loses-them-all).

## Intermittent freeze while a project loads

Cause found on 2026-10-14: the thread that vanished had lost its pipes to wineserver to a
stray close, and the closes come from Wine itself. Under fsync, every thread that ends
closes the fd whose number is its fsync index
([how-it-works §20](how-it-works.md#20-freezes-wine-threads-lose-their-pipes-to-wineserver)).
The fd guard that `install.sh` installs skips those closes and refuses stray closes of the
pipes by other code. If Altium still freezes like this, look in
`<bottle>/altium-on-linux/logs/fd-guard.log` first: `refused stray close`, `skipped` and
`double close` lines name the code involved.

Seen three times: the documents appear but nothing responds, and the UI thread uses no
CPU. The first two times, managed stacks (`scripts/dotnet-stacks.sh`) showed the UI thread
in

```
Altium.Har.Core.Project.HarnessReader.LoadDocumentsListParallel
  -> System.Threading.Tasks.Parallel.ForEach -> Task.Wait()
```

with no live thread-pool worker to run the tasks it waits for. One worker OS thread had
disappeared without .NET noticing (`clrthreads` still listed it, `winedbg` did not). The
project used schematic harness files (`.Harness`).

The third time (2026-10-10, a few minutes after start, with a schematic open) showed what
the vanished thread leaves behind. The UI thread, 36 `.NET TP Worker` threads and a few
others were all waiting (`futex`, `FUTEX_WAIT_PRIVATE`) on one `pthread_mutex_t` in
`ntdll.so`'s `.bss`: Wine's `fd_cache_mutex`, which `server_get_unix_fd` holds while it
asks wineserver for the Unix file descriptor behind a handle. The mutex's owner was a
thread that no longer existed. It had died inside that section, so the lock was never
released: every thread that next needed a file descriptor not yet cached stopped, and the
.NET thread pool kept adding workers that stopped too. A thread dying there fits
[Lost connection to wineserver](#lost-connection-to-wineserver): when a thread's pipe to
wineserver fails, Wine's `abort_thread` ends that thread, whatever locks it holds. Not
confirmed: Bottles sends Wine's output to `/dev/null`, so there was no log.

To spot it and get Altium going again without a restart (this worked on 2026-10-10):

1. `cat /proc/$PID/task/*/syscall | cut -d' ' -f1-3 | sort | uniq -c`, with `$PID` the
   host PID of X2.EXE. Dozens of threads in `202 <address> 0x80` with the same address
   are waiting on one lock.
2. Check that the address lies in the anonymous mapping right after `ntdll.so` in
   `/proc/$PID/maps` (in soda-11.0-7 it is `ntdll.so`'s load address + `0xc2a00`). The
   owner's thread ID is the `int` at address + 8, as seen inside the Flatpak; compare it
   with the last field of `NSpid` in `/proc/$PID/task/*/status`. No match means the owner
   is dead.
3. Clear the lock word, owner and user count, and detach:

   ```bash
   gdb -q -batch -nx -iex 'set architecture i386:x86-64' -ex 'handle all nostop noprint pass' \
       -p $PID -ex "set {int}($ADDR+8) = 0" -ex "set {int}($ADDR+12) = 0" \
       -ex "set {int}$ADDR = 0" -ex detach
   ```

   Attaching interrupts the waits; when the threads resume, the kernel restarts each
   wait, which now finds the lock free, and the threads take it in turn.

The same check finds a thread whose pipes were taken over (the 2026-10-14 case): compare
the inodes of each thread's server fds, read from its TEB (debugging.md).

## Lost connection to wineserver

These are the stray closes of [how-it-works §20](how-it-works.md#20-freezes-wine-threads-lose-their-pipes-to-wineserver):
mostly Wine's own thread exits closing an fsync index as if it were a fd, which the fd
guard now skips (and logs, with what the fd was). The notes below are from before the
cause was found. Both observations fit it: DXVK starts and ends threads when it creates a
device or swapchain, and the second one came right after a burst of worker threads
ended.

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

Next step: confirm over long sessions that no `wine client error` comes back, and report
the Wine bug upstream (ValveSoftware/wine).

## Smaller things

- **Orange box** at the bottom-left of the PCB view, origin unknown. It may be the glow that
  Altium puts behind its dialogs, left behind by one
  ([how-it-works §27](how-it-works.md#27-orange-box-over-the-installers-dialogs-hyprland-ignores-window-opacity));
  not checked.
- **"Not Signed In"** after starting Altium with a file on the command line; the license
  still works. Sign in from the account menu if needed.
- **`Runtime error 217` when starting Altium** (once): the previous session's wineserver
  was gone, but its `services.exe`, `explorer.exe`, `winedevice.exe` and six WebView2
  helpers were still running. After ending them, Altium started. If processes are left in
  the bottle after Altium closes (`pgrep -af 'msedgewebview2|explorer.exe'`), end them
  before starting it again.
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
