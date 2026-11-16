-- For the bridge machine: decode characters written to the SCC data registers (the ROM's serial
-- diagnostic output) and log the first SCC control accesses with values.
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/scc_tap.txt", "w")
local text = ""
local n = 0
local function sh(mask) if mask == 0xff000000 then return 24 elseif mask == 0x00ff0000 then return 16 elseif mask == 0x0000ff00 then return 8 else return 0 end end
local function rec(k, offset, data, mask)
  local v = (data >> sh(mask)) & 0xff
  local reg = offset & 0xf
  if k == "W" and (reg == 6 or reg == 4) then
    text = text .. ((v >= 32 and v < 127) and string.char(v) or string.format("<%02X>", v))
    log:write(string.format("SERIAL %s: %s\n", reg == 6 and "A" or "B", text)); log:flush()
  end
  n = n + 1
  if n <= 300 then log:write(string.format("%s %08X %02X\n", k, offset, v)) end
end
for _, base in ipairs({0x50004000, 0x50f04000}) do
  _G["r" .. base] = mem:install_read_tap(base, base + 0x1fff, "r", function(o,d,m) rec("R",o,d,m) return d end)
  _G["w" .. base] = mem:install_write_tap(base, base + 0x1fff, "w", function(o,d,m) rec("W",o,d,m) return d end)
end
stop_sub = emu.add_machine_stop_notifier(function() log:write("total " .. n .. "\nFINAL SERIAL: " .. text .. "\n") log:close() end)
