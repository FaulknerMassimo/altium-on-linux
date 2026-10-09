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

Useful `WINEDEBUG` channels:

| Channel | Shows | Found |
|---|---|---|
| `warn+seh` | every exception, with a short backtrace; `OutputDebugString` messages | the PCB crash (`c0000409`) |
| `+loaddll` | every DLL load with its base address, to map a crash address to a module | `simbeor_sdk.dll` |
| `+bcrypt` | cryptography calls and arguments | `BCRYPT_PAD_NONE` |
| `+exec,+winebrowser,+process` | `ShellExecute`, link opening, program starts | `cmd /c start` |
| `+start` | Wine's `start.exe` | the `search_path` overflow |
| `+thread` | thread names and exits (`RtlProcessFlsData`) | thread-pool workers |
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

## Missing functions: `scripts/scan-missing-imports.py`

Delay-loaded imports are resolved on first call, so a function Wine lacks only fails
when some code path reaches it (`External exception C06D007F`). The scanner compares
every delay import of Altium, its extensions and WebView2 with the runner's DLL exports
in a second. Needs `pefile`. For crashes rather than missing functions, map the crash
address to a module with a `+loaddll` log, then look for its imports.

## Windows: `scripts/wininfo.sh`

Lists Altium's windows as Wine sees them (class, title, rect, style, exstyle, owner,
parent). Compare with `hyprctl clients -j` to find windows the compositor places
differently from Wine, or windows that should not be top-levels at all.
`build/clearex.exe <hwnd> <bits>` clears extended-style bits on a live window for
experiments.

## Reading Wine's source

Most root causes were confirmed in Wine's source: `programs/start/start.c`
(`search_path`), `dlls/combase/combase.c` (`CoWaitForMultipleHandles`),
`dlls/winex11.drv/init.c` (`needs_offscreen_rendering`,
`X11DRV_client_surface_present`), `dlls/ntdll/loader.c` (when DLL-load notifications
fire relative to import resolution), `dlls/bcrypt/gnutls.c` at tag `wine-11.0`
(RSA decryption). GitHub's `wine-mirror/wine` serves raw files when GitLab's bot
protection blocks scripted downloads.
