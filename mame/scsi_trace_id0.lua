-- Log every NCR 5380 register access (physical 50F10000 region + pseudo-DMA + handshake) with value, PC and frame.
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local name = "mame/scsilog_id0.txt"
local log = io.open(name, "w")
local frames = 0
local n = 0
local regs = {[0]="CDR/ODR","ICR","MR","TCR","CSR/SER","BSR/SODR","IDR/SDS","RPI/SDTR"}
local function shift(mask) if mask == 0xff000000 then return 24 elseif mask == 0x00ff0000 then return 16 elseif mask == 0x0000ff00 then return 8 else return 0 end end
local function rec(kind, base, offset, data, mask)
  n = n + 1
  if n > 20000 then return end
  local reg = (offset - base) >> 4
  local val = (data >> shift(mask)) & 0xff
  local rn = regs[reg] or "?"
  log:write(string.format("f%05d %s %08X %-8s %02X pc %08X\n", frames, kind, offset, rn, val, cpu.state["PC"].value))
end
scsi_r = mem:install_read_tap(0x50f10000, 0x50f100ff, "scsi_r", function(offset, data, mask) rec("R", 0x50f10000, offset, data, mask) return data end)
scsi_w = mem:install_write_tap(0x50f10000, 0x50f100ff, "scsi_w", function(offset, data, mask) rec("W", 0x50f10000, offset, data, mask) return data end)
dma_r = mem:install_read_tap(0x50f12000, 0x50f13fff, "dma_r", function(offset, data, mask) n=n+1 if n<=20000 then log:write(string.format("f%05d R DMA %08X pc %08X\n", frames, offset, cpu.state["PC"].value)) end return data end)
dma_w = mem:install_write_tap(0x50f12000, 0x50f13fff, "dma_w", function(offset, data, mask) n=n+1 if n<=20000 then log:write(string.format("f%05d W DMA %08X pc %08X\n", frames, offset, cpu.state["PC"].value)) end return data end)
hsk_r = mem:install_read_tap(0x50f06000, 0x50f07fff, "hsk_r", function(offset, data, mask) n=n+1 if n<=20000 then log:write(string.format("f%05d R HSK %08X pc %08X\n", frames, offset, cpu.state["PC"].value)) end return data end)
hsk_w = mem:install_write_tap(0x50f06000, 0x50f07fff, "hsk_w", function(offset, data, mask) n=n+1 if n<=20000 then log:write(string.format("f%05d W HSK %08X pc %08X\n", frames, offset, cpu.state["PC"].value)) end return data end)
frame_sub = emu.add_machine_frame_notifier(function() frames = frames + 1 if frames % 600 == 0 then machine.video:snapshot() log:flush() end end)
stop_sub = emu.add_machine_stop_notifier(function() log:write(string.format("total scsi-related accesses: %d\n", n)) log:close() end)
