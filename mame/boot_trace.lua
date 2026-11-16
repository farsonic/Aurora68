-- MAME autoboot script: periodic screenshots + I/O access trace for the IIci ROM / System 7.1 boot
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open("mame/iolog.txt", "w")
local counts = {}
local seq = 0
local MAXSEQ = 4000
local function rec(kind, offset, mask)
  local key = string.format("%s %08X %08X", kind, offset, mask)
  counts[key] = (counts[key] or 0) + 1
  if seq < MAXSEQ then
    seq = seq + 1
    log:write(string.format("%6d %s %08X mask %08X pc %08X\n", seq, kind, offset, mask, cpu.state["PC"].value))
  end
end
-- I/O space (physical): VIA1 50F00000, VIA2 50F02000, SCC 50F04000, SCSI hsk 50F06000, SCSI 50F10000/12000, ASC 50F14000, SWIM 50F16000, VDAC 50F24000, RBV 50F26000
rt_keep = mem:install_read_tap(0x50f00000, 0x50f3ffff, "io_r", function(offset, data, mask) rec("R", offset, mask) return data end)
wt_keep = mem:install_write_tap(0x50f00000, 0x50f3ffff, "io_w", function(offset, data, mask) rec("W", offset, mask) return data end)
-- ROM and slot-space accesses are not traced; RAM is not traced.
local frames = 0
local shots = 0
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 300 == 0 then  -- every ~5 emulated seconds
    shots = shots + 1
    machine.video:snapshot()
    log:write(string.format("--- snapshot %d at frame %d, pc %08X\n", shots, frames, cpu.state["PC"].value))
    log:flush()
  end
end)
stop_sub = emu.add_machine_stop_notifier(function()
  log:write("\n=== access counts (kind addr mask count) ===\n")
  local keys = {}
  for k in pairs(counts) do keys[#keys+1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do log:write(string.format("%s %d\n", k, counts[k])) end
  log:close()
end)
