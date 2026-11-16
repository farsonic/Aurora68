-- lowmem.lua: snapshot every 600 frames (like shots.lua) and, at frames 600 and
-- at the end of the run, locate the Mac low-memory page in PHYSICAL RAM
-- (logical 0 is not identity-mapped on the IIci) and dump the timing globals
-- TimeSCSIDB ($B24), TimeVIADB ($CEA), TimeDBRA ($D00), TimeSCCDB ($D02).
-- The page is found by scanning 4 KiB-aligned RAM for ROMBase ($2AE) ==
-- 0x40800000 (in MAME with 8 MB it is physical 0x50000; also printed: VIA $1D4).
-- Output goes to the file named by the LOWMEM_LOG environment variable
-- (default mame/lowmem.txt).
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local logname = os.getenv("LOWMEM_LOG") or "mame/lowmem.txt"
local log = io.open(logname, "a")
local frames = 0
local ramsize = 8 * 1024 * 1024

local function find_lowmem()
  for base = 0, ramsize - 0x1000, 0x1000 do
    if mem:read_u32(base + 0x2ae) == 0x40800000 then return base end
  end
  return nil
end

local function dump(tag)
  local base = find_lowmem()
  if not base then
    log:write(string.format("%s frame %d: low-memory page not found\n", tag, frames))
  else
    log:write(string.format("%s frame %d: lowmem phys base %08X  TimeSCSIDB($B24)=%04X TimeVIADB($CEA)=%04X TimeDBRA($D00)=%04X TimeSCCDB($D02)=%04X  CPUFlag($12F)=%02X ROMBase=%08X VIA($1D4)=%08X\n",
      tag, frames, base,
      mem:read_u16(base + 0xb24), mem:read_u16(base + 0xcea),
      mem:read_u16(base + 0xd00), mem:read_u16(base + 0xd02),
      mem:read_u8(base + 0x12f), mem:read_u32(base + 0x2ae), mem:read_u32(base + 0x1d4)))
  end
  log:flush()
end

frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 600 == 0 then machine.video:snapshot() end
  if frames == 600 or frames % 3000 == 0 then dump("dump") end
end)
stop_sub = emu.add_machine_stop_notifier(function() dump("stop") log:close() end)
