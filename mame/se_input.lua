-- se_input.lua: the Macintosh SE testbed's own ADB keyboard and mouse, driven from MAME (macse_ps).
-- SPDX-License-Identifier: MPL-2.0
-- MAME's SE has an ADB keyboard and mouse of its own behind its transceiver (the macadb device: ports
-- :macadb:MOUSE0 the button, :macadb:MOUSE1 and :macadb:MOUSE2 the X and Y counters, :macadb:KEY0 to KEY5
-- the keys). They stand for the real SE's keyboard and mouse when the kernel adds its virtual ADB devices
-- (mac.adb, docs/08 item 151): this script moves MAME's mouse into the corner and onto the File menu, holds
-- the button there, lets go, and types an M on MAME's keyboard, with a snapshot after each step and one
-- every 600 frames, as shots.lua. SE_INPUT_START= is the frame it starts at (about 65 a second of the host's
-- time in the testbed). With SE_INPUT_WAIT=1 the frames count from the moment a menu bar with titles has stood
-- for 4800 frames (rows 1 and 2 white from x 40 to 440, and 60 to 800 dark pixels at x 40-250, y 5-13): the Mac OS
-- that starts A/UX shows its Finder's bar for a few seconds only, A/UX's grey start-up screen is no bar at all,
-- and A/UX's Finder stays.
-- One count a frame: the ports are 8-bit counters the transceiver's model reads when the host polls, and a host
-- busy with other devices polls seldom (250 frames at 3 counts wrapped the counter, and the pointer went astray).
-- A key is held for a second: the model takes eight frames to signal a service request, and a key let go by
-- then is never seen.
local machine = manager.machine
local frames, snaps = 0, 0
local start = tonumber(os.getenv("SE_INPUT_START") or "25000")
local wait_n, seen, base = tonumber(os.getenv("SE_INPUT_WAIT") or "0"), 0, nil
local function menus_up()
  local scr = machine.screens[":screen"]
  if not scr then for _, s2 in pairs(machine.screens) do scr = s2 end end
  if not scr then return false end
  local ok, res = pcall(function()
    local white = scr:pixel(300, 1)
    for x = 40, 440, 2 do if scr:pixel(x, 1) ~= white or scr:pixel(x, 2) ~= white then return false end end
    local c = 0
    for x = 40, 250 do for y = 5, 13 do if scr:pixel(x, y) ~= white then c = c + 1 end end end
    return c >= 60 and c <= 800
  end)
  return ok and res
end
local function port_field(tagpart, name)
  for tag, port in pairs(machine.ioport.ports) do
    if tag:find(tagpart, 1, true) then
      for fname, field in pairs(port.fields) do
        local first = fname:match("^(%S+)") or fname
        if fname:lower() == name:lower() or first:lower() == name:lower() then return field end
      end
    end
  end
end
local function snap(what)
  machine.video:snapshot()
  print(string.format("[se_input] snap %04d (frame %d): %s", snaps, frames, what))
  snaps = snaps + 1
end
local plan, moves = {}, {}
local function at(off, fn) plan[off] = fn end
local function move(off, n, dx, dy, every)
  every = every or 1
  for i = 0, n - 1 do
    local f = off + i * every
    local m = moves[f] or {0, 0}
    m[1] = m[1] + dx; m[2] = m[2] + dy
    moves[f] = m
  end
end
local function press(off, name)
  at(off, function() local f = port_field(":macadb:KEY", name) if f then f:set_value(1) else print("[se_input] no key " .. name) end end)
  at(off + 60, function() local f = port_field(":macadb:KEY", name) if f then f:set_value(0) end end)
end

move(0, 560, -1, -1)                      -- the corner
move(600, 50, 1, 0, 2); move(600, 8, 0, 1, 2)   -- onto "File"
at(760, function() snap("MAME's mouse: the pointer on File") end)
at(780, function() if button then button:set_value(1) end end)
at(1100, function() snap("MAME's mouse: the button down on File") end)
at(1500, function() if button then button:set_value(0) end end)
at(1600, function() snap("MAME's mouse: the button up") end)
press(1650, "m")
at(1900, function() snap("MAME's keyboard: M typed") end)

se_input_sub = emu.add_machine_frame_notifier(function()   -- a global: a local would be garbage-collected and the notifier dropped
  frames = frames + 1
  if frames == 1 then
    for tag, port in pairs(machine.ioport.ports) do
      if tag == ":macadb:MOUSE0" then for _, f in pairs(port.fields) do button = f end end
      if tag == ":macadb:MOUSE1" then for _, f in pairs(port.fields) do mouse_x = f end end
      if tag == ":macadb:MOUSE2" then for _, f in pairs(port.fields) do mouse_y = f end end
    end
    print(string.format("[se_input] start at frame %d; button %s, mouse %s/%s, the M key %s", start,
      button and "ok" or "missing", mouse_x and "ok" or "missing", mouse_y and "ok" or "missing", port_field(":macadb:KEY", "m") and "ok" or "missing"))
  end
  if frames % 600 == 0 then snap("periodic") end
  if wait_n > 0 and not base and frames % 120 == 0 then
    if menus_up() then
      seen = seen + 1
      if seen == 40 then base = frames; print(string.format("[se_input] a menu bar with titles for 4800 frames, at frame %d", frames)) end
    else seen = 0 end
  end
  local t = frames - start - (base or 0)
  if wait_n > 0 and not base then t = -1 end
  local m = moves[t]
  if m and mouse_x and mouse_y then
    mx = (mx + m[1]) % 256; my = (my + m[2]) % 256
    mouse_x:set_value(mx); mouse_y:set_value(my)
  end
  local fn = plan[t]
  if fn then fn() end
end)
