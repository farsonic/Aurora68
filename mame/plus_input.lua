-- plus_input.lua: the Macintosh Plus testbed's keyboard and mouse, driven from MAME (macplus_ps with -kbd usp).
-- SPDX-License-Identifier: MPL-2.0
-- Command-Q on the M0110A, then the Plus mouse opens the Apple menu and chooses "About This Macintosh",
-- with a snapshot after each step (and one every 600 frames, as shots.lua). Each snapshot's number and
-- what it shows go to the log as "[plus_input] snap N: ...". Frame numbers assume the Finder is up by
-- frame 12000 (System 7.1 at 32 MiB reaches it near 8400). PLUS_INPUT_START= moves the start.
local machine = manager.machine
local frames, snaps = 0, 0
local start = tonumber(os.getenv("PLUS_INPUT_START") or "12000")
local mx, my = 0, 0
-- a field by name: the whole name ("Command", "Backspace") or its first word (MAME names the letter keys
-- "q  Q", "p  P": the unshifted and shifted characters)
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
  print(string.format("[plus_input] snap %04d (frame %d): %s", snaps, frames, what))
  snaps = snaps + 1
end
local keys = {}
local function key(name) if not keys[name] then keys[name] = port_field(":kbd", name) end return keys[name] end
local button
local mouse_x, mouse_y
local plan = {}           -- frame offset -> function
local function at(off, fn) plan[start + off] = fn end
local moves = {}          -- frame -> {dx, dy}
local function move(off, n, dx, dy, every)
  every = every or 1
  for i = 0, n - 1 do
    local f = start + off + i * every
    local m = moves[f] or {0, 0}
    m[1] = m[1] + dx; m[2] = m[2] + dy
    moves[f] = m
  end
end
local function press(off, name) at(off, function() local f = key(name) if f then f:set_value(1) else print("[plus_input] no key " .. name) end end)
                                at(off + 8, function() local f = key(name) if f then f:set_value(0) end end) end

-- the Finder is in front (nothing in Startup Items): the Plus mouse into the top-left corner, onto the
-- Apple menu, down to the first item, "About This Macintosh", and let go
move(0, 250, -3, -3)
move(260, 14, 1, 0); move(260, 4, 0, 1)
at(300, function() snap("the Finder, the pointer on the Apple menu") end)
at(320, function() if button then button:set_value(1) end end)
at(360, function() snap("button down on the Apple menu") end)
move(370, 22, 0, 1, 2)
at(420, function() snap("dragged to About This Macintosh") end)
at(430, function() if button then button:set_value(0) end end)
at(700, function() snap("About This Macintosh") end)
at(1000, function() snap("About This Macintosh, later") end)

plus_input_sub = emu.add_machine_frame_notifier(function()   -- a global: a local would be garbage-collected and the notifier dropped
  frames = frames + 1
  if frames == 1 then
    button = port_field(":MOUSE0", "Mouse Button")
    for tag, port in pairs(machine.ioport.ports) do
      if tag == ":MOUSE1" then for _, f in pairs(port.fields) do mouse_x = f end end
      if tag == ":MOUSE2" then for _, f in pairs(port.fields) do mouse_y = f end end
    end
    local n = 0
    for tag, port in pairs(machine.ioport.ports) do if tag:find(":kbd", 1, true) then for _ in pairs(port.fields) do n = n + 1 end end end
    print(string.format("[plus_input] start at frame %d; keyboard fields %d, button %s, mouse %s/%s", start, n,
      button and "ok" or "missing", mouse_x and "ok" or "missing", mouse_y and "ok" or "missing"))
  end
  if frames % 600 == 0 then snap("periodic") end
  local m = moves[frames]
  if m and mouse_x and mouse_y then
    mx = (mx + m[1]) % 256; my = (my + m[2]) % 256
    mouse_x:set_value(mx); mouse_y:set_value(my)
  end
  local fn = plan[frames]
  if fn then fn() end
end)
