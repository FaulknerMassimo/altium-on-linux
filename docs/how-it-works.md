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
| 11 | PCB and schematic views one frame behind (a selection shows up only with the next redraw) | Wine copies an off-screen child window before Mesa's X11 present has updated it | Vulkan layer that waits for each present to complete |
| 12 | Schematic text with missing strokes, slow schematic redraws | Wine's d2d1 picks the text rendering mode without the zoom transform, fills glyph outlines unantialiased | `SHFolder.dll` proxy (DirectWrite hooks) |
| 13 | Black box around part of the splash screen | Wine cuts transparent pixels out with an X shape and never paints them; Hyprland ignores X shapes | `SHFolder.dll` proxy (layered windows) |
| 14 | PCB heads-up display frozen in the view, a live copy in the corner of the screen on every workspace | Wine gives a layered child window its own top-level X window | `SHFolder.dll` proxy (layered child windows) |
| 15 | Schematic symbols drawn with arcs (inductors) blank | Wine's d2d1 does not implement `ID2D1GeometrySink::AddArc` | `SHFolder.dll` proxy (Direct2D arcs) |
| 16 | Error dialog `Control 'View…' has no parent window`, now and then | a panel form keeps a removed view as its active control; VCL creates a window for it on the next focus change | `SHFolder.dll` proxy (VCL `DoNestedActivation`) |
| 17 | With two monitors: splash split across them, Messages panel on the other screen, too big | Hyprland misplaces X11 windows that open or move on a monitor other than the focused one when the monitors' scales differ; Wine's primary monitor is the laptop | external monitor made X11's primary (Hyprland hook) |

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

## 11. PCB and schematic views one frame behind (Vulkan present race)

The views showed the frame before the current one. In the schematic, clicking a
component showed no selection; clicking a second one showed the first one selected. In
the PCB editor, switching from the top to the bottom layer showed the top layer until
the next redraw.

Both views are child windows drawn with Direct3D 11 through DXVK (the schematic with
Wine's `d2d1` on top), so they end up as Vulkan swapchains. Wine's X11 driver renders a
child window's swapchain off-screen (`needs_offscreen_rendering()` returns TRUE when the
parent is not the desktop). In win32u's `vkQueuePresentKHR`, as soon as the host
`vkQueuePresentKHR` returns, it copies the off-screen window into the top-level window
(`client_surface_present` -> `X11DRV_client_surface_present`). Upstream Wine does the
same.

At that point the host present has not happened yet. Mesa's X11 WSI
(`src/vulkan/wsi/wsi_common_x11.c`, `x11_queue_present`) only puts the image on a queue.
Its "WSI swapchain queue" thread waits for rendering to finish (in FIFO mode, for the
previous present to complete) and only then sends `PresentPixmap`, and in FIFO mode the
X server waits for its next tick before doing the copy. Wine's copy usually runs first
and gets the previous frame, and nothing copies the window again until the next present.

To see it without clicking, post three wheel steps (down, down, up) to the view and
capture the window through X11 after each one (see debugging.md). The content should
move one step, two steps, then back to one step:

| | down | down | up |
|---|---|---|---|
| schematic, no fix | 0 px | -30 px | -60 px |
| schematic, fix | -30 px | -60 px | -30 px |
| PCB, fix | -80 px | -160 px | -80 px |

The first attempt, `dxgi.syncInterval = 0` in [`config/dxvk.conf`](../config/dxvk.conf),
makes DXVK present without vsync (`VK_PRESENT_MODE_IMMEDIATE_KHR`), so the X server does
the copy as soon as it gets the image. Whether that happens before Wine's copy is a race.
With it, the PCB view usually got the right frame (it passed the test above without the
layer), but the schematic, whose Direct2D frames take longer to render, lost every time.
Routing Direct3D 11 through WineD3D (OpenGL) instead of DXVK did not help and was slower.

**Fix:** [`src/presentwait`](../src/presentwait/presentwait.c), a Vulkan layer loaded by
the host Vulkan loader (on the Linux side, inside the Bottles sandbox), between Wine and
Mesa. After `vkQueuePresentKHR` it calls `vkWaitForPresentKHR` with the present ID that
DXVK attaches to every present (DXVK enables `VK_KHR_present_id` and
`VK_KHR_present_wait` when the driver has them, as Mesa does). It returns once the X
server reports the present complete, and Wine then copies the new frame. Presents without
an ID, and devices without present wait, pass through unchanged. A wait gives up after
100 ms.

Each present waits about 19 ms. Xwayland does the copy at once but reports it complete on
its next tick (`xwl_present_execute` queues the completion for `crtc_msc + 1`; the tick is
a 17 ms timer for windows it does not show itself). DXVK presents from its own thread, so
Altium's UI thread does not block, but a view now redraws at most about 50 times a
second. `dxvk.conf` stays: in FIFO mode Xwayland would also delay the copy itself to a
tick, making each wait up to a tick longer.

`scripts/install.sh` copies the layer and its manifest
([`VkLayer_altium_present_wait.json`](../src/presentwait/VkLayer_altium_present_wait.json))
to `<bottle>/altium-on-linux/vulkan` and sets the bottle variable
`VK_ADD_IMPLICIT_LAYER_PATH` to that folder, so no other bottle loads it.
`DISABLE_ALTIUM_PRESENT_WAIT=1` turns it off for a run, and
`ALTIUM_PRESENT_WAIT_DEBUG=1` logs every wait.

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

## 13. Black box around the splash screen (layered windows and X shapes)

The splash screen had a solid black area to the right of the circuit board, where it
should be transparent, while the transparent areas above and left of it were fine.

The splash is five windows: an opaque centre (`StartUpWindow`) and four "shadow" windows
around it (left, right, top and bottom, drawn from `System\edp_splash_*.png`) that
X2.EXE draws with per-pixel alpha through `UpdateLayeredWindow` (not through its import
of it: wrapping that import caught nothing). Wine gives layered windows a 32-bit ARGB
visual, so Hyprland blends their partly transparent pixels correctly. But Wine 11 also
builds an X shape from the alpha channel: `set_surface_shape` (`dlls/win32u/dce.c`)
leaves out every pixel with alpha 0, and `x11drv_surface_flush`
(`dlls/winex11.drv/bitblt.c`) sets that shape before it puts the image, so X clips the
paint and the cut-out pixels are never written. A compositor that applies X shapes hides
them. On Hyprland (XWayland) the shape is not applied, and those pixels show whatever
the window's buffer held: opaque black for the right window and the right half of the
bottom one, transparent for the others. winex11's `ShapeLayeredWindows` setting is still
read but no longer used in Wine 11.

Evidence: during start-up, each splash window was captured through X11 (`XGetImage` and
`XShapeGetRectangles`) and the screen with `grim`. The X contents and shapes were right
(490, 200 and 126 rectangles for the right, top and bottom windows), and the black area
on screen was exactly the pixels outside the shapes of the right window and the right
half of the bottom one. `XGetImage` returns zeros outside a window's shape, so it cannot
show what the buffer really holds there.

**Fix:** Wine's `user32` passes `UpdateLayeredWindow` and `UpdateLayeredWindowIndirect`
on to `win32u!NtUserUpdateLayeredWindow` through its import table, which the
`SHFolder.dll` proxy points at a wrapper when it loads; this catches every caller,
however it found the function. For per-pixel alpha (`ULW_ALPHA` with `AC_SRC_ALPHA`)
from a 32-bit DIB section with any pixel of alpha 0, the wrapper passes a copy of the
source area in which those pixels are black with alpha 1. They stay invisible, the shape
covers the whole window and Wine paints every pixel. One difference remains: Windows
lets clicks through pixels of alpha 0, and these pixels now take them. Layered windows
of other processes (WebView2) are not covered.

With the fix the four shadow windows had a one-rectangle shape, and of the splash's
70,156 fully transparent pixels all but 230 matched the screen behind it within 3%;
before, 28,980 of them were pure black.

## 14. Heads-up display frozen in the PCB view (layered child windows)

The heads-up display (the box at the top left of the PCB view with the cursor
coordinates, layer, snap and the object under the cursor) did not update: its place in the
view showed an old copy of it, or of the board, while a live copy sat in the top-left
corner of the screen, on top of every window and on every workspace.

The HUD (`TForm_HeadsUpTranslucent`) is a layered child window (`WS_EX_LAYERED`, which
Windows allows for child windows since Windows 8) of the Direct3D view `View_Graphical`,
drawn with `UpdateLayeredWindow` and per-pixel alpha. In Wine 11,
`NtUserUpdateLayeredWindow` asks for a layered window surface (`get_window_surface` with
`create_layered`, `dlls/win32u/window.c`), and winex11's `X11DRV_CreateWindowSurface`
switches the window to the ARGB visual (`set_window_visual`), which destroys and recreates
its "whole window": `create_whole_window` (`dlls/winex11.drv/window.c`) always creates it on
the root window. A child window has no whole window otherwise, and is drawn into its
top-level window's surface. So the HUD became an override-redirect top-level X window at its
parent-relative position (0,0 on the screen), and Hyprland showed it above everything. In
the view, nothing drew the HUD's rectangle any more: Wine copies the Direct3D frames into the
main window clipped to the view's visible region, from which wineserver's `clip_children`
removes every visible child (`View_Graphical` has `WS_CLIPCHILDREN`), so the rectangle kept
whatever was last drawn there.

Evidence: `scripts/wininfo.sh` listed the HUD as a visible child of `View_Graphical`;
`xprop` on the extra X window showed the HUD's title and its Win32 styles
(`_WINE_HWND_STYLE` 0x56000000, a child) and XGetImage showed it override-redirect, 32-bit,
with the live HUD in it (background alpha 217). The main window's capture showed an older
HUD (different coordinates) at the HUD's place.

**Fix:** the `SHFolder.dll` proxy's `NtUserUpdateLayeredWindow` wrapper (§13) handles
child windows itself and does not call Wine: it applies the position and size, keeps the
image composited over black in a bitmap, and draws it into the window, which paints into the
main window like any child. A subclassed window procedure draws the bitmap again after
`WM_PAINT` (which Wine sends and Windows does not; VCL would fill the window with the form's
colour), stops `WM_ERASEBKGND`, and on hide, move or resize has the view repaint the area the
HUD left (wineserver repaints nothing for these windows, `set_window_pos` adds
`SWP_NOREDRAW`), so Altium renders the board there again.

Windows blends the HUD over the board; here it is drawn over black, because the view is
drawn on the X server and cannot be read back. The HUD is a dark box at 85% opacity, so over
the default dark background the difference is a few levels of grey; over lighter objects the
box is darker than on Windows. Layered windows that are not children (the splash) still go
through Wine.

The proxy looks up the `user32` functions it needs at run time instead of importing them: a
build that imported `user32.dll` lost a thread's wineserver connection in both test runs
(see [known-issues.md](known-issues.md#lost-connection-to-wineserver)).

## 15. Blank schematic symbols: inductors (Direct2D arcs)

Symbols drawn with arcs were blank in the schematic editor: an inductor showed its two pin
stubs and nothing between them. Full circles (the voltage source) were fine.

Altium's schematic painter (`Direct2DGraphics.DrawArc` in `Altium.GeometryMaker`) draws a
full circle with `DrawEllipse`, and any other arc as a path geometry with one figure:
`BeginFigure` at the arc's start point, `AddArc` to its end point, `EndFigure`. Wine 11.0's
`d2d_geometry_sink_AddArc` (`dlls/d2d1/geometry.c`) is a stub (`FIXME ... stub!`) that only
adds the end point as a new vertex. Wine stores a segment's type on the vertex it starts
from, and the stub leaves that one unset (`D2D_VERTEX_TYPE_NONE`), so the outline code
skips the segment: the arc is not even drawn as a straight line. An inductor from
Altium's libraries is eight quarter-circle arcs and nothing else.

Evidence: with `--debug fixme-all,fixme+d2d`, opening the "6_Oscillator" example
(`Examples/Simulation Quick Start`) logged the stub eight times, once per arc of the
inductor L1 (eight `RECORD=12` arcs owned by L1 in the `.SchDoc`).

**Fix:** once Wine's builtin `d2d1.dll` has loaded, a short thread in the `SHFolder.dll`
proxy creates a path geometry and replaces the `AddArc` slot of its sink's vtable, which
every path geometry shares. The replacement converts the arc
([`d2d_arc.c`](../src/shfolderproxy/d2d_arc.c)) as the SVG implementation notes describe
(centre from the end points, radii scaled up when too small to reach the end point) into
quadratic Bézier curves of at most 30 degrees each, and adds them with
`AddQuadraticBeziers`. Each curve's control point is where the tangents at its ends meet,
so the curves join without corners and stay within 0.06% of the radius off the ellipse.
Quadratic, because Wine draws those exactly but approximates each cubic curve with one
quadratic. An arc with a zero radius goes in as a straight line, as in SVG.

`AddArc` gets only the end point; the start is the figure's current point, which the
interface does not expose. The proxy reads it from Wine's `struct d2d_geometry`: the figures
follow the sink in memory, and the current point is the last vertex of the last figure.
Before replacing anything, the thread checks this on its test path (the current point
must be found where expected, in the second figure, so the figure size is checked too) and
that Wine's `AddArc` still leaves a half circle out (the path's `GetBounds` stops at its
chord); otherwise, as on a Wine that implements arcs, nothing is patched. The log shows
`shfolderproxy: Direct2D arc hook installed in d2d1.dll`, and one `AddArc` stub line from
that check.

The thread finished before the schematic view's swapchain was created, so the first frame
already had the hook; Altium caches each arc's geometry, and a geometry built before the
hook would keep its missing arc. `tests/test_arc.py` checks the conversion on 20,000
random arcs against an independent construction. With the fix, the inductor's four loops
were drawn and nothing else on the sheet changed (an 11 x 85 pixel difference between
captures, at the loops).

## 16. `Control 'View…' has no parent window` (stale active control)

An error dialog, `Control 'View4296989184' has no parent window at 00000014003A377C` (the
number changes), came and went. **Continue** dismissed it. Seen after closing the floating
Messages panel that pops up when a project with warnings opens, while a document was
loading half a minute after start, after resizing the main window from the compositor
(`hyprctl dispatch resizewindowpixel`), after dragging a floating panel across monitors, and
in the component editor.

What happens, from the stack in a `warn+seh` log and from reading the running process.
X2.EXE ships no symbols, so these names come from Delphi's class tables (VMTs) in the binary:

1. Altium's panels are plain `TCustomForm`s hosted inside dock or floating windows. Closing
   the Messages panel hides its floating window and removes the panel's content (a
   `TViewComponent` named `View<address>`) from the form.
2. In the failing case that form's `ActiveControl` (and its focused control) still pointed
   at the removed view, which by then had neither a parent nor a window.
3. The next focus change sends that form `CM_ACTIVATE` or `CM_DEACTIVATE`, and VCL's
   `DoNestedActivation` (`X2.EXE+3512D0`) walks up from the active control to the nearest
   form to pass the message on. For the removed view the walk stops at the view itself.
   Asking it for its window handle makes VCL create the window, which needs a parent, so it
   raises (`TWinControl.CreateWnd`, `X2.EXE+3A377C`).

Why the form keeps the stale pointer is not known. VCL clears it when a control is hidden or
removed (`TCustomForm.DefocusControl`), and in two close cycles traced with gdb (breakpoints
on `SetFocusedControl`, `DefocusControl`, `RemoveControl` and the focus calls in `user32`) it
did. For both pointers to end up on the view, the view's own window must have got keyboard
focus after that clean-up, probably while its child windows were being destroyed: when Wine
hides a focused window, it moves the focus to that window's parent. The error never happened
while the tracer was attached.

**Fix:** the `SHFolder.dll` proxy replaces `DoNestedActivation` before Altium's own code
runs (X2.EXE imports the proxy). It finds the routine by its code: 126 bytes that include the
start of `TCustomForm.Activate` and its call back to the routine, which pins the offsets of
`Parent` and `ActiveControl`. A jump at the routine's start leads to a C version that does the
same, except when the walk ends at a control that has no parent and is not a form. Such a
control is in no form, so it cannot be this form's active control any more: the form's
`ActiveControl` is cleared, and its focused control too if that is inside the removed control,
as `DefocusControl` does when a control is removed, and nothing is sent. The C version calls
VCL's own `is` test, `GetHandle` and `SendMessage`, at addresses read from the matched code.
The proxy is now built with unwind tables (`-fasynchronous-unwind-tables`), so a Delphi
exception raised inside `GetHandle` unwinds through it.

The log shows `shfolderproxy: VCL DoNestedActivation replaced in X2.EXE` at start, and
`shfolderproxy: stale ActiveControl (a removed control) cleared` the first time the fix takes
over. In the first run with it, that happened 27 s after start, while the previous session's
documents were reopening, and no error dialog appeared.

Not covered: VCL also uses the active control when a form is activated through
`WM_ACTIVATE` (`SetWindowFocus`). The fix clears the stale pointer at the first
`CM_ACTIVATE` or `CM_DEACTIVATE`, so that path would only fail if it came first; it has not
been seen.

## 17. Windows split across monitors (Hyprland, mixed scaling)

With a laptop screen at 1.5x and an external monitor at 1x, and Altium started from the
external monitor:

- the splash screen came apart: its centre (logo and version) on the laptop, its circuit
  board on the external monitor, the pieces overlapping each other;
- the main window opened on the external monitor, but the floating Messages panel (shown
  when a project with warnings opens) on the laptop, partly past its left edge and too big;
- once dragged across, floating panels kept the wrong size.

Omarchy sets `xwayland:force_zero_scaling = true`, so X11 (and Wine) see each monitor at its
physical size, laid out left to right, while Hyprland's own layout uses scaled sizes:

| | Laptop (2880x1800, scale 1.5) | External (2560x1440, scale 1) |
|---|---|---|
| X11 | x 0 to 2880 | x 2880 to 5440 |
| Hyprland | x 0 to 1920 | x 1920 to 4480 |

Hyprland converts between the two per monitor, and in two places it picks the wrong one.
Both are in Hyprland 0.56.2 and still in its development branch (October 2026):

1. **Opening a window.** A new window belongs to the focused monitor. If the position it
   asks for is on another monitor, `CWindowTarget::desiredGeometry` moves it onto the
   focused one: it converts the position with the monitor it is on (dividing by 1.5) and
   shifts it by the difference between the monitors' origins, but leaves the size in X11
   pixels. It does this for override-redirect windows too (popups, which X11 window managers
   must not move), one window at a time, so windows placed relative to each other come
   apart.
2. **Moving a window.** Later positions from the client (`CWindow::onX11ConfigureRequest`,
   and `unmanagedSetGeometry` for override-redirect windows) are converted with the monitor
   the window belongs to (`xwaylandToWaylandCoords(pos, m_monitor)`), wherever the position
   is. A position on the laptop is then read with the external monitor's formula: 960 px
   (2880 − 1920) too far left, y not scaled, and the window keeps the external monitor's
   scale, so on the laptop it is 1.5 times too big.

Wine's side of it: XWayland sets no primary output, so Wine takes the monitor at X11 (0,0),
the laptop, as its primary and puts it at Windows (0,0). Altium centres the splash on the
primary monitor and saves the main window and its floating panels as percentages of it
(`UserTools.TLT` in Altium's roaming settings: `FormNormalRect='0.42, 2.11, 88.47, 79.33'`
is the main window, `FrameFloatRect` each floating frame), so nearly every window it opens
asks for a place on the laptop.

Evidence: X11 events on the root window (creation, mapping, every geometry change) logged
next to `hyprctl clients`, during a start and while a project opened:

- Splash, five override-redirect windows: the centre mapped at 466,123 and Hyprland moved
  it to 3191,82 (path 1); Altium then moved it to its final place, 1141,813, which Hyprland
  showed at 181,813 (path 2; right would be 761,542). The four shadow windows mapped at
  their final places, 1123 to 1876, and were each moved to 3629 to 3854 (path 1): the
  right one now started inside the centre.
- Messages panel: mapped at 514,249, 1863x1127; Hyprland moved it to 3223,166 (path 1);
  17 ms later Altium put it back at 514,249, which Hyprland showed at −446,249 (path 2).
- Main window: asked for 12,38 and was moved to the external monitor (path 1); Altium did
  not move it back, so it stayed there.
- Plain X11 test windows behave the same, without Wine.

Turning off `force_zero_scaling` would make the two layouts equal, but would not fix the
splash: path 1 still moves each window onto the focused monitor, and the centre's later move
takes it back.

**Fix:** make the external monitor X11's primary output (`xrandr --output HDMI-A-1
--primary`). [`config/hyprland/altium.lua`](../config/hyprland/altium.lua) does this when
Hyprland starts and whenever a monitor is added, for the first monitor that is not built in
(or the one named in `altium_primary`); XWayland adds a monitor's output a moment after
Hyprland reports it, so the command retries for 10 seconds. Wine reads the primary when the
bottle starts, puts the external monitor at Windows (0,0), and Altium's splash, centred
dialogs, main window and floating panels open there. As long as you work on that monitor,
every window Altium opens or moves stays on the focused monitor, where Hyprland's conversion
is right.

With the fix, all five splash windows were on the external monitor, each at its X11
position minus 960 (centre 2901,633, right shadow 3221 = 2901 + 320, bottom 867 = 633 + 234),
the main window at 1931,30, and the Messages panel on the external monitor.

Side effects of moving Windows (0,0):

- Window sizes and positions that Altium saves as percentages follow the primary monitor.
  A size saved while the bug was active stays wrong: here the Messages frame had been saved
  at 97% by 94% of the screen (`'-2.60, 23.06, 94.44, 117.06'`). Resize the panel once;
  Altium saves the new size when it exits.
- Dialog positions saved in pixels (registry, `DesignExplorer\Forms\<form class>`, `Left`
  and `Top`) are now relative to the external monitor. Those that were on the external
  monitor point past its right edge; subtract the laptop's width (in X11 pixels) from
  `Left` once. Here that was the Design Rule Check dialog (3778) and the menu that picks one
  of several objects under the cursor (4093).
- Other X11 programs also treat the external monitor as primary.

Not fixed: starting Altium while the laptop has the focus, and moving Altium windows onto
the laptop, still go through Hyprland's two code paths (see
[known-issues.md](known-issues.md#windows-misplaced-on-the-other-monitor-mixed-scaling)).
