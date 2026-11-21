-- mame/floppy.lua - the floppy drive of the bridged Macintosh SE (macse_ps) in the testbed (docs/08 item 93).
--
-- A bridged bus cycle costs tens of microseconds of wall clock, while the floppy turns in MAME's
-- emulated time, which the bridge pins to the wall clock: the ROM's floppy code, which reads the IWM
-- once per GCR byte (16 us), sees one byte in five or so. While the IWM moves data (a byte with bit 7
-- read from the data register with the drive enabled, or a byte written in write mode) this script
-- slows emulated time by IWM_TDIV (bridge68k's state register TDIV, docs/06) and lets it run at the
-- wall clock's pace again IWM_TAIL seconds (emulated) after the last such byte. It also puts the disk
-- back ten seconds after the Mac ejects it, at most REINSERTS (2) times, or inserts it
-- only at FLOP_AT seconds (emulated) when that is given; and takes a screenshot every ten seconds.
--
-- Environment: FLOPPATH (the image, also given to MAME as -flop1 unless FLOP_AT is set), IWM_TDIV
-- (16), IWM_TAIL (0.05), REINSERTS (2), FLOP_AT (0: at the start), IWMLOG (the log of what it did).
--     MAME_SCRIPT=mame/floppy.lua MAME_EXTRA="-flop1 $PWD/floppy.img" FLOPPATH=$PWD/floppy.img \
--       MACHINE=macse_ps tools/run-fullstack.sh 600 mac.board=se ...
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local log = io.open(os.getenv("IWMLOG") or "floppy-lua.log", "w")
local K = tonumber(os.getenv("IWM_TDIV") or "16")
local tail = tonumber(os.getenv("IWM_TAIL") or "0.05")
local flop = os.getenv("FLOPPATH")
local reinserts = tonumber(os.getenv("REINSERTS") or "2")
local flop_at = tonumber(os.getenv("FLOP_AT") or "0")
local function now() return machine.time:as_double() end
local function say(s) log:write(string.format("%.3f %s\n", now(), s)) log:flush() end
local img = machine.images[":fdc:0:35dd"]

-- the IWM's lines as the accesses set them (register n at $DFE1FF + n * $200: line n / 2, on if n is odd)
local q6, q7, en = 0, 0, 0
local last_data, dilated, data_reads, data_writes = -100, false, 0, 0
local function set_div(d)
  cpu.state["TDIV"].value = d
  dilated = d > 1
  say("TDIV " .. d .. " (data bytes read " .. data_reads .. ", written " .. data_writes .. ")")
end
local function access(o, d, m, write)
  local line = (o >> 9) & 15
  local which, on = line >> 1, line & 1
  if which == 6 then q6 = on elseif which == 7 then q7 = on elseif which == 4 then en = on end
  local v = ((m & 0xff) ~= 0) and (d & 0xff) or ((d >> 8) & 0xff)
  local busy = false
  if en == 1 and not write and q6 == 0 and q7 == 0 and (v & 0x80) ~= 0 then data_reads = data_reads + 1 busy = true end
  if en == 1 and write and q6 == 1 and q7 == 1 then data_writes = data_writes + 1 busy = true end
  if busy then
    last_data = now()
    if K > 1 and not dilated then set_div(K) end
  end
end
iwm_r = mem:install_read_tap(0xdfe000, 0xdfffff, "iwmr", function(o, d, m) access(o, d, m, false) return d end)
iwm_w = mem:install_write_tap(0xdfe000, 0xdfffff, "iwmw", function(o, d, m) access(o, d, m, true) return d end)

local frames, gone_at = 0, nil
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 600 == 0 then machine.video:snapshot() end
  if dilated and now() - last_data > tail then set_div(1) end
  if not img or not flop then return end
  if flop_at > 0 then
    if now() >= flop_at then
      say("the disk inserted: " .. tostring(img:load(flop)))
      flop_at = 0
    end
    return
  end
  if not img.exists and not gone_at then gone_at = now() say("the disk was ejected (MAME wrote the image back)") end
  if gone_at and now() - gone_at > 10 and reinserts > 0 then
    reinserts = reinserts - 1
    say("the disk put back: " .. tostring(img:load(flop)))
    gone_at = nil
  end
end)
stop_sub = emu.add_machine_stop_notifier(function() say("stop: data bytes read " .. data_reads .. ", written " .. data_writes) log:close() end)
