-- act.lua: snapshots every 600 frames plus a scripted ADB mouse, for machines whose ports sit under a device
-- (":macadb:MOUSE0" on macse_ps). ACT_PLAN env: "frame:op;frame:op;..." with op one of
--   Mx,y   park the pointer at the top left, then walk it to (x,y)
--   Rdx,dy walk the pointer by (dx,dy) from where it is
--   D / U  button down / up        S  snapshot
-- A walk takes 20 frames to park plus one frame per 30 pixels; leave room before the next op.
local machine = manager.machine
local function field(tagpart)
  for tag, port in pairs(machine.ioport.ports) do
    if tag:find(tagpart, 1, true) then for _, f in pairs(port.fields) do return f end end
  end
end
local btn, mx, my = field("MOUSE0"), field("MOUSE1"), field("MOUSE2")
local plan = {}
for item in string.gmatch(os.getenv("ACT_PLAN") or "", "[^;]+") do
  local f, op, a, b = item:match("^(%d+):(%a)(-?%d*),?(-?%d*)$")
  if f then plan[tonumber(f)] = { op = op, a = tonumber(a) or 0, b = tonumber(b) or 0 } end
end
local frames, q = 0, {}
local function rel(dx, dy) if mx then mx:set_value(dx) end if my then my:set_value(dy) end end
local function walk(x, y)
  local n = math.max(math.ceil(math.abs(x) / 30), math.ceil(math.abs(y) / 30), 1)
  local sx, sy = 0, 0
  for i = 1, n do
    local tx, ty = math.floor(x * i / n + 0.5), math.floor(y * i / n + 0.5)
    table.insert(q, { tx - sx, ty - sy }); sx, sy = tx, ty
  end
end
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 600 == 0 then machine.video:snapshot() end
  local p = plan[frames]
  if p then
    print(string.format("[act] frame %d: %s %d,%d", frames, p.op, p.a, p.b))
    if p.op == "M" then for i = 1, 20 do table.insert(q, { -127, -127 }) end walk(p.a, p.b)
    elseif p.op == "R" then walk(p.a, p.b)
    elseif p.op == "D" then btn:set_value(1)
    elseif p.op == "U" then btn:set_value(0)
    elseif p.op == "S" then machine.video:snapshot() end
  end
  if #q > 0 then local m = table.remove(q, 1); rel(m[1], m[2]) else rel(0, 0) end
end)
