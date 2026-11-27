-- act.lua: snapshots every 600 frames plus a scripted ADB mouse, for machines whose ports sit under a device
-- (":macadb:MOUSE0" on macse_ps). ACT_PLAN env: "frame:op;frame:op;..." with op one of
--   Mx,y   park the pointer at the top left, then walk it to (x,y)
--   Rdx,dy walk the pointer by (dx,dy) from where it is
--   D / U  button down / up        S  snapshot
-- ACT_WAIT=menus: the plan's frame numbers count from 1200 frames after menu titles show to the right of the
-- Apple (x 40-250, y 5-13 of a 512x342 screen) for the second time following 50 s without any: the first time is
-- the Mac OS that starts A/UX, the second A/UX's Finder. A slow host then does not matter.
-- The mouse ports are 8-bit counters: a running value is kept and set each frame, 4 counts a frame at most.
-- Parking takes 200 frames, a walk one frame per 4 counts; leave room before the next op.
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
local frames, q, cx, cy = 0, {}, 0, 0
local wait_menus, base, gap, seen = (os.getenv("ACT_WAIT") == "menus"), nil, 0, 0
local function menus_up()
  local scr = machine.screens[":screen"]
  if not scr then for _, s2 in pairs(machine.screens) do scr = s2 end end
  if not scr then return false end
  -- the reference pixel has the samples' parity (x even, y odd), so the desktop's checkerboard counts as empty
  local ok, n = pcall(function()
    local first, c = scr:pixel(300, 9), 0
    for x = 40, 250, 2 do for y = 5, 13, 2 do if scr:pixel(x, y) ~= first then c = c + 1 end end end
    return c
  end)
  if not ok then print("[act] screen:pixel failed: " .. tostring(n)); wait_menus = false; return false end
  return n > 20
end
local function rel(dx, dy)
  cx = (cx + dx) % 256; cy = (cy + dy) % 256
  if mx then mx:set_value(cx) end if my then my:set_value(cy) end
end
local function walk(x, y)
  local n = math.max(math.ceil(math.abs(x) / 4), math.ceil(math.abs(y) / 4), 1)
  local sx, sy = 0, 0
  for i = 1, n do
    local tx, ty = math.floor(x * i / n + 0.5), math.floor(y * i / n + 0.5)
    table.insert(q, { tx - sx, ty - sy }); sx, sy = tx, ty
  end
end
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 600 == 0 then machine.video:snapshot() end
  if wait_menus and not base and frames % 120 == 0 then
    if menus_up() then
      if gap >= 3000 then
        seen = seen + 1
        print(string.format("[act] menus at frame %d after %d frames without (appearance %d)", frames, gap, seen))
        if seen == 2 then
          base = frames + 1200
          machine.video:snapshot()
        end
      end
      gap = 0
    else
      gap = gap + 120
    end
  end
  local p = plan[wait_menus and (base and frames - base or -1) or frames]
  if p then
    print(string.format("[act] frame %d: %s %d,%d", frames, p.op, p.a, p.b))
    if p.op == "M" then for i = 1, 200 do table.insert(q, { -4, -4 }) end walk(p.a, p.b)
    elseif p.op == "R" then walk(p.a, p.b)
    elseif p.op == "D" then btn:set_value(1)
    elseif p.op == "U" then btn:set_value(0)
    elseif p.op == "S" then machine.video:snapshot() end
  end
  if #q > 0 then local m = table.remove(q, 1); rel(m[1], m[2]) end
end)
