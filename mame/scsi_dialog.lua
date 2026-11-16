-- Decode the SCSI dialogue on the MMU-translated alias (0x50010000 regs, 0x50012000 pseudo-DMA)
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/scsi_dialog.txt", "w")
local frames, nlog = 0, 0
local regs = {[0]="ODR","ICR","MR","TCR","SER","SODR","SDS","SDTR"}
local rregs = {[0]="CDR","ICR","MR","TCR","CSR","BSR","IDR","RPI"}
local burst, burst_bytes, burst_first = 0, 0, {}
local function sh(mask) if mask == 0xff000000 then return 24 elseif mask == 0x00ff0000 then return 16 elseif mask == 0x0000ff00 then return 8 else return 0 end end
local function flush_burst()
  if burst_bytes > 0 then
    local s = {} for i = 1, math.min(#burst_first, 24) do s[#s+1] = string.format("%02X", burst_first[i]) end
    log:write(string.format("f%05d   pDMA READ burst #%d: %d bytes: %s%s\n", frames, burst, burst_bytes, table.concat(s, " "), burst_bytes > 24 and " ..." or ""))
    burst_bytes, burst_first = 0, {}
  end
end
w = mem:install_write_tap(0x50010000, 0x500100ff, "w", function(offset, data, mask)
  flush_burst()
  local reg = (offset - 0x50010000) >> 4; local v = (data >> sh(mask)) & 0xff
  if nlog < 6000 then nlog = nlog + 1; log:write(string.format("f%05d W %-4s %02X pc %08X\n", frames, regs[reg] or "?", v, cpu.state["PC"].value)) end
  return data end)
r = mem:install_read_tap(0x50010000, 0x500100ff, "r", function(offset, data, mask)
  local reg = (offset - 0x50010000) >> 4
  if reg == 0 or reg == 6 then flush_burst(); local v = (data >> sh(mask)) & 0xff
    if nlog < 6000 then nlog = nlog + 1; log:write(string.format("f%05d R %-4s %02X pc %08X\n", frames, rregs[reg], v, cpu.state["PC"].value)) end end
  return data end)
d = mem:install_read_tap(0x50012000, 0x50013fff, "d", function(offset, data, mask)
  if burst_bytes == 0 then burst = burst + 1 end
  local n = (mask == 0xffffffff) and 4 or ((mask == 0xffff0000 or mask == 0x0000ffff) and 2 or 1)
  for i = 0, n-1 do if #burst_first < 24 then burst_first[#burst_first+1] = (data >> (24 - 8*(i + (mask == 0x0000ffff and 2 or 0)))) & 0xff end end
  burst_bytes = burst_bytes + n
  return data end)
dw = mem:install_write_tap(0x50012000, 0x50013fff, "dw", function(offset, data, mask)
  if nlog < 6000 then nlog = nlog + 1; log:write(string.format("f%05d W pDMA %08X mask %08X\n", frames, data, mask)) end return data end)
frame_sub = emu.add_machine_frame_notifier(function() frames = frames + 1 end)
stop_sub = emu.add_machine_stop_notifier(function() flush_burst() log:close() end)
