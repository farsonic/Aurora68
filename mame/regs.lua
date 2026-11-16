local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/regs.txt", "w")
local frames, hits = 0, 0
log:write("state entries: ")
for k, v in pairs(cpu.state) do log:write(k .. " ") end
log:write("\n")
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  local pc = cpu.state["PC"].value
  if (pc == 0x4080786A or pc == 0x40807870) and hits < 6 then
    hits = hits + 1
    local a3 = cpu.state["A3"].value
    local function st(n) local s = cpu.state[n] return s and string.format("%X", s.value) or "n/a" end
    log:write(string.format("frame %d pc %08X A3 %08X A4 %08X D1 %08X D5 %08X SR %s TC %s SRP %s CRP %s\n", frames, pc, a3, cpu.state["A4"].value, cpu.state["D1"].value, cpu.state["D5"].value, st("SR"), st("TC"), st("SRP"), st("CRP")))
    log:write(string.format("   phys read via space: [A3+40]=%02X  [50F10040]=%02X  lowmem SCSIBase($C00)=%08X MMU32bit($CB2)=%02X\n", mem:read_u8(a3 + 0x40), mem:read_u8(0x50f10040), mem:read_u32(0xc00), mem:read_u8(0xcb2)))
    log:flush()
  end
end)
stop_sub = emu.add_machine_stop_notifier(function() log:close() end)
