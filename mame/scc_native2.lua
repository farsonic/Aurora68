-- native: log data-register writes (loopback bytes) with emulated time, and the PC of the first ones
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/scc_native2.txt", "w")
local n = 0
local function sh(mask) if mask == 0xff000000 then return 24 elseif mask == 0x00ff0000 then return 16 elseif mask == 0x0000ff00 then return 8 else return 0 end end
local function rec(o,d,m)
  local reg = o & 0xf
  if reg == 6 then n = n + 1
    if n <= 20 or n % 50 == 0 then log:write(string.format("t=%8.4f W %08X %02X pc %08X (#%d)\n", machine.time.seconds + machine.time.attoseconds/1e18, o, (d >> sh(m)) & 0xff, cpu.state["PC"].value, n)) end
  end
end
w1 = mem:install_write_tap(0x50004000, 0x50005fff, "w1", function(o,d,m) rec(o,d,m) return d end)
w2 = mem:install_write_tap(0x50f04000, 0x50f05fff, "w2", function(o,d,m) rec(o,d,m) return d end)
stop_sub = emu.add_machine_stop_notifier(function() log:write("total data writes " .. n .. "\n") log:close() end)
