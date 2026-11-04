-- Altium Designer under Wine (Bottles, Proton-based runner), for Hyprland 0.56+ with the Lua
-- config. Load it from ~/.config/hypr/hyprland.lua with the path of your clone:
--
--   dofile(os.getenv("HOME") .. "/altium-on-linux/config/hyprland/altium.lua")
--
-- With SteamAppId=altium in the bottle's environment, Proton's winex11 gives every
-- Altium window the class steam_app_altium (SteamGameId does not work).

-- The repository this file is in: refresh-displays.sh is run from it.
local altium_on_linux = debug.getinfo(1, "S").source:match("^@(.*)/config/hyprland/altium%.lua$")
  or (os.getenv("HOME") .. "/altium-on-linux")

-- * focus_on_activate = false: Wine sends activate requests constantly; honouring them
--   warps the cursor to the window centre (Omarchy applies the same fix to Telegram).
-- * no_follow_mouse: with input:follow_mouse = 1, the cursor leaving a popup for the main
--   window focuses the main window, and Altium closes the popup (the PCB editor's list for
--   picking one of several objects under the cursor). Click to focus, as on Windows
--   (Omarchy applies the same fix to JetBrains IDEs).
hl.window_rule({ match = { class = "^steam_app_altium$" }, focus_on_activate = false, no_follow_mouse = true })
-- * float everything but the main window, whose title has the version in brackets
--   ("<project> - Altium Designer Professional (26.11.1)"): tiled, Delphi dialogs end up
--   behind the main window and spawn ghost frames; floating, they stay above it.
hl.window_rule({
  match = { class = "^steam_app_altium$", title = "negative:.*Altium Designer.*\\(\\d+\\.\\d+.*" },
  float = true,
})

-- Altium and its installer put an orange glow behind their dialogs: a second window
-- (TBehindWindow), 4 px larger on every side, at 62% opacity through
-- _NET_WM_WINDOW_OPACITY. Hyprland ignores that property and stacks the newest window on
-- top, so the glow, opaque, covers the dialog (the installer's Sign In dialog, its
-- warnings). Send the glow to the back and give the focus to the dialog
-- (docs/how-it-works.md §27).
local function altium_glow_around(outer, inner)
  local l = inner.at.x - outer.at.x
  local t = inner.at.y - outer.at.y
  local r = outer.at.x + outer.size.x - inner.at.x - inner.size.x
  local b = outer.at.y + outer.size.y - inner.at.y - inner.size.y
  local lo, hi = math.min(l, t, r, b), math.max(l, t, r, b)
  return lo >= 1 and hi <= 8 and hi - lo <= 1 -- 4 px, or 2-3 on a scaled monitor
end

hl.on("window.open", function(w)
  if w.class ~= "steam_app_altium" then return end
  for _, other in ipairs(hl.get_windows({ class = "steam_app_altium" })) do
    if other.address ~= w.address then
      if altium_glow_around(w, other) then -- the glow opened after its dialog
        hl.dispatch(hl.dsp.window.alter_zorder({ mode = "bottom", window = "address:" .. w.address }))
        hl.dispatch(hl.dsp.focus({ window = "address:" .. other.address }))
        return
      elseif altium_glow_around(other, w) then -- before it
        hl.dispatch(hl.dsp.window.alter_zorder({ mode = "bottom", window = "address:" .. other.address }))
      end
    end
  end
end)

-- Multiple monitors: make the external monitor X11's primary output. Wine puts the X11
-- primary at Windows (0,0); Altium centres its splash and dialogs on it and restores saved
-- window positions relative to it. With monitors at different scales and
-- xwayland:force_zero_scaling, Hyprland misplaces X11 windows that open or move on a monitor
-- other than the focused one (docs/how-it-works.md §17), so keep Altium where you work.
-- Takes effect the next time the bottle starts.
local altium_primary = nil -- an output name such as "DP-1"; nil: the first external monitor

-- Monitors plugged, unplugged or rearranged while Altium runs (a closed lid, a monitor
-- switched off and on) leave Wine with a stale monitor list: a lost monitor, two primaries,
-- Altium drawing offset inside its window. scripts/refresh-displays.sh resyncs it; it does
-- nothing while the bottle isn't running (docs/how-it-works.md §19).
local altium_refresh_displays = altium_on_linux .. "/scripts/refresh-displays.sh --quiet"

local function altium_wants_primary(name)
  if altium_primary then return name == altium_primary end
  return not (name:match("^eDP") or name:match("^LVDS") or name:match("^DSI"))
end

local function altium_set_x11_primary(name)
  -- XWayland adds a monitor's output a moment after Hyprland does: retry for 10 s. Wine
  -- sees no event for a new primary, so resync it afterwards.
  hl.exec_cmd("for i in $(seq 20); do xrandr --listmonitors 2>/dev/null | grep -q ' " .. name ..
    "$' && { xrandr --output " .. name .. " --primary; sleep 1; exec " .. altium_refresh_displays ..
    "; }; sleep 0.5; done")
end

hl.on("hyprland.start", function()
  for _, m in ipairs(hl.get_monitors()) do
    if altium_wants_primary(m.name) then return altium_set_x11_primary(m.name) end
  end
end)

hl.on("monitor.added", function(m)
  if altium_wants_primary(m.name) then altium_set_x11_primary(m.name) end
end)

-- Fires for added, removed and resized monitors; wait for XWayland to settle.
hl.on("monitor.layout_changed", function()
  hl.exec_cmd("sleep 3; " .. altium_refresh_displays)
end)

-- A monitor that goes away leaves Altium half drawn (a stale monitor list) until the resync:
-- run it at once too; XWayland drops the output within moments, and the run above follows.
hl.on("monitor.removed", function()
  hl.exec_cmd("sleep 0.5; " .. altium_refresh_displays)
end)
