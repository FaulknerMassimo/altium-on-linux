# How each fix works

Every problem below was hit on a fresh install and traced to a root cause before it was
fixed. Each section gives the symptom, the cause, the evidence and the fix. The
techniques used are in [debugging.md](debugging.md).

Tested on Altium Designer 26.10.1 (offline installer), Bottles 67.0 (Flatpak), runner
soda-11.0-7 (a Proton-based Wine 11.0), DXVK 3.1, vkd3d-proton 3.0.1, Hyprland 0.56
(Omarchy) on XWayland, Intel Arc graphics (Mesa 26.1).

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Installer: `Cannot get symbol u_charsToUChars from libicuuc Error: 127` | Wine's `icu.dll` forwards to a missing `icuuc68.dll` | `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1` |
| 2 | Dialogs hidden behind windows, ghost copies, frozen orange box | Hyprland tiles every X11 window, including Delphi dialogs | float rule on `steam_app_altium` (`SteamAppId=altium`) |
| 3 | Offline installer can't find its packages | Bottles' file picker exposes only the `.exe` (document portal) | Flatpak access to the setup folder |
| 4 | Freeze, black or corner-only windows, ~55 MB/s leak | Proton's `tabtip.exe` UI Automation loop | disable `tabtip.exe` |
| 5 | Embedded sign-in box stays blank | WebView2 sign-in under Wine | browser sign-in (`System.UnifiedLogin`) |
| 6 | "Use your browser to sign in", but no browser opens | Wine `start.exe` overflows a 260-char buffer on the sign-in URL | `cmd.exe` shim |
| 7 | Cursor jumps to window centres | Omarchy's `misc:focus_on_activate` + Wine's activate requests | `focus_on_activate = false` |
| 8 | Whole UI in Times New Roman | no Segoe UI | Selawik renamed to "Segoe UI" |
| 9 | File menu / right-click: `External exception C06D007F` | `user32!InheritWindowMonitor` missing in Wine | `SHFolder.dll` proxy |
| 10 | Opening a PCB kills Altium (`GnuTLS error: Decryption has failed`) | Wine's GnuTLS `bcrypt` ignores `BCRYPT_PAD_NONE` | `SHFolder.dll` proxy (raw RSA) |
| 11 | PCB view one step behind on layer switches | Wine copies the off-screen child window before vsync updates it | `dxgi.syncInterval = 0` (experimental) |
| 12 | Schematic text with missing strokes, slow schematic redraws | Wine's d2d1 picks the text rendering mode without the zoom transform, fills glyph outlines unantialiased | `SHFolder.dll` proxy (DirectWrite hooks) |

## 1. ICU error in the installer

The installer's sign-in step runs on .NET, which on Windows loads ICU from `icu.dll`.
Wine ships an `icu.dll` whose exports are all forwarders to `icuuc68.dll`, and the
runner has no `icuuc68.dll`. .NET finds `icu.dll`, `GetProcAddress("u_charsToUChars")`
fails with error 127 (`ERROR_PROC_NOT_FOUND`) and .NET aborts.

```
$ strings .../lib/wine/x86_64-windows/icu.dll | grep u_UChars
icuuc68.u_UCharsToChars_68
```

**Fix:** the bottle environment variable `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1` makes
.NET use Windows NLS instead of ICU. Altium itself already sets
`System.Globalization.UseNls` in `X2.runtimeconfig.json`; the variable matters for the
installer.

## 2. Hidden dialogs and ghost windows (Hyprland)

Hyprland tiles every X11 window. Delphi apps create an invisible application window and
owned dialogs; tiled, the dialogs end up behind the main window (the "orange box" that
cannot be moved), and moving things around produces duplicate frames.

**Fix:** float all of Altium's windows. To match them, the bottle sets
`SteamAppId=altium`: Proton's `winex11` then names the windows
`steam_app_altium`. Setting `SteamGameId` instead did not change the class. Without
either, every Proton app shares the class `steam_proton`. See
[`config/hyprland/altium.lua`](../config/hyprland/altium.lua).

Bottles' "Virtual desktop" option is an alternative, but it was not applied to "Run
executable" launches with this runner, and a fixed-size box is awkward to work in.

## 3. Offline installer finds no packages

Choosing `Installer.Exe` in Bottles' file picker goes through the Flatpak document
portal, which exposes that single file (`/run/user/1000/doc/<id>/Installer.Exe`), not
the `Altium Cache`, `Extensions` and `Metadata` folders next to it. The log shows the
installer looking for `Installer.cfg` in the portal folder.

**Fix:** give Bottles access to the extracted setup folder and run the installer from
there:

```bash
flatpak override --user --filesystem="$HOME/Downloads/OfflineSetup_Altium_Designer_Public_26_10_1" com.usebottles.bottles
```

## 4. Freezes, black boxes and a runaway memory leak (`tabtip.exe`)

Opening the sign-in dialog or the Advanced Settings dialog froze Altium: the dialog was
black (or only a corner rendered), CPU stayed at one core and memory grew by about
55 MB/s until the machine ran out.

A `winedbg` backtrace during the leak showed Altium's UI thread inside Wine's
`uiautomationcore`, and another thread marshalling UI Automation objects through
`oleaut32` (with matching `err:ole:marshal_object` errors). Proton starts `tabtip.exe`
(the helper that pops up the Steam Deck keyboard when a text field gets focus) and it
listens to UI Automation events from every window. When Altium opened these WPF
dialogs, its UI thread and Wine's UIA thread kept passing work back and forth and never
got out of it.

Killing `tabtip.exe` after the loop had started did not stop it; preventing it from
starting did: with `tabtip.exe` disabled the same dialogs render and memory stays flat.

**Fix:** `HKCU\Software\Wine\DllOverrides` `"tabtip.exe"=""` (disabled). The same .reg
file also switches WPF to software rendering (`Avalon.Graphics\DisableHWAcceleration`),
which was set while chasing the black boxes; it may no longer be needed.

## 5. Blank embedded sign-in

By default Altium signs in through an embedded WebView2 page. Under Wine it stays
blank. Altium also has a browser-based flow (open the system browser, then poll
Altium's "action wait" service until the sign-in completes), switched on by the
advanced setting `System.UnifiedLogin`, stored as
`HKCU\Software\Altium\Altium Designer {GUID}\DesignExplorer\Preferences\AltiumPortal\Account`
`"UnifiedLogin"="1"`.

The preference "Use external web browser" is unrelated: it controls ordinary links.

**Fix:** set `UnifiedLogin` (the installer does it once Altium has run once and created
its registry key).

## 6. Browser sign-in: nothing opens (Wine `start.exe` overflow)

With browser sign-in on, Altium shows "Use your browser to sign in" and nothing opens.
The installer's sign-in step does the same.

`Altium.Identity.Authentication.OpenBrowser` runs `cmd /c start <url>`. Running the
installer's `Authorizer.exe` under `WINEDEBUG=+start,+seh` shows:

```
trace:start:search_path Searching in 'Z:\home\...\https:\auth.altium.com\connect\' for 'authorize?client_id=...
trace:seh:dispatch_exception code=c0000005 (EXCEPTION_ACCESS_VIOLATION)
```

Wine's `start.exe` treats the URL as a relative file name and searches for it. In
`search_path()` (programs/start/start.c) the directory and the URL's last component are
concatenated into `WCHAR thisDir[MAX_PATH]` with `lstrcatW` and no length check. The
OAuth URL is about 270 characters, so `start.exe` crashes before it reaches
`ShellExecuteEx`. The same function is unchanged in upstream Wine master.

**Fix:** [`src/cmdshim`](../src/cmdshim/cmdshim.c), a 4 KB `cmd.exe` placed next to
`X2.EXE`. `CreateProcess` searches the application's folder before `system32`, so
Altium (and only Altium) runs it. `/c start <http(s) url>` is opened directly with
`ShellExecuteExW` (which reaches `winebrowser` and the desktop's default browser);
every other command line is passed unchanged to the real `system32\cmd.exe`.

## 7. Cursor jumping to window centres (Hyprland/Omarchy)

Omarchy sets `misc:focus_on_activate = true`. Wine sends `_NET_ACTIVE_WINDOW` requests
whenever Altium activates a window, which it does constantly (panels, popups, the modal
sign-in dialog). Hyprland focuses the window and warps the pointer to its centre.

**Fix:** a per-window `focus_on_activate = false` for `steam_app_altium`, the same
exception Omarchy makes for Telegram.

## 8. Times New Roman everywhere (Segoe UI)

Altium asks for "Segoe UI", which Wine does not ship, and the fallback was serif.
`FontSubstitutes` (`Segoe UI` -> `Selawik`) fixed only part of the UI, because part of
it looks fonts up by family name.

**Fix:** Microsoft's open-source [Selawik](https://github.com/microsoft/Selawik) (OFL)
is metric-compatible with Segoe UI, so layouts keep their sizes.
[`fonts/rename_to_segoeui.py`](../fonts/rename_to_segoeui.py) makes local copies whose
family name is "Segoe UI" (name IDs 1, 4, 16, and 6 for the PostScript name); every
text API then finds them. The fonts are built on your machine and not redistributed.
RGB font smoothing is set at the same time.

## 9. `External exception C06D007F` on the File menu (`InheritWindowMonitor`)

`C06D007F` is the delay-load "procedure not found" exception. Scanning every Altium
binary's delay-import table against the runner's Wine exports
([`scripts/scan-missing-imports.py`](../scripts/scan-missing-imports.py)) finds:

```
MISSING  X2.EXE                      user32.dll!InheritWindowMonitor
MISSING  AdvSch.dll                  user32.dll!GetPointerFrameInfoHistory
MISSING  EmbeddedBrowserWebView.dll  user32.dll!GetPointerInputTransform
```

`InheritWindowMonitor`, a recent addition to the Windows API, is called from X2.EXE
every time a popup form is created, with no version check (disassembled with capstone: the caller
fetches both window handles and calls it unconditionally). In upstream Wine 11.19 it is
still a commented-out stub in `user32.spec`. Faking an older Windows version therefore
does not help.

**Fix:** [`src/shfolderproxy`](../src/shfolderproxy/shfolderproxy.c), a proxy
`SHFolder.dll` in Altium's folder. X2.EXE imports `SHFolder.dll` at startup, so the
proxy loads before any Altium code runs; its two exports forward to `shell32` exactly
like Wine's own `shfolder`. When it loads, and for every DLL loaded later (via
`LdrRegisterDllNotification`), it points the delay-import slots of these functions at
small fallbacks: `InheritWindowMonitor` returns TRUE, the two pointer queries return
FALSE with `ERROR_NOT_SUPPORTED` (as on a machine without pen input). A slot is only
patched when `GetProcAddress(user32, name)` fails, so a Wine that implements a function
takes over automatically. An `AppDefaults\X2.EXE` DLL override makes sure the proxy
loads instead of Wine's builtin `shfolder`.

`llvm-readobj` cannot be used for the scan: it fails on Delphi binaries ("RVA ... for
debug directory not found"), which is why the script uses `pefile`.

## 10. Opening a PCB kills Altium (Simbeor, raw RSA in `bcrypt`)

Opening any PCB closed Altium with no dialog. Logged with `WINEDEBUG=warn+seh`:

```
GnuTLS error: Decryption has failed.
err:seh:NtRaiseException Unhandled exception code c0000409 flags 1 addr 0x6ffffa5dbef1
```

It is not a network problem (disabling TLS 1.3 changed nothing). With `+loaddll`, the
crash address falls inside `simbeor_sdk.dll`, the Simbeor impedance/stack-up engine the
PCB editor loads. Its FlexLM license check, under `+bcrypt`:

```
BCryptImportKeyPair(RSA, "RSAPRIVATEBLOB", 155 bytes)   a 512-bit private key
BCryptDecrypt(key, 64 bytes, ..., flags 0x1)            0x1 = BCRYPT_PAD_NONE (raw RSA)
GnuTLS error: Decryption has failed.
```

Wine 11.0's GnuTLS backend implements `BCryptDecrypt` for RSA with
`gnutls_privkey_decrypt_data`, which always strips PKCS#1 v1.5 padding and ignores
`BCRYPT_PAD_NONE`. Raw data has no such padding, so it fails with
`STATUS_INTERNAL_ERROR`; Simbeor throws a C++ exception on a worker thread, nothing
catches it, and the CRT terminates the process (`c0000409`). Upstream Wine has since
replaced the GnuTLS backend with SymCrypt (Microsoft's own library), so newer runners
will probably not need this.

**Fix:** the same proxy wraps `simbeor_sdk.dll`'s `BCryptImportKeyPair`,
`BCryptDecrypt` and `BCryptDestroyKey` imports (patched from the DLL-load notification,
which Wine sends after imports are resolved and before `DllMain`). It remembers RSA
private-key blobs; a `BCRYPT_PAD_NONE` decryption with one of them is done in
[`rsa_raw.c`](../src/shfolderproxy/rsa_raw.c): d = e⁻¹ mod (p-1)(q-1), then cᵈ mod n
with Montgomery multiplication, for moduli up to 2048 bits. Every other call goes to
Wine's `bcrypt`. [`tests/test_rsa.py`](../tests/test_rsa.py) checks it against Python's
`pow()` on random 512- to 2048-bit keys.

## 11. PCB view one step behind (experimental)

Switching from the top to the bottom layer showed the top layer until the next redraw;
switching back showed the bottom layer.

The PCB view is a child window. Wine's X11 driver renders every child window
off-screen (`needs_offscreen_rendering()` returns TRUE when the parent is not the
desktop) and, on each Present, copies the off-screen image into the top-level window
(`X11DRV_client_surface_present`). Altium presents with vsync (`VK_PRESENT_MODE_FIFO`),
so the X server only updates the off-screen image at the next vertical blank, after
Wine has already copied it: the copy is always the previous frame.

Routing Direct3D 11 through WineD3D (OpenGL) instead of DXVK did not help and was
slower.

**Fix (unverified):** [`config/dxvk.conf`](../config/dxvk.conf) with
`dxgi.syncInterval = 0`, so presents are immediate. DXVK reads `dxvk.conf` from the
working directory, which Bottles sets to Altium's folder.

## 12. Schematic text broken and redraws slow (Direct2D text)

Small schematic text lost strokes: `U_Cell` read as `I. Cell`, every underscore was
missing, and scrolling or zooming a sheet took close to a second per step.

The schematic view does not use GDI+, although `AdvSch.dll` imports it: it is drawn by
the .NET assemblies `Altium.Sch.Painter` and `Altium.GeometryMaker` through SharpDX
with Direct2D and DirectWrite, which are Wine's builtin `d2d1` and `dwrite` (on top of
DXVK's `d3d11`). A sampling profile of the UI thread during redraws
(`scripts/profile.py` around `scripts/redraw-bench.sh`) was mostly `d2d1` and
FreeType; decompiling the two assemblies (see debugging.md) showed how they
draw text.

Altium builds its geometry once in "world" units and applies the zoom as a Direct2D
transform (`SchPainterSettings.DefaultDrawTransform` scales internal units by 0.00064,
so 6.4 world units per mil). Text layouts are created at that size: a 10 pt label has
an em size of about 900. Altium leaves the text antialias mode at its default.
In Wine 11.0 (`dlls/d2d1/device.c`, `d2d_device_context_draw_glyph_run`):

- The rendering mode comes from `IDWriteFontFace_GetRecommendedRenderingMode` with the
  untransformed em size. At 100 and above it returns `OUTLINE`, so every run takes
  `d2d_device_context_draw_glyph_run_outline`: a new path geometry from the glyph
  outlines, triangulated on the CPU, on every frame. Wine's geometry fill ignores the
  antialias mode, so the glyphs are rasterised with hard pixel coverage and strokes
  thinner than a pixel disappear. Native Direct2D takes the transform into account.
- In the bitmap path, `dwrite` bypasses its glyph cache whenever a transform is set
  ("For now bypass cache for transformed cases" in `dlls/dwrite/font.c`), so FreeType
  renders every glyph again on every frame.

**Fix:** once Wine's builtin `dwrite.dll` has loaded, the `SHFolder.dll` proxy wraps two
slots of its vtables, which every font face and factory share (a short thread does it
outside the loader lock). Both wrappers act only on calls whose return address is in
Wine's builtin `d2d1.dll`:

- `IDWriteFontFace::GetRecommendedRenderingMode`: `OUTLINE` becomes
  `NATURAL_SYMMETRIC`, so text is drawn from antialiased glyph bitmaps.
- `IDWriteFactory2::CreateGlyphRunAnalysis`: for a uniform scale `s` (no rotation) and
  natural measuring, the run is passed on with em size, advances and offsets multiplied
  by `s`, the origin mapped through the matrix and no transform. The glyphs land in the
  same places (now hinted at their on-screen size) and the glyph cache applies again.
  Rotated text keeps the transform.

On a 317 KB sheet, ten scroll steps posted to the view took 3.6 to 4.4 s of UI-thread
CPU before and 0.8 s after, in fresh sessions (`scripts/redraw-bench.sh` measures this). If `dwrite.dll` or `d2d1.dll` is not Wine's builtin,
nothing is patched.
