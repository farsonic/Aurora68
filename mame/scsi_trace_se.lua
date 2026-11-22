-- SPDX-License-Identifier: MPL-2.0
-- The SE's NCR 5380 ($580000-$5FFFFF on macse_ps): every register access (register from address bits 4-6,
-- pseudo-DMA when bit 9 is set) with value and frame, the first LIMIT of them, into LOGNAME. A snapshot
-- every ten seconds, as shots.lua. For decoding the commands the ROM sends (22 November 2026).
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open(os.getenv("SCSILOG") or "mame/scsilog_se.txt", "w")
local limit = tonumber(os.getenv("SCSILIMIT") or "400000")
local frames, n, dma = 0, 0, 0
local regs = {[0]="CDR/ODR","ICR","MR","TCR","CSR/SER","BSR/SODR","IDR/SDS","RPI/SDTR"}
local function rec(kind, offset, data, mask)
  local d = (offset & 0x200) ~= 0
  if d then dma = dma + 1 end
  n = n + 1
  if n > limit then return end
  local val = (mask & 0xff00) ~= 0 and ((data >> 8) & 0xff) or (data & 0xff)
  if d then log:write(string.format("f%05d %s DMA %02X\n", frames, kind, val))
  else log:write(string.format("f%05d %s %-8s %02X\n", frames, kind, regs[(offset >> 4) & 7], val)) end
end
tr = mem:install_read_tap(0x580000, 0x5fffff, "se_scsi_r", function(o, d, m) rec("R", o, d, m) return d end)
tw = mem:install_write_tap(0x580000, 0x5fffff, "se_scsi_w", function(o, d, m) rec("W", o, d, m) return d end)
fs = emu.add_machine_frame_notifier(function() frames = frames + 1 if frames % 600 == 0 then machine.video:snapshot() log:flush() end end)
ss = emu.add_machine_stop_notifier(function() log:write(string.format("total %d accesses, %d pseudo-DMA\n", n, dma)) log:close() end)
