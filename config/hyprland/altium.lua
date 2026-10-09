-- Altium Designer under Wine (Bottles, Proton-based runner). Add to
-- ~/.config/hypr/hyprland.lua (Hyprland 0.56+ Lua config, Omarchy's o.window helper).
--
-- With SteamAppId=altium in the bottle's environment, Proton's winex11 gives every
-- Altium window the class steam_app_altium (SteamGameId does not work).
-- * float: tiling hides Delphi dialogs behind the main window and spawns ghost frames.
-- * focus_on_activate = false: Wine sends activate requests constantly; honouring them
--   warps the cursor to the window centre (Omarchy applies the same fix to Telegram).
o.window("^steam_app_altium$", { float = true, focus_on_activate = false })
