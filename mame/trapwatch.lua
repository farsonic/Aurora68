-- trapwatch.lua: on the stock maciici (real 68030), log writes into the OS trap table
-- (logical $400-$7FF = physical $50400-$507FF with 8 MB) made by code running from RAM,
-- with the CPU registers, plus the first longs of the table the patch installer walks.
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open(os.getenv("TRAPWATCH_LOG") or "mame/trapwatch.txt", "w")
local n = 0
local function r(name) return cpu.state[name].value end
trap_w = mem:install_write_tap(0x50400, 0x507ff, "trap_w", function(offset, data, mask)
  local pc = r("PC")
  total = (total or 0) + 1
  if total <= 5 or (pc < 0x40000000 and n < 60) then
    n = n + 1
    log:write(string.format("W %08X <- %08X mask %08X pc %08X a0 %08X a1 %08X d0 %08X d1 %08X d2 %08X sp %08X\n",
      offset, data, mask, pc, r("A0"), r("A1"), r("D0"), r("D1"), r("D2"), r("SP")))
    log:flush()
  end
  return data
end)
local frames = 0
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 300 == 0 then log:write(string.format("frame %d pc %08X total trap-table writes %d; [400]=%08X [408]=%08X [410]=%08X\n", frames, r("PC"), total or 0, mem:read_u32(0x50400), mem:read_u32(0x50408), mem:read_u32(0x50410))) log:flush() end
  if frames == 3000 then machine.video:snapshot() end
end)
