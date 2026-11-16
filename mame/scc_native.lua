-- log SCC accesses (both aliases) with values and PC on the native maciici, first 300 after MMU enable-ish (frame > 60)
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/scc_native.txt", "w")
local n, frames = 0, 0
local function sh(mask) if mask == 0xff000000 then return 24 elseif mask == 0x00ff0000 then return 16 elseif mask == 0x0000ff00 then return 8 else return 0 end end
local function rec(k, offset, data, mask)
  if frames < 30 then return end
  n = n + 1
  if n <= 400 then log:write(string.format("f%04d %s %08X %02X pc %08X\n", frames, k, offset, (data >> sh(mask)) & 0xff, cpu.state["PC"].value)) end
end
r1 = mem:install_read_tap(0x50004000, 0x50005fff, "r1", function(o,d,m) rec("R",o,d,m) return d end)
w1 = mem:install_write_tap(0x50004000, 0x50005fff, "w1", function(o,d,m) rec("W",o,d,m) return d end)
r2 = mem:install_read_tap(0x50f04000, 0x50f05fff, "r2", function(o,d,m) rec("R",o,d,m) return d end)
w2 = mem:install_write_tap(0x50f04000, 0x50f05fff, "w2", function(o,d,m) rec("W",o,d,m) return d end)
frame_sub = emu.add_machine_frame_notifier(function() frames = frames + 1 end)
stop_sub = emu.add_machine_stop_notifier(function() log:write("total " .. n .. "\n") log:close() end)
