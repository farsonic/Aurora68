-- click.lua: snapshots every 600 frames plus scripted ADB mouse input for the testbed machine.
-- CLICK_PLAN env: "frame:x,y[:dbl];frame:x,y..." — at the given frame, move the pointer to (x,y)
-- (screen 640x480, pointer parked at 0,0 first) and click (double-click with :dbl).
local machine = manager.machine
local ports = machine.ioport.ports
local function field(port, name) local p = ports[port]; if not p then return nil end for k, f in pairs(p.fields) do if k == name or f.name == name then return f end end return nil end
local btn = field(":MOUSE0", "Mouse Button 1") or field(":MOUSE0", "Mouse Button")
local mx = field(":MOUSE1", "Mouse X") or field(":MOUSE1", "Mouse X 1")
local my = field(":MOUSE2", "Mouse Y") or field(":MOUSE2", "Mouse Y 1")
local plan = {}
for item in string.gmatch(os.getenv("CLICK_PLAN") or "", "[^;]+") do
  local f, x, y, d = item:match("(%d+):(%d+),(%d+):?(%a*)")
  if f then table.insert(plan, { frame = tonumber(f), x = tonumber(x), y = tonumber(y), dbl = (d == "dbl") }) end
end
local frames, step, pos = 0, nil, nil
local function rel(dx, dy) if mx then mx:set_value(dx) end if my then my:set_value(dy) end end
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 600 == 0 then machine.video:snapshot() end
  if step then
    -- a small state machine, one action per frame
    step.t = step.t + 1
    if step.t <= 20 then rel(-127, -127)                          -- park at top-left
    elseif step.t <= 20 + step.moves then rel(step.dx, step.dy)    -- walk to the target
    elseif step.t == 21 + step.moves then rel(0, 0) btn:set_value(1)
    elseif step.t == 23 + step.moves then btn:set_value(0)
    elseif step.dbl and step.t == 26 + step.moves then btn:set_value(1)
    elseif step.dbl and step.t == 28 + step.moves then btn:set_value(0)
    elseif step.t > 40 + step.moves then rel(0, 0) step = nil
    end
  else
    rel(0, 0)
    for i, p in ipairs(plan) do
      if p.frame == frames then
        local n = math.max(math.ceil(p.x / 60), math.ceil(p.y / 60), 1)
        step = { t = 0, moves = n, dx = math.floor(p.x / n), dy = math.floor(p.y / n), dbl = p.dbl }
        machine.video:snapshot()
      end
    end
  end
end)
