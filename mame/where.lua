-- PC histogram + access counts over both the 32-bit and 24-bit I/O windows (per 4 KiB page)
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/where.txt", "w")
local pcs, pages, frames = {}, {}, 0
local function cnt(tag) return function(offset, data, mask) local k = string.format("%s %08X", tag, offset & ~0xfff) pages[k] = (pages[k] or 0) + 1 return data end end
t1 = mem:install_read_tap(0x00f00000, 0x00ffffff, "io24r", cnt("24R"))
t2 = mem:install_write_tap(0x00f00000, 0x00ffffff, "io24w", cnt("24W"))
t3 = mem:install_read_tap(0x50f00000, 0x50f3ffff, "io32r", cnt("32R"))
t4 = mem:install_write_tap(0x50f00000, 0x50f3ffff, "io32w", cnt("32W"))
t5 = mem:install_read_tap(0x00800000, 0x0087ffff, "rom24", cnt("ROM24R"))
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  local pc = cpu.state["PC"].value
  pcs[pc] = (pcs[pc] or 0) + 1
  if frames % 600 == 0 then machine.video:snapshot() end
end)
stop_sub = emu.add_machine_stop_notifier(function()
  log:write("=== PC samples (one per frame) ===\n")
  local ks = {} for k in pairs(pcs) do ks[#ks+1] = k end
  table.sort(ks, function(a,b) return pcs[a] > pcs[b] end)
  for i = 1, math.min(25, #ks) do log:write(string.format("%08X %d\n", ks[i], pcs[ks[i]])) end
  log:write("=== page access counts ===\n")
  local pk = {} for k in pairs(pages) do pk[#pk+1] = k end
  table.sort(pk)
  for _, k in ipairs(pk) do log:write(string.format("%s %d\n", k, pages[k])) end
  log:write(string.format("frames %d\n", frames))
  log:close()
end)
