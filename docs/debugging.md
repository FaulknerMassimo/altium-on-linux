# Debugging Altium under Wine

The techniques that found every fix in [how-it-works.md](how-it-works.md), wrapped in
scripts. All scripts take `--bottle NAME` (default `Altium`) and `--help`.

## Ground rules

- **One wineserver per prefix.** Each `flatpak run com.usebottles.bottles` gets its own
  `/tmp`, so a second one starts a second wineserver on the same prefix, and the two
  overwrite each other's registry. To act on a running bottle, enter its sandbox:
  `flatpak enter <instance> ...`, where the instance id is in
  `/proc/<pid>/root/.flatpak-info`. `scripts/lib.sh` (`in_running_bottle`) does this.
- **Registry changes made while Altium runs** must go through that running session
  (`wine reg add` inside it). Edits made any other way are lost when the session exits
  and Wine saves its in-memory registry.
- **The Bottles sandbox cannot read this repository.** Files it needs (registry files,
  tools, logs) go to `<bottle>/altium-on-linux/`.
- **Close the Bottles window before changing `bottle.yml`**: it keeps its own copy and
  writes it back.
- **Watch memory** when testing anything that might loop (`scripts/watchdog.sh`); a
  leak of 55 MB/s fills 32 GB in ten minutes.
- `pkill -f PATTERN` also matches the shell running it if the pattern appears in the
  command line; anchor or bracket the pattern (`'[X]2.EXE'`).

## Logs: `scripts/debug-launch.sh`

Starts Altium with the same environment Bottles uses and writes Wine and DXVK output to
`<bottle>/altium-on-linux/logs/`. Pass a document to open it straight away, which
reproduces problems without clicking through the UI:

```bash
scripts/debug-launch.sh --debug fixme-all,warn+seh,+timestamp,+loaddll \
    "$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/Altium/drive_c/users/Public/Documents/Altium/MyBoard.PcbDoc"
```

`--env NAME=VALUE` (repeatable) adds a variable after the bottle's own, for A/B runs:
`DISABLE_ALTIUM_PRESENT_WAIT=1` starts Altium without the present-wait layer
(how-it-works §11), `ALTIUM_PRESENT_WAIT_NO_DAMAGE=1` makes it wait for present
completion only (its first version), and `ALTIUM_PRESENT_WAIT_DEBUG=1` makes it log every
wait (`altium-present-wait: present 31 at 223188.866608 s: waited 0.73 ms, ended by damage
(swapchain 0x5555..., present 0, wait 2)`, the time being `CLOCK_MONOTONIC`).
`grep VkLayer /proc/<pid>/maps` shows whether the layer is loaded.
`ALTIUM_KEEP_D2D_SWAPS=1` leaves out the proxy's Direct2D state-swap hook (how-it-works §18).

To measure a view's redraw rate, start Altium with `ALTIUM_PRESENT_WAIT_DEBUG=1`, post
wheel steps to the view faster than it redraws (two `postmsg.exe` runs at once, one
scrolling down and one up, 150 steps each 4 ms apart, so the view ends where it started)
and wait until no present has been logged for 2 s. Every step is one redraw; the number of
presents over the time between the first and the last is the rate.

Useful `WINEDEBUG` channels:

| Channel | Shows | Found |
|---|---|---|
| `warn+seh` | every exception, with a short backtrace; `OutputDebugString` messages | the PCB crash (`c0000409`) |
| `+loaddll` | every DLL load with its base address, to map a crash address to a module | `simbeor_sdk.dll` |
| `+bcrypt` | cryptography calls and arguments | `BCRYPT_PAD_NONE` |
| `+exec,+winebrowser,+process` | `ShellExecute`, link opening, program starts | `cmd /c start` |
| `+start` | Wine's `start.exe` | the `search_path` overflow |
| `+thread` | thread names and exits (`RtlProcessFlsData`) | thread-pool workers |
| `fixme+d2d` (after `fixme-all`) | Direct2D functions Wine has not implemented, each time one is called | `AddArc` |
| `+timestamp` | a timestamp on every line | timing |

Wine's timestamps are seconds of monotonic time and do not count suspend; to get wall
clock time, anchor on the log file's creation time and the first timestamp in it.

## Freezes: `scripts/backtrace.sh` and `scripts/dotnet-stacks.sh`

`backtrace.sh` attaches `winedbg` to X2.EXE, prints `bt all` to a file and summarises
which modules each thread is in. A UI thread sitting in `uiautomationcore` is how the
`tabtip.exe` loop showed up. Note the CPU of each thread first (`top -H -p <pid>`): an
idle UI thread means a deadlock or an invisible modal dialog, a busy one means a loop.

.NET frames show up in `winedbg` only as `coreclr+0x...`. `dotnet-stacks.sh` names
them: it downloads Microsoft's `dotnet-dump`, runs it inside the bottle against Altium's
own .NET runtime (`DOTNET_ROOT=C:\Program Files\Altium\AD26\System\DotNet\Runtime`),
writes a full dump and prints `clrthreads` and `clrstack -all`. Quirks under Wine:

- The default `--type Heap` dump is a few hundred KB and useless; use `--type Full`.
- `dotnet-dump ps` and `dotnet-stack ps` fail (Wine cannot enumerate `\\.\pipe\`); pass
  `-p <pid>` with the Windows PID (`winedbg --command "info proc"` lists it in hex).
- `dotnet-stack report` collects fine but its parser calls
  `tdh.dll!TdhGetEventInformation`, which Wine lacks, and crashes.

### A thread waiting for wineserver forever

When an idle UI thread waits on a lock (`/proc/<pid>/task/*/syscall`: `202 <address>
0x80`), find the lock's owner (the `int` at address + 8, as seen inside the Flatpak,
matches the last field of `NSpid` in `/proc/<pid>/task/*/status`) and see what it is doing.
If the owner is in `read()` (`0 <fd>`) on a pipe that wineserver also holds
(`/proc/<wineserver pid>/fd`) while wineserver sits in `epoll_wait`, the owner has
probably lost its pipes ([how-it-works §20](how-it-works.md#20-freezes-wine-threads-lose-their-pipes-to-wineserver)).
`winedbg` cannot help here: attaching starts a thread, which needs the loader lock.

- **Stack without a debugger:** the last two fields of `/proc/<pid>/task/<tid>/syscall`
  are the stack pointer and the program counter. Read the stack from `/proc/<pid>/mem`,
  keep the values that point into executable mappings (`/proc/<pid>/maps`), and turn each
  into `module+offset`. Wine's unix libraries are stripped, but `readelf
  --debug-dump=frames` gives every function's range, and Wine's TRACE format strings in a
  function name it (`client_surface_present` and `client_surface_update` are even
  exported). gdb attaches (with `-iex 'set architecture i386:x86-64' -ex 'handle all
  nostop noprint pass'`; without the first it falls back to 32-bit) but cannot unwind
  these frames.
- **Each thread's server fds:** `gdb -p <pid>` with `$gs_base` per thread gives its TEB.
  The Wine thread ID is at TEB+0x48, and the request, reply and two wait fds are four
  `int`s at TEB+0x384 (soda-11.0-7). A thread is broken if its pipes' inodes are not from
  the same time (the request and wait pipes are created together), if two threads have the
  same fd number, or if wineserver no longer holds the pipe.
- **The fd guard's log**, `<bottle>/altium-on-linux/logs/fd-guard.log` (also in the
  `debug-launch.sh` log): `refused stray close of fd N ... called from <module>+<offset>`
  and a backtrace, for each close it refused. `ALTIUM_FD_GUARD=log` (with `--env`) lets
  them through and only logs them.

## Slow redraws: `scripts/redraw-bench.sh` and `scripts/profile.py`

`redraw-bench.sh` measures how expensive a view's redraws are. With a document open, it
posts mouse-wheel steps to the view's window (`build/postmsg.exe`, so the real keyboard
and mouse stay free and nothing takes focus) and reports the CPU time of Altium's UI
thread, which is the process's first thread. Every step renders the view again; half go
down and half back up, so the view ends where it started. The UI thread uses some CPU
even when left alone (8% here, timers and wineserver round trips); the script measures
that first and reports what the redraws add:

```
$ scripts/redraw-bench.sh
==> Scrolling SchView 000105e0 (10 steps, 1500 ms apart, 3 runs)
run 1: 10 steps, 0.87 s UI-thread CPU above idle (87 ms per step); idle 8%, 2.2 s total
run 2: 10 steps, 0.66 s UI-thread CPU above idle (66 ms per step); idle 8%, 2.2 s total
run 3: 10 steps, 0.66 s UI-thread CPU above idle (66 ms per step); idle 8%, 2.2 s total
```

The default view is the schematic (`SchView`); `--class` takes another class from
`scripts/wininfo.sh` (only `SchView` has been tried). Compare runs within one session, or
fresh sessions with each other: the same redraws cost twice as much in a session that had
been running for an hour (and had shown an error dialog).

`profile.py` is a sampling profiler for the UI thread. Every 2 ms it stops the thread
with ptrace (Wine processes accept any tracer), records the instruction pointer, the
system call in progress and the stack words that look like return addresses, and lets
it go. Run it around the benchmark:

```bash
scripts/profile.py --save build/redraw.json -- scripts/redraw-bench.sh --runs 1
scripts/profile.py --load build/redraw.json --focus win32u   # report again, other filter
```

The report counts the samples taken while the thread was running (`--all` adds the
sleeping ones) by module and by function, at the top of the stack ("self") and anywhere
on it ("inclusive"), and by system call. How to read it:

- Function names are the nearest exported symbol before the address (pefile for Windows
  DLLs, `nm -D` for Unix libraries; `.so` files of the Flatpak runtime are read through
  `/proc/<pid>/root`). Wine's DLLs, DXVK and Altium export little, so a name is a hint
  about where the code is, not proof; code far from any export is listed as
  `(unexported code)`. Without pefile the DLLs are listed by module only (the
  `scan-missing-imports.py` docstring shows how to get it, or `uv run --with pefile`).
- .NET code, compiled at runtime, shows up as `[anon]`; use `dotnet-stacks.sh` or the
  decompiler below to see what it does.
- Wine's Unix side runs on its own stack, so a sample taken in `win32u.so` or
  `ntdll.so` shows the Unix frames but not the Windows code that called them.
- `read` called from `ntdll.so` is a thread waiting for wineserver's reply: a high share
  means many server round trips.
- Sampling costs the thread a little time per sample; benchmark without the profiler.

For the schematic (how-it-works §12) the first profile was mostly `d2d1.dll` and
FreeType, which pointed at Direct2D text rather than GDI+.

## .NET code: decompiling Altium's assemblies

Much of Altium is .NET (`file X.dll` says "Mono/.Net assembly"), and reading the
decompiled code is the quickest way to see what it asks Windows for. To find which
assembly holds a class or setting, `grep -la NAME "$AD/System"/*.dll` (`$AD` is Altium's
folder in the bottle). ILSpy's command-line decompiler runs without the .NET SDK, from
its NuGet package and a .NET runtime (`dotnet --list-runtimes`; the package's
`tools/net*` folder names the version it needs):

```bash
curl -sSL -o build/ilspycmd.nupkg https://www.nuget.org/api/v2/package/ilspycmd
unzip -q build/ilspycmd.nupkg -d build/ilspycmd
dotnet build/ilspycmd/tools/net10.0/any/ilspycmd.dll -p -o build/decompiled/painter \
    -r "$AD/System" "$AD/System/Altium.Sch.Painter.dll"
```

`-p` writes a project with one file per class. Keep the output to yourself: it is
Altium's code.

## Missing functions: `scripts/scan-missing-imports.py`

Delay-loaded imports are resolved on first call, so a function Wine lacks only fails
when some code path reaches it (`External exception C06D007F`). The scanner compares
every delay import of Altium, its extensions and WebView2 with the runner's DLL exports
in a second. Needs `pefile`. For crashes rather than missing functions, map the crash
address to a module with a `+loaddll` log, then look for its imports.

## Windows: `scripts/wininfo.sh`

Lists Wine's display adapters and monitors (in physical pixels, as Altium sees them), then
Altium's windows (class, title, rect, style, exstyle, owner, parent). Compare the monitors
with `xrandr --listmonitors`: Wine's primary is at (0,0) and every other monitor is offset
by the X11 primary's position. A missing monitor, two adapters flagged primary (`flags`
0x4) or a size that X11 does not have is the stale list of
[how-it-works §19](how-it-works.md#19-altium-drawn-offset-after-a-monitor-goes-away-and-comes-back);
`scripts/refresh-displays.sh` repairs it in a running session. Compare the windows with
`hyprctl clients -j` to find windows the compositor places differently from Wine, or
windows that should not be top-levels at all.
`build/clearex.exe <hwnd> <bits>` clears extended-style bits on a live window for
experiments.

`build/postmsg.exe <hwnd> <msg> <wparam> <lparam> [count] [delay ms]` (hex, except count
and delay) posts a window message, for example to drive a view while you keep working in
another window. Run these tools in the bottle's session (`wine_running` in
`scripts/lib.sh`, as `wininfo.sh` does). Useful messages: `WM_MOUSEWHEEL` (`20a`; wParam
`ff880000` scrolls down one step, `00780000` up; lParam is the screen point, `yyyyxxxx`)
and `WM_CLOSE` (`10`) to the main window to close Altium. Altium ignores the Ctrl bit in
a posted wheel message (it checks the real key state), so zooming needs the keyboard. Do
not resize Altium's main window from the compositor in tests (`hyprctl dispatch
resizewindowpixel`): twice in a row it raised `Control 'View…' has no parent window`
(before the fix in [how-it-works §16](how-it-works.md#16-control-view-has-no-parent-window-stale-active-control)).

Posted mouse clicks (`WM_LBUTTONDOWN`/`WM_LBUTTONUP`) on the schematic view do not select
anything; Altium seems to check the real button state. Posted `WM_MOUSEMOVE`s do not update
the PCB heads-up display either (it follows the real cursor), and `wininfo.sh` lists only
visible windows, so a hidden HUD is missing from it. The main window's handle changes
from run to run: look it up with `wininfo.sh` (`cls=TDocumentForm`) before sending
`WM_CLOSE`. After Altium exits, its WebView2 processes can keep the bottle's wineserver
running, and `debug-launch.sh` refuses to start until they are gone; once `X2.EXE` has
exited, `in_running_bottle "$RUNNER_DIR/bin/wineserver" -k` ends them.

To see a window without screenshotting the desktop, which may show something else on
top, capture it through X11: `xprop -root _NET_CLIENT_LIST` lists Altium's X window ids,
and `import -window <id> out.png` (ImageMagick) saves that window's contents even when
another window covers it. The capture is what Wine copied into the top-level window, so
it also shows whether a view is a frame behind: post one wheel step at a time (down,
down, up), capture after each, and measure how far the content moved. A view that is a
frame behind shows each step one step late, and the first not at all.

## Reading Wine's source

Most root causes were confirmed in Wine's source: `programs/start/start.c`
(`search_path`), `dlls/combase/combase.c` (`CoWaitForMultipleHandles`),
`dlls/winex11.drv/init.c` (`needs_offscreen_rendering`,
`X11DRV_client_surface_present`), `dlls/ntdll/loader.c` (when DLL-load notifications
fire relative to import resolution), `dlls/bcrypt/gnutls.c` at tag `wine-11.0`
(RSA decryption), `dlls/d2d1/device.c` (`d2d_device_context_draw_glyph_run`),
`dlls/d2d1/geometry.c` (`d2d_geometry_sink_AddArc`, how path figures are stored),
`dlls/dwrite/font.c` (recommended rendering mode, glyph cache), `dlls/dwrite/main.c`
(default rendering parameters) and `dlls/win32u/vulkan.c` (`win32u_vkQueuePresentKHR`,
which copies off-screen windows right after the host present). GitHub's `wine-mirror/wine`
serves raw files when GitLab's bot protection blocks scripted downloads; the runner's own
source is Valve's `ValveSoftware/wine` (branch `bleeding-edge` for soda's
"experimental.bleeding.edge" builds). Below Wine: Mesa's
`src/vulkan/wsi/wsi_common_x11.c` (the X11 present queue and present wait) and Xwayland's
`hw/xwayland/xwayland-present.c` (when copies are done and reported complete).
