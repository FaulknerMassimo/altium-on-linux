-- Altium Designer under Wine (Bottles, Proton-based runner). Add to
-- ~/.config/hypr/hyprland.lua (Hyprland 0.56+ Lua config, Omarchy's o.window helper).
--
-- With SteamAppId=altium in the bottle's environment, Proton's winex11 gives every
-- Altium window the class steam_app_altium (SteamGameId does not work).
-- * float: tiling hides Delphi dialogs behind the main window and spawns ghost frames.
-- * focus_on_activate = false: Wine sends activate requests constantly; honouring them
--   warps the cursor to the window centre (Omarchy applies the same fix to Telegram).
o.window("^steam_app_altium$", { float = true, focus_on_activate = false })

-- Multiple monitors: make the external monitor X11's primary output. Wine puts the X11
-- primary at Windows (0,0); Altium centres its splash and dialogs on it and restores saved
-- window positions relative to it. With monitors at different scales and
-- xwayland:force_zero_scaling, Hyprland misplaces X11 windows that open or move on a monitor
-- other than the focused one (docs/how-it-works.md §17), so keep Altium where you work.
-- Takes effect the next time the bottle starts.
local altium_primary = nil -- an output name such as "DP-1"; nil: the first external monitor

local function altium_wants_primary(name)
  if altium_primary then return name == altium_primary end
  return not (name:match("^eDP") or name:match("^LVDS") or name:match("^DSI"))
end

local function altium_set_x11_primary(name)
  -- XWayland adds a monitor's output a moment after Hyprland does: retry for 10 s.
  hl.exec_cmd("for i in $(seq 20); do xrandr --listmonitors 2>/dev/null | grep -q ' " .. name ..
    "$' && exec xrandr --output " .. name .. " --primary; sleep 0.5; done")
end

hl.on("hyprland.start", function()
  for _, m in ipairs(hl.get_monitors()) do
    if altium_wants_primary(m.name) then return altium_set_x11_primary(m.name) end
  end
end)

hl.on("monitor.added", function(m)
  if altium_wants_primary(m.name) then altium_set_x11_primary(m.name) end
end)
