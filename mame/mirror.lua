local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/mirror.txt", "w")
local pages = {}
local function cnt(tag) return function(offset, data, mask) local k = string.format("%s %08X", tag, offset & ~0xfff) pages[k] = (pages[k] or 0) + 1 return data end end
t1 = mem:install_read_tap(0x50000000, 0x50ffffff, "ior", cnt("R"))
t2 = mem:install_write_tap(0x50000000, 0x50ffffff, "iow", cnt("W"))
stop_sub = emu.add_machine_stop_notifier(function()
  local pk = {} for k in pairs(pages) do pk[#pk+1] = k end
  table.sort(pk)
  for _, k in ipairs(pk) do log:write(string.format("%s %d\n", k, pages[k])) end
  log:close()
end)
